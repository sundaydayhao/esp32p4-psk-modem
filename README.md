# ESP32-P4 PSK Modem

ESP-IDF PSK modem receiver for the Espressif ESP32-P4 Function EV Board.

The demodulator core and mode table are ported from `C:\projects\hpm6750-psk-modem`.
The IDF project layout and audio task style follow `C:\projects\esp32s3-psk-modem`,
with the audio backend changed to the ESP32-P4 Function EV Board microphone codec
through the official BSP and `esp_codec_dev`.

## Build

Open the ESP-IDF PowerShell environment, then build:

```powershell
cd C:\projects\esp32p4-psk-modem
idf.py set-target esp32p4
idf.py build
```

The IDF environment command supplied for this machine is:

```powershell
C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe -NoExit -ExecutionPolicy Bypass -NoProfile -Command "& {. 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1'}"
```

## Runtime

Decoded text is printed to the serial console as `[rx] ...`.

Console commands:

- `HELP`
- `STATUS`
- `MODE BPSK31`
- `MODE BPSK63`
- `MODE BPSK125`
- `MODE QPSK31`
- `TONE 1500`
- `AFC 0` / `AFC 1`
- `SQL 0` / `SQL 1`
- `LOG TEXT` / `LOG HEX`
