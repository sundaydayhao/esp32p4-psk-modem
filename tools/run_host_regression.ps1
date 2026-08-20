# Runs the real shipped decoder (via build/host-tools/host_psk_rtty_wav.exe)
# against a WAV corpus. The corpus is an input stimulus, not protocol ground
# truth; use multiple corpora when changing symbol mapping or FEC behavior.
param(
    [string]$Exe = "$PSScriptRoot\..\build\host-tools\host_psk_rtty_wav.exe",
    [string]$WavDir = "C:\projects\psk-script\generated-wav",
    [string]$Expected = "C:\projects\psk-script\input.txt"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) {
    Write-Error "host_psk_rtty_wav.exe not found at $Exe - run build_host_tools.ps1 first"
    exit 1
}

Write-Output "EXE=$Exe"
Write-Output "WAV_DIR=$WavDir"
Write-Output "EXPECTED=$Expected"
Write-Output ""

# Mode name -> WAV file base name (matches parse_mode() in tools/host_psk_rtty_wav.c)
$modes = @(
    "BPSK31", "BPSK63", "BPSK63F", "BPSK125", "BPSK250", "BPSK500", "BPSK1000",
    "12xPSK125", "6xPSK250", "2xPSK500", "4xPSK500", "2xPSK800", "2xPSK1000",
    "QPSK31", "QPSK63", "QPSK125", "QPSK250", "QPSK500",
    "8PSK125", "8PSK250", "8PSK500", "8PSK1000",
    "8PSK125F", "8PSK250F", "8PSK500F", "8PSK1000F", "8PSK1200F"
)

$results = @()
foreach ($mode in $modes) {
    $wav = Join-Path $WavDir "$mode.wav"
    if (-not (Test-Path $wav)) {
        $results += [pscustomobject]@{ Mode = $mode; Result = "NO-WAV"; Detail = $wav }
        continue
    }
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = & $Exe $mode $wav $Expected 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $prevEap

    $summaryLine = ($output | Select-String -Pattern "^\S+ decoded=" | Select-Object -First 1)
    $status = if ($exitCode -eq 0) { "PASS" } else { "FAIL" }
    $results += [pscustomobject]@{
        Mode   = $mode
        Result = $status
        Detail = if ($summaryLine) { $summaryLine.Line } else { ($output | Select-Object -First 1) }
    }
}

$results | Format-Table -AutoSize -Wrap
$passCount = @($results | Where-Object { $_.Result -eq "PASS" }).Count
$failCount = @($results | Where-Object { $_.Result -eq "FAIL" }).Count
$noWavCount = @($results | Where-Object { $_.Result -eq "NO-WAV" }).Count
Write-Output ""
Write-Output "PASS=$passCount FAIL=$failCount NO-WAV=$noWavCount TOTAL=$($results.Count)"
if ($failCount -ne 0 -or $noWavCount -ne 0) {
    exit 1
}
