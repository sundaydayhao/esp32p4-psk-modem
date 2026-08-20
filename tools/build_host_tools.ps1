# Builds tools/host_psk_rtty_wav.exe using the MSVC host toolchain (not the ARM cross
# toolchain used by the ESP-IDF firmware build). This lets the real shipped decoder
# (components/psk_rtty/psk_rtty.c) run natively on Windows against WAV files, instead
# of relying on tools/wav_decode_regression.py's independent Python reimplementation.
param(
    [string]$VcVarsAll = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
    [string]$CygwinGcc = "C:\cygwin64\bin\gcc.exe",
    [string]$OutDir = "$PSScriptRoot\..\build\host-tools"
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outExe = Join-Path $OutDir "host_psk_rtty_wav.exe"
if (Test-Path $outExe) {
    Remove-Item -Force $outExe
}

$srcTool = Join-Path $root "tools\host_psk_rtty_wav.c"
$srcCore = Join-Path $root "components\psk_rtty\psk_rtty.c"
$srcDecim = Join-Path $root "main\audio_decim.c"
$incDir = Join-Path $root "components\psk_rtty\include"
$incDecim = Join-Path $root "main"

$built = $false
if (Test-Path $VcVarsAll) {
    $cmd = "call `"$VcVarsAll`" x64 >NUL 2>NUL && cl /nologo /std:c11 /O2 /MT /W3 /I `"$incDir`" /I `"$incDecim`" `"$srcTool`" `"$srcCore`" `"$srcDecim`" /Fe:`"$outExe`" /Fo:`"$OutDir\\`""
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $result = cmd.exe /c $cmd 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    Write-Output $result
    $built = ($exitCode -eq 0 -and (Test-Path $outExe))
}

if (-not $built -and (Test-Path $CygwinGcc)) {
    $env:PATH = "$(Split-Path -Parent $CygwinGcc);$env:PATH"
    & $CygwinGcc -std=c11 -O2 -Wall -Wextra `
        -I $incDir -I $incDecim `
        $srcTool $srcCore $srcDecim -lm -o $outExe
    if ($LASTEXITCODE -ne 0) {
        Write-Error "cygwin gcc build failed: $outExe not produced"
        exit 1
    }
    $built = Test-Path $outExe
}

if (-not $built) {
    Write-Error "build failed: no working MSVC or Cygwin gcc toolchain produced $outExe"
    exit 1
}
Write-Output "OK: $outExe"
