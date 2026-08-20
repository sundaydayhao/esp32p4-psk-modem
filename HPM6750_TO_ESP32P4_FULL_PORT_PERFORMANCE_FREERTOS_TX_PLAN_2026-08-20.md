# HPM6750 → ESP32-P4 全功能移植：性能保证 + FreeRTOS 任务架构 + 全模式发射链路 分析

日期：2026-08-20

本文档是纯评估/规划文档，**未修改 `C:\projects\hpm6750-psk-modem` 或 `C:\projects\esp32p4-psk-modem` 任一项目的任何代码、构建脚本或配置文件**。用于回答用户提出的三个问题：

1. 把 `hpm6750-psk-modem` 的全部功能移植到 `esp32p4-psk-modem`，需要做什么？
2. 怎么保证移植后的性能？
3. 怎么给 `esp32p4-psk-modem` 补上 FreeRTOS 任务架构和"所有模式的发射链路"？

本文档与同目录下另外两份文档是同一条调研链路的延续，请配合阅读：

- `STM32H7_TO_ESP32P4_MIGRATION_STRATEGY_2026-08-20.md`（今天早些时候完成）：结论是 `stm32h7-psk-modem` 的 27 模式接收功能已经基本移植完成，`stm32h7-psk-modem` **完全没有发射功能**，所以那份文档不涉及 TX。
- `C:\projects\hpm6750-psk-modem\docs\FREERTOS_SMP_TASK_SPLIT_TX_EVALUATION_2026-08-20.md`（同样是今天完成）：回答的是"HPM6750 项目自己要不要上 FreeRTOS SMP、Core0/Core1 怎么拆任务、TX 怎么覆盖全部 27 模式"，但立足点是 HPM6750 自己的 **AMP**（双镜像独立调度器）架构。

本文档要做的是：把上面第二份文档里"TX 覆盖全部模式"的差距分析和设计思路，**重新映射到 ESP32-P4 的 SMP（单一调度器、双核共享地址空间）架构**上，同时结合 ESP32-P4 现有代码的实际任务模型和硬件能力给出移植与性能保证方案。

## 0. 结论摘要

| 问题 | 结论 |
| --- | --- |
| HPM6750 全部功能能否移植到 ESP32-P4 | **接收侧已经不需要移植**（ESP32-P4 项目的 27 种 PSK 模式解码已经比 HPM6750 更完整，还多了 FT8/FT4/CW/MFSK16）。**真正需要移植/新建的是发射（TX）链路**，且 HPM6750 自己的 TX 也只覆盖 27 种模式里的 6 种（无 FEC 单载波 BPSK），要做到"所有模式"发射，对两个项目都是**新开发工作**，不是简单搬代码。 |
| ESP32-P4 是否已经用 FreeRTOS | **是**（ESP-IDF 强制基于 FreeRTOS），但目前只有 3 个粗粒度任务（`psk_audio`/`psk_ui`/`taskLVGL`），没有做任何核心绑定，音频采集+全部解调都挤在一个任务里。"添加 FreeRTOS" 应理解为"把现有粗粒度任务拆分为职责单一、显式核心绑定、事件驱动同步的任务架构"，而不是字面意义的引入新引擎。 |
| ESP32-P4 上做任务拆分/双核分工是否比 HPM6750 简单 | **明显更简单**。HPM6750 是 AMP（每核独立 FreeRTOS 镶像、独立地址空间），核间通信要手写 seqlock + 手动 cache 一致性维护（`l1c_dc_writeback`/`invalidate`+`fence`）。ESP32-P4 双核共享同一地址空间、同一个 FreeRTOS SMP 调度器，用标准 `xTaskCreatePinnedToCore`+队列/任务通知即可，**HPM6750 的 IPC 机制不需要、也不应该被移植过来**。 |
| 音频输出硬件是否已经就位 | **是，比预期更现成**。ESP32-P4 项目的 BSP（`esp32_p4_function_ev_board`）在硬件层已经是 I2S 全双工，并提供现成的 `bsp_audio_codec_speaker_init()`（ES8311，`ESP_CODEC_DEV_TYPE_IN_OUT`），但应用层（`main/audio_input.c`）目前只调用了只读的麦克风初始化，从未调用过写入函数。这意味着 TX 的"硬件适配层"工作量比 HPM6750（WM8960 手写 ISR 环形缓冲、无 DMA）小得多，可以直接复用 ESP-IDF I2S 驱动自带的 DMA 双缓冲机制。 |
| 性能保证的关键点 | 见第 6 节。核心是三件事：(1) 显式核心绑定，把"采集+解调"和"未来的 TX 编码"分别隔离、互不抢占；(2) 用 ESP-IDF I2S 驱动自带 DMA 代替 HPM6750 式的手写 ISR 单样点搬运；(3) 在新增任何任务/模块之前，先补一份 ESP32-P4 真机 KPI 基线（这也是前一份迁移文档里排第一优先级、至今仍未完成的工作）。 |

## 1. 现状盘点：ESP32-P4-psk-modem（读实际代码，非文档）

### 1.1 当前 FreeRTOS 任务架构

`main/app_main.c` 第 298-301 行创建两个应用任务：

```c
BaseType_t task_ok = xTaskCreate(audio_task, "psk_audio", 16384, NULL, 8, NULL);
task_ok = xTaskCreate(ui_task, "psk_ui", 6144, NULL, 2, NULL);
```

| 任务 | 优先级 | 栈 | 绑核 | 职责 |
| --- | --- | --- | --- | --- |
| `psk_audio` | 8 | 16384 B | 无（`xTaskCreate`，非 Pinned） | 读音频块 → PSK/RTTY/CW/MFSK16/FT8/FT4 **全部解码** → 发布解码文本 → 发布频谱数据 |
| `psk_ui` | 2 | 6144 B | 无 | 消费解码队列、串口命令/SW1 按键、`spectrum_analyze`、调用 LVGL API 更新界面 |
| `taskLVGL`（BSP/`esp_lvgl_port` 隐式创建） | 4 | 7168 B | 无（`task_affinity=-1`） | `lv_timer_handler` 周期刷新（10ms） |
| `app_main` 主任务 | 默认 | 8192 B | — | 初始化后 `while(true) vTaskDelay(1000)`，纯挂起 |

关键结论：

- `CONFIG_FREERTOS_NUMBER_OF_CORES=2`、`CONFIG_FREERTOS_UNICORE` 未设置（双核 SMP 已使能），但**全项目零次调用 `xTaskCreatePinnedToCore`**，双核分配完全交给调度器自动均衡，没有为"音频/解调"和"UI/显示"做任何显式隔离。
- 任务间通信只有 3 个队列（`s_decoded_queue` 8×96B，`s_control_queue` 4×event，`s_spectrum_queue` 深度1用 `xQueueOverwrite` 当"最新值"槎位），main/ 里没有信号量、没有事件组；唯一的锁是 `esp_lvgl_port` 内部的递归互斥锁（保护 `psk_ui` 与 `taskLVGL` 对 LVGL 对象的并发访问）。
- `audio_task` 内部每处理 64 个采样后插入 `vTaskDelay(1)`（`app_main.c` 第167行附近）——这是**人为限流**，不是真正的"数据到来即唤醒"，属于可优化点（见第 6.2 节）。
- 没有裸机式忙等或 superloop 残留，整体已经是标准 FreeRTOS 任务+队列风格,但任务粒度太粗,没有发挥优先级抢占和核心隔离的价值——这与 HPM6750 自己的评估文档对 Core0/Core1"1个task塞所有职责"的判断本质相同,只是 ESP32-P4 起点已经是两三个任务而不是一个。

### 1.2 TX（发射）现状

**结论：完全空白，连桩代码都没有。**

- `components/psk_rtty/include/psk_rtty.h` 只导出 4 个函数：`psk_rtty_rx_init/set_mode/set_tone/feed`,全部是 `_rx_` 前缀,没有任何编码/调制函数。
- `grep` `tx_/modulate/encode/发射/ptt/dac_out` 在 `main/` 和 `components/psk_rtty/` 下只命中 Viterbi 解码内部的 `fec_last_encoded_bit`/`push_fec_encoded_bit`——这是解码器"已解出的软比特"状态变量,与发射调制无关。
- `main/transport.c::transport_send()` 只是把解码文本写到 USB CDC stdout,是"吐出解码结果"的调试通道,跟 RF/音频发射毫无关系。
- 没有任何"发射测试"隐藏命令（`@MODE/@CAP/@DUMP/@CAPDUMP` 均服务于接收侧调试）。

### 1.3 音频输出路径（关键发现：硬件已就位，应用层未使用）

`main/audio_input.c` 只调用了只读路径：

```c
s_mic_dev = bsp_audio_codec_microphone_init();
esp_codec_dev_open(s_mic_dev, &fs);   // 仅 esp_codec_dev_read，全项目无 esp_codec_dev_write
```

但 BSP（`managed_components/espressif__esp32_p4_function_ev_board/esp32_p4_function_ev_board.c`）底层：

- `bsp_audio_init()` 用 `i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan)` **同时**创建 TX/RX 通道并分别 enable，硬件层是全双工。
- 已经提供 `bsp_audio_codec_speaker_init()`（ES8311，`dev_type = ESP_CODEC_DEV_TYPE_IN_OUT`），可以直接拿到一个支持 `esp_codec_dev_write()` 的句柄。
- 当前应用代码从未调用过它。

这意味着**TX 的音频输出硬件适配层几乎不需要新写**，直接调用现成的 `bsp_audio_codec_speaker_init()` + `esp_codec_dev_write()` 即可，且 ESP-IDF 的 I2S 驱动本身自带 DMA 描述符链/双缓冲，不需要像 HPM6750 那样手写 ISR 逐样点搬运。

### 1.4 已实现的解码模式（确认覆盖面）

`main/demod_primitives.c` 的 `demod_profile_t` 表（7 个 family 级条目：BPSK31/PSK63/FT8/FT4/RTTY/CW/MFSK16）+ `main/psk_modes.c` 的 26 个 PSK 子模式（BPSK 全系列、多载波 Nx、QPSK 全系列、8PSK 全系列及 F 后缀 FEC 变体）。这已经覆盖 HPM6750 27 种 PSK 模式的全集，并且额外多出 FT8/FT4/CW/MFSK16（HPM6750 完全没有这四类——见 2.4 节）。`components/psk_rtty` 同 HPM6750 的 `third_party/psk_rtty` 一样，是**纯解码库，不含任何编码/调制函数**。

### 1.5 硬件/性能相关配置

```
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=400
CONFIG_FREERTOS_NUMBER_OF_CORES=2      # 双核SMP已使能，但代码未显式绑核
CONFIG_SPIRAM=y / SPIRAM_MODE_HEX / SPIRAM_SPEED_200M
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384   # <16KB分配强制走内部RAM
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y # 允许但代码未使用，任务栈仍在内部RAM
```

**PSRAM 实际标注情况（纠正前一份迁移文档的推测）**：全项目仅两处使用 PSRAM：

1. `main/audio_input.c`：`s_capture[32768]`（64KB `int16_t`，`EXT_RAM_BSS_ATTR`）——只是 `@CAP/@DUMP` 调试抓包缓冲。
2. `main/display.c`：瀑布图画布用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)`。

**FT8 monitor、MFSK16 Viterbi 状态、`decoder_layer_t`、任务栈、三个队列全部在内部 RAM**，前一份文档第 6 节担心的"热表/度量数组被放进 PSRAM 拖慢高速 FEC 模式"这个风险,在当前代码里**并不存在**——当前设计已经是正确的"热数据留内部 SRAM,只有调试用大缓冲和 UI 画布放 PSRAM",不需要额外的冷热分层改造。真正待确认的是内部 SRAM 的**剩余余量**（见第 7 节待核实项）。

### 1.6 CLI/调试命令

`@MODE <name>`、`@CAP [n]`、`@DUMP [n]`、`@CAPDUMP`，全部服务于"选模式"和"抓取/回放接收样本用于离线回归"，没有任何发射相关命令。

## 2. 现状盘点：HPM6750-psk-modem 的 TX 与 RTOS（读实际代码 + 今日已有评估文档）

### 2.1 RTOS 架构：AMP，非 SMP

Core0 和 Core1 各跑**独立**的 FreeRTOS V10.5.1 镜像（`app/common/multicore_boot.c` 用 `rom_sdp_memcpy` 把 Core1 镖像 DMA 拷贝进对方 ILM 后 `sysctl_release_cpu` 释放）。两核各自只有 1 个业务任务：

| 核心 | 任务 | 优先级 | 栈 | 职责 |
| --- | --- | --- | --- | --- |
| Core0 | `core0_task` | 27（`configMAX_PRIORITIES-5`） | 8192字/32KB | 串口CLI轮询、RX文本环消费、GT911触摸轮询(30ms)、UI帧消费+频谱+LCD渲染、2000ms诊断打印。**不接触任何音频/DSP** |
| Core1 | `audio_modem_task` | 28（`configMAX_PRIORITIES-4`） | 4096字/16KB | 控制指令消费、**TX填充**(`service_psk_tx`)、`audio_input_read`、**RX解调**(`psk_modem_process`)、25ms周期诊断发布 |

核间通信（`app/common/ui_transport.c`）：共享内存 + 手写 seqlock，放在 `ATTR_SHARE_MEM` 的 16KB AXI SRAM 窗口，所有跨核访问都要手动 `l1c_dc_writeback`/`l1c_dc_invalidate`+`fence rw,rw` 维护缓存一致性；控制指令通道是**纯软件轮询**（无中断/mailbox 唤醒）。

**这套机制是 AMP 特有的工程量，ESP32-P4 上不需要，也不应该被移植**——ESP32-P4 双核共享同一地址空间、同一个 FreeRTOS SMP 调度器，标准队列/任务通知即可安全跨核传递数据，即使两个任务被调度到不同核心也不需要手动维护缓存一致性（**但这一点在 ESP32-P4 上具体的缓存一致性模型仍需要用官方资料核实，见第7节**）。

### 2.2 TX 现状：仅覆盖 6/27 种模式，且实现是原型级

`app/modem/psk_tx.c::psk_tx_start_text()` 显式拒绝：

```c
if (!info || info->family != PSK_MODE_FAMILY_BPSK ||
    info->bits_per_symbol != 1U || info->carriers != 1U || info->fec != 0U ||
    info->baud <= 0.0f) {
    return -2;
}
```

只有 BPSK31/63/125/250/500/1000（无 FEC 单载波）可以发射，QPSK/8PSK/多载波 Nx/全部 F 后缀 FEC 模式（共 21 种）结构上无法发射。

实现质量是**原型级**，不是可以直接照搬的成熟实现：

- 相位平滑用升余弦(Hann窗)直接在 codec 采样率(48kHz)上做**相位域**插值(`smooth = 0.5 - 0.5*cos(pi*x)`)，**没有真正的脉冲成形 FIR 匹配滤波器，也没有独立的插值/上采样阶段**。
- 帧结构：preamble(默认32个0比特) → varicode文本(字符间2个0比特分隔) → postamble(等长1比特) → idle。
- 输出：`s_tx_ring[8192]` 环形缓冲，`isr_i2s0` 里逐样点弹出发送给 I2S，**非DMA**，是手写单样点搬运。
- **全仓库没有任何 PTT 硬件控制代码**（grep `PTT`/`GPIO` 零匹配）——发射纯粹靠把波形写到耳机/线路输出，由外部电台的 VOX 或人工按键触发。
- 幅度固定为 `PSK_TX_DEFAULT_AMPLITUDE=9000`（约27% int16量程），载波频率取当前 `tone_hz`（默认1500Hz）。
- CLI `TX=<text>` 命令触发，检查逻辑与 `psk_tx_start_text` 相同；没有独立的"按下/释放PTT"语义。

### 2.3 内存与性能数据（RX侧，TX侧从未测量过）

- Core1 DLM 已用 93.75%（240KB/256KB，仅剩16KB）：3个K=16 Viterbi metric buffer(3×64KB)+16KB输出查表+32KB主栈。
- SDRAM 用了28.39%：2个Viterbi decision buffer(2×768KB)+2个LCD framebuffer+第4个metric buffer(64KB)+16KB heap。
- **TX缓冲区(`psk_tx_t g_tx`、`s_tx_ring[8192]`)是普通`.bss`静态变量，没有任何特殊内存放置**，体量远小于RX的Viterbi状态。
- HPM6750自己的评估文档（`docs/FREERTOS_SMP_TASK_SPLIT_TX_EVALUATION_2026-08-20.md`第9节）明确指出：**新TX编码器（尤其卷积编码+交织用于覆盖21个待补FEC/多载波模式）的CPU负载从未测量过**，只能待实现后逐模式复测，且要警惕挤占已接近满载的8PSK250F/8PSK1200F解调任务时间片。

### 2.4 模式集合对照（重要澄清）

HPM6750 的 27 种模式全部是 BPSK/QPSK/8PSK 变体，**没有 RTTY 或 CW**——`psk_rtty` 只是历史命名，其 `PSK_RTTY_MODE_RTTY` 枚举值仅被引用了波特率常量，从未被 `psk_modes.c` 暴露为可选模式,是死代码。也没有 FT8/FT4/MFSK16。ESP32-P4 项目额外实现的 FT8/FT4/CW/MFSK16 是**纯增量功能，不在本次移植范围内的"缺口"里**，只是需要说明：这四类新模式目前也没有任何 TX 设计，如果"所有模式发射"要求覆盖它们，工作量会进一步增加（本文档默认"所有模式"指 HPM6750 的 27 种 PSK/QPSK/8PSK 家族，FT8/FT4/CW/MFSK16 的 TX 需求需要用户确认是否在范围内，见第 7 节）。

## 3. 差距分析总表

| 维度 | HPM6750（源） | ESP32-P4（现状） | 结论 |
| --- | --- | --- | --- |
| RX 27种PSK模式解码 | 全部支持 | 已完成，且更完整（多FT8/FT4/CW/MFSK16） | **无需移植，已超出源项目** |
| RTOS 任务模型 | AMP双镜像，每核1个超级循环任务 | SMP单调度器，3个粗粒度任务，未绑核 | **架构基础更好，但任务粒度同样太粗，需要重新设计（非直接移植HPM6750的拆分方案）** |
| 核间/任务间通信 | 手写seqlock+手动cache一致性维护，控制通道纯轮询 | 标准队列(3个)+LVGL内部互斥锁 | **HPM6750机制不适用，ESP32-P4应保持并扩展现有的标准FreeRTOS原语** |
| TX覆盖范围 | 6/27模式，无FEC单载波BPSK | 0/27，完全空白 | **两边都需要新开发21个模式的编码器；HPM6750现有6模式代码可作为BPSK部分的参考起点，但实现质量需要提升** |
| 音频输出硬件 | WM8960+手写ISR环形缓冲，非DMA | BSP已就位全双工I2S+现成`bsp_audio_codec_speaker_init()`，应用层未调用 | **ESP32-P4起点更好，可以直接用DMA双缓冲，不必复刻HPM6750的手写方式** |
| PTT控制 | 无 | 无 | **两边都缺，需要作为新设计项，且需要确认目标硬件是否有PTT/功放使能引脚** |
| Viterbi/FEC热数据内存放置 | 精细手工分区DLM/SDRAM，DLM已93.75%满 | 已经全部在内部RAM，PSRAM仅用于调试缓冲和UI画布 | **ESP32-P4当前设计已经正确，无需改造；但需要测量内部RAM剩余余量** |
| TX性能KPI | 从未测量 | 不存在（TX未实现） | **两边都是空白，ESP32-P4需要在实现后建立自己的KPI方法论** |

## 4. FreeRTOS 任务架构设计建议（ESP32-P4 SMP 版本）

设计原则参考 HPM6750 评估文档"把确定性强、时间关键的采集+解调路径，和确定性弱、可能耗时更长的TX编码/控制响应/诊断发布路径解耦"的思路，但用 ESP32-P4 的 SMP 原语重新实现，**不引入 AMP 式的手写IPC**。

| 建议任务 | 职责 | 建议优先级 | 建议绑核 | 触发方式 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `audio_capture_task` | 从 I2S 读取采样块（替代现 `audio_task` 里读取部分） | 最高 | Core 0 | 由 I2S 事件队列/`xTaskNotifyFromISR` 唤醒，取代现在的"每64采样`vTaskDelay(1)`"节流方式 | ESP-IDF 的 `i2s_channel_read`/`esp_codec_dev_read` 本身是阻塞式调用，天然贴合"数据到来即返回"，不需要像HPM6750一样手写ISR环形缓冲，只需确认当前是否已经是阻塞读（若是，任务本身已经是事件驱动，只是缺少绑核） |
| `psk_demod_task` | 现`audio_task`里的全部解调分发(`decoder_layer_feed`及各family) | 最高，等同或略低于capture | Core 0 | 由capture任务通过队列/任务通知触发 | 这是当前实际DSP大头，必须保证不被UI或未来TX任务抢占过久 |
| `psk_tx_task`（新增） | 承载全部TX编码/调制生成，向codec写入 | 高，但明确低于demod | Core 0（若TX编码耗时可控）或 Core 1（若担心和demod抢占，见6.1节权衡） | 定期检查输出队列余量，按需生成填充 | 拆出来的直接好处：未来更复杂的FEC编码器（交织+卷积批量计算）不会拖慢解调任务的调度节奏，与HPM6750评估文档的建议一致 |
| `control_task` | 消费`s_control_queue`，应用模式/音调/AFC/传输方式等设置 | 中 | Core 1 | 队列阻塞等待，天然事件驱动，现有实现已经是这个模式,只需要从`psk_ui`里独立出来 | |
| `psk_ui` / `display_task` | LVGL对象更新、串口命令解析 | 中低 | Core 1 | 保持现有队列驱动方式 | |
| `taskLVGL`（BSP自带） | `lv_timer_handler`渲染 | 4（保持现值或按需调整） | Core 1 | 保持现状 | |
| `diag_task`（可选新增） | 周期性CPU负载/队列水位诊断打印 | 最低 | Core 1 | `vTaskDelay`周期 | 用于建立第7节要求的P4真机KPI基线 |

**核心分工原则**：Core 0 专职"时间关键的DSP路径"（采集+解调+TX编码），Core 1 专职"确定性要求较低的交互/显示/控制"路径。这与HPM6750的Core0=UI/Core1=DSP分工思路一致（只是核编号方向相反，不重要），但因为ESP32-P4是SMP，**不需要**HPM6750式的双镖像+手写seqlock，用`xTaskCreatePinnedToCore`+标准队列/任务通知即可实现同等程度的隔离，且实现和调试复杂度远低于AMP方案。

## 5. 全模式发射链路设计建议

### 5.1 模块结构（沿用HPM6750评估文档8.3节的模块划分思路，改写为ESP32-P4目标）

- **`psk_tx_symbol.c/.h`**（覆盖BPSK/QPSK/8PSK共15种无FEC模式）：Gray映射表+差分相位累加，统一的相位插值/NCO输出框架。BPSK部分可参考HPM6750`psk_tx.c`的varicode+相位平滑逻辑作为起点，但建议**不要原样照搬其"相位域升余弦"做法**，而是设计真正的脉冲成形（例如RRC匹配滤波+上采样），原因：
  1. HPM6750自己的实现是"原型级"，没有验证过在低SNR/宽频偏条件下的频谱纯净度（旁瓣/带外辐射），直接照搬会把这个已知的工程债务带入新项目；
  2. ESP32-P4的CPU/RAM余量明显好于HPM6750的Core1（400MHz双核+硬件FPU+PSRAM vs HPM6750单核24-816MHz级别的Core1独立DLM 256KB已用满93.75%），做真正的FIR成形在ESP32-P4上没有明显的资源压力理由要走"偷工减料"的路线。
- **`psk_tx_fec.c/.h`**（覆盖全部12种FEC模式）：按`fec_k/fec_poly1/fec_poly2/inlv_depth/inlv_len/punctured`参数实现卷积编码+交织+打孔。**关键前置工作**：这些参数目前只存在于`components/psk_rtty/psk_rtty.c`的RX侧运行时状态里，需要先提炼成一份TX/RX共享的静态模式参数表（增补到`main/psk_modes.c`的`psk_mode_info_t`），这一步在HPM6750和ESP32-P4上是**同样的工作量**，可以互相校验（两边解码器本来就共享同源`psk_rtty`算法血统，参数应该是一致的，是很好的交叉验证机会）。
- **`psk_tx_multicarrier.c/.h`**（覆盖6种多载波Nx模式）：N路独立NCO叠加输出，复用单路`psk_tx_symbol`逻辑。
- **`psk_tx.c/.h`**（对外接口层）：`psk_tx_start_text`/`psk_tx_generate`/`psk_tx_active`，内部按`family/fec/carriers`分派到上述模块。

### 5.2 音频输出与硬件适配（ESP32-P4相对HPM6750的结构性优势）

- 直接调用现成的`bsp_audio_codec_speaker_init()`（ES8311，`ESP_CODEC_DEV_TYPE_IN_OUT`）获取输出句柄，`psk_tx_task`按块生成样本后`esp_codec_dev_write()`。
- **不需要**复刻HPM6750的手写`s_tx_ring`+ISR逐样点弹出方案——ESP-IDF的I2S驱动本身基于DMA描述符链，`i2s_channel_write`/`esp_codec_dev_write`已经是双缓冲语义，只要`psk_tx_task`按合理的块大小喂数据、不阻塞过久即可,不需要应用层再手搭一层环形缓冲+关中断保护（这是HPM6750平台没有现成DMA驱动时的无奈之举，不应该被当作"标准做法"移植过来）。
- **PTT**：全仓库（两个项目）都没有PTT硬件控制。需要先确认目标ESP32-P4硬件板（Function EV Board 或其他自定义板）上是否有预留GPIO用于键控外部电台/功放。如果没有明确硬件需求，建议先按"纯音频线出，由外部设备VOX触发"的模式实现（与HPM6750现状一致），并在软件接口层预留一个可选的PTT GPIO回调，方便未来硬件确定后快速接入。**这是需要用户确认的硬件范围问题，见第7节**。

### 5.3 分阶段实施顺序（利用ESP32-P4已有解码器做闭环验证——独有优势）

1. **阶段0：移植HPM6750现有的6模式无FEC BPSK发射逻辑**（framing/varicode/相位生成的思路，不是逐行照搬代码），接入`bsp_audio_codec_speaker_init()`，跑通"文本→波形→codec输出"整条链路。这一步的验证目标是打通硬件和任务架构，不追求调制质量。
2. **阶段1：QPSK/8PSK Gray符号映射器**（+9种模式），复用阶段0的NCO/相位框架扩展到4/8相。
3. **阶段2：多载波Nx NCO组**（+6种模式）。
4. **阶段3：FEC编码器+交织器+打孔器**（+12种模式），工作量最大，需要先完成5.1节提到的"参数表提炼"。

**ESP32-P4相对HPM6750的一个独特优势**：由于ESP32-P4项目的RX侧27种模式解码已经验证通过（依据`STM32H7_TO_ESP32P4_MIGRATION_STRATEGY_2026-08-20.md`的调研结论），每完成一个TX阶段，都可以直接用**同一块板子的TX输出接回RX输入**（物理音频线自环或未来考虑软件层直接的样本级自环）做自发自收闭环验证，不需要像HPM6750/psk-script那样依赖外部PC播放WAV的方式做板测。这能显著加快TX开发的验证效率，建议在阶段0就把自环测试脚手架搭起来（例如新增一个仅用于测试的CLI命令，让TX直接写入一段已知文本，同时让RX解调该文本，比较发送/接收文本是否一致）。

## 6. 性能保证策略

### 6.1 TX任务与RX解调任务的核心隔离

参考HPM6750评估文档的判断："新的TX编码逻辑不能挤占解调任务的调度节奏，尤其是已经接近满载的8PSK250F/8PSK1200F这类高速模式"。在ESP32-P4上，建议：

- `psk_demod_task`独占Core 0的最高优先级时间片；`psk_tx_task`可以和`psk_demod_task`同核但更低优先级（允许被demod抢占），或者放到Core 1和UI共享（如果TX编码本身不追求极低延迟，例如允许几十毫秒的生成提前量）。**具体选择需要在阶段3实现FEC编码器后，用实测CPU负载数据决定**，不要在设计阶段假设结论。
- 无论选哪种绑核方案,都应该给TX输出队列留足够的"提前量"缓冲（例如提前生成200-500ms的音频样本存入队列），这样即使`psk_tx_task`被短暂抢占，也不会造成codec输出欠载(underrun)。

### 6.2 用事件驱动替代当前的固定节流

当前`audio_task`每64采样`vTaskDelay(1)`是一种人为限流,不是真正的"数据驱动"。建议：

- 确认`esp_codec_dev_read()`/底层`i2s_channel_read()`是否已经是阻塞调用（如果是，说明任务本身已经是"数据到来即返回"，`vTaskDelay(1)`只是多余的额外延迟，可能反而拖慢响应，建议评估去除或换成基于`I2S_EVENT`回调的任务通知）。
- 若确认需要保留限流（例如是为了给低优先级任务让出CPU），应该基于实测的音频消耗速率计算合理的延迟值，而不是拍脑袋定为1 tick。

### 6.3 内存策略

- 当前ESP32-P4项目的PSRAM/内部RAM分配已经是正确方向（热数据在内部RAM，调试缓冲/UI画布在PSRAM），**不需要改造**，只需要：
  1. 用`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`和`uxTaskGetStackHighWaterMark()`测出当前内部RAM剩余余量，作为后续新增TX模块/任务栈的预算依据（这一步至今未做，属于空白，应在动手写TX代码前先补上）。
  2. 新增的TX编码表（Gray映射表、卷积码生成多项式派生表、交织表）体量参考HPM6750同类结构（数KB到数十KB级别，远小于RX的Viterbi metric/decision数组），可以直接放内部RAM，不需要PSRAM，除非实测后发现内部RAM余量不足。
  3. 新增任务栈优先用内部RAM（`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`已使能但当前未使用，只在内部RAM实测吃紧时才考虑切换到PSRAM栈，PSRAM栈访问延迟更高，不适合时间关键任务）。

### 6.4 KPI基线方法论（复用HPM6750/STM32H7两个项目已验证的方法论）

- 在开始任何TX开发或任务拆分之前，先补齐ESP32-P4的**真机RX KPI基线**（这是前一份`STM32H7_TO_ESP32P4_MIGRATION_STRATEGY_2026-08-20.md`文档排第一优先级、至今未完成的工作，本文档重申其必要性——没有基线，无法判断后续任务拆分/TX新增是否引入回归）。
- 复用HPM6750`docs/HIGH_RATE_BOARD_KPI_20260819.md`的方法：WAV语料库全量播放、板端串口HEX采集、payload BER/CER与完整流BER/CER分离统计、DSP平均/峰值耗时、队列水位/溢出计数。
- TX的KPI补充项（两个项目都尚不存在）：TX编码任务的CPU占用（尤其FEC编码+交织，在长报文/高速模式下的批量计算耗时）、codec输出队列的水位/欠载(underrun)计数、自发自收闭环的BER/CER。

## 7. 风险与待核实项

1. **ESP32-P4双核共享SRAM的缓存一致性模型细节未核实**：本文档假设"标准FreeRTOS队列/任务通知即可安全跨核传递数据,不需要手动cache维护",这是基于ESP32-P4两核共享同一地址空间、且此前迁移文档中的判断，但**本次会话未能查阅ESP-IDF/ESP32-P4官方技术参考手册来确认内部SRAM(L2MEM)对两个HP核心是否是完全一致、无需软件维护的**（一些RISC-V多核设计仍需要软件fence或存在per-core cache需要显式维护）。建议在实现任务拆分前，查阅ESP-IDF关于`portMUX`/SMP并发访问共享内存的官方说明，或者直接用一个双核并发读写测试来实测验证。
2. **目标硬件是否有PTT/功放使能引脚未确认**：本文档假设"纯音频线出，外部电台VOX触发"，如果目标产品需要主动控制发射机（例如驱动继电器/光耦PTT或CAT控制），需要用户明确硬件连接方式，这会影响是否需要新增GPIO驱动模块。
3. **"所有模式"的确切范围未完全确认**：HPM6750的27种PSK/QPSK/8PSK模式肯定在范围内；FT8/FT4/CW/MFSK16这四类ESP32-P4独有的增量模式是否也需要发射能力，需要用户明确（FT8/FT4的发射涉及完全不同的技术栈——时隙同步、GFSK/8FSK音调发生、LDPC编码——工作量和优先级应该独立评估，不建议和PSK系列TX混在一起规划）。
4. **ESP32-P4内部SRAM当前实际剩余余量未测量**：sdkconfig显示PSRAM/内部RAM策略配置合理，但没有实测数据佐证"内部RAM还能再塞多少新任务栈/TX编码表"，这是6.3节列出的待办。
5. **HPM6750现有BPSK TX的调制质量（带外辐射/频谱纯净度）未经专业测量**，只是从代码逻辑上判断"缺少真正的脉冲成形"，如果决定完全重新设计而非移植，建议在设计初期用频谱分析仪或至少FFT截图确认新实现相对旧实现的改善,避免"看起来更规范但实际效果没有变化"的情况。

## 8. 建议的推进顺序（供参考，非本次实施内容）

1. **第零阶段（低风险，必须先做）**：补齐ESP32-P4真机RX KPI基线（第6.4节），并测量内部RAM/栈余量（第6.3节第1点）。这是后续所有改动的判断基准，目前完全空白。
2. **第一阶段（中风险，架构调整）**：按第4节把现有3个粗粒度任务拆分为核心绑定的细粒度任务，先只做RX路径的拆分（不引入TX），验证拆分后KPI相对第零阶段基线没有回归。
3. **第二阶段（新功能，独立立项）**：按第5.3节的阶段0-3推进TX开发，每个子阶段做自发自收闭环验证和逐模式CPU负载复测，优先在第一阶段拆出的独立`psk_tx_task`上运行。
4. **第三阶段（可选，视用户范围确认结果）**：视第7节第3点的范围确认结果，决定是否需要为FT8/FT4/CW/MFSK16单独规划发射能力。

## 9. 声明

本文档为评估/规划文档，撰写过程中未修改 `hpm6750-psk-modem` 或 `esp32p4-psk-modem` 任一项目的任何源代码、构建脚本或配置文件。文档内容基于对两个项目实际代码的调研结果：`esp32p4-psk-modem` 的 `main/app_main.c`、`main/audio_input.c`、`main/display.c`、`main/transport.c`、`main/psk_modes.c`、`main/demod_primitives.c`、`main/ui.c`、`components/psk_rtty/include/psk_rtty.h`、`sdkconfig`、`main/CMakeLists.txt`、`managed_components/espressif__esp32_p4_function_ev_board/esp32_p4_function_ev_board.c`、`managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c`；`hpm6750-psk-modem` 的 `src/main.c`、`core1/src/main.c`、`app/common/multicore_boot.c`、`app/common/ui_transport.c`、`app/modem/psk_tx.c`/`.h`、`app/modem/psk_modes.c`、`app/platform/audio_input.c`、`third_party/psk_rtty/include/psk_rtty.h`、`app.yaml`、`CMakeLists.txt`、`core1/CMakeLists.txt`，以及该项目 `docs/` 下的 `FREERTOS_SMP_TASK_SPLIT_TX_EVALUATION_2026-08-20.md`、`PSK_COMMERCIALIZATION_REVIEW_2026-08-18.md`、`HIGH_RATE_BOARD_KPI_20260819.md`、`PSK1200F_COMMERCIAL_RELIABILITY.md`、`PSK_DECODE_OPTIMIZATION_REVIEW.md`、`MCU_SELECTION_EVALUATION.md`。ESP32-P4双核缓存一致性模型等第7节列出的具体待核实项，因本次会话网络访问受限或需要实机测试，未能现场核实，请在制定实施方案前另行验证。
