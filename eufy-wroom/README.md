# Eufy RoboVac 15C – ESP32-WROOM SWD über WLAN

Getrennter Build des [cmsis_dap_tcp_esp32](https://github.com/bkuschak/cmsis_dap_tcp_esp32)-Projekts für das **klassische ESP32-WROOM-32 Dev-Board**. Das originale Eufy-TYWE1S-Modul bleibt eingebaut; die originale App wird nicht ersetzt.

## Pinbelegung
| ESP32-WROOM | GD32F103VCT6 | Signal |
|---|---|---|
| GPIO18 (D18) | PA14 | SWCLK |
| GPIO19 (D19) | PA13 | SWDIO |
| GND | GND | gemeinsame Masse |

Den ESP32 separat per USB versorgen. Keine Verbindung zwischen den 3,3-V-Ausgängen herstellen.

UART1-TCP-Brücke ist auf GPIO17 (TX) und GPIO16 (RX), TCP-Port 4442, konfiguriert, wird aber zunächst **nicht** zum GD32 verdrahtet: Der originale TYWE1S kommuniziert bereits über UART, und ein zweiter Sender würde einen elektrischen Konflikt verursachen.

## WLAN

Das ESP32-Board erstellt den WLAN-AP **Eufy-SWD-WROOM** (192.168.4.1). Das WPA2-Passwort wird beim Build neu generiert und im ZIP-Artefakt unter `wifi-credentials.txt` abgelegt. CMSIS-DAP TCP läuft auf Port 4441. Der TCP-Dienst hat selbst keine zusätzliche Anmeldung.

## Flashen

Die Datei `Eufy-WROOM-SWD-full.bin` mit `esptool --chip esp32 write-flash 0x0 Eufy-WROOM-SWD-full.bin` auf den **ESP32** schreiben (nicht auf den GD32). Vor erstem Einsatz: WLAN und SWD bei stehendem Roboter testen.

## OpenOCD

Es wird ein OpenOCD-Build benötigt, der den Netzwerk-Backend-Befehl `cmsis-dap backend tcp` kennt. Minimale Schnittstellen-Konfiguration:

```tcl
adapter driver cmsis-dap
cmsis-dap backend tcp
cmsis-dap tcp host 192.168.4.1
cmsis-dap tcp port 4441
transport select swd
adapter speed 100
reset_config none
```

Der ESP32 wird **zusätzlich** zum Original-WLAN-Modul betrieben. Die Eufy-App bleibt verfügbar, solange wir den GD32 nicht anhalten oder zurücksetzen.

Build- und SWD-Tests auf der physischen Hardware stehen noch aus.