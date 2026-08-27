# Builds tools/host_psk_rtty_wav.exe using the MSVC host toolchain (not the ARM cross
# toolchain used by the ESP-IDF firmware build). This lets the real shipped PSK
# modem code run natively on Windows against WAV files.
param(
    [string]$VcVarsAll = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
    [string]$CygwinGcc = "C:\cygwin64\bin\gcc.exe",
    [string]$OutDir = "$PSScriptRoot\..\build\host-tools"
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outExe = Join-Path $OutDir "host_psk_rtty_wav.exe"
$roundtripExe = Join-Path $OutDir "host_tx_roundtrip.exe"
foreach ($exe in @($outExe, $roundtripExe)) {
    if (Test-Path $exe) {
        Remove-Item -Force $exe
    }
}

$srcTool = Join-Path $root "tools\host_psk_rtty_wav.c"
$srcCore = Join-Path $root "components\psk_rtty\psk_rtty.c"
$srcDecim = Join-Path $root "main\audio_decim.c"
$srcRx = Join-Path $root "main\psk_rx.c"
$srcModes = Join-Path $root "main\psk_modes.c"
$srcCodec = Join-Path $root "main\psk_codec.c"
$srcTx = Join-Path $root "main\psk_tx.c"
$srcRoundtrip = Join-Path $root "tools\host_tx_roundtrip.c"
$incDir = Join-Path $root "components\psk_rtty\include"
$incDecim = Join-Path $root "main"

$built = $false
if (Test-Path $VcVarsAll) {
    $cmd = "call `"$VcVarsAll`" x64 >NUL 2>NUL && cl /nologo /std:c11 /O2 /MT /W3 /I `"$incDir`" /I `"$incDecim`" `"$srcTool`" `"$srcCore`" `"$srcDecim`" `"$srcRx`" `"$srcModes`" `"$srcCodec`" /Fe:`"$outExe`" /Fo:`"$OutDir\\`" && cl /nologo /std:c11 /O2 /MT /W3 /I `"$incDir`" /I `"$incDecim`" `"$srcRoundtrip`" `"$srcCore`" `"$srcDecim`" `"$srcRx`" `"$srcModes`" `"$srcCodec`" `"$srcTx`" /Fe:`"$roundtripExe`" /Fo:`"$OutDir\\`""
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $result = cmd.exe /c $cmd 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    Write-Output $result
    $built = ($exitCode -eq 0 -and (Test-Path $outExe) -and (Test-Path $roundtripExe))
}

if (-not $built -and (Test-Path $CygwinGcc)) {
    $env:PATH = "$(Split-Path -Parent $CygwinGcc);$env:PATH"
    & $CygwinGcc -std=c11 -O2 -Wall -Wextra `
        -I $incDir -I $incDecim `
        $srcTool $srcCore $srcDecim $srcRx $srcModes $srcCodec -lm -o $outExe
    if ($LASTEXITCODE -ne 0) {
        Write-Error "cygwin gcc build failed: $outExe not produced"
        exit 1
    }
    & $CygwinGcc -std=c11 -O2 -Wall -Wextra `
        -I $incDir -I $incDecim `
        $srcRoundtrip $srcCore $srcDecim $srcRx $srcModes $srcCodec $srcTx -lm -o $roundtripExe
    if ($LASTEXITCODE -ne 0) {
        Write-Error "cygwin gcc build failed: $roundtripExe not produced"
        exit 1
    }
    $built = (Test-Path $outExe) -and (Test-Path $roundtripExe)
}

if (-not $built) {
    Write-Error "build failed: no working MSVC or Cygwin gcc toolchain produced host tools"
    exit 1
}
Write-Output "OK: $outExe"
Write-Output "OK: $roundtripExe"
