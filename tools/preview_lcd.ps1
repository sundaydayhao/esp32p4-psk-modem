param(
    [string]$CygwinGcc = 'C:/cygwin64/bin/gcc.exe',
    [string]$Ninja = 'C:/Espressif/tools/ninja/1.12.1/ninja.exe',
    [string]$Python = 'C:/Espressif/tools/python/v5.5.5/venv/Scripts/python.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'build/lcd-preview'
$env:PATH = "$(Split-Path -Parent $CygwinGcc);$env:PATH"
cmake -S "$PSScriptRoot/lcd_preview" -B $out -G Ninja "-DCMAKE_C_COMPILER=$CygwinGcc" "-DCMAKE_MAKE_PROGRAM=$Ninja"
if ($LASTEXITCODE) { throw 'LCD preview configure failed' }
cmake --build $out -j 8
if ($LASTEXITCODE) { throw 'LCD preview build failed' }
Push-Location $out
try {
    & ./lcd_preview.exe
    if ($LASTEXITCODE) { throw 'LCD interaction checks failed' }
    & $Python "$PSScriptRoot/lcd_preview/ppm_to_png.py" $out
    if ($LASTEXITCODE) { throw 'LCD image export failed' }
} finally { Pop-Location }
Write-Output "LCD previews: $out"
