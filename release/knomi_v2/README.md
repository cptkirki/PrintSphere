# Knomi v2 release images

Build and package with:

```powershell
idf.py -B build-knomi_v2 -DPRINTSPHERE_HW_VARIANT=knomi_v2 reconfigure build
python tools/package_initial_flash.py --build-dir build-knomi_v2 --release-root release/knomi_v2 --version v1.6.2-knomi_v2
```

- `initial/printsphere_full.bin` — USB factory flash
- `ota/printsphere_ota.bin` — OTA only for devices already running the Knomi v2 variant

Hardware: BigTreeTech Knomi v2 (ESP32-S3, GC9A01 240×240, CST816S). Hold BOOT while plugging USB if the serial port does not appear.
