param(
    [Parameter(Mandatory = $true)][string]$Port,
    [ValidateSet('P4', 'C6')][string]$Chip = 'P4'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildDir = if ($Chip -eq 'C6') { Join-Path $root 'build/c6' } else { Join-Path $root 'build' }
$targetChip = if ($Chip -eq 'C6') { 'esp32c6' } else { 'esp32p4' }
if (!(Test-Path "$buildDir/flash_args")) { throw "Build $Chip firmware first." }
if ($Port -notin [System.IO.Ports.SerialPort]::GetPortNames()) { throw "Port $Port is not connected." }
. 'C:/Espressif/tools/Microsoft.v5.5.5.PowerShell_profile.ps1'
Push-Location $buildDir
try {
    # Explicit chip and port; esptool checks identity. Never use --force.
    python -m esptool --chip $targetChip -p $Port -b 460800 --before default_reset --after hard_reset write_flash '@flash_args'
    if ($LASTEXITCODE) { throw "$Chip flash failed" }
} finally { Pop-Location }
