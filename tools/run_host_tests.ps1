# Runs the host WAV regression against the corpus in C:\projects\psk-script.
param(
    [string]$WavDir = "C:\projects\psk-script\generated-wav-48000",
    [string]$Expected = "C:\projects\psk-script\input.txt",
    [int]$MinFragment = 4
)

$ErrorActionPreference = "Stop"

& python (Join-Path $PSScriptRoot "wav_decode_regression.py") `
    --project (Resolve-Path "$PSScriptRoot\..") `
    --wav-dir $WavDir `
    --input $Expected `
    --min-fragment $MinFragment
exit $LASTEXITCODE
