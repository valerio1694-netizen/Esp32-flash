param([string]$Port = "COM10")
$ErrorActionPreference = "Stop"
$bin = Join-Path $PSScriptRoot "Eufy-WROOM-TELEMETRY-app-only.bin"
if (-not (Test-Path $bin)) { throw "Firmwaredatei fehlt: $bin" }
Write-Host "WICHTIG: ESP32 von Eufy-3,3V trennen, dann NUR ESP32 per USB anschliessen."
Write-Host "Flash: ESP32 $Port, App 0x10000; kein Erase und kein GD32-Zugriff."
$py = Get-Command python -ErrorAction Stop
& $py.Source -m esptool --chip esp32 --port $Port --baud 460800 write-flash 0x10000 $bin
if ($LASTEXITCODE -ne 0) { throw "Flash fehlgeschlagen (Exitcode $LASTEXITCODE)" }
Write-Host "ESP32-App geflasht. USB entfernen, erst danach Eufy-3,3V wieder verbinden."
Write-Host "Telemetrie nach WLAN-Verbindung: http://192.168.178.177:8080/telemetry"
