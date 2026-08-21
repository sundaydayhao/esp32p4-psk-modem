& { . 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1' }
Set-Location 'C:\projects\esp32p4-psk-modem'
idf.py build
exit $LASTEXITCODE
