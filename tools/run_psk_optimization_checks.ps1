param([string]$Python = 'C:/Espressif/tools/python/v5.5.5/venv/Scripts/python.exe')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$gcc = 'C:/cygwin64/bin/gcc.exe'
$env:PATH = 'C:/cygwin64/bin;' + $env:PATH
$out = Join-Path $root 'build/host-tools'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$common = @('-std=c11', '-O3', '-Wall', '-Wextra', '-I', 'main', '-I', 'components/psk_rtty/include')
$receiver = @('main/psk_rx.c', 'main/psk_modes.c', 'main/psk_codec.c', 'components/psk_rtty/psk_rtty.c')
Push-Location $root
try {
    & $gcc @common tools/host_viterbi_check.c main/psk_codec.c -lm -o "$out/host_viterbi_check.exe"
    if ($LASTEXITCODE) { throw 'Viterbi oracle build failed' }
    & "$out/host_viterbi_check.exe"
    if ($LASTEXITCODE) { throw 'Viterbi/deinterleaver oracle failed' }

    & $gcc @common -DPAYLOAD_BYTES=512 tools/host_rate_check.c main/audio_decim.c main/psk_tx.c @receiver -lm -o "$out/host_k16_check.exe"
    if ($LASTEXITCODE) { throw 'K16 test build failed' }
    foreach ($mode in @('8PSK125F', '8PSK250F')) {
        foreach ($drops in @(0, 1)) {
            & "$out/host_k16_check.exe" $mode 0 $drops
            if ($LASTEXITCODE) { throw "$mode payload/reacquisition check failed" }
        }
    }

    $fixture = 'tests/data/hpm_2x8psk1200f_adc.wav'
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $fixture).Hash.ToLowerInvariant()
    if ($hash -ne '505b8a3b7b508c0215745d9d3eebf75937906199de676488228a5108eaebfd22') { throw 'Physical fixture checksum mismatch' }
    & $gcc @common tools/host_replay_adc.c @receiver -lm -o "$out/host_replay_adc.exe"
    if ($LASTEXITCODE) { throw 'ADC replay build failed' }
    & "$out/host_replay_adc.exe" 2x8PSK1200F $fixture "$out/adc_replay.raw"
    if ($LASTEXITCODE) { throw 'ADC replay failed' }
    & $Python tools/check_adc_replay.py "$out/adc_replay.raw"
    if ($LASTEXITCODE) { throw 'Physical ADC payload differs' }

    $clockFixture = 'tests/data/hpm_2x8psk1200f_clock_adc.wav'
    $clockHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $clockFixture).Hash.ToLowerInvariant()
    if ($clockHash -ne 'ef4bb5cb28500fd3c82792c01d7645e435c9491245528f743f0a4495bd001ba1') { throw 'Clock fixture checksum mismatch' }
    & "$out/host_replay_adc.exe" 2x8PSK1200F $clockFixture "$out/clock_replay.raw"
    if ($LASTEXITCODE) { throw 'Clock ADC replay failed' }
    & $Python tools/check_adc_replay.py "$out/clock_replay.raw" --expected tests/data/hpm_2x8psk1200f_clock_expected.txt --frames 143
    if ($LASTEXITCODE) { throw 'Physical clock payload differs' }
} finally { Pop-Location }
