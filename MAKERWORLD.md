# PrintSphere: MakerWorld Guide

PrintSphere is a standalone round status display for Bambu Lab printers based on the `Waveshare ESP32-S3 AMOLED 1.75`. This page is meant for MakerWorld users who want a quick overview of the project, the required hardware, the assembly steps, and the most important setup notes.

## What PrintSphere Can Do

PrintSphere can act as a small desktop companion display for supported Bambu printers.

Depending on printer model, source mode, and current code support, it can show:

- print progress
- printer lifecycle / current state
- remaining time
- temperatures
- layer information
- Wi-Fi and battery state
- cloud cover image and project title
- local camera snapshots
- printer error details and HMS information

It also supports:

- Bambu Cloud login from the built-in Web Config
- email-code / 2FA handling during cloud login
- hybrid cloud + local routing
- local MQTT path for supported models
- chamber light toggle on supported printers
- live arc-color customization from Web Config
- touch PIN unlock for protected Web Config access

## Hardware Overview

The project is built around the `Waveshare ESP32-S3 1.75 inch AMOLED Round Display` and a custom printed housing.

The printed housing is designed to combine:

- the display board
- a LiPo battery
- a Qi charging coil
- a small slide switch
- a magnetic base / ring element

## Parts List

The printed parts are part of the MakerWorld project.

Additional hardware:

- `ESP32-S3 1.75inch AMOLED Round Display x 1`
  - `https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm?sku=31261`
- `Lithium Polymer battery 3.7 V 2000 mAh 103454 x 1`
  - size: `10 mm x 34 mm x 54 mm`
- `Qi standard wireless charging coil x 1`
  - example source: `aliexpress.com/item/1005006741966938.html`
- `JST MX1.25 mm soft silicone cable x 2`
  - one for the battery
  - one to replace the cable on the wireless charger
- `Mini slide switch SS12D00G x 1`
  - knob length: `3 mm`
- `M2.5x25 mm screw x 1`
- `M2.5 nut x 1`
- `Magnetic circle ring plate sheet x 1`
  - example source: `aliexpress.com/item/1005006966440001.html`

## Before Assembly

- Print the case parts from the MakerWorld project.
- Make sure the battery dimensions fit your print and do not force the housing.
- Double-check LiPo polarity before connecting anything.
- Double-check the polarity and output of the Qi charging module before soldering or plugging it into the display.
- If you replace the charging cable with a JST cable, verify the pin order yourself before first power-on.

## Assembly Guide

The exact printed-part names and some orientation details still need to be filled in. The structure below is intended as the assembly flow for the MakerWorld page.

### 1. Print Preparation

- Print all case parts from the MakerWorld project.
- Clean the support material and test-fit all electronics before final assembly.
- `[Add exact printed-part names here]`

### 2. Prepare The Display Board

- Unpack the `Waveshare ESP32-S3 AMOLED 1.75` board.
- Check where the USB-C port, battery connector, and mounting points sit inside the case.
- `[Add note about board orientation in the case here]`

### 3. Prepare The Slide Switch

- Install the `SS12D00G` slide switch into the matching slot in the printed housing.
- Make sure the switch can move freely after insertion.
- `[Add exact switch orientation here]`
- `[Add note about whether the switch is glued, press-fit, or retained by the shell here]`

### 4. Prepare The Qi Charging Coil

- If needed, remove the original cable from the Qi charging coil.
- Solder the replacement `JST MX1.25 mm` soft silicone cable to the charging board / coil assembly.
- Route the cable so it does not sit under pressure once the case is closed.
- `[Add exact solder points and polarity here]`
- `[Add note about coil orientation in the housing here]`

### 5. Prepare The Battery Cable

- Use the second `JST MX1.25 mm` cable if your battery does not already match the required connector.
- Verify battery polarity with a multimeter before plugging it into the board.
- Never guess LiPo polarity.
- `[Add note here if a pre-crimped battery with correct plug is preferred]`

### 6. Install The Magnetic Ring

- Place the `magnetic circle ring plate sheet` into the intended recess of the printed base or rear shell.
- Ensure it sits flat and does not interfere with the charging coil.
- `[Add exact part location here]`
- `[Add note whether adhesive backing is enough or glue is recommended]`

### 7. Install The Qi Coil

- Place the Qi charging coil into its dedicated pocket in the printed part.
- Align it as centrally as possible for reliable charging.
- Route the cable through the intended channel.
- `[Add exact facing direction here]`
- `[Add note about spacer foam / tape if required]`

### 8. Install The Battery

- Place the `103454 2000 mAh` battery into the battery compartment.
- Make sure the battery is not bent, pinched, or compressed by the shell.
- Route the cable so it does not cross the screw channel or get trapped at the shell edge.
- `[Add whether tape or foam pad is recommended here]`

### 9. Install The Display Board

- Place the display board into the front shell / main housing.
- Check that the screen sits cleanly in the visible opening.
- Connect the battery.
- Connect the wireless charging lead if your design version uses it directly on the board.
- `[Add exact connector positions here]`
- `[Add photo or note for cable routing here]`

### 10. Final Cable Check

- Before closing the housing, confirm:
  - the switch moves freely
  - no cable is trapped under the board
  - the battery is not under pressure
  - the Qi coil cable is not crossing the screw path
  - all connectors are fully seated

### 11. Close The Housing

- Join the housing parts carefully.
- Insert the `M2.5x25 mm` screw.
- Secure it with the `M2.5` nut.
- Tighten only enough to hold the assembly safely.
- Do not overtighten and crush the printed part.
- `[Add exact screw direction here]`

### 12. First Power Test

- Turn the slide switch on.
- Connect USB-C once for the first test if needed.
- Check whether the board powers up normally.
- Check whether wireless charging reacts as expected.
- `[Add expected LED / screen behavior here]`

## Flashing The Firmware

1. Download the latest [`release/firmware.bin`](release/firmware.bin).
2. Connect the display to your computer with USB.
3. Flash the firmware with one of these tools:
   - `https://web.esphome.io/`
   - `https://www.espboards.dev/tools/program/`
   - `https://espressif.github.io/esptool-js/`
4. On `web.esphome.io`:
   - connect the device
   - choose the COM port
   - do not use `Prepare for first use`
   - install `firmware.bin` directly
5. On `espboards.dev` or `esptool-js`:
   - write `firmware.bin` to address `0x0`

The bootloader is already included in the merged image.

## Quick Setup Summary

1. After flashing, PrintSphere starts a setup AP:
   - SSID: `PrintSphere-Setup`
   - password: `printsphere`
2. Connect to that Wi-Fi and open `http://192.168.4.1`.
3. Save your home Wi-Fi credentials.
4. After reboot, open the new device IP in your home network.
5. Hold the display for about one second to show the Web Config PIN.
6. Enter the PIN in the browser.
7. In Web Config, connect Bambu Cloud:
   - email
   - password
   - region
8. If Bambu requests an email code or 2FA code, enter it there.
9. Optionally add the local printer path:
   - printer IP / hostname
   - printer serial number
   - access code

## Recommended Setup Notes

- For most users, start with `Hybrid`.
- A working Bambu Cloud login is currently the main real-world setup path.
- On some printers, cloud data may already be enough for progress, temperatures, remaining time, and layer information.
- If cloud data looks incomplete on your model, adding the local MQTT path may still improve the result.
- The printer serial number may be resolved automatically from the cloud path, but entering it manually can still help while testing.

## Model Notes

- `P1`, `A1`, and often `X1` family:
  - best current candidates for `Hybrid`
- `P2` and `H2` family:
  - often best to start cloud-first
- `P2S` local status is not supported in the current code
- `H2` local status may require Developer Mode

Camera notes:

- local JPEG camera support currently fits best for `A1`, `A1 Mini`, `P1P`, and `P1S`
- `X1`, `P2`, and `H2` families use an RTSP-style path in code, but that path is not considered finished for this hardware

## Troubleshooting

- If Web Config is locked, hold the display again to request a new PIN.
- If the device does not join your home Wi-Fi, reconnect to `PrintSphere-Setup` and recheck the credentials.
- If Bambu asks for a code during login, enter it directly in Web Config.
- If cloud setup appears to work but printer data is still incomplete, add the local printer path and test `Hybrid`.
- If the camera page matters to you, the strongest current local camera support is on `A1` and `P1` family printers.

## Notes For Future Expansion

Suggested placeholders to fill later:

- exact printed-part names
- switch orientation
- Qi charger wiring
- battery polarity / connector photos
- cable routing photos
- screw direction
- first-boot expected screen photo
