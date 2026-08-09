# PrintSphere (Knomi & Waveshare)

Round ESP32-S3 companion display for Bambu Lab printers — print progress, temperatures, remaining time, AMS, errors, cover previews, and supported camera snapshots.

Works with **Bambu Cloud**, the printer’s **local** connection, or both. Home Assistant is not required.

> **Based on [PrintSphere by cptkirki](https://github.com/cptkirki/PrintSphere).**  
> This repository is a derivative under the [Federation Non-Commercial License (FNCL) v1.1](LICENSE).  
> See [NOTICE.md](NOTICE.md) for attribution and a summary of changes (notably **BigTreeTech Knomi v2** support).

Latest packaged line: **v1.6.2** (+ Knomi v2 variant)

## Supported hardware

| Hardware | Variant id | Battery |
| --- | --- | --- |
| Waveshare ESP32-S3 Touch AMOLED 1.75 | `amoled_1_75` | Yes (charging / USB detection) |
| Waveshare ESP32-S3 Touch LCD 2.8C | `lcd_2_8c` | Yes (board measurement) |
| BigTreeTech Knomi v2 | `knomi_v2` | No (USB/DC); dim / screen-off still apply |

Flash **only** the firmware built for your board. Variants are not interchangeable.

## Install

You need a USB data cable and desktop Chrome or Edge.

1. Open the **Web Installer** for this fork (GitHub Pages / Releases — see below).
2. Select your hardware variant.
3. Connect the device and install the matching full image.
4. Keep USB connected so Wi‑Fi can be provisioned over serial.
5. Open **Web Config** on the IP shown on the display and connect your printer.

Upstream installer (original project): [cptkirki Web Installer](https://cptkirki.github.io/PrintSphere/flash/)

### Wi‑Fi fallback

- SSID: `PrintSphere-Setup`
- Password: `printsphere`
- Setup: [http://192.168.4.1](http://192.168.4.1)

## Connect your printer

Web Config modes:

- **Hybrid (recommended)** — cloud + local; picks the better status path; local JPEG camera when supported  
- **Cloud only** — Bambu Cloud status & covers; no local MQTT / local camera  
- **Local only** — direct printer MQTT; no cloud covers  

Local connection needs printer IP/hostname, serial, and access code.

## Updates (OTA)

Use an **OTA** image so Wi‑Fi and printer settings stay in NVS.

1. Install or download the matching `printsphere_ota-*.bin` from [Releases](../../releases).
2. In Web Config → Firmware Update, flash from file or URL.

Full USB images (`printsphere_full-*.bin`) are for first install or recovery.

## What’s in the box (features)

- Progress, remaining time, layers, nozzle/bed temps  
- AMS + external spool  
- Cloud cover + job title  
- Local JPEG camera on supported models (A1 / A1 Mini / A2L / P1P / P1S)  
- Chamber light on supported models  
- Multi-printer profiles  
- HMS / print-error text from the embedded lookup table  
- Display rotation, status colors, sounds, portal PIN  
- Battery-aware power policy on Waveshare boards  

## Printer notes

Status paths exist for:  
`A1`, `A1 Mini`, `A2L`, `P1P`, `P1S`, `P2S`, `H2C`, `H2D`, `H2D Pro`, `H2S`, `X1`, `X1C`, `X1E`, `X2D`

RTSP cameras (X1 / H2 / P2S / X2D families) are **not** shown as live video on ESP32-S3. Cloud covers can still appear.

## Build from source

ESP-IDF **v5.5.4**, Windows or Linux. See [docs/Build/README.md](docs/Build/README.md).

```bash
# Example: Knomi v2
idf.py -B build-knomi_v2 -DPRINTSPHERE_HW_VARIANT=knomi_v2 -p COMx build flash
```

## Releases

GitHub Releases publish:

| File | Use |
| --- | --- |
| `printsphere_full-*-knomi_v2.bin` | USB first flash / recovery (Knomi) |
| `printsphere_ota-*-knomi_v2.bin` | OTA (Knomi) |
| `printsphere_full-*-2.8c.bin` / `…-amoled…` | Waveshare variants when packaged |

## Credits

- **Upstream:** [cptkirki/PrintSphere](https://github.com/cptkirki/PrintSphere) — core firmware, Web Config, cloud/local paths, Waveshare BSPs  
- **This fork:** Knomi v2 port, layout fixes, rotation/HMS fixes — see [NOTICE.md](NOTICE.md)

## License

[Federation Non-Commercial License (FNCL) v1.1](LICENSE) — non-commercial use; keep license + mark modifications. Commercial use needs a separate license from the copyright holder.
