# Eufy RoboVac 15C: EIN-KLICK PowerShell fuer unbeaufsichtigten WLAN-Wechsel
# Firmware-Download/Speicherung vor WLAN-Wechsel; danach OTA + Peng + Recovery.
# Start: powershell.exe -NoProfile -ExecutionPolicy Bypass -File "<Datei>.ps1"
# Keine Eingaben; KEINE Aenderung an der GD32-Firmware.
# WARNUNG: Beim Wechsel zu Eufy-SWD-WROOM bricht der WireGuard-/Remotezugriff ab.
# Eigenstaendige Task-Scheduler-Jobs arbeiten danach lokal weiter.
param(
    [ValidateSet('Start','Worker','Recovery')][string]$Mode='Start',
    [string]$HomePassword,
    [string]$EspPassword
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$HomeSSID = 'Peng'
$EspSSID = 'Eufy-SWD-WROOM'
$EspIP = '192.168.4.1'
$TaskWorker = 'EufyPeng-OneClick-Worker'
$TaskRecovery = 'EufyPeng-OneClick-Recovery'
$StateDir = Join-Path $env:LOCALAPPDATA 'EufyPengOneClick'
$SavedScript = Join-Path $StateDir 'Eufy-EinKlick-Peng-OTA.ps1'
$LogPath = Join-Path $env:USERPROFILE 'Downloads\Eufy-Peng-OTA.log'
$NewApPassword = $null
$ReleaseBase = 'https://github.com/valerio1694-netizen/Esp32-flash/releases/download/eufy-apsta-peng-20261118'
$FirmwarePath = Join-Path $StateDir 'Eufy-WROOM-OTA-app-only.bin'
$CredsPath = Join-Path $StateDir 'wifi-credentials.txt'
$LocalHomeKey = Join-Path $StateDir 'local-peng-secret.dpapi'
$LocalEspKey = Join-Path $StateDir 'local-esp-secret.dpapi'
$ExpectedSHA256 = '6685A7BD598F1B04802DA395B25D3C172F69EB94E74D6F5A3F1ACDA0099BE391'
$FirmwareLength = 909808

function Log([string]$msg) {
    $line = '[' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '] ' + $msg
    try { Add-Content -LiteralPath $LogPath -Value $line -Encoding UTF8 } catch {}
    Write-Output $line
}
# DPAPI protects secrets for THIS Windows user; scheduled tasks run under
# the same account. No credentials are committed to the public repository.
function Save-LocalSecret([string]$file,[string]$value) {
    if ([string]::IsNullOrEmpty($value) -or $value.Length -lt 8 -or $value.Length -gt 63) {
        throw 'WiFi password must be 8 to 63 characters.'
    }
    $encrypted=ConvertTo-SecureString $value -AsPlainText -Force | ConvertFrom-SecureString
    [IO.File]::WriteAllText($file,$encrypted,(New-Object System.Text.UTF8Encoding($false)))
}
function Read-LocalSecret([string]$file) {
    if (!(Test-Path -LiteralPath $file)) { return $null }
    $cipher=[IO.File]::ReadAllText($file)
    $secure=ConvertTo-SecureString $cipher
    $ptr=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}
function Get-SavedWLANKey([string]$name) {
    # No WLAN scan or location permission is required: read only a KNOWN saved profile.
    $folder = Join-Path $env:TEMP ('eufykey_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    try {
        & netsh.exe wlan export profile "name=$name" "folder=$folder" 'key=clear' 2>$null | Out-Null
        $xs = @(Get-ChildItem -LiteralPath $folder -Filter '*.xml' -File -ErrorAction SilentlyContinue)
        foreach ($file in $xs) {
            try {
                [xml]$xml = [IO.File]::ReadAllText($file.FullName)
                $ssidNode = $xml.SelectSingleNode('//*[local-name()="SSIDConfig"]/*[local-name()="SSID"]/*[local-name()="name"]')
                $keyNode = $xml.SelectSingleNode('//*[local-name()="sharedKey"]/*[local-name()="keyMaterial"]')
                if ($ssidNode -and $ssidNode.InnerText -eq $name -and $keyNode -and $keyNode.InnerText) {
                    return $keyNode.InnerText
                }
            } catch {}
        }
        # Alternative when WLAN profile export is unavailable, still no network scan.
        $lines = (& netsh.exe wlan show profile "name=$name" 'key=clear' 2>$null | Out-String)
        $match = [regex]::Match($lines,'(?im)^\s*(?:Schl.sselinhalt|Key Content)\s*:\s*(.+?)\s*$')
        if ($match.Success) { return $match.Groups[1].Value.Trim() }
        return $null
    } finally {
        Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
    }
}
function Set-WlanProfile([string]$ssid,[string]$password) {
    if ([string]::IsNullOrWhiteSpace($ssid) -or $password.Length -lt 8 -or $password.Length -gt 63) {
        throw "Ungueltige WPA2-Zugangsdaten fuer $ssid"
    }
    $sid = [Security.SecurityElement]::Escape($ssid)
    $pw  = [Security.SecurityElement]::Escape($password)
    $xml = @"
<?xml version="1.0" encoding="UTF-8"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>$sid</name>
  <SSIDConfig><SSID><name>$sid</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType><connectionMode>manual</connectionMode>
  <MSM><security><authEncryption>
    <authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX>
  </authEncryption><sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>$pw</keyMaterial></sharedKey>
  </security></MSM>
</WLANProfile>
"@
    $tmp = Join-Path $env:TEMP ('eufy_wifi_' + [guid]::NewGuid().ToString('N') + '.xml')
    try {
        [IO.File]::WriteAllText($tmp, $xml, (New-Object System.Text.UTF8Encoding($false)))
        $netshResult=(& netsh.exe wlan add profile "filename=$tmp" 'user=current' 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) {
            # A profile might already exist and be usable. Let the following
            # real connection and HTTP test decide, instead of aborting
            # without trying the existing profile.
            $profileResult=(& netsh.exe wlan show profile "name=$ssid" 2>&1 | Out-String)
            if ($LASTEXITCODE -ne 0) {
                throw "WLAN-Profil $ssid nicht verfuegbar. netsh: $netshResult"
            }
            Log "WLAN-Profil $ssid vorhanden; Import abgewiesen: $netshResult"
        }
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
}
function Connect-WLAN([string]$ssid) {
    Log "WLAN-Wechsel angefordert: $ssid"
    $netshResult=(& netsh.exe wlan connect "name=$ssid" 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Windows konnte nicht zu $ssid wechseln. netsh: $netshResult"
    }
}
function Get-HTTP([string]$url,[int]$seconds) {
    $end = (Get-Date).AddSeconds($seconds)
    do {
        try {
            return Invoke-RestMethod -Uri $url -TimeoutSec 5 -UseBasicParsing
        } catch {
            Start-Sleep -Seconds 3
        }
    } while ((Get-Date) -lt $end)
    throw "HTTP nicht erreichbar: $url"
}
function Restore-Peng {
    for ($i=0; $i -lt 12; $i++) {
        try {
            Connect-WLAN $HomeSSID
            Start-Sleep -Seconds 5
            Log "Rueckverbindung zu $HomeSSID angefordert."
            return
        } catch {
            Start-Sleep -Seconds 8
        }
    }
    Log "WARNUNG: Rueckverbindung zu $HomeSSID konnte nicht bestaetigt werden."
}
function Write-Firmware {
    if (!(Test-Path -LiteralPath $FirmwarePath)) { throw "Firmware-BIN fehlt: $FirmwarePath" }
    $f=Get-Item -LiteralPath $FirmwarePath
    if ($f.Length -ne $FirmwareLength) { throw "Firmwaregroesse falsch: $($f.Length)" }
    $sha=(Get-FileHash -LiteralPath $FirmwarePath -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($sha -ne $ExpectedSHA256) { throw "Firmware-Pruefsumme falsch: $sha" }
    $stream=[IO.File]::OpenRead($FirmwarePath)
    try { if ($stream.ReadByte() -ne 233) { throw 'ESP32-App-Header ungueltig' } }
    finally { $stream.Dispose() }
    return $FirmwarePath
}
function Get-NewAPPassword {
    if (!(Test-Path -LiteralPath $CredsPath)) { throw 'Neue AP-Zugangsdaten fehlen.' }
    $line=Get-Content -LiteralPath $CredsPath |
        Where-Object { $_ -match '^PASSWORD=' } | Select-Object -First 1
    if (!$line) { throw 'wifi-credentials.txt ohne PASSWORD' }
    $pass=$line.Substring(9).Trim()
    if ($pass.Length -lt 8 -or $pass.Length -gt 63) { throw 'Ungültiges Passwort im Build' }
    return $pass
}
function Download-Firmware {
    # Wichtig: Beide Dateien werden noch ueber Peng heruntergeladen.
    New-Item -ItemType Directory -Path $StateDir -Force | Out-Null
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    foreach ($item in @(
        @{Url=($ReleaseBase+'/Eufy-WROOM-OTA-app-only.bin'); Path=$FirmwarePath},
        @{Url=($ReleaseBase+'/wifi-credentials.txt'); Path=$CredsPath}
    )) {
        $part=$item.Path+'.download'
        try {
            Log ('Herunterladen und speichern: '+[IO.Path]::GetFileName($item.Path))
            Invoke-WebRequest -Uri $item.Url -OutFile $part -TimeoutSec 180 -UseBasicParsing -ErrorAction Stop
            Move-Item -LiteralPath $part -Destination $item.Path -Force
        } finally {
            Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
        }
    }
    $null=Write-Firmware
    $null=Get-NewAPPassword
    Log 'Firmware und WLAN-Konfigurationsdatei gespeichert, SHA256 verifiziert.'
}

function Make-Task([string]$name,[int]$minutes,[string]$role,[int]$limitMinutes) {
    # Run as the CURRENT logged-on Windows user in a Task Scheduler process.
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent().Name
    $principal=New-ScheduledTaskPrincipal -UserId $identity -LogonType Interactive -RunLevel Limited
    $action=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument (
        '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $SavedScript + '" -Mode ' + $role
    )
    $trigger=New-ScheduledTaskTrigger -Once -At ((Get-Date).AddMinutes($minutes))
    $settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes $limitMinutes) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Force -ErrorAction Stop | Out-Null
}
function Remove-Task([string]$name) {
    Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
}
function Worker {
    $succeeded=$false
    try {
        Log 'WORKER gestartet: weiterhin auf Peng, WLAN-Zugangsdaten pruefen.'
        # Prefer locally provisioned DPAPI key, fallback to Windows WLAN profile.
        $pengPassword=Read-LocalSecret $LocalHomeKey
        if (!$pengPassword) { $pengPassword=Get-SavedWLANKey $HomeSSID }
        if (!$pengPassword -or $pengPassword.Length -lt 8 -or $pengPassword.Length -gt 63) {
            throw 'Peng-Passwort nicht aus gespeichertem Windows-WLAN-Profil lesbar. Kein WLAN-Wechsel.'
        }
        $savedApPassword=Read-LocalSecret $LocalEspKey
        if (!$savedApPassword) { $savedApPassword=Get-SavedWLANKey $EspSSID }
        # Peng is ALREADY connected, and Windows already has its working
        # WLAN profile. Never overwrite it: the previous version failed here.
        # Recovery calls 'netsh wlan connect name=Peng' using this profile.
        Log 'Peng-Profil bleibt unveraendert; nutze bestehende Verbindung.'
        $firmware=Write-Firmware
        $NewApPassword=Get-NewAPPassword
        Log 'Firmware bereit. Recovery-Task laeuft unabhaengig.'
        # Candidates: existing Windows AP profile and freshly downloaded build credentials.
        # Keep Peng untouched; AP import is only attempted for candidate keys.
        $espPassword=$null
        $status=$null
        $candidates = @($savedApPassword,$NewApPassword) |
            Where-Object { $_ -and $_.Length -ge 8 -and $_.Length -le 63 } |
            Select-Object -Unique
        foreach ($candidate in $candidates) {
            try {
                Set-WlanProfile $EspSSID $candidate
                Connect-WLAN $EspSSID
                $status=Get-HTTP "http://$($EspIP):8081/status" 23
                $espPassword=$candidate
                break
            } catch {
                Log 'ESP-Zugang noch nicht moeglich; naechstes lokal vorhandenes Profil.'
            }
        }
        if (!$espPassword -or !$status) { throw 'Kein Zugang zum ESP-AP moeglich; Laptop wird zurueckgestellt.' }
        Log "ESP OTA erreichbar, aktive Partition: $($status.running)"
        $activePassword=$espPassword
        $newFirmware=$false
        try {
            $wifi=Invoke-RestMethod -Uri "http://$($EspIP):8081/wifi/status" -TimeoutSec 5 -UseBasicParsing
            if ($wifi.mode -eq 'AP+STA') {
                Log 'AP+STA-Firmware bereits vorhanden: OTA nicht erneut noetig.'
                $newFirmware=$true
            }
        } catch {}
        if (!$newFirmware) {
            Log 'Uebertrage ausschliesslich ESP32-App-BIN ueber funktionierendes OTA.'
            $response=Invoke-RestMethod -Uri "http://$($EspIP):8081/upload" -Method Post `
                -Headers @{'X-OTA-Token'=$espPassword} -InFile $firmware `
                -ContentType 'application/octet-stream' -TimeoutSec 180 -UseBasicParsing
            if ($response.result -ne 'accepted') { throw 'OTA wurde vom ESP32 nicht akzeptiert' }
            Log 'OTA akzeptiert; warte auf Neustart und neuen ESP-AP.'
            Start-Sleep -Seconds 8
            $newFirmware=$false
            foreach ($candidate in @($NewApPassword,$espPassword)) {
                try {
                    Set-WlanProfile $EspSSID $candidate
                    Connect-WLAN $EspSSID
                    $null=Get-HTTP "http://$($EspIP):8081/status" 32
                    $wifi=Get-HTTP "http://$($EspIP):8081/wifi/status" 16
                    if ($wifi.mode -eq 'AP+STA') {
                        $activePassword=$candidate
                        $newFirmware=$true
                        break
                    }
                } catch {
                    Log 'ESP-AP noch nicht erreichbar; pruefe alternatives Passwort.'
                }
            }
            if (!$newFirmware) { throw 'ESP32 nach OTA nicht als AP+STA erreichbar.' }
        }
        # Avoid ever printing the Home WLAN password in logs or commands.
        Log "Speichere Peng-Zugangsdaten im ESP-NVS und starte STA-Verbindung."
        $headers=@{
            'X-OTA-Token'=$activePassword;
            'X-STA-SSID'=$HomeSSID;
            'X-STA-PASSWORD'=$pengPassword
        }
        try {
            $null=Invoke-RestMethod -Uri "http://$($EspIP):8081/wifi/config" `
                -Method Post -Headers $headers -TimeoutSec 20 -UseBasicParsing
        } catch {
            Log 'POST unterbrochen (moeglicher AP-Kanalwechsel), pruefe WLAN-Status.'
        }
        $ip=$null
        $end=(Get-Date).AddSeconds(115)
        while ((Get-Date) -lt $end) {
            try {
                $wifi=Invoke-RestMethod -Uri "http://$($EspIP):8081/wifi/status" -TimeoutSec 5 -UseBasicParsing
                if ($wifi.sta_connected -and $wifi.sta_ip -and $wifi.sta_ip -ne '0.0.0.0') {
                    $ip=[string]$wifi.sta_ip
                    break
                }
            } catch {
                try { Connect-WLAN $EspSSID } catch {}
            }
            Start-Sleep -Seconds 4
        }
        Log 'Stelle Laptop-WLAN wieder auf Peng.'
        Connect-WLAN $HomeSSID
        if (!$ip) { throw 'Keine STA-IP vom ESP ermittelt; Laptop zurueck auf Peng, AP-Fallback bleibt erhalten.' }
        Log "ESP32 im Heimnetz Peng: $ip"
        $healthy=Get-HTTP "http://$($ip):8081/status" 70
        Log "ESP OTA im Heimnetz erreichbar, laufende Partition: $($healthy.running)"
        try {
            $null=Invoke-RestMethod -Uri "http://$($ip):8080/health" -TimeoutSec 8 -UseBasicParsing
            Log "ESP Telemetrie-HTTP unter http://$($ip):8080/telemetry erreichbar."
        } catch {
            Log 'Hinweis: Telemetrie-HTTP antwortet nicht; Heimnetz und OTA sind trotzdem erreichbar.'
        }
        Log "FERTIG: Laptop auf Peng, ESP IP=$ip, OTA-Port 8081."
        $succeeded=$true
        Remove-Task $TaskRecovery
    } catch {
        Log ('FEHLER: ' + $_.Exception.Message)
        Log 'Rueckkehr zu Peng wird sofort versucht; zusaetzlicher Recovery-Task bleibt aktiv.'
        try { Restore-Peng } catch {}
    } finally {
        # Keep the recovery timer unless connection to STA OTA was verified.
        if ($succeeded) { Remove-Task $TaskRecovery }
        Remove-Task $TaskWorker
    }
}
function Bootstrap {
    New-Item -ItemType Directory -Path $StateDir -Force | Out-Null
    # Stable script path for tasks even when Downloads or remote session is closed.
    if ($PSCommandPath -ne $SavedScript) { Copy-Item -LiteralPath $PSCommandPath -Destination $SavedScript -Force }
    # PRE-FLIGHT: provided passwords are saved locally with Windows DPAPI.
    # Check and download everything before changing Wi-Fi or losing VPN.
    if ($HomePassword) { Save-LocalSecret $LocalHomeKey $HomePassword }
    if ($EspPassword) { Save-LocalSecret $LocalEspKey $EspPassword }
    $homePass=Read-LocalSecret $LocalHomeKey
    if (!$homePass) { $homePass=Get-SavedWLANKey $HomeSSID }
    if (!$homePass -or $homePass.Length -lt 8 -or $homePass.Length -gt 63) {
        throw 'Peng-Passwort weder als Parameter noch als Windows-Profil vorhanden. Kein WLAN-Wechsel.'
    }
    $espPass=Read-LocalSecret $LocalEspKey
    if (!$espPass) { $espPass=Get-SavedWLANKey $EspSSID }
    if (!$espPass -or $espPass.Length -lt 8 -or $espPass.Length -gt 63) {
        throw 'ESP-AP-Passwort im Windows-Profil nicht lesbar. Kein WLAN-Wechsel.'
    }
    # Mandatory preflight: previous Windows netsh calls failed even when
    # returning to Peng. Do not risk the VPN unless the same reconnect
    # command used by the recovery task works while Peng is still active.
    Log 'Sicherheitstest: netsh kann die bestehende Peng-Verbindung steuern.'
    Connect-WLAN $HomeSSID
    # Downloads and checksum validation happen while Peng is still active.
    # WLAN profile changes happen only after the recovery task is armed.
    Download-Firmware
    if (Get-ScheduledTask -TaskName $TaskWorker -ErrorAction SilentlyContinue) {
        if ((Get-ScheduledTask -TaskName $TaskWorker).State -eq 'Running') {
            throw 'Es laeuft bereits ein Eufy-Worker-Task.'
        }
    }
    # Independent rescue runs even if this worker or the VPN connection fails.
    # Worker is triggered manually (scheduled time is 1 day in the future).
    Make-Task $TaskRecovery 15 'Recovery' 4
    try {
        Make-Task $TaskWorker 1440 'Worker' 14
        Start-ScheduledTask -TaskName $TaskWorker -ErrorAction Stop
    } catch {
        Remove-Task $TaskRecovery
        throw
    }
    Log 'START ERFOLGREICH: Worker ist als unabhaengige Windows-Aufgabe gestartet.'
    Log 'In etwa 15 Minuten stellt ein zweiter Task notfalls WLAN Peng wieder her.'
    Log 'Fernzugriff kann gleich abbrechen; selbststaendiger Ablauf laeuft weiter.'
    Log ('Protokoll: ' + $LogPath)
}

if ($Mode -eq 'Recovery') {
    Log 'RECOVERY-TIMER: WLAN Peng wieder verbinden.'
    Restore-Peng
    Remove-Task $TaskRecovery
} elseif ($Mode -eq 'Worker') {
    Worker
} else {
    Bootstrap
}