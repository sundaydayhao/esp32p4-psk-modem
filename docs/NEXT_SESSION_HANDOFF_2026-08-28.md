# Next Session Handoff - ESP32-P4 PSK Modem

Date: 2026-08-28

Closeout update: 2026-08-28 evening

Workspace: `C:\projects\esp32p4-psk-modem`

Reference repos used:

- `C:\projects\fldigi`
- `C:\projects\hpm6750-psk-modem`

Hardware setup:

- HPM6750 TX is on `COM20` and is considered known-good.
- ESP32-P4 RX is on `COM15`.
- Current tested levels are `OUTVOL 255 127` on HPM TX and `INGAIN 20` on ESP RX.
- The user wired HPM left-channel audio plus GND into ESP input. This is reasonable for mono receive. Do not short L/R outputs together directly.

## Current Status

The current code builds on host and ESP32-P4. Host TX/RX roundtrip passes all PSK modes. The latest firmware was built and flashed to the ESP32-P4 on `COM15`.

This handoff reflects the code state being prepared for push after the HPM6750-to-ESP32P4 hardware BER debugging session. The push should include the source changes, the reusable HPM->ESP BER/spectrum test scripts, and the investigation/handoff documents. Large generated hardware logs and WAV/spectrum captures should remain local artifacts unless a future issue needs exact raw evidence.

The hardware issue is still not closed. The most recent evidence says:

- Basic PSK modes had previously been narrowed back to working over HPM->ESP hardware, including `BPSK31`, `BPSK125`, `QPSK31`, `QPSK125`, `8PSK125`, and `12xPSK125` in focused runs.
- `8PSK125F` can decode cleanly in some automatic-lock runs, but is still unstable across current code variants.
- `8PSK250F` remains the main failing K16 mode.
- `8PSK1200F` is not a hard failure, but remains around warning-level BER, roughly 6-8%.

Important: a fldigi-like "instant character-level branch voting" experiment was tried and was bad on hardware. Do not reintroduce it as-is.

## Latest Code State

K16 FEC branch handling:

- The previous experimental hard-coded K16 zero-preamble/known-branch lock was removed.
- The failed character-level immediate branch voting path was reverted.
- Dual-branch K16 output is back to a conservative rule: branches may run and collect metrics, but text is emitted only from a locked/selected branch.
- `FEC_K16_BRANCH_LOCK_VOTES` is currently `16`, matching the safer reference-style value.
- `8PSK125F` currently uses `acquire_symbols = 64`.
- Extra RX diagnostics were added: per-branch metrics, pair counts, vote candidate/count, output enabled, and locked branch.

Diagnostic serial commands:

- `RXDIAG` / `DIAG` prints one ESP log line with RX state, FEC metrics, branch pair counters, selected/locked branch, squelch, AFC, timing, and frame counters.
- `INGAIN <0..42>` changes ESP input gain.
- `FECBRANCH AUTO|0|1` was added as a diagnostic command intended to force K16 branch output, but the latest hardware logs show it is not yet reliably taking effect in the decoder.

The current `FECBRANCH` caveat matters: the UI parser logs receipt of the command, but `RXDIAG` still reports `locked=255` after `FECBRANCH 0` or `FECBRANCH 1` in the latest runs. Treat branch-forcing test results as invalid until the command is acknowledged from the audio/control side.

Other current changes:

- `audio_input_set_gain_db()` was added and wired to the `INGAIN` serial command.
- `transport_send()` uses USB Serial/JTAG directly on ESP so decoded text is emitted over the same test serial path.
- Host WAV decode now always runs through `audio_decim_process()` for 48 kHz input, even for decimation factor 1.
- `host_tx_roundtrip` now explicitly includes `8PSK125F` and `8PSK250F`.
- `hpm_esp_link_ber.py` was added for HPM TX -> ESP RX BER testing with optional `--rxdiag` and `--fec-branch`.
- `hpm_esp_capture_spectrum.py` was added for input capture/spectrum checks.

## Repository Closeout Notes

Base commit before this worktree snapshot: `6f72d10`.

Files intended to be committed/pushed:

- PSK decoder/runtime changes under `components/psk_rtty/`.
- ESP control-path changes under `main/`, including `RXDIAG`, `INGAIN`, `FECBRANCH`, USB Serial/JTAG decoded-text output, and RX debug snapshots.
- Host/test tooling updates under `tools/host_*`, `tools/hw_wav_test.py`, `tools/hpm_esp_link_ber.py`, and `tools/hpm_esp_capture_spectrum.py`.
- Documentation: this handoff plus `docs/QPSK_8PSK_BER_INVESTIGATION_2026-08-27.md` and `docs/8PSK_125F_250F_1200F_INVESTIGATION_2026-08-28.md`.

Local artifacts intentionally not intended for commit:

- `tools/hw-logs-*`
- `tools/spectrum-*`
- `tools/capture-*.wav`

## Latest Validation

Host tools build passed:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_host_tools.ps1
```

Host TX roundtrip passed all modes on the current code:

```powershell
build\host-tools\host_tx_roundtrip.exe
```

Result:

```text
summary: failures=0
```

ESP firmware build passed:

```powershell
$env:PATH='C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;' + $env:PATH
C:\Espressif\tools\ninja\1.12.1\ninja.exe -C build
```

Latest image result:

```text
psk_modem_p4.bin binary size 0xe8a20 bytes.
Smallest app partition is 0x400000 bytes.
0x3175e0 bytes (77%) free.
```

ESP flash passed:

```powershell
C:\Espressif\tools\ninja\1.12.1\ninja.exe -C build flash
```

Esptool still probes `COM20` first and fails because that is the HPM board, then flashes `COM15` successfully.

## Latest HPM->ESP Hardware Results

Test command shape:

```powershell
python tools\hpm_esp_link_ber.py --tx-port COM20 --rx-port COM15 --modes ... --outvol 255 127 --ingain 20 --min-timeout 60 --timeout-scale 1.5 --rxdiag --log-dir <dir>
```

Bad char-vote experiment, log dir `tools\hw-logs-hpm2esp-charvote-20260828`:

```text
8PSK125F   HIGH  BER=52.9762%  decoded=800
8PSK250F   HIGH  BER=45.4613%  decoded=550
8PSK1200F  WARN  BER= 7.6637%  decoded=24056
```

Useful diag from that run:

```text
8PSK125F:  lock=3 sq=1 fec=503/705 sel=1 locked=1 pairs=1024/8168 total=16337 vote=1/16 soft=0
8PSK250F:  lock=0 sq=0 fec=0/0   sel=0 locked=255 pairs=19/19 total=38 soft=18
8PSK1200F: lock=3 sq=1 fec=511/0 sel=0 locked=255 pairs=148738/107 total=148738 soft=3
```

Interpretation: instant branch selection by current metric polluted the output stream and must stay removed. The `8PSK250F` diag was captured after squelch closed, so it is not useful for final branch judgment.

Forced branch attempts after adding `FECBRANCH`, log dirs:

- `tools\hw-logs-hpm2esp-k16-branch1-persist-20260828`
- `tools\hw-logs-hpm2esp-k16-branch0-persist-20260828`

Results:

```text
FECBRANCH 1:
  8PSK125F HIGH, decoded=0, BER=100%
  8PSK250F HIGH, decoded=0, BER=100%

FECBRANCH 0:
  8PSK125F HIGH, decoded=0, BER=100%
  8PSK250F HIGH, decoded=0, BER=100%
```

These are not valid branch-quality conclusions because `RXDIAG` still showed `locked=255`:

```text
FECBRANCH 1 / 8PSK125F: locked=255 pairs=600/599 total=1199
FECBRANCH 1 / 8PSK250F: locked=255 pairs=1417/1417 total=2834
FECBRANCH 0 / 8PSK125F: locked=255 pairs=1282/1282 total=2564
FECBRANCH 0 / 8PSK250F: locked=255 pairs=1969/1969 total=3938
```

Next session should first fix or prove the `FECBRANCH` control path before using forced-branch results.

## Recommended Next Work

1. Make `FECBRANCH` observable from the audio/control side. Add an ESP log in `apply_control_event()` after `psk_rx_set_fec_branch_override()`, or change the command flow so the script waits for an applied acknowledgement before starting TX. Also consider increasing `s_control_queue` depth or checking `xQueueSend()` return values.

2. Re-run three focused K16 tests after `FECBRANCH` is confirmed:

```powershell
python tools\hpm_esp_link_ber.py --tx-port COM20 --rx-port COM15 --modes 8PSK125F,8PSK250F --outvol 255 127 --ingain 20 --min-timeout 60 --timeout-scale 1.5 --rxdiag --fec-branch AUTO --log-dir tools\hw-logs-next-auto
python tools\hpm_esp_link_ber.py --tx-port COM20 --rx-port COM15 --modes 8PSK125F,8PSK250F --outvol 255 127 --ingain 20 --min-timeout 60 --timeout-scale 1.5 --rxdiag --fec-branch 0 --log-dir tools\hw-logs-next-b0
python tools\hpm_esp_link_ber.py --tx-port COM20 --rx-port COM15 --modes 8PSK125F,8PSK250F --outvol 255 127 --ingain 20 --min-timeout 60 --timeout-scale 1.5 --rxdiag --fec-branch 1 --log-dir tools\hw-logs-next-b1
```

3. If `8PSK250F` is still bad after confirmed branch forcing, capture ESP input audio and decode it offline:

```powershell
python tools\hpm_esp_capture_spectrum.py --tx-port COM20 --rx-port COM15 --mode 8PSK250F --outvol 255 127 --ingain 20 --samples 65536 --delay 3 --out-dir tools\spectrum-hpm2esp-8psk250f-next
build\host-tools\host_psk_rtty_wav.exe 8PSK250F tools\spectrum-hpm2esp-8psk250f-next\8PSK250F.wav tools\short_regression_input.txt 40
```

4. After K16 is stable, tune `8PSK1200F` separately. It shares the K=7 punctured path with `8PSK1000F`; current evidence still points to high-baud timing/AFC/filter margin rather than a discrete Viterbi bug.

5. Run full HPM->ESP regression after focused fixes:

```powershell
python tools\hpm_esp_link_ber.py --tx-port COM20 --rx-port COM15 --outvol 255 127 --ingain 20 --min-timeout 60 --timeout-scale 1.5 --rxdiag --log-dir tools\hw-logs-hpm2esp-all-next
```

## Cautions

- Do not trust branch-forcing logs until `RXDIAG locked=` reflects the requested branch.
- Do not restore instant per-character branch voting; it produced 45-53% BER on hardware.
- Do not directly combine the HPM left and right audio outputs. Use one channel plus GND, or combine with resistors if a true mono mix is needed later.
- Generated hardware logs and WAV captures under `tools\hw-logs-*`, `tools\spectrum-*`, and `tools\capture-*.wav` are diagnostic artifacts; avoid committing them unless specifically needed.
