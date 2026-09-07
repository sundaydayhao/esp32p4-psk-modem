# ESP32-P4 速率评估与手机接收页面

> 历史记录：2026-09-08 已完成 HPM→P4 三模式音频线实测和优化，更新了内存布局，并修复双载波 ±20 Hz 理想信道回归。当前结果以 [联合测试与优化记录](PSK_LINK_OPTIMIZATION_2026-09-08.md) 为准；下文保留初始测量和当时的限制。

日期：2026-09-07。P4 基线 `f55aa69e1e8d512bcf61c19ee446d369bb30ca9c`；参考 HPM6750 基线 `c84c683eb9e282c3ff7c819623f7a4f1d4078abb`。仅修改 P4 仓库。用户确认实物是带 ESP32-C6 的 Function EV Board；评估后已连接 COM21，完成下述 P4 烧录和启动检查。

## 结论与可用范围

本次实现并通过理想信道完整载荷检查的最高模式是 **2x8PSK1200F**：每载波 **1230.769 Baud**，2 个载波，信道编码位速率 **7384.615 bit/s**，2/3 FEC 后的理论信息位速率 **4923.077 bit/s**。这是 HPM 自定义模式，不能假定 fldigi 支持。单载波最高无 FEC 模式 **8PSK1000** 为 **1000 Baud / 3000 bit/s**。

这不是 ESP32-P4 的绝对算力上限，也不是已经测得的板上零误码上限。MHz、核心数、FEC 纠错或解码锁定状态，都不能单独保证零误码。当前没有板级 CPU 测量、模拟链路 BER、Wi-Fi 手机实连结果。实际限制还包括音频带宽、频偏、采样时钟差、信噪比、削顶和最坏调度延迟。

具体边界：双载波 2000 Hz、无噪声、无频偏时通过；同样测试加入 **+20 Hz / -20 Hz** 发射频偏后均失败。双载波在 1500 Hz 中心频率也未通过严格长载荷检查；参考 HPM 当前默认中心是 2000 Hz，本次 P4 默认已对齐 2000 Hz。标准单载波模式可以手工改变中心频率。两端必须一致。

## 算力与带宽预算

- 当前配置：双核 400 MHz；音频以 48 kHz 输入，解码块 256 样本。每块音频时间 5.333 ms，单个 400 MHz 核约有 213 万时钟周期可用。该数字是预算，不是测得的空闲周期。
- 双载波 1200F 内部基带 16 kHz，两级 65 tap 复数 FIR，约 `16000 × 2 × 65 × 2 = 4.16M` 复数 tap/s，约 8.32M 浮点乘法及 8.32M 加法/s。另有混频、定时、AFC、AGC、缓存访问与调度开销。此工作量支持把 4.92 kbit/s 作为合理验证目标；不能据此报出未经测量的 CPU 占用百分比。
- K=7 Viterbi 为 64 状态；双载波约 `4923 × 64 = 315k` 状态更新/s。反而 8PSK250F 的 K=16 为 32768 状态，双分支约 `375 × 32768 × 2 = 24.6M` 状态更新/s，明显更重。“符号率低”不一定“解调轻”。K13/K16 模式的板上实时性仍需单独确认。
- 双载波间隔 `1.4 × 1230.769 = 1723.077 Hz`；2000 Hz 中心下，载波约 1138.46 / 2861.54 Hz。应使用能覆盖约 0–4 kHz 的音频通路，不能把瀑布图显示范围当作 ADC 的实际带宽，也不能假定普通窄语音通道适用。同日后续 LCD 同步已将显示范围从 0–3 kHz 扩展为 0–4 kHz。
- 信息速率还没有扣除 Varicode、前导、交织填充或应用分包开销。不能把 4923 bit/s 直接写成文本、图片、语音或文件的净吞吐。首次速率工作未新增 HPM 图像/AP580 协议；同日后续移植见 [图片和语音记录](IMAGE_AUDIO_2026-09-07.md)。

## 实现

- 对照 HPM 模式表、TX/字符编码接口，增加双载波 1200F；接入逐载波八次方 AFC、1200F 两级匹配滤波及分数符号时钟。P4 原有其他模式和 K16 分支诊断继续保留。
- 1200F 文本直接输出原始字符；移除历史隐式 `~H1200,...~` 包装解析，避免把普通文本中的标点/旧协议字面串吞掉。旧私有包装不再自动解包。
- 音频任务在 core 0；HTTP、USB 输出和 LVGL 在 core 1。ESP-Hosted 系统任务仍使用组件自身调度，不声称所有网络任务都被固定在 core 1。
- 接收字节写入 16 KiB 环形历史；HTTP 和 USB 使用独立游标。音频任务不等待 HTTP 或 USB 发送。慢客户端被历史覆盖时会收到丢失字节数，网页明确提示；USB 部分写入/覆盖另有计数。LCD 显示队列依旧有界，LCD 文本不是用于 BER 的无损记录。
- K<=7 使用内部 512 字节 metric 工作区；大状态 metric 与 traceback 放 PSRAM。速率改版时的 size 报告（后续 LCD 改版前）：DIRAM 334892 B / 655360 B，静态余量 320468 B（约 313 KiB）；外部静态 RAM 2284084 B。运行时任务栈、DMA、Wi-Fi 和显示继续占用内存，网页 API 暴露最小内部空闲 heap。
- 每秒记录实际采样数量、DSP 平均负载、最慢 DSP 块、每块时间预算、累计超预算块、读取错误和锁定状态。DSP 耗时包含被抢占的墙钟时间，未包含整个采集/显示任务的所有开销；超预算计数不等于已丢样。BSP 不公开 I2S 句柄，当前没有硬件 DMA 溢出计数，不能把未测量的溢出数报告为零。

## 手机连接

1. P4 固件启动后，通过板载 C6 开启 2.4 GHz WPA2 AP。
2. 手机连接 **ESP32P4-PSK**，密码 **pskmodem32**。
3. 浏览器打开 **http://192.168.4.1/**。若手机提示无互联网，选择继续使用此 Wi-Fi。
4. 两端选择相同调制模式及音频中心频率；双载波建议先使用 2000 Hz。页面可设置接收模式/频点，读取当前实际应用状态，并暂停显示、清空本页或保存文本。

页面完全离线，无外部 JS/CDN。接收文本按 ASCII 显示，其他字节显示为 `\xNN`；`/api/rx` 保持原始二进制内容。没有增加自动弹出的 captive portal；手动打开上述地址即可。热点参数可在 `menuconfig → PSK receiver hotspot` 修改。

默认 USB 输出与 AP 历史同时开启；选择 AP 输出时停止 USB 文本镜像，AP 继续工作。BT 后端未实现，选择会被拒绝。

## 构建与烧录

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_idf_build.ps1
powershell -ExecutionPolicy Bypass -File tools/build_c6_firmware.ps1
```

本次两套固件均已编译成功：

- P4：`build/psk_modem_p4.bin`，1439632 B，配套 bootloader 和分区表在 `build/`。APP 分区 4 MiB。
- C6：`build/c6/network_adapter.bin`，1226800 B；包含匹配 ESP-Hosted 2.12.11，板型及 SDIO 配置已选定。配套 bootloader、分区表及 OTA 初始化数据在 `build/c6/`。
- Host 依赖已在 `dependencies.lock` 锁定：`esp_wifi_remote 1.6.3`、`esp_hosted 2.12.11`，ESP-IDF 5.5.5。

连接实物后，必须显式指定串口，脚本不会自动探测/轮流烧录其他板子：

```powershell
# COM15 只是历史端口示例，请用当前 P4 端口
powershell -ExecutionPolicy Bypass -File tools/flash_board.ps1 -Chip P4 -Port COM15
```

C6 出厂已可能带 ESP-Hosted。如果 AP 初始化提示 C6 通信/版本问题，再通过板上的 **C6 编程接口**连接 USB-UART/ESP-Prog，并烧录匹配的 C6 整套镜像。P4 的 USB 串口不等于 C6 编程串口。操作时按官方板级指南使 P4 停在 bootloader，防止干扰 C6 下载，再执行同一脚本 `-Chip C6 -Port <C6串口>`。本次仅烧录 P4，C6 使用板上已有固件。

## 实物烧录与启动检查（2026-09-07 补充）

- COM21，USB Serial/JTAG，识别为 ESP32-P4 v3.2。使用 `tools/flash_board.ps1 -Chip P4 -Port COM21` 写入 bootloader、分区表与应用，三项写入校验均通过。
- 重启确认 400 MHz、32 MiB PSRAM（200 MHz，内存自检通过）、48 kHz ES8311 音频初始化，音频接收、TX 与 UI 任务启动。`STATUS` 返回 RX / BPSK31 / 2000 Hz，TX 未启用；`RXDIAG` 有响应，未锁定有效信号。
- SDIO 初始化成功，识别板载 ESP32-C6；约启动后 5 秒出现 `softap started`、`AP ESP32P4-PSK ready` 和 DHCP 地址 `192.168.4.1`。这是板端 AP/HTTP 启动证据，尚未完成手机实连网页和模拟链路 BER 验收。
- 板上旧 C6 固件触发 `Host [2.12.0] > Co-proc [0.0.0]` 版本警告，但本次 AP 初始化成功。该字段是运行时协议信息，不替代锁文件的组件包版本；C6 尚未升级到已构建镜像。
- 当前固件仍为持续 AP 接收网页模式；临时热点配网、STA 连接成功后关闭 AP、BLE 配网尚未实现。
- 日志：`build/flash-p4-com21.log`、`build/board-com21-boot.log`。

## 验证结果

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_host_tools.ps1
$env:PATH = 'C:/cygwin64/bin;' + $env:PATH
build/host-tools/host_tx_roundtrip.exe
powershell -ExecutionPolicy Bypass -File tools/run_rate_checks.ps1
```

现有 28 模式 + 3 个循环场景：`failures=0`。这组多数只检查正确片段，不能标成全流零误码。

新增严格检查：48 kHz；256 样本分块；与板上一样经过 factor=1 的 DC blocker；单次非重复 4096 字节确定性 ASCII 载荷；额外训练段；唯一头尾标记；完整帧 4136 字节逐字节比较。不是从反复发送中挑一段正确子串。输入没有 AWGN 或采样漂移。

| 模式 | 中心 Hz | 理论信息 bit/s | 完整帧比较 |
| --- | ---: | ---: | --- |
| BPSK1000 | 1500 | 1000 | 4136/4136，0 不同字节 |
| 2xPSK1000 | 1500 | 2000 | 4136/4136，0 不同字节 |
| 8PSK1000 | 1500 | 3000 | 4136/4136，0 不同字节 |
| 8PSK1200F | 1500 | 2461.539 | 4136/4136，0 不同字节 |
| 2x8PSK1200F | 2000 | 4923.077 | 4136/4136，0 不同字节 |

边界探测命令（当前预期失败，不能当通过的门槛）：

```powershell
build/host-tools/host_rate_check.exe 2x8PSK1200F 20
build/host-tools/host_rate_check.exe 2x8PSK1200F -20
```

该字节比较只判断完整载荷是否相同。若发生插入/删除，不把对齐后的字符差异伪装成物理 bit BER。

环形历史测试通过：多个独立读者、原始 NUL/高字节、回绕、大块覆盖、旧游标、空读取。手机网页用本地模拟服务在 390×844 下检查了文本安全显示、参数提交后实际状态、暂停/继续、清空；浏览器无 JS 错误。**模拟服务不是 ESP-IDF HTTP、Wi-Fi 或实物 AP 测试**。

另行回归旧包装字面串 `Literal text ~H1200,0,3,A3830348:QUJD~ must be preserved.`：`failures=0`，1200F 的有限与循环测试仍要求完整载荷出现。

速率改版时固件 SHA-256（后续 LCD 改版产物见 [LCD 同步记录](UI_SYNC_2026-09-07.md)）：

- P4：`d6396dc8b29bea74bd416c42f26c75ad753bffe265834fc211cdc524133ec759`
- C6：`221e104f84667770cd7267b9994ff21109ca174c6c41f82947865337df9cf538`

关键日志：`build/rate-checks.log`、`build/roundtrip.log`、`build/build-ap.log`、`build/size-ap.log`、`build/build-c6.log`。

板上验收仍需：手机持续轮询同时长数据接收；固定序号完整负载与 CRC/逐字节对比；采样应持续接近 48000/s；查看峰值 DSP 是否低于 5333 µs、平均占用是否留足调度余量、内部 heap 最小值；单独验证频偏、时钟漂移、噪声与模拟输入电平。有限样本观察到零错不能证明任意时长永久零误码。

## 官方硬件/网络依据

- [ESP32-P4 Wi-Fi Expansion / ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32p4/api-guides/wifi-expansion.html)：P4 通过无线协处理器提供标准 Wi-Fi API。
- [Function EV Board 官方指南](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-function-ev-board/user_guide.html)：板载 C6 与编程接口；具体硬件修订请按板上标记选择对应指南。
- 本次实际使用的板型、SDIO 引脚和 C6 烧录步骤见下载组件内 `managed_components/espressif__esp_hosted/docs/esp32_p4_function_ev_board.md`，与锁文件版本配套。
