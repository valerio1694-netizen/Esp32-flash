# Eufy RoboVac 15C – ESP32 SWD-Telemetrie + OTA + Android

## Status
Quellcode: ESP32-WROOM-32, Read-only SWD-Backend über GD32F103VCT6. 
ESP32-Firmwarebuild per GitHub Actions; Android-APK separat.
Die Firmware ist NICHT am echten Gerät validiert.

## ESP Erstinstallation
Im GitHub Actions Artifact `Eufy-WROOM-OTA-TELEMETRY` die Dateien entpacken.
- Externe ESP32-Versorgung von Eufy-3,3V trennen; ESP per USB an Laptop.
- PowerShell `./Flash-COM10-OTA.ps1 -Port COM10`
- Danach USB entfernen und mit der vorgesehenen Versorgung verbinden.
Dies programmiert ausschließlich den ESP32. Es schreibt nicht in den GD32.

Eine bereits installierte ältere Partitionstabelle kann nicht durch reines App-only Flashen
OTA-fähig werden. Für den Partitionswechsel ist die obige einmalige Vollinstallation
über USB mit `Eufy-WROOM-OTA-full.bin` erforderlich.

## WLAN/Ports
Default AP Eufy-SWD-WROOM, IP `192.168.4.1`, AP-Passwort in `wifi-credentials.txt`
(je Build neu erstellt). Ein anderer Betriebsmodus kann eine andere IP haben.
- Port 4441: OpenOCD CMSIS-DAP TCP
- Port 4442: UART-Brücke (keine zweite Sendeleitung zum OEM-WLAN verdrahten)
- Port 8080: `GET /telemetry` liefert gesamtes bekanntes SWD-Rohdaten-JSON
- Port 8081: `GET /status`, `POST /upload` für ESP32-OTA

OTA-Upload muss den HTTP-Header `X-OTA-Token` mit dem AP-Passwort enthalten.
Lade ausschließlich `Eufy-WROOM-OTA-app-only.bin` aus einem kompatiblen Build.
Beim OTA wird die inaktive App-Partition geschrieben und die neue App gebootet.
Die Android-App kann den Upload durchführen; OTA wird erst nach initialem USB-Flash verfügbar.
HTTP/OTA innerhalb des geschützten APs, kein TLS. Keine Portweiterleitung ins Internet.

## Messwerte
Bekannte Werte: Encoder, Motorströme, Motor-/Lüfter-/Bürsten-PWM, Akku/ADC,
Lade-ADC, Hindernis-/Absturz-ADC, Bürsten-ADC, Roboterstatus, Betriebsmodi,
Lauf-/Dockzeiten, Wand-Zähler, Lüfterlevel, Flags, Firmware-Diagnostik 202–219
soweit direkte RAM/Peripherie-Adressen sicher aus Disassembly ableitbar sind.
`diagnostics.valid=false`: erweiterte Feldwerte nicht aktuell.
`gpio_inputs` gibt ausschließlich rohe GPIO-Portzustände an. 
Bumper-Zuordnung ist **noch nicht belegt** und wird explizit als
`bumper.mapped=false` ausgegeben. Nicht nachgewiesene Einheiten werden nicht erfunden.
Status der OEM-Firmware ist 1.1.8. Sicherheitsregel: kein Zielhalt/Schreiben/Reset durch
Telemetrie; bei OpenOCD-Verbindung pausiert der Sampler.

## Android
Native Java-Anwendung, alle neuen/alten JSON-Felder werden rekursiv angezeigt.
Einstellbare ESP-Adresse; Liveaktualisierung; BIN-Datei über Dokumentauswahl
für OTA einspielen; AP-Passwort wird dafür eingegeben und nicht gespeichert.
APK im GitHub-Actions-Artefakt `Eufy-Android-APK`.
