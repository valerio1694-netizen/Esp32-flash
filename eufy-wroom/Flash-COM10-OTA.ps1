# Flash ESP32-WROOM only; never GD32. First install OTA layout via USB once.
param([string]$Port = 'COM10', [int]$Baud = 460800)
$ErrorActionPreference = 'Stop'
$full = Join-Path $PSScriptRoot 'Eufy-WROOM-OTA-full.bin'
if (!(Test-Path $full)) { throw "Vollimage fehlt: $full" }
$expected = Get-Content (Join-Path $PSScriptRoot 'SHA256SUMS') |
  Where-Object { $_ -match 'Eufy-WROOM-OTA-full.bin' } |
  Select-Object -First 1
if (!$expected) { throw 'SHA256SUMS Eintrag fehlt' }
$hash = (Get-FileHash $full -Algorithm SHA256).Hash.ToLowerInvariant()
if ($hash -ne ($expected -split '\s+')[0].ToLowerInvariant()) { throw 'SHA256 Fehler' }
Write-Host "ESP32 WROOM $Port: OTA-Partitionswechsel erfordert EINMAL Vollflash 0x0."
Write-Host "WICHTIG: externes 3.3V am ESP trennen, ESP nur per USB versorgen."
Write-Host "GD32 wird dabei nicht programmiert."
$python = if (Get-Command py -ErrorAction SilentlyContinue) { 'py' } else { 'python' }
$args = if ($python -eq 'py') { @('-3') } else { @() }
& $python @args -m esptool --chip esp32 --port $Port --baud $Baud write-flash 0x0 $full
if ($LASTEXITCODE -ne 0) { throw "ESP Flashfehler: $LASTEXITCODE" }
Write-Host "Fertig. AP-Passwort/OTA-Token in wifi-credentials.txt."
Write-Host "Telemetrie: http://192.168.4.1:8080/telemetry"
Write-Host "OTA-Status: http://192.168.4.1:8081/status"
