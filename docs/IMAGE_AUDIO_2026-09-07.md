# 图片与 AP580 语音移植

参考 HPM6750 提交 `c84c683eb9e282c3ff7c819623f7a4f1d4078abb`。未修改或烧录 HPM 工程。P4 为 Function EV Board + C6，COM21，芯片 v3.2、400 MHz。

## 使用

- LCD 顶部选择 Rx/Tx 和 Text/Image/Audio。Image TX 循环发送与 HPM 相同的 96×96 热气球测试图；Image RX 逐行显示通过 CRC 的内容与进度。
- Image 模式过滤预计超过 180 秒的调制档位。当前模式不适用时自动选择 `2x8PSK1200F`；**这只是协议速率选择，不代表实物链路零误码**。本次实物连线用 `8PSK1000 / 2000 Hz` 收到了完整图片，接收增益为 10 dB。
- Audio 固定 `2x8PSK1200F / 2000 Hz`。接收时显示有效分组、帧、CRC 错误、UART 收发和最近一帧；UART 已配置时按模块节拍播放。TX 从 AP580 UART 获取压缩语音，没有输入时不启动调制。
- 手机连 `ESP32P4-PSK`，密码 `pskmodem32`，刷新 `http://192.168.4.1/`。选择内容和匹配的接收模式后应用；可保存图片 BMP 或 AP580 编码帧 BIN。AP580 BIN 不是 WAV，网页没有软件 AP580 解码器。
- 串口命令：`CONTENT TEXT` / `CONTENT IMAGE` / `CONTENT AUDIO`，`ROLE RX` / `ROLE TX`，`MODE <接收模式>`，`TXMODE <发送模式>`，`TONE 2000`，`TXTONE 2000`，`APRATE 600/1200/2400`，`RXDIAG`。

当前语音实物接线未确认，默认 `CONFIG_PSK_AP580_UART_ENABLED` 关闭、TX/RX GPIO 为 -1。`idf.py menuconfig → AP580 voice module` 可配置 UART1 的实际 TX/RX GPIO、115200 波特率和模块档位，再启用编译。P4 TX 接 AP580 RX，P4 RX 接 AP580 TX，按模块规格接供电和共地，避开板上已占用引脚。两端 AP580 的物理档位必须匹配；`APRATE` 不会改变模块跳线或向模块发送配置命令。P4 的 ES8311 负责 PSK 波形，不替代 AP580 语音编解码。

## 与 HPM 一致的格式

图片直接复用 `app/content/modem_image.[ch]` 和原始像素数组：

```text
~IMG:96:96:RGB332\n
~Irr:<192 个十六进制字符>:CCCC\n
```

每行 96 个 RGB332 像素；CRC16-CCITT-FALSE，初值 FFFF、多项式 1021，覆盖行号数值字节及像素。每张 19506 字节，另加 8 个换行作为循环间隔。表内编码位数 199077 / 165639（PSK / MFSK Varicode）不含循环间隔；加间隔为 199133 / 165727。错行不应用；丢失头仍能从有效行恢复，重复发送保留未完成图片的有效行。完成后下一帧重新开始，行为与 HPM 一致。

语音采用原始二进制位流，无 Varicode：

```text
UART: 59 4F 01 LEN payload padding T       （16 字节）
PSK:  DD AA 96 E7 DESC records CRC_H CRC_L
```

`DESC` 高半字节为档位 1/2/3（600/1200/2400），低半字节为记录数；兼容 HPM 的通用档位码 0。CRC 覆盖 DESC 和 records。每条 record 是 payload 加原始不透明尾字节 T；T 不被当作已验证的校验和。接收先验证分组 CRC，再补齐 UART 头和零填充。payload 内出现同步字不触发重新找包。

| 档位 | 有效载荷/记录 | 每组上限 | 满组 PSK 字节 | UART 播放节拍 |
| --- | --- | --- | --- | --- |
| 600 | 7 B + T | 6 | 55 | 90 ms |
| 1200 | 11 B + T | 8 | 103 | 67.5 ms |
| 2400 | 7 B + T | 6 | 55 | 22.5 ms |

满组、空闲 300 ms 或组龄 760 ms 时发送。CRC 错包严格丢弃；播放缺帧最多重复两次后停止，恢复时不追赶积压的播放时刻。UART 发送泵支持部分写入、完成确认、40 ms 超时和会话取消。队列与发送状态带会话编号，内容、模式和角色切换后不继续发送旧会话待处理帧。

## 验证与边界

`tools/run_content_checks.ps1` 使用生产 P4 的协议、调制和解调代码：

- 图片头丢失、逐字节输入、错误行 CRC 拒收、重复修补、9216 像素完整比对，以及编码位数验证通过。
- 三个档位的分组/还原、UART 碎片、NUL、FF、payload 内同步字、尾字节保持、无效 DESC、坏 CRC 和超时组包通过；独立 HPM 示例 CRC `13B4 / E6A9 / B448` 一致。
- 完整 PSK 二进制回环：600/2400 各 144 个不同帧、1200 为 192 个不同帧，分别 24 个分组逐字节一致；测试含 768 个零字节训练及尾部排空，未宣称无训练条件下首包无丢失。
- 完整 PSK 图片回环：双载波理想信道 35.024 秒收到全部 96 行、9216 像素一致，CRC 错行 0。这是生成的音频时长，不是 P4 CPU 耗时或实物 BER。
- 复用 HPM 播放节拍和 UART 发送泵测试：三档位、时钟回绕、短暂停顿、有限重复、恢复、部分写、超时和取消通过。
- 原有五个高速文本模式的完整 4096 字节载荷回归通过，会话隔离历史缓存测试通过。
- 实际 LVGL 的 LCD 渲染/下拉交互测试通过；浏览器在桌面及 390×844 尺寸检查生产 HTML，图片加载和 Audio 切换正常。浏览器测试使用本地样例 API；本机网络未连接板子 AP，未宣称在电脑上实际调用了板上 HTTP 接口。
- ESP-IDF 编译、AP580 启用分支的真实 SDK 编译检查、COM21 烧录校验通过。板上 AP/HTTP、Text/Image/Audio 切换正常，无启动异常。

实物连线观察：HPM（COM23）已有 Image TX，P4 能直接解析其数据。默认双载波、P4 20 dB 增益时，100 秒内 72/96 行、18 个校验失败的行，未收到整张；降低至 0 dB 后的 60 秒观察也未完成。改为两端 `8PSK1000 / 2000 Hz`、P4 增益 10 dB 后，90 秒时 93/96 行，后续连续接收确认 `image_completed=1`、累计错行 2，说明重传补齐了完整图。各次单载波诊断的 DSP 负载约 10.1%–11.3%，最慢块不超过 1856/5333 μs，累计超预算 0、读错误 0，采集约 48000 samples/s。该观察不能证明所有模拟链路都零误码，也没有硬件 I2S 丢样计数的证明。

AP580 未确认接线，UART 仍关闭，**未进行真实话音编码、无线语音链路或模块扬声器试听**。完整收图不是“所有接收行都无差错”：CRC 拒收和重复修补是当前图片协议的一部分。HPM 的运行参数已改为单载波 Image TX，P4 保持匹配的 Image RX，便于继续观察；重启 P4 仍默认 Text / BPSK31。

## 固件与日志

- P4 应用 `build/psk_modem_p4.bin`：1510336 B；SHA-256 `305c0dd885f02d852acc30ace7f89e2a9eb6d1c528cc5385545769789fddd5f7`。
- 构建、烧录：`build/build-content-final.log`、`build/flash-content-com21.log`。
- 主机：`build/content-checks.log`、`build/rate-content-regression.log`、`build/preview-content.log`。
- 板上切换：`build/board-content-com21.log`（首次媒体固件的启动/切换检查）。
- 实物双载波：`build/board-hpm-image-rx.log`、`build/board-hpm-image-rx-gain0.log`。
- 实物单载波：`build/board-image-8PSK1000-p4.log`、`build/board-image-8PSK1000-hpm.log`、`build/board-hpm-image-rx-single-continue.log`。
- 可重复运行：`tools/check_board_content.py`、`tools/board_content_link.py`、`tools/observe_board_image.py`。后两者会更改运行时收发参数，执行前确认端口和音频连线。

本次未实现 BLE 配网、临时 AP 或 STA 后关闭 AP；现有热点仍持续开启。
