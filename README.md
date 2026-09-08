# ESP32-P4 PSK 接收机

基于 ESP32-P4 Function EV Board（板载 ESP32-C6）、ESP-IDF 5.5.5。参考 `C:/projects/hpm6750-psk-modem`，支持 28 种 PSK/QPSK/8PSK 模式、文本、96×96 图片和 AP580 语音帧收发，以及 LCD / 手机 AP 网页。

## 速率结论

本次最高通过理想信道完整载荷测试的模式是 **2x8PSK1200F**：每载波 **1230.769 Baud**，双载波 FEC 后 **4923.077 bit/s**。单载波无 FEC 的 **8PSK1000** 为 **3000 bit/s**。二者都是信息位速率，不是文本或文件净吞吐。

4096 字节完整校验已通过。2026-09-08 已使用 HPM6750（COM23）经音频线发送、P4（COM21）接收，修复 125F/250F 的 K16 实时超载，并优化双载波判决和定时。该链路使用 **2000 Hz、HPM `OUTVOL 220 122`、P4 默认输入增益 22 dB**；最终三种模式各 60 秒原始窗口全部相同，双载波两组不同数据累计 180 秒、102344 B 无差异。原始电平下仍有少量错帧，因此使用时需保持上述电平配合。双载波 ±20 Hz 理想信道完整载荷回归也通过。实测与统计边界见 [三模式联合测试与优化](docs/PSK_LINK_OPTIMIZATION_2026-09-08.md)；速率计算见 [初始评估](docs/RATE_AND_AP_2026-09-07.md)。有限样本无错不代表任意信道和时长都能保证零误码。

反向 **P4 发送 → HPM 接收** 已检查 16 种高速文本模式，音量 60 / 2000 Hz 下有 14 种通过首轮 30 秒全窗口校验；**BPSK1000、2xPSK800 仍有错误**。BPSK1000 关闭 HPM 静噪后显著改善，但换数据的长窗口仍未完全通过。双载波 1200F 在原默认音量 70 下复现误码，P4 默认输出现改为 **60**；烧录后换数据测 120 秒，**67953 B、434 个完整帧全部一致**。完整模式表、参数对照和复测脚本见 [P4 高速文本发射检查](docs/P4_TEXT_TX_2026-09-08.md)。HPM 代码和固件未改。

## 手机使用

开发板 LCD 已同步 HPM6750 `c84c683` 的新版布局：顶部 Rx/Tx、Text/Image/Audio 和分级模式下拉框；左侧 0–4 kHz 瀑布图，右侧显示文本、渐进图片或语音状态。Text 支持全部 28 个模式，Image 过滤预计超过 180 秒的模式，Audio 固定 `2x8PSK1200F / 2000 Hz`。协议与验证见 [图片和语音移植记录](docs/IMAGE_AUDIO_2026-09-07.md)。

1. 烧录 P4 固件，上电后连接热点 **ESP32P4-PSK**，密码 **pskmodem32**。
2. 浏览器打开 **http://192.168.4.1/**；手机提示无互联网时选择继续连接。
3. 页面设置与发射端相同的接收模式和中心频率；默认 BPSK31 / **2000 Hz**。双载波模式使用 2000 Hz 起测。
4. 内容选择 Text、Image 或 Audio 后应用。图片实时显示已通过 CRC 的行，可保存 BMP；语音显示包、帧和 UART 状态，可保存 AP580 编码帧。无需路由器或外网。

语音沿用 HPM 的**外接 AP580** 编解码模块。板载 ES8311 负责 PSK 波形，不解码 AP580 压缩语音。当前已按确认接线启用 UART1：P4 **GPIO32 TX → AP580 RX**、**GPIO33 RX ← AP580 TX**，共地，115200 / 8N1，默认编码档位 **2400 bit/s**；可在 `menuconfig → AP580 voice module` 修改。浏览器不能把 AP580 帧直接当 WAV 播放。

2026-09-08 已验证 P4 → 音频线 → HPM6750 的双载波文本发射：P4 输出音量 60、2000 Hz，30 秒接收窗口共 16958 B 全部一致（184 个完整测试帧）。AP580 实物语音检查中 P4 UART 原始输入仍为 0，尚未完成端到端语音验证；接线和复测方法见 [AP580 2.4 kbps 发射检查](docs/AP580_TX_2400_2026-09-08.md)。

LCD 切换 `Tx → Image` 会循环发送与 HPM 相同的内置热气球测试图；`Tx → Audio` 会发送 AP580 UART 输入，模块没有输入时不会启动语音调制。串口提供 `CONTENT TEXT/IMAGE/AUDIO`、`ROLE RX/TX`、`APRATE 600/1200/2400`。`APRATE` 只修改软件组包参数，不修改模块跳线或发送模块配置命令。

网页默认与 USB 文本输出同时启用；历史环形缓冲为 16 KiB。页面显示 ASCII，其他字节显示为 `\xNN`，原始 API 保留二进制。当前 AP 持续开启，尚未实现临时热点配网、STA 连接后关闭 AP 或 BLE 配网。BT 后端未实现。

ESP32-P4 本体没有 Wi-Fi；本项目通过板载 C6 / SDIO / ESP-Hosted 实现 AP。C6 需要兼容的 ESP-Hosted 固件，本项目提供匹配版本的构建脚本。

## 构建与烧录

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_idf_build.ps1
# 需要更新 C6 时，先构建匹配固件
powershell -ExecutionPolicy Bypass -File tools/build_c6_firmware.ps1
# 接板后显式填写当前 P4 串口；当前为 COM21
powershell -ExecutionPolicy Bypass -File tools/flash_board.ps1 -Chip P4 -Port COM21
```

- P4 产物：`build/psk_modem_p4.bin`，配套 bootloader、partition table、flash_args 位于 `build/`。
- C6 产物：`build/c6/network_adapter.bin`，配套文件位于 `build/c6/`；C6 使用独立编程接口，不能把 P4 USB 串口当作 C6 串口。
- 调整热点：`idf.py menuconfig` → `PSK receiver hotspot`。
- 自定义分区：partition table `0x10000`，factory app `0x20000`，容量 4 MiB。
- 芯片/板型和 C6 烧录注意事项见评估文档。

## 验证

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_host_tools.ps1
$env:PATH = 'C:/cygwin64/bin;' + $env:PATH
build/host-tools/host_tx_roundtrip.exe
powershell -ExecutionPolicy Bypass -File tools/run_rate_checks.ps1
powershell -ExecutionPolicy Bypass -File tools/run_content_checks.ps1
powershell -ExecutionPolicy Bypass -File tools/run_psk_optimization_checks.ps1
powershell -ExecutionPolicy Bypass -File tools/preview_lcd.ps1
```

现有回环覆盖 28 模式及 3 个循环场景，多数采用片段匹配；严格速率检查独立验证 5 个高速模式的完整 4096 字节载荷。PC 运行耗时不是 ESP32-P4 CPU 耗时，也不是实物链路 BER。

架构与任务分工见 [arch.md](arch.md)。
