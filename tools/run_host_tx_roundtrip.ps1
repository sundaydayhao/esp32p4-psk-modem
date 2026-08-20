param(
    [string]$Exe = "$PSScriptRoot\..\build\host-tools\host_tx_roundtrip.exe",
    [string]$Text = "THE QUICK BROWN FOX 0123456789 THE QUICK BROWN FOX 0123456789 ESP32P4 PSK MODEM LOOPBACK HOST TEST END"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Error "host_tx_roundtrip.exe not found at $Exe - run build_host_tools.ps1 first"
    exit 1
}

& $Exe $Text
exit $LASTEXITCODE
