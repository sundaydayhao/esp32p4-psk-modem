# 调试记录 2026-08-23

## 目标
- 最终以 `C:\projects\fldigi` 的接收逻辑为准。
- 只看 PC 播放 WAV -> 板子 RX 解码，不围绕 TX 逻辑展开。
- 设备是 ESP32P4 Function EV Board。

## 当前已确认
- 工程主线在 `components/psk_rtty/psk_rtty.c`。
- `idf.py build` 已通过。
- `tools/build_host_tools.ps1` 已通过。
- 当前工作区里保留的是有意修改，不要回滚用户已有改动。

## 参考对照
- fldigi 关键路径：
  - `src/psk/psk.cxx`
  - `src/include/psk.h`
  - `rx_process()`
  - `rx_symbol()`
  - `rx_pskr()`
  - `phaseafc()`
  - `tx_carriers()`
- 结论：fldigi 的多载波接收是“最后一个载波触发一次符号推进”，不是每个载波单独推进。

## 已做修改
- `components/psk_rtty/psk_rtty.c`
  - 将多载波间隔参数从固定写法改成按模式区分：
    - 单载波保持 `1.4 * baud`
    - 多载波改成 `2.0 * baud`
  - 这一步是对齐 fldigi 的 `separation` 逻辑。
- `main/display.c`
  - 瀑布图色阶再次压暗，底噪不会那么亮。
- `tools/host_psk_rtty_wav.c`
  - 增加了日志输出，方便后续主机回归排查。

## 试过但判定错误的方向
- 之前把 `acquire_symbols` 拉长的方向不对，已经回退到基线。
- 这类问题不是简单“门控越长越好”。

## 现场现象
- 瀑布图噪声太亮的问题已处理一轮，但仍需继续看最终观感。
- 板测里存在高 BER 模式，尤其是多载波和部分 FEC 模式。
- 之前出现过 LVGL `blur_walk_cb` 崩溃栈，但这次记录的主线仍是 PSK RX 逻辑。

## 主机测试状态
- `tools/run_host_regression.ps1` 运行结果目前仍是全部 FAIL。
- 目前问题不在 build，而在 host 侧输出/判定链路没跑通，后续要先确认：
  - host 工具是否真正进入 main
  - 日志是否写到预期位置
  - 回归脚本是否拿到了有效 summary

## 板测状态
- 串口板测端口：`COM21`
- 目标是 PC 播放 `C:\projects\psk-script\generated-wav-48000` 下的 wav，板子只做 RX 解码。

## 下一步建议
1. 继续按 fldigi 精读 RX 路径，重点看多载波和符号推进时机。
2. 先把主机回归链路修到能稳定输出 summary，再批量测各模式。
3. 再回到板测，逐个修高 BER 模式。

