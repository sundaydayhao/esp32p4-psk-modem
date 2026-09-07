# Builds the matching Wi-Fi coprocessor image without modifying managed components.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. 'C:/Espressif/tools/Microsoft.v5.5.5.PowerShell_profile.ps1'
$source = Join-Path $root 'managed_components/espressif__esp_hosted'
if (!(Test-Path "$source/slave/CMakeLists.txt")) { throw 'Build the P4 project first to resolve ESP-Hosted.' }
$copy = Join-Path $root 'build/c6-source/esp_hosted'
New-Item -ItemType Directory -Force -Path $copy | Out-Null
Get-ChildItem -LiteralPath $source | Copy-Item -Destination $copy -Recurse -Force
$defaults = Join-Path $root 'build/c6-source/sdkconfig.board'
@'
CONFIG_IDF_TARGET="esp32c6"
CONFIG_ESP_HOST_DEV_BOARD_P4_FUNC_BOARD=y
CONFIG_ESP_SDIO_HOST_INTERFACE=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
'@ | Set-Content -Encoding ascii -LiteralPath $defaults
$config = Join-Path $root 'build/c6-source/sdkconfig'
$buildDir = Join-Path $root 'build/c6'
Push-Location "$copy/slave"
try {
    idf.py -B $buildDir -D IDF_TARGET=esp32c6 -D "SDKCONFIG=$config" -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32c6;$defaults" build
    if ($LASTEXITCODE) { throw 'C6 firmware build failed' }
} finally { Pop-Location }
