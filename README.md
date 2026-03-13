# PrintSphere

Round ESP32-S3 printer companion for Bambu Lab: live status, progress ring, camera snapshots, cloud + LAN sync, and touch setup on a circular display.

## Native ESP-IDF Project

PrintSphere is the native ESP-IDF rebuild and the better successor to
[`big-printsphere.yaml`](../big-printsphere.yaml), not just a subproject of it.

## Goals

- ESP-IDF `v5.5.x`
- LVGL `v9.4.0`
- ESP32-S3 Touch AMOLED 1.75
- no Home Assistant required
- direct Bambu LAN status client in LAN Mode

## Already Implemented

- official Waveshare BSP for display, touch, and LVGL
- AXP2101 PMU integration via `XPowersLib`
- NVS-based configuration storage
- `AP+STA` Wi-Fi manager
- local setup portal on `esp_http_server`
- first native PrintSphere LVGL screen
- local Bambu MQTT status client via `device/{serial}/report`
- custom 16 MB partition table for the larger LVGL/BSP binary

## Setup Flow

1. The ESP always starts a setup AP with SSID `PrintSphere-Setup`
   and password `printsphere`
2. In setup AP mode, the portal only asks for:
   - Wi-Fi SSID
   - Wi-Fi password
3. Save Wi-Fi and let the ESP reboot
4. After the ESP joins your home network, reopen the portal on its new IP
5. Connect Bambu Cloud with:
   - Bambu email
   - Bambu password
   - optional email code or 2FA code if Bambu requests verification
6. It is recommended to connect the local printer path for LAN MQTT and camera snapshots with:
   - printer IP or hostname
   - printer serial number
   - LAN-Mode access code

Cloud and local printer credentials are applied live from the web portal without another reboot.

## UI Status

The current screen already shows:

- print progress
- lifecycle status
- job name
- temperatures
- layer information
- remaining time
- Wi-Fi status
- battery and USB status

## Current Limitations

- LAN Mode must be enabled on the printer
- camera and MJPEG decoding are intentionally not fully finished yet
- active printer control is intentionally out of scope and not planned for this project
- MQTT TLS is working, but is not yet pinned to dedicated Bambu certificate handling

## Build

On the first build, the ESP-IDF Component Manager downloads the required
dependencies for the official board BSP.

```bash
idf.py set-target esp32s3
idf.py build
```

Example for Windows with a local ESP-IDF installation:

```powershell
& 'C:/esp/v5.5.2/esp-idf/export.ps1'
idf.py build
```

A normal `idf.py build` also generates a merged initial-flash image at
`release/firmware.bin` plus a versioned copy such as `release/firmware-v1.bin`.

The currently used release version is set in `CMakeLists.txt` via
`PRINTSPHERE_RELEASE_VERSION`.

If you only want to regenerate the release artifact from an existing build:

```bash
idf.py release_initial_flash
```

## Flash

```bash
idf.py -p PORT flash
```

Alternatively, the merged initial-flash image can be written directly at
offset `0x0`:

```bash
python -m esptool --chip esp32s3 --port PORT write_flash 0x0 release/firmware.bin
```
