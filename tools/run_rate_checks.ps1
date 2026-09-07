param([switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$gcc = 'C:/cygwin64/bin/gcc.exe'
$env:PATH = 'C:/cygwin64/bin;' + $env:PATH
$outDir = Join-Path $root 'build/host-tools'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Push-Location $root
try {
    & $gcc -std=c11 -O2 -Wall -Wextra -I main tools/host_rx_history.c main/rx_history.c -o "$outDir/host_rx_history.exe"
    if ($LASTEXITCODE) { throw 'History test build failed' }
    & $gcc -std=c11 -O2 -Wall -Wextra -I main -I components/psk_rtty/include tools/host_rate_check.c main/audio_decim.c main/psk_rx.c main/psk_modes.c main/psk_codec.c main/psk_tx.c components/psk_rtty/psk_rtty.c -lm -o "$outDir/host_rate_check.exe"
    if ($LASTEXITCODE) { throw 'Rate test build failed' }
    if (!$BuildOnly) {
        & "$outDir/host_rx_history.exe"
        if ($LASTEXITCODE) { throw 'History tests failed' }
        & "$outDir/host_rate_check.exe"
        if ($LASTEXITCODE) { throw 'Some rate checks failed; see individual results' }
    }
} finally { Pop-Location }
