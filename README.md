# ESP32-P4 PSK 接收机

基于 ESP32-P4 Function EV Board（板载 ESP32-C6）、ESP-IDF 5.5.5。参考 `C:/projects/hpm6750-psk-modem`，支持 28 种 PSK/QPSK/8PSK 模式、音频收发、LCD 频谱/文本和手机 AP 网页。

## 速率结论

本次最高通过理想信道完整载荷测试的模式是 **2x8PSK1200F**：每载波 **1230.769 Baud**，双载波 FEC 后 **4923.077 bit/s**。单载波无 FEC 的 **8PSK1000** 为 **3000 bit/s**。二者都是信息位速率，不是文本或文件净吞吐。

4096 字节完整校验已通过；P4 尚未接串口，**板上实时性能及实际零误码上限尚未测量**。双载波在 ±20 Hz 频偏测试中仍有错误。完整计算、测试条件、内存结果和限制见 [速率与 AP 评估](docs/RATE_AND_AP_2026-09-07.md)。

## 手机使用

1. 烧录 P4 固件，上电后连接热点 **ESP32P4-PSK**，密码 **pskmodem32**。
2. 浏览器打开 **http://192.168.4.1/**；手机提示无互联网时选择继续连接。
3. 页面设置与发射端相同的接收模式和中心频率；默认 BPSK31 / **2000 Hz**。双载波模式使用 2000 Hz 起测。
4. 页面显示接收文本及解调状态，可暂停显示、清空本页、保存文本。无需路由器或外网。

网页默认与 USB 文本输出同时启用；历史环形缓冲为 16 KiB。页面显示 ASCII，其他字节显示为 `\xNN`，原始 API 保留二进制。BT 后端未实现。

ESP32-P4 本体没有 Wi-Fi；本项目通过板载 C6 / SDIO / ESP-Hosted 实现 AP。C6 需要兼容的 ESP-Hosted 固件，本项目提供匹配版本的构建脚本。

## 构建与烧录

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_idf_build.ps1
# 需要更新 C6 时，先构建匹配固件
powershell -ExecutionPolicy Bypass -File tools/build_c6_firmware.ps1
# 接板后显式填写当前 P4 串口；COM15 是历史端口示例
powershell -ExecutionPolicy Bypass -File tools/flash_board.ps1 -Chip P4 -Port COM15
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
```

现有回环覆盖 28 模式及 3 个循环场景，多数采用片段匹配；严格速率检查独立验证 5 个高速模式的完整 4096 字节载荷。PC 运行耗时不是 ESP32-P4 CPU 耗时，也不是实物链路 BER。

架构与任务分工见 [arch.md](arch.md)。
