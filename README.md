# PrintSphere

Round ESP32-S3 printer companion for Bambu Lab with a circular display, touch setup, hybrid cloud/local routing, and the current code paths for cover preview, camera snapshots, and battery-aware operation.

<img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/820c2e9b-10a7-4430-949c-e8b0adc1357d" /><img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/5923dc59-0123-4df1-b54d-673c6dbad23b" />

## MakerWorld

- MakerWorld model: `https://makerworld.com/de/models/2517189-printsphere-bambu-status-display-standalone-1-75`
- End-user setup guide: [`MAKERWORLD.md`](MAKERWORLD.md)

## Hardware And Stack

- ESP-IDF `v5.5.x`
- LVGL `v9.4.0`
- Waveshare ESP32-S3 AMOLED 1.75
- AXP2101 PMU integration via `XPowersLib`
- no Home Assistant required

## Current Codebase Features

- official Waveshare BSP for display, touch, and LVGL
- NVS-based configuration storage
- `AP+STA` Wi-Fi manager
- local setup portal on `esp_http_server`
- Web Config that stays open during initial provisioning and switches to PIN/session unlock on the home network afterwards
- Bambu Cloud login flow with email/password plus email-code or 2FA handling
- hybrid routing logic between Bambu Cloud and the local printer path
- cloud MQTT as the primary live cloud status path plus metadata / cover preview handling
- local MQTT status path with live reconnect logic from the web portal
- embedded Bambu CA bundle for local MQTT TLS verification
- merged status, metrics, temperatures, errors, and HMS data depending on model and source availability
- cloud cover image/title page in the UI where available
- local camera snapshot page with tap refresh and periodic refresh while open
- arc color tuning from Web Config with live preview behavior
- hardware display rotation with touch alignment and a Web Config restart action
- chamber light toggle path on supported models
- battery and USB status with power-aware behavior
- embedded on-device error text lookup database without a separate storage partition

## Setup Flow

1. Flash [`https://github.com/cptkirki/PrintSphere/blob/main/release/initial/printsphere_full.bin`](printsphere_full.bin)
2. On first boot, the device starts a setup AP:
   - SSID: `PrintSphere-Setup`
   - password: `printsphere`
3. Open `http://192.168.4.1` and save your home Wi-Fi.
4. After the reboot, reopen Web Config on the device IP in your home network.
5. During provisioning, Web Config stays open without a PIN until the selected source path is ready.
6. Choose the connection mode you want to run:
   - `Cloud only`
   - `Local only`
   - `Hybrid`
7. Complete only the source path that your chosen mode requires:
   - `Cloud only`: connect Bambu Cloud
   - `Local only`: connect the local printer path
   - `Hybrid`: connect either path first; the portal unlocks setup once one path is working, and you can add the other later
8. Bambu Cloud login supports:
   - email + password
   - optional email code
   - optional 2FA code
9. The local printer path uses:
   - printer IP or hostname
   - printer serial number
   - access code
10. After provisioning is complete, Web Config on the home network uses the on-device PIN/session unlock flow.
   - hold anywhere on the display for about one second to request a six-digit PIN
   - PIN lifetime: about 2 minutes
   - unlocked browser session: about 10 minutes

Cloud and local credentials can usually be applied live from Web Config without another reboot. Changing the connection mode itself or applying a new screen rotation still restarts the device.

## Connection Modes

- `Hybrid`:
  Current default recommendation. PrintSphere tries to combine cloud and local data, picks the better active path at runtime, and still uses the local camera when available. Provisioning is considered complete once Wi-Fi is connected and either Cloud or Local is working.
- `Cloud only`:
  Cloud monitoring and cover preview only. Local MQTT and the local camera page are disabled. Provisioning is complete once Wi-Fi and Bambu Cloud are connected.
- `Local only`:
  Local MQTT monitoring and the local camera path without requiring Bambu Cloud. Provisioning is complete once Wi-Fi and the local printer path are connected. The cloud cover page is not used in this mode.

## Model Notes

- Cloud can now also carry progress, remaining time, layers, and temperatures on many models.
- In `Hybrid`, the current code prefers cloud status for `P2S` and the `H2` family.
- The current code has local status paths for:
  `A1`, `A1 Mini`, `P1P`, `P1S`, `X1`, `X1C`, `X1E`
- `P2S` local status is not supported.
- The `H2` family requires Developer Mode for local status.
- The local JPEG camera path is currently written for:
  `A1`, `A1 Mini`, `P1P`, `P1S`
- RTSP-style camera handling is recognized for:
  `P2S`, `H2C`, `H2D`, `H2D Pro`, `H2S`, `X1`, `X1C`, `X1E`
  but that path will not be implemented as the hardware is not capable of displaying video live feeds with more than about 0.5 fps.
- The code currently exposes chamber light control on supported `P1S`, `P2`, `H2`, and `X1` models.

## UI Overview

- Page 1:
  can show progress ring, lifecycle state, job name, temperatures, layers, remaining time, Wi-Fi, battery, and USB state
- Page 2:
  can show cloud cover preview and title
- Page 3:
  can show local camera snapshots when that path is working
- Long-press on the display:
  requests a Web Config PIN
- Tap on the camera page:
  requests a refresh of the current image
- Tap the center logo on page 1 on supported printers:
  requests a chamber light toggle
- Dynamic secondary text such as IP addresses, project titles, and portal hints uses a more complete font for better glyph coverage

## Web Config

Web Config currently exposes sections for:

- `Step 1 - Wi-Fi`
- `Connection Mode`
- `Step 2 - Bambu Cloud`
- `Step 3 - Local Printer Path`
- `Screen Rotation`
- `Arc Colors`

During initial provisioning, Web Config stays open until the selected source mode has at least one working path. After that, the portal uses the on-device PIN/session unlock flow on the home network.

Arc colors are intended to preview live immediately and can be saved without restarting the device. Screen rotation uses the display controller's hardware rotation and applies on restart so touch stays aligned.

<img width="1078" height="686" alt="image" src="https://github.com/user-attachments/assets/9b80b93e-5963-46da-a284-471fd7be27f0" />

## Current Limitations

- local camera and MJPEG/RTSP work are not fully finished yet
- cloud/local behavior on newer families still needs broader real-world validation
- `P2S` local status is not supported in the current code
- `H2` local status requires Developer Mode
- local MQTT TLS now uses an embedded Bambu CA bundle
- `Local only` works, but the broadest hands-on validation so far is still in `Hybrid` and `Cloud only`
- most hands-on testing so far has been on `P1S` and `P1P`

## Flashing

[`release/firmware.bin`](release/firmware.bin) is the merged initial-flash image for empty devices.

The newest merged image stays in `/release/`; older versioned builds can be moved to `/release/archive/`.

### Web Flashers

`web.esphome.io`
`https://web.esphome.io/`

- connect USB
- choose the COM port
- do not use "Prepare for first use"
- install `firmware.bin` directly

`espboards.dev`
`https://www.espboards.dev/tools/program/`

- write `firmware.bin` to address `0x0`

`esptool-js`
`https://espressif.github.io/esptool-js/`

- write `firmware.bin` to address `0x0`

The bootloader is already included in the merged image.

### Local Build / Flash

If you clone the repo and build it yourself:

```bash
idf.py -p PORT flash
```

Alternatively, write the merged image directly with `esptool`:

```bash
esptool.exe --chip esp32s3 --port PORT write_flash 0x0 release/firmware.bin
```
