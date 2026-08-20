# ESP32-S3 PSK Modem Session Handoff - 2026-08-18

This document records the current debug context for `C:\projects\esp32s3-psk-modem`.

## Hardware / Environment

- Board: LCKFB SZPI ESP32-S3 board.
- Serial port used in this session: `COM14`.
- ESP-IDF environment:
  `C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1`
- Build/flash command:
  ```powershell
  C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1'; Set-Location 'C:\projects\esp32s3-psk-modem'; idf.py -p COM14 flash }"
  ```
- WAV test files:
  `C:\projects\psk-script\generated-wav-48000`
- Expected text:
  `C:\projects\psk-script\input.txt`
- Board reference project:
  `C:\projects\lichuang-esp32s3-demo`
- STM32H7 regression reference:
  `C:\projects\stm32h7-psk-modem`

## Current Firmware State

- Settings menu was removed from the active interaction path.
- SW1 now cycles modes directly.
- Default RX tone is `1500 Hz`.
- 8PSK and multi-carrier modes are enabled in the SW1 cycle and serial command path.
- Serial debug mode command was added over USB Serial/JTAG RX:
  - Examples: `@BPSK63`, `@QPSK125`, `@8PSK500F`, `@12xPSK125`
  - This is used by `tools/hw_wav_test.py` to automate board tests.
- Periodic audio/modem debug logs are disabled in `main/board_config.h`:
  - `BOARD_AUDIO_DEBUG_LOGS 0`
  - `BOARD_MODEM_DEBUG_LOGS 0`
  This avoids mixing diagnostic logs with decoded text on stdout.
- Text display now scrolls upward and stays on the newest decoded text.
- ES7210 is initialized directly from `main/audio_input.c` using the new I2C master driver.
  - Do not edit `managed_components`.
  - `main` no longer depends on `espressif__es7210`, avoiding old/new I2C driver conflict.

## Important Code Changes

- `main/ui.c`
  - SW1 mode cycle includes BPSK, multi-carrier xPSK, QPSK, 8PSK, 8PSK-F, FT8, FT4, RTTY, CW, MFSK16.
  - Hidden serial commands parse all PSK mode names, including 8PSK.
  - Uses `driver/usb_serial_jtag.h` for RX command input.
- `main/app_main.c`
  - RX tone initialized to `1500`.
  - Decoded output publish logic factored into `publish_decoded_text()`.
  - Audio decode feed is chunked into 64-sample pieces with `vTaskDelay(1)` between chunks.
- `components/psk_rtty/psk_rtty.c`
  - Added ESP-only cooperative yield in high-state-count Viterbi loops.
  - This avoids 8PSK-F watchdog resets seen earlier.
  - It is in local component code, not `managed_components`.
- `tools/hw_wav_test.py`
  - New hardware regression helper.
  - Sends `@MODE` to board, creates a temporary clipped WAV segment, plays it through Windows audio, captures serial decoded text, and computes text-level BER/CER.
  - Supports `--mode`, `--all-bpsk-qpsk`, `--all-psk`, `--max-seconds`, `--gain`, `--offset`.

## Build / Flash Status

- Last `idf.py build`: PASS.
- Last `idf.py -p COM14 flash`: PASS.
- Last flashed firmware includes:
  - 8PSK/multi-carrier enabled.
  - 64-sample audio chunking.
  - Viterbi cooperative yield.
  - debug logs disabled.

## Hardware Test Method

Typical command:

```powershell
C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe tools\hw_wav_test.py --port COM14 --max-seconds 25 --gain 0.25 --min-fragment 30 --mode BPSK125
```

For slower/FEC modes, use longer clips:

```powershell
C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe tools\hw_wav_test.py --port COM14 --max-seconds 120 --gain 0.25 --min-fragment 30 --mode BPSK63F --mode QPSK31
```

Note: The reported BER/CER is text-level comparison between decoded serial text and `input.txt`, not raw demodulator bit-decision BER.

## BPSK / QPSK Hardware Results

Test gain was `0.25`.

| Mode | Result | Clip | BER | CER | Notes |
|---|---:|---:|---:|---:|---|
| BPSK31 | PASS | 60s | 0.69% | 1.72% | OK |
| BPSK63 | PASS | 60s | 1.43% | 3.69% | OK |
| BPSK63F | PASS | 120s | 0.00% | 0.00% | Needs longer lock/FEC delay; 60s produced no output |
| BPSK125 | PASS | 25s | 5.96% | 16.08% | Passes, with preamble/acquisition garbage |
| BPSK250 | PASS | 25s | 2.39% | 6.30% | OK |
| BPSK500 | PASS | 25s | 1.17% | 3.24% | OK |
| BPSK1000 | FAIL | 60s | 32.18% | 93.00% | Lots of wrong text; does not find expected fragment |
| QPSK31 | PASS | 120s | 0.70% | 1.59% | Needs longer lock/FEC delay; 60s produced no output |
| QPSK63 | PASS | 60s | 1.21% | 3.88% | OK |
| QPSK125 | PASS | 25s | 1.39% | 3.38% | OK |
| QPSK250 | PASS | 25s | 2.90% | 7.71% | OK |
| QPSK500 | PASS | 25s | 1.70% | 4.61% | OK |

Python algorithm-side regression for these 12 BPSK/QPSK WAV files passed all modes, including BPSK1000:

```powershell
python tools\wav_decode_regression.py --wav-dir C:\projects\psk-script\generated-wav-48000 --input C:\projects\psk-script\input.txt --mode BPSK31 --mode BPSK63 --mode BPSK63F --mode BPSK125 --mode BPSK250 --mode BPSK500 --mode BPSK1000 --mode QPSK31 --mode QPSK63 --mode QPSK125 --mode QPSK250 --mode QPSK500
```

Conclusion: BPSK1000 failure is likely in board audio capture / sample-rate / realtime chain, not the WAV itself.

## Multi-Carrier / 8PSK Hardware Results

First full group run used `--max-seconds 25 --gain 0.25 --min-fragment 30`.

| Mode | Result | Clip | BER | CER | Notes |
|---|---:|---:|---:|---:|---|
| 12xPSK125 | FAIL | 25s | 31.74% | 91.44% | No expected fragment |
| 6xPSK250 | PASS | 25s | 31.80% | 91.25% | Fragment found, but aggregate error is very high |
| 2xPSK500 | PASS | 25s | 30.05% | 85.47% | Fragment found, but aggregate error is very high |
| 4xPSK500 | FAIL | 25s | 32.47% | 91.62% | No expected fragment |
| 2xPSK800 | FAIL | 25s | 32.39% | 92.60% | No expected fragment |
| 2xPSK1000 | FAIL | 25s | 33.61% | 93.68% | No expected fragment |
| 8PSK125 | PASS | 25s | 18.35% | 52.73% | Fragment found, but error is high |
| 8PSK250 | PASS | 25s | 27.92% | 79.79% | Fragment found, but error is high |
| 8PSK500 | PASS | 25s | 31.95% | 91.94% | Fragment found, but error is very high |
| 8PSK1000 | FAIL | 25s | 33.58% | 92.85% | No expected fragment |
| 8PSK125F | FAIL | 25s | 38.98% | 97.96% | Before Viterbi yield, triggered task WDT |
| 8PSK250F | FAIL | 25s | 38.48% | 97.59% | Before Viterbi yield, captured WDT backtrace text |
| 8PSK500F | FAIL | 25s | 33.45% | 92.84% | No expected fragment |
| 8PSK1000F | FAIL | 25s | 33.94% | 93.16% | No expected fragment |
| 8PSK1200F | FAIL | 25s | 33.69% | 93.02% | No expected fragment |

After adding Viterbi cooperative yield and reflashing:

| Mode | Result | Clip | BER | CER | Notes |
|---|---:|---:|---:|---:|---|
| 8PSK125F | FAIL | 25s | 100.00% | 100.00% | No WDT; no output |
| 8PSK250F | FAIL | 25s | 100.00% | 100.00% | No WDT; no output |
| 8PSK125F | FAIL | 60s | 100.00% | 100.00% | No WDT; no output |
| 8PSK250F | FAIL | 60s | 30.46% | 90.78% | No WDT; wrong text |
| 8PSK500F | FAIL | 60s | 33.58% | 94.07% | No WDT; wrong text |
| 8PSK1000F | FAIL | 60s | 34.18% | 93.95% | No WDT; wrong text |
| 8PSK1200F | FAIL | 60s | 34.22% | 94.04% | No WDT; wrong text |

Conclusion: Viterbi yield fixed the watchdog symptom, but 8PSK-F modes still do not decode correctly on hardware.

## Host / Python Regression Baseline

BPSK/QPSK all pass in Python regression.

Multi-carrier and non-FEC 8PSK Python regression:

```powershell
python tools\wav_decode_regression.py --wav-dir C:\projects\psk-script\generated-wav-48000 --input C:\projects\psk-script\input.txt --mode 12xPSK125 --mode 6xPSK250 --mode 2xPSK500 --mode 4xPSK500 --mode 2xPSK800 --mode 2xPSK1000 --mode 8PSK125 --mode 8PSK250 --mode 8PSK500 --mode 8PSK1000
```

Result:

- PASS: `2xPSK500`, `2xPSK800`, `8PSK125`, `8PSK250`, `8PSK500`, `8PSK1000`
- FAIL: `12xPSK125`, `6xPSK250`, `4xPSK500`, `2xPSK1000`

The host/Python 8PSK-F comparison was started but interrupted by the user. A residual Python process was killed afterward.

## Known Issues

1. `BPSK1000` fails on hardware but passes Python WAV regression.
   - Likely board audio input / sample-rate / real-time chain issue.
2. 8PSK-F modes no longer trigger WDT after cooperative yield, but they still fail decoding.
3. Some multi-carrier modes fail even in Python regression, so those failures should not be diagnosed only from hardware.
4. The text-level BER/CER numbers include acquisition/preamble garbage. PASS/FAIL is based on whether an expected fragment is present.
5. If diagnostic logs are re-enabled, hardware text regression becomes polluted unless `strip_logs()` is updated or decoded output uses a separate channel.

## Suggested Next Steps

1. Add an audio capture dump path on ESP32-S3:
   - Capture post-decimation samples from `audio_input_read()`.
   - Save/stream them to PC.
   - Replay captured samples into host decoder to separate analog frontend errors from decoder errors.
2. For `BPSK1000`, compare:
   - generated WAV -> Python decoder
   - generated WAV -> board analog input -> captured samples -> host decoder
   - live board decode
3. For 8PSK-F:
   - Keep the cooperative yield.
   - Check FEC deinterleaver/phase search against fldigi/HPM6750 behavior.
   - Run host/Python 8PSK-F baseline again without interruption.
4. Consider adding a test-only serial command to report audio RMS/clip counters on demand instead of periodic logs.
5. If automated board tests are continued, keep `BOARD_AUDIO_DEBUG_LOGS` and `BOARD_MODEM_DEBUG_LOGS` disabled.

## Useful Commands

Build:

```powershell
C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1'; Set-Location 'C:\projects\esp32s3-psk-modem'; idf.py build }"
```

Flash:

```powershell
C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1'; Set-Location 'C:\projects\esp32s3-psk-modem'; idf.py -p COM14 flash }"
```

Hardware test one mode:

```powershell
C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe tools\hw_wav_test.py --port COM14 --max-seconds 25 --gain 0.25 --min-fragment 30 --mode 8PSK125
```

Hardware test all PSK modes:

```powershell
C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe tools\hw_wav_test.py --port COM14 --all-psk --max-seconds 25 --gain 0.25 --min-fragment 30
```

Check for leftover Python processes:

```powershell
Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'python*' } | Select-Object ProcessId,CommandLine
```
