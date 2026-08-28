param(
    [string]$Exe = "$PSScriptRoot\..\build\host-tools\host_tx_roundtrip.exe",
    [string]$Text = "One warm and sunny spring morning, Lulu hopped into the green forest to help a tiny bird return to its nest. True bravery is choosing kindness even when you are scared."
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Error "host_tx_roundtrip.exe not found at $Exe - run build_host_tools.ps1 first"
    exit 1
}

& $Exe $Text
exit $LASTEXITCODE
