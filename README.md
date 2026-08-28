# ESP32-P4 多模式数字解调器

这是一个 ESP-IDF 项目，用 ESP32-P4 Function EV Board 实现音频输入数字模式接收和解码。工程保留 ESP32-S3/HPM 参考工程的 modem/host 测试核心，并把板级显示、音频和内存布局移植到 ESP32-P4 BSP。

更详细的架构图和模块划分见 [arch.md](arch.md)。

## 当前功能

当前 UI/串口命令入口对齐 HPM6750 工程，使用 PSK/QPSK/8PSK 模式：

- BPSK31
- PSK63 及扩展 PSK/QPSK/8PSK 模式

主要功能：

- 统一 PSK 解码入口：所有音频进入 `modem_process()` / `decoder_layer_feed()`。
- PSK：`components/psk_rtty`，host 回归覆盖 27 个 PSK/QPSK/8PSK 模式。
- 显示：通过 ESP32-P4 BSP/LVGL 显示频谱、瀑布图和解码文本。
- 音频：通过 ESP32-P4 BSP `esp_codec_dev` 麦克风路径采样，48 kHz 输入按 HPM6750 逻辑直接进入 modem 处理。
- 输出：USB 串口可用；WiFi 在 ESP32-P4 上不可用；BT 入口保留但未实现 BLE/SPP 后端。

## 架构概览

```text
ESP32-P4 BSP microphone / I2S
    -> audio_input.c
    -> rx_modem.c
    -> rx_decoder.c
    -> mode backend
    -> decoded bytes
    -> transport.c
```

核心文件：

- `main/audio_input.c`：ESP32-P4 BSP 麦克风输入、采样和抽取。
- `main/display.c`：ESP32-P4 BSP/LVGL 显示、瀑布图和触摸调谐。
- `main/rx_modem.c`：公开 modem API、模式切换、频点设置和输出缓冲。
- `main/rx_decoder.c`：统一解码层，按模式 family 分发到后端。
- `components/psk_rtty`：PSK 后端。

## 硬件默认配置

- 目标芯片：`esp32p4`
- 目标板：ESP32-P4 Function EV Board BSP
- Codec 输入采样率：`BOARD_CODEC_SAMPLE_RATE_HZ = 48000`
- Modem 处理采样率：`BOARD_MODEM_SAMPLE_RATE_HZ = 48000`
- 瀑布图显示流：8 kHz / 512 samples / 8192 FFT / 0-3000 Hz
- I2S/I2C/LCD/触摸：使用 BSP 定义的 P4 Function EV Board 引脚
- 用户按键：GPIO0，用于循环切换模式

PSK FEC/Viterbi 大表使用 `EXT_RAM_BSS_ATTR` 放入 PSRAM，`sdkconfig.defaults` 已启用 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`。

## 构建

在 ESP-IDF v5.5.5 PowerShell 环境中构建：

```powershell
C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe -NoExit -ExecutionPolicy Bypass -NoProfile -Command "& {. 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1'}"
idf.py -B build set-target esp32p4
idf.py -B build build
```

固件产物和 host 工具都统一生成在 `build` 目录下。

## Host 回归

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_host_tools.ps1
powershell -ExecutionPolicy Bypass -File .\tools\run_host_regression.ps1 -WavDir C:\projects\psk-script\generated-wav-48000 -Expected C:\projects\psk-script\input.txt
```

当前 WAV 回归结果与 HPM6750 工程一致：`PASS=26 FAIL=1 NO-WAV=0 TOTAL=27`，唯一失败项为 `2xPSK1000` 语料匹配。TX/RX roundtrip 结果：`failures=0`。

## 烧录

```powershell
idf.py -B build -p COMx flash monitor
```

将 `COMx` 替换为实际串口号。

## 分区

项目使用自定义 `partitions.csv`：

- partition table offset：`0x10000`
- factory app offset：`0x20000`
- factory app size：`0x400000`

该布局适配 ESP32-P4 bootloader 体积和当前 modem/LVGL/BSP 固件大小。
