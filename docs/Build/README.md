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
powershell -ExecutionPolicy Bypass -File tools/package_release.ps1 -Version v1.7.0
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
python tools/package_initial_flash.py --build-dir build-amoled_1_75 --release-root release --version v1.7.0
```

LCD 2.8C:

```powershell
python tools/package_initial_flash.py --build-dir build-lcd_2_8c --release-root release/2.8c --version v1.7.0-2.8c
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

1. Set the release version in the root `VERSION` file.
2. Build and test both hardware variants.
3. Package both variants with an explicit version.
4. Verify the four generated images and their versioned archive copies.
5. Update the browser installer and release notes.
6. Commit and publish the source, installer, notes and binaries together.

## v1.7.0 verification and update compatibility

`VERSION` is the single release-version source. Use ESP-IDF v5.5.4 and retain the checked-in `dependencies.lock`. A new build uses its own `build-*/sdkconfig`; existing caches can be migrated with `-DSDKCONFIG=<absolute build directory>/sdkconfig`. The pinned LVGL adapter patch is applied/verified automatically during CMake configuration. Source drift stops the build.

After dependency resolution, run `python tools/test_host.py` with a native GCC/G++ toolchain on PATH. `--cxx <path to g++>` selects a portable compiler. These tests execute production protocol/policy code and the ConfigStore against an NVS testdouble; they do not simulate printer networking or display hardware.

The packager validates the application descriptor and writes `manifest.json` with board, image version, SDK build string, commit, source/dependency/configuration hashes and firmware hashes. Pass the actual compiled version; relabelling an older image is rejected.

Read [v1.7.0 release notes](../../release/RELEASE_NOTES_v1.7.0.md) before upgrading: rollback needs a matching rollback-enabled bootloader, and the new atomic configuration records are not a downgrade-compatible format. Real account/2FA, camera, board and rollback tests remain necessary before public release.

Normal builds disable `CONFIG_MBEDTLS_DEBUG`. Existing cached sdkconfigs must also disable it via menuconfig (Component config > mbedTLS > Enable mbedTLS debugging). A derived `CONFIG_MBEDTLS_DEBUG_LEVEL=1` alone does not select the warning-level choice; fresh configs can select verbose instead. ESP-IDF resets the mbedTLS log tag each time it creates a TLS context, so a one-time runtime log filter is insufficient. Release configuration rejects cached TLS tracing; deliberately enable it only with a `-debug`/`_debug` version. Standard transport and certificate error logs remain available without the internal TLS trace.

If the release check stops configuration before menuconfig can open, edit the active `build-<board>/sdkconfig` directly: replace `CONFIG_MBEDTLS_DEBUG=y` with `# CONFIG_MBEDTLS_DEBUG is not set`, then run `idf.py ... reconfigure`. Changing only `sdkconfig.defaults` does not override an existing cached configuration.

Run `python tools/test_lvgl_host.py` for the actual LVGL UI regressions on both layouts. It builds under `build-host/lvgl-ui` with a native compiler, CMake and Ninja; `--cxx`, `--cmake` and `--ninja` override tool discovery. See [test scope](../../tests/lvgl_host/README.md). The live device's `/api/health` reports UI phase, pending work and five-second timing summaries without acquiring the LVGL lock or logging every frame.


### Display synchronization (v1.7.5)

LVGL must use semaphore synchronization (`CONFIG_LV_USE_FREERTOS_TASK_NOTIFY=n`).
Set `CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES=2`: adapter wakeups use index 0,
GPIO-TE SPI completion uses index 1. The checked-in defaults select these values;
CMake rejects an incompatible cached sdkconfig. Update existing board configs
through menuconfig before rebuilding if this guard fires.

The adapter patcher is applied during CMake configuration, with source-drift checks.
The PowerShell entry point delegates to the same Python implementation. The actual
AMOLED DMA staging regression runs as part of `python tools/test_host.py`.
`/api/health` exposes `display_output_phase` and `display_output_age_ms` for the
GPIO-TE path without acquiring the LVGL mutex or printing per-frame logs. See the
[v1.7.5 notes](../../release/RELEASE_NOTES_v1.7.5.md) for diagnostic limits.
