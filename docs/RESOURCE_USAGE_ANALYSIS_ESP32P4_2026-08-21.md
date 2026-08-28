# ESP32P4 PSK Modem 内存和资源使用分析

日期：2026-08-21
仓库：`C:\projects\esp32p4-psk-modem`
提交：`53130cb`
目标：ESP32-P4 Function EV Board / ESP-IDF 5.5.5

## 结论

当前 ESP32P4 版本的 Flash 和 PSRAM 余量充足，主要需要持续关注的是内部 DRAM heap 和高码率 FEC 的实时性。静态内部 DIRAM 使用约 49.85%，剩余约 321 KiB；但运行时还会叠加应用任务栈、LVGL task 栈、DMA/LCD draw buffer、I2S buffer 和 IDF 系统任务。后续如果出现不稳定，优先看 `MALLOC_CAP_INTERNAL` 最小剩余和任务栈 high-water。

大块 Viterbi traceback/metrics 已经大部分搬到 PSRAM，解决了内部内存压力。代价是 8PSK FEC 高码率模式会更依赖 PSRAM 带宽和 cache 命中；`psk_rtty.c` 中对大状态数 Viterbi 解码还有 `vTaskDelay(1)` 让步逻辑，实时音频路径需要继续用板级 HOST wav/回环和实机音频验证。

## 构建和复现说明

已确认当前 build 产物存在，时间为 2026-08-21 00:31:19。此前成功构建命令：

```powershell
$env:IDF_PATH='C:\esp\v5.5.5\esp-idf'
$env:Path='C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;' + $env:Path
ninja -C build
```

普通 PowerShell 中直接复跑 `python C:\esp\v5.5.5\esp-idf\tools\idf_size.py build\psk_modem_p4.map` 会失败，原因是当前 Python 环境缺少 `esp_idf_size` 模块。下面的 IDF size 摘要来自同一 build 产物在 IDF 环境可用时的输出；本次复核重新检查了产物、map 区段和 `nm` 大符号。

## 构建产物

| 文件 | 大小 |
|---|---:|
| `build\psk_modem_p4.bin` | 914,832 B / 893.4 KiB |
| `build\psk_modem_p4.elf` | 14,627,456 B |
| `build\psk_modem_p4.map` | 5,994,062 B |
| `build\bootloader\bootloader.bin` | 24,768 B |

Flash 配置：

- `CONFIG_ESPTOOLPY_FLASHSIZE=16MB`
- custom partition table：`partitions.csv`
- factory app partition：offset `0x20000`，size `0x400000` / 4 MiB
- app 使用约 21.8%，剩余 `0x320a70`，约 3.13 MiB

## IDF Size 摘要

| 项目 | 使用量 | 总量/备注 |
|---|---:|---|
| External RAM static | 1,851,392 B / 1,808 KiB | 约 2.76% of 64 MiB |
| DIRAM/internal usable | 326,728 B | 655,360 B 总量，约 49.85% |
| DIRAM/internal remaining | 328,632 B | 静态剩余 |
| Flash sections | 803,162 B | app flash sections |
| HP core RAM | 570 B | 静态占用很小 |
| LP RAM | 56 B | 静态占用很小 |

主要 map 区段：

| 区段 | 地址 | 大小 |
|---|---:|---:|
| `.iram0.text` | `0x4ff20000` | 96,512 B |
| `.dram0.data` | `0x4ff37900` | 14,432 B |
| `.dram0.bss` | `0x4ff3b160` | 215,784 B |
| `.ext_ram.bss` | `0x48000000` | 1,851,392 B |
| `_heap_start` | `0x4ff6fc50` | 内部 heap 起点 |

PSRAM 配置：

- `CONFIG_SPIRAM=y`
- `CONFIG_SPIRAM_SPEED=200`
- `CONFIG_SPIRAM_USE_MALLOC=y`
- `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`
- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`

## 大符号

| 符号 | 大小 | 所在区域 | 说明 |
|---|---:|---|---|
| `s_vit_decisions1` | 786,432 B | PSRAM `.ext_ram.bss` | Viterbi traceback |
| `s_vit_decisions0` | 786,432 B | PSRAM `.ext_ram.bss` | Viterbi traceback |
| `s_audio_modem` | 88,952 B | internal `.dram0.bss` | 主 modem context |
| `s_vit_metrics1_b` | 65,536 B | internal `.dram0.bss` | Viterbi metric buffer，当前未带 `PSK_FAST_BSS` |
| `s_vit_metrics1_a` | 65,536 B | PSRAM `.ext_ram.bss` | Viterbi metric buffer |
| `s_vit_metrics0_b` | 65,536 B | PSRAM `.ext_ram.bss` | Viterbi metric buffer |
| `s_vit_metrics0_a` | 65,536 B | PSRAM `.ext_ram.bss` | Viterbi metric buffer |
| `s_capture` | 65,536 B | PSRAM `.ext_ram.bss` | audio capture buffer，32768 samples |
| `s_vit_out_pair` | 16,384 B | PSRAM `.ext_ram.bss` | Viterbi output pair lookup |
| `s_tx_raw_bits` | 16,384 B | internal `.dram0.bss` | TX raw bit buffer |
| `s_tx_punctured_bits` | 16,384 B | internal `.dram0.bss` | TX punctured/interleaved bit buffer |
| `s_tx_interleave_table` | 2,560 B | internal `.dram0.bss` | TX interleave table |
| `s_tx_frame_text` | 768 B | internal `.dram0.bss` | 1200F framed text |
| `s_audio_samples` | 512 B | internal `.dram0.bss` | audio task block buffer |

`components/psk_rtty/psk_rtty.c` 在 ESP 平台上将 `PSK_FAST_BSS` 和 `PSK_SDRAM_BSS` 定义为 `EXT_RAM_BSS_ATTR`，所以大部分 Viterbi buffer 会进 PSRAM。`s_vit_metrics1_b` 当前没有该属性，仍在内部 DRAM。

## 显示和 LVGL 资源

LCD 配置：

- `CONFIG_BSP_LCD_TYPE_1024_600=y`
- `BSP_LCD_H_RES=1024`
- `BSP_LCD_V_RES=600`
- `CONFIG_BSP_LCD_DPI_BUFFER_NUMS=1`

主要运行时显示内存：

| 资源 | 估算大小 | 内存类型 | 来源/说明 |
|---|---:|---|---|
| MIPI-DSI framebuffer | `1024 * 600 * 2 = 1,228,800 B` | PSRAM + DMA | RGB565，1 个 DPI framebuffer |
| waterfall canvas | `1024 * 200 * 2 = 409,600 B` | 优先 PSRAM | `s_lcd_h / 3 = 200`，`heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` |
| LVGL draw buffer | `1024 * 50 * 2 = 102,400 B` | internal DMA | BSP draw buffer 默认 `BSP_LCD_H_RES * 50` |
| LVGL port task stack | 7,168 B | internal | `ESP_LVGL_PORT_INIT_CONFIG()` 默认 |

`main/display.c` 中 waterfall canvas 如果 PSRAM 分配失败会 fallback 到 `MALLOC_CAP_8BIT`。在当前内部 DRAM 预算下，400 KiB canvas 不应落到 internal heap；若 fallback 发生，基本可以视为严重内存异常。

## 任务和系统栈

应用任务：

| 任务 | 栈大小 | core | 优先级 |
|---|---:|---:|---:|
| `psk_audio` | 16,384 B | 0 | 8 |
| `psk_tx` | 8,192 B | 0 | 7 |
| `psk_ui` | 6,144 B | 1 | 2 |

IDF/LVGL 相关配置：

- `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`
- `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304`
- `CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=1536`
- `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=2048`
- LVGL port task stack：7,168 B，internal heap

应用任务栈合计 30 KiB，LVGL port task 再占约 7 KiB；这些都会压缩 internal heap 的运行时余量。

## 风险和建议

1. 内部 DRAM 是主要容量风险。静态剩余约 321 KiB，但运行时还需要 DMA-capable buffer、任务栈、queue、event loop、I2S/LCD driver 分配。建议在启动完成、RX 高负载、TX 高负载、UI 刷新时分别记录 `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`。
2. PSRAM 容量很充足，但 Viterbi 高码率路径的性能依赖 PSRAM/cache。若 8PSK500F/8PSK1200F 在板上出现实时性问题，优先 profile Viterbi decode 时间，而不是先扩大任务栈。
3. `s_vit_metrics1_b` 留在 internal DRAM，占 64 KiB。若内部 heap 紧张，可以考虑给它加 `PSK_FAST_BSS`/`EXT_RAM_BSS_ATTR`，但必须重新跑 HOST wav、HOST 回环和板级高码率测试确认性能。
4. waterfall canvas 的 PSRAM 分配应保持成功。可以在分配后打印 caps 信息，或禁止 fallback 到 internal 以便尽早暴露配置/PSRAM 初始化问题。
5. Flash 不是当前瓶颈。4 MiB app partition 只用约 21.8%，即使继续增加 UI 和调制解调代码，短期也不需要调整分区。

## 建议补充的运行时指标

建议在日志中加入：

```c
ESP_LOGI(TAG, "heap internal free=%u min=%u psram free=%u min=%u",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
```

建议分别记录：

- 刚启动进入 UI 后。
- RX BPSK/QPSK/8PSK 普通模式。
- RX 8PSK250F/8PSK500F/8PSK1200F。
- TX 连续长文本。
- HOST wav 测试和 HOST 回环测试对应的板级模式。
