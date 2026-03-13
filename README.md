# PrintSphere

Round ESP32-S3 printer companion for Bambu Lab: live status, progress ring, camera snapshots, cloud + LAN sync, and touch setup on a circular display.

## Native ESP-IDF App

PrintSphere ist die native ESP-IDF-Neuauflage und der bessere Nachfolger von
[`big-printsphere.yaml`](../big-printsphere.yaml) und nicht nur ein
Unterprojekt davon.

## Ziel

- ESP-IDF `v5.5.x`
- LVGL `v9.4.0`
- ESP32-S3 Touch AMOLED 1.75
- ohne Home Assistant
- direkter Bambu-LAN-Statusclient im LAN Mode

## Bereits umgesetzt

- offizielles Waveshare-BSP fuer Display, Touch und LVGL
- AXP2101-PMU-Anbindung ueber `XPowersLib`
- NVS-basierter Konfigspeicher
- `AP+STA`-WLAN-Manager
- lokales Setup-Portal auf `esp_http_server`
- erster nativer PrintSphere-LVGL-Screen
- lokaler Bambu-MQTT-Statusclient ueber `device/{serial}/report`
- eigene 16-MB-Partitionstabelle fuer das groessere LVGL-/BSP-Binary

## Setup-Ablauf

1. ESP startet immer einen Setup-AP mit SSID `PrintSphere-Setup`
   und Passwort `printsphere`
2. Portal oeffnen und eintragen:
   - WLAN-SSID
   - WLAN-Passwort
   - Drucker-IP oder Hostname
   - Drucker-Seriennummer
   - Bambu Access Code fuer lokalen LAN-Statuszugriff
3. Speichern
4. ESP startet neu und versucht parallel:
   - im WLAN einzubuchen
   - den Drucker lokal per MQTT auf Port `8883` zu erreichen

## UI-Status

Der aktuelle Screen zeigt bereits:

- Druckfortschritt
- Lifecycle-Status
- Jobname
- Temperaturen
- Layer
- Restzeit
- WLAN-Status
- Akku-/USB-Status

## Aktuelle Grenzen

- LAN Mode am Drucker wird vorausgesetzt
- Kamera und MJPEG-Decoding sind bewusst noch nicht enthalten
- aktive Drucker-Steuerung ist noch nicht umgesetzt
- MQTT-TLS ist funktional angebunden, aber noch nicht auf ein eigenes
  Bambu-Zertifikat-Handling gepinnt

## Build

Beim ersten Build zieht der ESP-IDF Component Manager die benoetigten
Abhaengigkeiten fuer das offizielle Board-BSP nach.

```bash
idf.py set-target esp32s3
idf.py build
```

Unter Windows mit lokalem ESP-IDF-Setup zum Beispiel:

```powershell
& 'C:/esp/v5.5.2/esp-idf/export.ps1'
idf.py build
```

Ein normales `idf.py build` erzeugt jetzt zusaetzlich ein gemergtes
Initial-Flash-Image unter `release/firmware.bin` sowie eine versionierte
Kopie wie `release/firmware-v1.bin`.

Die aktuell verwendete Release-Version wird in `CMakeLists.txt` ueber
`PRINTSPHERE_RELEASE_VERSION` gesetzt.

Wenn nur das Release-Artefakt aus einem bestehenden Build neu geschrieben
werden soll:

```bash
idf.py release_initial_flash
```

## Flash

```bash
idf.py -p PORT flash
```

Das gemergte Initial-Flash-Image kann alternativ direkt bei Offset `0x0`
geschrieben werden:

```bash
python -m esptool --chip esp32s3 --port PORT write_flash 0x0 release/firmware.bin
```
