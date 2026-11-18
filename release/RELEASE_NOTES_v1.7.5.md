# PrintSphere v1.7.5

Changes since v1.6.2:

- Improved Bambu Cloud login, verification and connection recovery, plus more reliable local printer status.
- Automatically discovers multiple printers in a cloud account and lets you select them on the display.
- Smoother page navigation, more responsive status updates and improved display handling.
- Safer configuration storage, printer-specific command handling and OTA image validation.
- Quieter normal-operation logs and improved connection diagnostics.

Choose the firmware for your hardware in the [web installer](https://cptkirki.github.io/PrintSphere/flash/). Use the OTA image for an existing device and the initial image for a new installation.

The first OTA upgrade from v1.6.2 requires selecting the correct board image. Existing settings are read during upgrade; older firmware may not read settings saved by v1.7.5. Automatic rollback also requires a rollback-enabled bootloader, which an OTA update does not replace.
