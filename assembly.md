## Before Assembly

- Print the case parts from the MakerWorld project.
- Make sure the battery dimensions fit your print and do not force the housing.
- Double-check LiPo polarity before connecting anything.
- Double-check the polarity and output of the Qi charging module before soldering it to the display.
- If you replace the charging cable with a JST cable, verify the pin order yourself before first power-on.

## Assembly Guide


### 1. Print Preparation

- Print all case parts from the MakerWorld project.
- Clean the support material and test-fit all electronics before final assembly.

### 2. Prepare The Display Board

- Unpack the `Waveshare ESP32-S3 AMOLED 1.75` board.
- Check where the USB-C port, battery connector, and mounting points sit inside the case.
  
### 3. Prepare The Battery Cable

- Use the second `JST MX1.25 mm` cable if your battery does not already match the required connector.
- Verify battery polarity with a multimeter before plugging it into the board.
- Never guess LiPo polarity.
- cut one wire of (+ or -) 
- solder the cut wire to the slide switch and add heat-shrink tubing to the switch contacts.
<img width="500" height="666" alt="image" src="https://github.com/user-attachments/assets/9eb3d04a-818d-43c4-bdc2-1852d4b4e9c8" />

### 4. Prepare The Slide Switch

- Install the `SS12D00G` slide switch into the matching slot in the printed housing.
- Make sure the switch can move freely after insertion.
- secure the switch with a drop of glue (superglue or hotglue) be careful not to block the switch's function

### 5. Prepare The Qi Charging Coil

- If needed, remove the original cable from the Qi charging coil.
- Solder the replacement `JST MX1.25 mm` soft silicone cable to the charging board / coil assembly.
- Route the cable so it does not sit under pressure once the case is closed.

### 6. Install The Magnetic Ring

- Place the `magnetic circle ring plate sheet` into the intended recess of the printed base or rear shell.
- ensure the magnetic polarity (test it with a magnetic charger) before glueing it in place with the pre-attached double sided tape

### 7. Install The Qi Coil

- Place the Qi charging coil into its dedicated pocket in the printed part.
- Route the cable through the intended channel
- be careful with the thin copper wires
- secure the charging coil with the printed circle

### 8. Install The Battery

- Place the `103454 2000 mAh` battery into the battery compartment.
- Make sure the battery is not bent, pinched, or compressed by the shell.
- Route the cable so it does not cross the screw channel or get trapped at the shell edge.
<img width="500" height="666" alt="image" src="https://github.com/user-attachments/assets/251a2e2c-1684-4d05-9bea-ddafe645daab" />

### 9. Mount the Display

- Place the display in the printed display frame
- Secure the display using the screws provided, or use M.2 screws such as M2 x 3 mm

### 9. Wiring

- Thread the battery cable and the wireless charging coil cable together through a piece (1.5 cm) of heat-shrink tubing and shrink it in the area of the hinge.
- Feed the bundle through the opening in the lid into the interior of the upper section 
<img width="500" height="666" alt="image" src="https://github.com/user-attachments/assets/efe68972-e904-4b0f-9413-e00f833c5df4" />
<img width="1119" height="893" alt="image" src="https://github.com/user-attachments/assets/0b96cf91-3a6e-4a7b-ad6a-4509580cb9de" />

### 9. Connect the Board

- remove the pin header gently with some small side-cutter pliers
- Solder the cables to the circuit board, make sure to connect positive (+5V) to VBUS and negative to GND.
- switch the `SS12D00G` slide switch to off position then connect the battery to the board.
<img width="500" height="666" alt="image" src="https://github.com/user-attachments/assets/c3dc9121-f1b9-48ed-a6a7-6a921a6a6859" />

### 10. Final Cable Check

- Before closing the housing, confirm:
  - the switch moves freely
  - no cable is trapped under the board
  - the battery is not under pressure
  - the Qi coil cable is not crossing the screw path
  - all connectors are fully seated

### 11. First Power Test

- Turn the slide switch on.
- Connect USB-C once for the first test if needed.
- Check whether the board powers up normally.
- Check whether wireless charging reacts as expected.

### 12. Close The Housing

- The display frame with the display can only be inserted into the lid in one orientation.
- Be careful not to pinch any cables when inserting the frame into the cover
- Once the frame is flush with the cover, use the printed key ring to snap the display into place by turning it clockwise

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
5. In Web Config, connect Bambu Cloud:
   - email
   - password
   - region
6. If Bambu requests an email code or 2FA code, enter it there.
7. Optionally add the local printer path:
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
- `X1`, `P2`, and `H2` families use an RTSP-style path in code, but that path is not yet implemented for this hardware

## Troubleshooting

- If Web Config is locked, hold the display again to request a new PIN.
- If the device does not join your home Wi-Fi, reconnect to `PrintSphere-Setup` and recheck the credentials.
- If Bambu asks for a code during login, enter it directly in Web Config and then click "Submit Code"
- If cloud setup appears to work but printer data is still incomplete, add the local printer path and test `Hybrid`.
- If the camera page matters to you, the strongest current local camera support is on `A1` and `P1` family printers.

