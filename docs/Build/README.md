# Building PrintSphere

This document is for developers and advanced users who want to clone, build, flash or package PrintSphere themselves. Most users should use the [PrintSphere Web Installer](https://cptkirki.github.io/PrintSphere/flash/).

## Requirements

- Windows, Linux or macOS
- Git
- Python 3
- ESP-IDF `v5.5.4`
- A USB data cable
- Enough free disk space for separate build directories

The project uses C17, C++17 and LVGL `v9.5.0`.

## Clone the repository

```powershell
git clone https://github.com/cptkirki/PrintSphere.git
cd PrintSphere
```

Activate the ESP-IDF environment before running `idf.py`. On Windows, use the ESP-IDF PowerShell or command prompt installed with ESP-IDF.

## Hardware variants

Always select the hardware explicitly. Each variant must use its own build directory.

| Variant | Build flag | Build directory | Release directory |
| --- | --- | --- | --- |
| Waveshare ESP32-S3 Touch AMOLED 1.75 | `amoled_1_75` | `build-amoled_1_75` | `release/` |
| Waveshare ESP32-S3 Touch LCD 2.8C | `lcd_2_8c` | `build-lcd_2_8c` | `release/2.8c/` |

Do not reuse one variant's build directory for the other variant.

## Build the AMOLED 1.75 variant

```powershell
idf.py -B build-amoled_1_75 -DPRINTSPHERE_HW_VARIANT=amoled_1_75 reconfigure build
```

Build, flash and open the serial monitor:

```powershell
idf.py -B build-amoled_1_75 -DPRINTSPHERE_HW_VARIANT=amoled_1_75 -p COM9 build flash monitor
```

Replace `COM9` with the correct port on your system.

## Build the LCD 2.8C variant

```powershell
idf.py -B build-lcd_2_8c -DPRINTSPHERE_HW_VARIANT=lcd_2_8c reconfigure build
```

Build, flash and open the serial monitor:

```powershell
idf.py -B build-lcd_2_8c -DPRINTSPHERE_HW_VARIANT=lcd_2_8c -p COM7 build flash monitor
```

Replace `COM7` with the correct port on your system.

## Flash or monitor without rebuilding

AMOLED 1.75:

```powershell
idf.py -B build-amoled_1_75 -DPRINTSPHERE_HW_VARIANT=amoled_1_75 -p COM9 flash
idf.py -B build-amoled_1_75 -DPRINTSPHERE_HW_VARIANT=amoled_1_75 -p COM9 monitor
```

LCD 2.8C:

```powershell
idf.py -B build-lcd_2_8c -DPRINTSPHERE_HW_VARIANT=lcd_2_8c -p COM7 flash
idf.py -B build-lcd_2_8c -DPRINTSPHERE_HW_VARIANT=lcd_2_8c -p COM7 monitor
```

## Package both release variants

Build both variants first. Then run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/package_release.ps1 -Version v1.6.2
```

The script creates the four current release images plus versioned archive copies:

| Hardware | Initial USB image | OTA image |
| --- | --- | --- |
| AMOLED 1.75 | `release/initial/printsphere_full.bin` | `release/ota/printsphere_ota.bin` |
| LCD 2.8C | `release/2.8c/initial/printsphere_full.bin` | `release/2.8c/ota/printsphere_ota.bin` |

Versioned copies are stored below the corresponding `archive/` directories.

## Package one variant manually

AMOLED 1.75:

```powershell
python tools/package_initial_flash.py --build-dir build-amoled_1_75 --release-root release --version v1.6.2
```

LCD 2.8C:

```powershell
python tools/package_initial_flash.py --build-dir build-lcd_2_8c --release-root release/2.8c --version v1.6.2-2.8c
```

## Initial image versus OTA image

- `printsphere_full.bin` is a merged image containing the bootloader, partition table, OTA data and application. Use it for an empty device, recovery or a partition-layout migration.
- `printsphere_ota.bin` contains only the application. Install it through the running device's Web Config so ESP-IDF writes it to the inactive OTA slot and preserves NVS configuration.

Do not use an OTA image as a full image at address `0x0`.

Devices on v1.5.x or older require one full v1.6.x flash because v1.6 changed the partition layout. Devices already using the v1.6 layout can normally use OTA.

## Manual full-image flashing

AMOLED 1.75:

```powershell
esptool.exe --chip esp32s3 --port COM9 write_flash 0x0 release/initial/printsphere_full.bin
```

LCD 2.8C:

```powershell
esptool.exe --chip esp32s3 --port COM7 write_flash 0x0 release/2.8c/initial/printsphere_full.bin
```

The merged image already contains the bootloader and partition table.

## Reconfigure and clean builds

Use `reconfigure` first when CMake settings, dependencies or hardware options change:

```powershell
idf.py -B build-amoled_1_75 -DPRINTSPHERE_HW_VARIANT=amoled_1_75 reconfigure
idf.py -B build-lcd_2_8c -DPRINTSPHERE_HW_VARIANT=lcd_2_8c reconfigure
```

`fullclean` is normally unnecessary. If a build cache is genuinely broken, remove or recreate only the affected variant's build directory.

## Important project files

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt` | Release version, hardware selection and packaging target |
| `main/CMakeLists.txt` | Application sources and ESP-IDF component dependencies |
| `sdkconfig.defaults` | Shared ESP-IDF project defaults |
| `partitions.csv` | NVS, OTA and LittleFS partition layout |
| `tools/package_release.ps1` | Packages both hardware variants |
| `tools/package_initial_flash.py` | Creates merged initial and OTA images |
| `flash/index.html` | Browser installer page |
| `release/` | Published firmware images and release notes |

## Release checklist

1. Set the release version in the root `CMakeLists.txt`.
2. Build and test both hardware variants.
3. Package both variants with an explicit version.
4. Verify the four generated images and their versioned archive copies.
5. Update the browser installer and release notes.
6. Commit and publish the source, installer, notes and binaries together.
