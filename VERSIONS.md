# Versionsverwaltung – Eufy 15C

Zwei unabhängige Versionsreihen:

- ESP32-Firmware: `eufy-wroom/VERSION`. Erster versionierter Stand `v0.1.0`. Die Versionsnummer wird in `PROJECT_VER`, `GET :8081/status` (Feld `firmware_version`), den BIN-Dateinamen und den GitHub-Artefaktnamen übernommen.
- Android-App: `android/app/build.gradle`. Neuer Stand `versionName '0.2.1'` / `versionCode 3`. Die Version steht in der App, dem APK-Dateinamen und dem Release-Tag `eufy-android-vX.Y.Z`.

Jede neu veröffentlichte Firmware bzw. APK muss ihre eigene Versionsnummer erhöhen (Patch: Korrektur, Minor: neue Funktionen, Major: inkompatible Änderung). App- und Firmware-Version dürfen unterschiedlich sein.

GitHub-Build und `dist/source/build-versions.txt` dokumentieren die Commit-SHA. Kompiliert bedeutet nicht auf Hardware getestet.

WLAN: Keine AP-Zugangsdaten in öffentliche GitHub-Releases aufnehmen. Das Build-ZIP enthält eine separate `wifi-credentials.txt` für den Übergang zum neuen Firmwarestand. Bei der ersten AP+STA-Installation kann sich das AP-Passwort ändern.
