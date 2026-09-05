# PrintSphere v1.6.3

Patch release on top of v1.6.2 for German umlauts and other Latin characters
on the display. OTA-compatible with v1.6.2 (no partition table change).

## Release Scope

- **Base**: v1.6.2. Devices on v1.6 or older should follow the existing v1.6
  full-flash/OTA upgrade path.
- **Toolchain target**: ESP-IDF v5.5.4, LVGL v9.5.0.
- **Hardware variants**: Waveshare ESP32-S3 Touch AMOLED 1.75 and ESP32-S3
  Touch LCD 2.8C.

## Fixes

- **German umlauts render on the UI**: Job names, printer names and detail
  text with `Ä Ö Ü ß ä ö ü` no longer appear as blank tofu. Dosis and
  Montserrat now include printable ASCII, the degree sign, and those Latin
  glyphs.
- **UTF-8 strings stay intact**: Truncating job names, status labels and
  short display names no longer splits a multi-byte character in the middle.

## Known Notes

- Existing v1.6.x devices can update via OTA. A factory/full flash is only
  required for recovery or a first install.
