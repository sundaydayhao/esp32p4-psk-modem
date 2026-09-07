$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$gcc = 'C:/cygwin64/bin/gcc.exe'
$env:PATH = 'C:/cygwin64/bin;' + $env:PATH
$out = Join-Path $root 'build/host-tools'
New-Item -ItemType Directory -Force -Path $out | Out-Null
Push-Location $root
try {
    & $gcc -std=c11 -O2 -Wall -Wextra -I main -I components/psk_rtty/include tools/host_content_check.c main/modem_image.c main/voice_protocol.c main/psk_rx.c main/psk_modes.c main/psk_codec.c main/psk_tx.c components/psk_rtty/psk_rtty.c -lm -o "$out/host_content_check.exe"
    if ($LASTEXITCODE) { throw 'Content test build failed' }
    & "$out/host_content_check.exe"
    if ($LASTEXITCODE) { throw 'Content protocol or PSK roundtrip failed' }
    & $gcc -std=c11 -O2 -Wall -Wextra -I main tools/host_ap580_playout.c main/ap580_playout.c -o "$out/host_ap580_playout.exe"
    if ($LASTEXITCODE) { throw 'Playout build failed' }
    & "$out/host_ap580_playout.exe"
    if ($LASTEXITCODE) { throw 'Playout checks failed' }
    & $gcc -std=c11 -O2 -Wall -Wextra -I main tools/host_uart_tx_pump.c main/uart_tx_pump.c main/ap580_playout.c -o "$out/host_uart_tx_pump.exe"
    if ($LASTEXITCODE) { throw 'UART pump build failed' }
    & "$out/host_uart_tx_pump.exe"
    if ($LASTEXITCODE) { throw 'UART pump checks failed' }
} finally { Pop-Location }
