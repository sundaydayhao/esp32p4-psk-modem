# STM32H7 PSK Modem 全功能移植到 ESP32-P4：可行性与内存权衡策略

日期：2026-08-20

本文档是纯评估/策略文档，**未修改任何代码**。用于回答两个问题：

1. `C:\projects\stm32h7-psk-modem` 的全部功能是否可以移植到 `C:\projects\esp32p4-psk-modem`？
2. 类似 HPM6750 项目中遇到的"DLM 片内内存紧张"问题，在 ESP32-P4 上是否存在，如果存在有哪些权衡之策？

## 0. 先说明一个关键背景

`stm32h7-psk-modem` 自己的 `doc/MCU_SELECTION_EVALUATION.md`（2026-08-13）**曾经明确否决过 ESP32-P4** 作为下一代主控，理由是"片上RAM约768KB，低于当前项目约848KB需求…不适合当前重Viterbi实时解码主控"，并推荐 i.MX RT1176（首选）或 HPM6750（次选，成本优化）。

但事实上 `esp32p4-psk-modem` 这个项目**已经存在，并且已经构建成功、跑通了 27 种 PSK 模式加 FT8/FT4/CW/MFSK16 的解码**——它选择的解决方式正是当年被否决方案里点名担心的那条路：**把内存热数据的一部分挪进外置 PSRAM**（`CONFIG_SPIRAM`、`EXT_RAM_BSS_ATTR`）。这不是"移植是否可行"的问题被推翻了，而是**移植团队做出了和评估文档不同的工程取舍**,目前还没有真机 KPI 数据验证这个取舍在最吃内存/CPU 的高速 FEC 模式（8PSK250F 双支路捕获、8PSK1200F 打孔 K=7）上是否站得住。这正是本文档要重点评估的地方。

## 1. 结论摘要

| 问题 | 结论 |
| --- | --- |
| stm32h7-psk-modem 的功能能否全部搬到 esp32p4-psk-modem | **能，而且大部分已经做完了。** stm32h7-psk-modem 是纯接收端（无发送/调制代码），esp32p4-psk-modem 现有代码已实现全部 27 种 PSK 模式解码 + 更多（FT8/FT4/CW/MFSK16），加上 LVGL UI、I2S+codec 音频输入、触摸交互。剩余工作是**验证与补齐**，不是"从零实现"。 |
| 是否还缺什么 | 缺真机高速模式 KPI 验证（尤其是内存/CPU 双重承压的 8PSK250F、8PSK1200F）、缺文档清理（3 份历史文档是 ESP32-S3 阶段遗留、未更新）、缺一个无用组件清理（`esp_lcd_touch_ft5x06`)。 |
| DLM 类问题是否存在 | **存在,但形态不同。** ESP32-P4 没有 HPM6750 式的"每核心独立 256KB DLM"限制,是两个 HP 核心共享同一块片内 L2MEM SRAM(常见规格约 768 KB,具体数值请以官方最新数据手册核对,本文档所用数字来源于 stm32h7 团队评估文档的引用,未能在本次会话中直接访问 Espressif 官网核实)。真正的紧张点是"~768KB 共享片内 SRAM"能否装下 STM32H7 静态分配方式下所需的~848KB Viterbi 热数据。 |
| 有没有权衡之策 | 有,见第 6 节,核心思路是"冷热分层 + 按需分支释放",而不是简单地整体搬进 PSRAM。 |

## 2. 三个项目的谱系关系（背景确认）

`SESSION_HANDOFF_2026-08-18.md`、`arch.md` 等文档确认了这条链路：

```
stm32h7-psk-modem（纯接收,单核 Cortex-M7,裸机)
        │  共享 psk_modes.c / psk_rtty.c 核心算法
        ▼
esp32s3-psk-modem（中间阶段,细节留在了本项目的历史文档里)
        │
        ▼
esp32p4-psk-modem（当前目标,已构建、含 FT8/FT4/CW/MFSK16 扩展)
```

hpm6750-psk-modem 是另一条独立分支（同样源自与 stm32h7 共享的 `psk_modes.c`/`psk_rtty.c`,但走的是双核 AMP + 部分 TX 支持路线,与本次评估的 stm32h7→esp32p4 方向不是同一条链路,不要混淆——hpm6750 有 BPSK 单载波 TX,stm32h7 完全没有 TX)。

## 3. 功能对照表

| 功能域 | stm32h7-psk-modem（源) | esp32p4-psk-modem（现状) | 移植结论 |
| --- | --- | --- | --- |
| RX 解码：27 种 PSK 模式 | 全部支持（`decode_supported=1`）,`third_party/psk_rtty` 统一后端,含双支路 K=16 Viterbi FEC | 已实现 27 种模式(`psk_modes.c` 移植 + `components/psk_rtty`) | **已完成**,需要真机回归验证是否 27/27 通过（stm32h7 侧最新板测是 PASS=27） |
| 其它数字模式 | 无（stm32h7 只做 PSK/RTTY） | 已额外实现 FT8/FT4（`components/ft8lib`,移植自 `kgoba/ft8_lib`）、CW、MFSK16 | **超出源项目范围**,不是移植缺口,是增量功能 |
| TX/调制发送 | **完全没有**（无任何 `tx_`/`modulate`/`encode` 函数） | 同样未发现 TX 代码 | **无需移植**,源项目本身是纯接收机;若未来要做发送,是新功能而非移植缺口(参考另一份 hpm6750 项目的 TX 差距分析文档) |
| 音频输入 | WM8978 codec, I2S2+DMA1, 48828Hz 实际采样→7-tap FIR 抽3→16276Hz 解码块 | I2S_std + `esp_codec_dev` + ESP32-P4 BSP,48kHz→16kHz,同款 `audio_decim.c` 抽取器 | **已完成**,抽取算法直接复用,只是外设驱动层换成了 ESP-IDF I2S/codec 抽象 |
| UI/显示 | LVGL 8.3,800×480 LTDC+外置 SDRAM 帧缓冲,DMA2D 加速瀑布图滚动 | LVGL(通过 `managed_components__lvgl`),1024×600 MIPI-DSI 显示（ESP32-P4 Function EV Board BSP,`CONFIG_BSP_LCD_TYPE_1024_600`,RGB565,DMA2D） | **已完成**,分辨率更高,DMA2D 加速思路一致 |
| 触摸 | GT911,I2C2 | GT911（`managed_components__espressif__esp_lcd_touch_gt911`） | **已完成**,同型号触摸芯片,驱动来自 ESP-IDF 组件仓库而非手写 |
| 串口 CLI / 诊断 | 轮询 UART1,`MODE/TONE/AFC/SQL/LOG/INPUT/LANE/GAIN/AGC/DUMP/CAP/CAPDUMP/STATUS` | USB-Serial,`@MODE/@CAP/@DUMP` 命令,用于自动化硬件测试 | **基本对应**,命令集不完全一致,需要逐条核对是否有 stm32h7 侧命令未被覆盖(尤其 `LANE`/`GAIN`/`AGC` 这类音频前端调试命令,取决于 ESP32-P4 codec 驱动是否暴露等价接口) |
| RTOS/并发模型 | 裸机,单核,超级循环 | ESP-IDF(FreeRTOS SMP),`audio_task`(prio 8)+`ui_task`(prio 2)双任务 | **架构上已经更优**,不是移植困难点,是移植带来的顺带改善(见第 5 节) |
| Viterbi/FEC 内存放置 | 精细手工分区到 DTCM/RAM_D1/RAM_D2/RAM_D3,避免任何可缓存/慢速内存承载热数据 | 已用 `EXT_RAM_BSS_ATTR` 把部分表格放进 PSRAM | **需要重新审查**,详见第 6 节,这是本次移植真正的技术风险点 |

## 4. ESP32-P4 硬件特性（用于评估内存策略的基础）

以下信息基于公开的 ESP32-P4 产品资料和训练知识,**本次会话中网络访问被沙箱环境阻止,无法现场核对 Espressif 最新数据手册的精确数字**,建议在制定最终方案前用 `idf.py`/官方 Technical Reference Manual 核实具体字节数（尤其是片内 SRAM 总量和 TCM 划分方式）:

- **双核 RISC-V "HP"（High-Performance）系统 CPU**,主频可达约 400 MHz,单精度 FPU,运行 ESP-IDF 自带的 **FreeRTOS SMP** 分支——**一个调度器统一管理两个核心**,任务可以浮动或用 `xTaskCreatePinnedToCore` 固定到某个核心。这与 HPM6750 的"每核心一份独立 FreeRTOS 镶像 + 独立地址空间"的 AMP 架构本质不同：**ESP32-P4 两个核心共享同一份片内 SRAM 地址空间**,不需要像 HPM6750 项目那样为跨核通信设计 cache 一致性维护的共享内存协议。
- 另有一个独立的单核 RISC-V **LP（Low-Power）核**,主频较低,通常用于低功耗常驻/外设唤醒管理,不参与主 DSP 负载。
- **片内 SRAM（L2MEM）**：两个 HP 核心共享,常见宣传规格约 **768 KB**（这正是 stm32h7 团队评估文档里引用并据以否决 ESP32-P4 的数字）。另有更小的 TCM（紧耦合内存）区域可配置,具体大小请以芯片型号版本和最新 SDK 文档核实。
- **外置 PSRAM**：支持 Octal/Hex SPI PSRAM,`esp32p4-psk-modem` 当前 `sdkconfig` 配置为 `CONFIG_SPIRAM_MODE_HEX=y`、`CONFIG_SPIRAM_SPEED_200M=y`,常见开发板搭配 32 MB PSRAM。PSRAM 通过 cache 访问,延迟明显高于片内 SRAM,且更适合**顺序/批量访问**模式（如帧缓冲、大块音频采样缓存),对**随机/高频访问的小粒度热数据**（如 Viterbi ACS 循环里的度量数组）不友好。
- 无内置 Wi-Fi/BT 射频（`esp32p4-psk-modem` README 也确认了这点,WiFi 不可用,BT 是预留桩），如需无线连接需要外接 ESP32-C6/C5 协处理器,与本次内存评估无直接关系。

## 5. SMP 架构对本次移植的正面影响（顺带说明,非本文档主线）

因为 ESP32-P4 两个 HP 核心共享同一片内存空间、由同一个 FreeRTOS 调度器管理,`app_main.c` 里的 `audio_task`/`ui_task` 拆分不需要像 HPM6750 项目那样处理跨核缓存一致性（`l1c_dc_writeback`/`invalidate`+`fence` 那一套),也不需要设计 seqlock 式共享内存协议——用标准的 FreeRTOS 队列/信号量即可安全跨任务传递数据,即使两个任务被调度到不同核心也是如此。这是移植到 ESP32-P4 相对 HPM6750/裸机 STM32H7 而言,在"任务拆分/并发设计"维度上的**净收益**,不是本文档要解决的风险点,这里提一句是为了让读者理解为什么第 6 节的内存策略里不需要考虑"哪块内存哪个核心能访问"这类 AMP 特有的复杂度。

## 6. 内存权衡策略（核心问题：DLM 类紧张在 ESP32-P4 上怎么办）

### 6.1 问题的本质

STM32H7 侧的 ~848 KB 需求,拆解开看,并不是"27 种模式同时运行需要 848KB",而是**裸机静态全局数组为了覆盖所有可能配置,按最坏情况一次性全部分配**造成的,具体构成（引用自 stm32h7 项目实测数据）：

| 结构 | 大小 | 说明 |
| --- | --- | --- |
| `s_vit_out_pair` | 16 KB | 输出比特查找表 |
| `s_vit_metrics0_a` / `_b` | 64 KB × 2 = 128 KB | 分支 0 的度量数组(ping-pong) |
| `s_vit_metrics1_a` / `_b` | 64 KB × 2 = 128 KB | 分支 1 的度量数组(ping-pong,双支路捕获用) |
| `s_vit_decisions0` | 256 KB | 分支 0 的判决/回溯表 |
| `s_vit_decisions1` | 256 KB | 分支 1 的判决/回溯表(双支路捕获用) |

**关键观察**：分支 1（`metrics1_a/b` + `decisions1`,合计约 320 KB）**只在"双支路捕获"阶段才需要**——这正是 8PSK125F/8PSK250F 用来解决相位/比特对齐二义性的技巧,锁定单支路后,另一支路的状态就是纯粹的死重（HPM6750 侧文档也提到"锁定后停止失败支路,显著降低持续负载",但那是指停止计算,并没有释放内存,因为裸机静态数组本来就无法释放）。

也就是说,**stm32h7 团队引用的"~848KB"这个数字本身就包含了大量"仅捕获阶段需要、锁定后闲置"的冗余分配**,在裸机+静态数组的世界里没办法优化掉,但在 ESP32-P4（有真正的堆、有 `heap_caps_malloc`）上是可以优化的。

### 6.2 具体权衡策略（按优先级排列）

1. **按需分配/释放双支路捕获状态（优先级最高,收益最大,风险最低）**
   把"分支 1"专用的 `metrics1_a/b` + `decisions1`（约 320 KB）改为进入捕获状态时用 `heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` 动态申请,锁定单支路后 `heap_caps_free()` 释放。
   - 效果：常态（已锁定/单载波无 FEC 模式)下,片内 SRAM 占用直接减少约 320 KB,只在捕获瞬间（通常是信号刚出现的几百毫秒到几秒)才需要额外的峰值内存。
   - 这一步**不需要用到 PSRAM**,单靠"消除裸机式的静态冗余分配"就可能让原本~848KB 的需求降到~530KB 左右(848-320≈528KB),已经明显低于 ESP32-P4 约 768KB 的片内 SRAM,风险最低,应该作为**第一优先级**去验证。
   - 代价：需要在捕获/锁定状态切换的地方插入分配/释放调用,并处理分配失败（内存紧张时)的降级路径(例如降级为单支路捕获,牺牲一点捕获成功率换内存安全)。

2. **冷热分层：只把"读多写少、访问模式规整"的表放进 PSRAM,度量数组/判决数组本身留在片内 SRAM**
   - 需要重新核对 `esp32p4-psk-modem` 现有代码里 `EXT_RAM_BSS_ATTR` 具体标注了哪些数组。如果标注对象是**静态查找表类**（varicode 表、卷积码生成多项式派生表、输出比特 LUT 之类,只读、体积固定、访问模式规整,容易被 cache 命中)——这是安全的用法,风险低。
   - 如果标注对象包含**度量数组（metrics）或判决/回溯数组（decisions）**——这两类在 Viterbi 的 ACS(加-比-选)和回溯阶段是**逐符号更新、访问模式接近随机跳转**的热数据,和 stm32h7 团队自己在 `doc/arch.md` 里写的原则("热表和度量应放在片内快速 RAM,SDRAM 只当作显示/UI 存储")直接冲突——这类数据如果长期放 PSRAM,在高速模式（250/500/1000+ baud,尤其 8PSK250F/8PSK1200F)下,PSRAM 的访问延迟和 cache miss 率可能直接复现 STM32H7/HPM6750 两个项目都曾经踩过的"高速 FEC 模式抖动/丢块"问题。这是**必须用真机 KPI 验证,不能只凭编译通过判断**的地方。
   - 建议动作：审查现有 `EXT_RAM_BSS_ATTR` 标注清单,凡是命中"度量/判决/回溯"类结构,先临时挪回片内 SRAM 做一次基线测试,再决定是否真的需要放 PSRAM(结合第 1 条的按需释放,片内空间可能已经够用,根本不需要为热数据启用 PSRAM)。

3. **利用 TCM 放最内层循环状态（如果 ESP-IDF 工具链支持显式 TCM 段放置)**
   stm32h7 把最小、最热的两个数组（`s_vit_out_pair` 16KB + 一个 metrics 数组 64KB)放进 DTCM(零等待周期)。ESP32-P4 若有等价的 TCM 区域并且链接脚本/属性宏支持显式放置,应该优先把 ACS 内循环里访问最频繁的"当前阶段度量数组"放 TCM,而不是不加区分地放进普通 L2MEM 或 PSRAM。这一条需要先确认 ESP-IDF 对 ESP32-P4 TCM 的实际支持粒度和大小,本文档不下确定结论,列为待核实项。

4. **兜底方案:降低任意时刻需要维持的"并行假设支路数"或压缩存储粒度**
   如果第 1、2 条做完仍不够,可以参考 HPM6750 项目评估文档里记录过的两个"已验证会导致 CPU 超预算而被否决"的方案作为参照（不是直接照搬,而是提醒:这类"压缩换内存"的路子在同源代码库上已经试过、代价是 CPU 负载大幅上升):
   - 4-bit 打包判决输出（HPM6750 上导致 8PSK250F 负载从可接受升到 ~57.7%);
   - 把热表移到较慢的内存区(HPM6750 上导致捕获期负载升到 166%-185% 并溢出)。
   在 ESP32-P4 上,如果第 1、2 条策略已经把片内占用压到 768KB 以内,应该**不需要走到这一步**;只有在 1、2 两条策略验证后仍然不够、又不想牺牲某个高速模式的双支路捕获能力时,才考虑这类"CPU 换内存"的压缩方案,并且必须重新做一次针对 ESP32-P4 实际主频/cache 行为的 CPU 负载复测,不能假设 HPM6750 的负载数字可以直接套用。

### 6.3 与 HPM6750 项目的对照(帮助理解"DLM 问题"在两个平台上的不同表现形式)

| 维度 | HPM6750 | ESP32-P4 |
| --- | --- | --- |
| 紧张内存的物理形态 | 每核心独立 256KB DLM,已用 93.75%,只剩 16KB | 两核心共享约 768KB 片内 SRAM(具体数字待核实),尚未有 esp32p4-psk-modem 自己的实测占用数据 |
| 紧张的根本原因 | 单一在用模式的 Viterbi 状态(K=16 三个度量缓冲+一个大 LUT)本身就把 256KB 挤满,没有"多支路冗余"问题 | STM32H7 传承下来的静态全局分配方式里,包含大量"仅捕获阶段需要、锁定后闲置"的双支路冗余(约 320KB),真正稳态需求可能远低于表面数字 |
| 已验证的权衡结果 | 压缩/搬迁两个方案都因 CPU 超预算被否决,最终选择保留在紧张的 DLM 里 | 尚未有 esp32p4-psk-modem 自己的实测验证;本文档建议的"按需分配双支路状态"是 HPM6750(裸机风格、无堆)做不到,但 ESP32-P4(有 ESP-IDF 堆管理)可以做到的**结构性优势**,应该优先尝试 |

## 7. 移植可行性结论

**结论：全部功能可以移植,而且已经基本移植完成。剩余工作是验证类而非实现类。**

具体待办清单：

1. **真机高速模式 KPI 验证（最高优先级)**：目前 `esp32p4-psk-modem` 没有任何针对 P4 真机的高速/FEC 模式基准测试数据(现有 `SESSION_HANDOFF` 里的 BER/CER 表是 ESP32-S3 阶段的,不是 P4)。应该参照 stm32h7/hpm6750 两个项目已经建立的方法论(WAV 语料库全量播放、板端串口 HEX 采集、payload BER/CER + 完整流 BER/CER 分离统计),补一份 P4 专属的 KPI 文档,重点覆盖 8PSK250F(双支路捕获)和 8PSK1200F(打孔 K=7,STM32H7 自己都还没解决的高速 FEC 采集问题)。
2. **审查 `EXT_RAM_BSS_ATTR` 标注清单**：确认哪些数组被放进了 PSRAM,按第 6.2 节第 2 条的"冷热分层"原则重新分类,对命中"度量/判决/回溯"类的做基线对比测试。
3. **实施按需分配双支路捕获状态(第 6.2 节第 1 条)**：这是收益最大、风险最低的一步,建议作为独立的一次小改动优先落地和测试(本文档不涉及具体实现,留给后续实施阶段)。
4. **文档清理**：`arch.md`(标题仍是 ESP32-S3)、`SESSION_HANDOFF_2026-08-18.md`(记录的是 esp32s3-psk-modem 的调试上下文)、`recomment_board.md`(ESP32-S3 阶段的选板笔记)三份文档需要重写或标注"历史文档,不代表当前 P4 状态",避免误导后续开发者对"当前完成度"的判断。
5. **清理无用组件**：`components/espressif__esp_lcd_touch_ft5x06` 未被 `main/` 任何代码引用,是 ESP32-S3 board 的遗留物,P4 EV Board 实际用的是 GT911,应删除或明确标注为保留备用。
6. **CLI 命令集对照**：核对 stm32h7 侧 `LANE`/`GAIN`/`AGC`/`INPUT` 等音频前端调试命令是否在 esp32p4 的 `@MODE/@CAP/@DUMP` 体系里有等价能力,如果 `esp_codec_dev` 抽象层不支持某些精细控制(如强制声道选择),需要评估是否要补齐。
7. **提交检查点**：`esp32p4-psk-modem` 仓库目前 0 commits,所有文件都是未跟踪状态,建议在继续修改前先做一次初始提交,固定当前"能构建、能跑通 27 模式解码"的基线,避免后续实验性改动(尤其是内存布局调整)无法回退。

## 8. 声明

本文档为评估/策略文档,撰写过程中未修改 `stm32h7-psk-modem` 或 `esp32p4-psk-modem` 任一项目的任何源代码、构建脚本或配置文件。文档内容基于对两个项目代码与文档的实际调研结果(`stm32h7-psk-modem` 的 `app/modem/psk_modes.c`、`third_party/psk_rtty/psk_rtty.c`、`STM32H743IITX_FLASH.ld`、`doc/MCU_SELECTION_EVALUATION.md`、`doc/DEBUG_NOTES.md`、`doc/HIGH_RATE_BOARD_KPI_20260819.md`、`doc/arch.md`、`doc/PSK_DECODE_OPTIMIZATION_REVIEW.md`;`esp32p4-psk-modem` 的 `README.md`、`arch.md`、`SESSION_HANDOFF_2026-08-18.md`、`sdkconfig`/`sdkconfig.defaults`、`main/`、`components/psk_rtty`、`components/ft8lib`)。ESP32-P4 硬件规格部分因本次会话网络访问受限,未能现场核对 Espressif 官方最新数据手册,列出的片内 SRAM/TCM 具体数值请在制定实施方案前另行核实。
