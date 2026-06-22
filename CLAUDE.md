# bambustat Round AMOLED Project Notes

Current project name from 2026-06-07: `bambustat`. Keep `PrintSphere` only
when referring to the upstream/reference project, legacy code namespace,
historical reports, or compatibility paths.

## Objective

Use the PrintSphere codebase as the upstream base for the Waveshare ESP32-S3-Touch-AMOLED-1.75 round screen, but ship the visible Bambu printer status experience as `bambustat` without disturbing the 7 inch WS700 or 2 inch WS200 branches.

## Storage Rule

- GitHub-suitable development progress, source changes, build scripts, and acceptance docs should be committed to an appropriate GitHub-backed bambustat/round-AMOLED repository or fork. Do not push Sean-specific changes directly to the third-party upstream `cptkirki/PrintSphere` unless explicitly approved.
- Raw/private/bulky hardware evidence, device backups, NVS-containing files, camera captures, and local acceptance archives that are not suitable for the project repo must be archived locally and indexed from Obsidian/KnowledgeVault; KnowledgeVault itself should sync through GitHub.

## Screen Branch Isolation

Human rule from 2026-06-06: keep each physical screen on its own branch, and keep the 7 inch WS700 screen saved separately.

- This worktree is only for the 1.75 inch round AMOLED / bambustat track.
- Before editing, confirm `git branch --show-current` is `codex/round-amoled-printsphere`.
- Do not port unrelated 7 inch WS700 or 2 inch WS200 code into this branch mechanically.
- 7 inch WS700 preservation lives in `/Users/seanlee/Documents/bambuhelper-ws700-ws700-preserve` on `codex/ws700-preserve`.
- 2 inch WS200 BambuHelper work lives in `/Users/seanlee/Documents/bambuhelper-ws700-ws200-port` on `codex/ws200-port`.
- The mixed `/Users/seanlee/Documents/bambuhelper-ws700` `master` worktree is reference/recovery only during the screen split; do not add new screen-specific changes there.

## Device Safety

- Do not flash `/dev/cu.usbmodem101` without explicit human approval.
- Treat the current physical round AMOLED as preserving a working legacy PrintSphere v1.5.1 state until a versioned bambustat migration plan is accepted.
- Do build-only and source-audit passes before any future flash.
- Before any approved future flash, run a read-only state backup with `./tools/backup_device_state.sh` and archive the resulting path. This backs up the current partition table, NVS, OTA data, PHY init, and device evidence without writing flash.

## Build-Only Verification

- Use `./tools/build_only.sh` for local compile checks.
- The script must not flash, upload, monitor, or write to serial ports.
- Preferred toolchain lane is ESP-IDF `>= v6.0.0`, because `main/idf_component.yml` declares `idf: ">=6.0.0"`.
- If using Docker, prefer the official Espressif image `espressif/idf:release-v6.0`. Do not pull large images automatically from scripts; make the download decision explicit in the execution report.

## Device-State Backup

- Use `./tools/backup_device_state.sh --dry-run` first to confirm the exact read-only plan.
- Default backup target is `/dev/cu.usbmodem101`; override with `--port` only after device identity is verified.
- Default output root is `/Users/seanlee/Documents/data/device-backups/printsphere-round-amoled/`.
- The backup can contain Wi-Fi credentials, Bambu credentials/tokens, printer access codes, and local settings inside `nvs.bin`; do not commit, upload, or share backup contents.
- `--full-flash` is optional and off by default. Use it only when a full 16 MB pre-flash image is actually needed.

## Guarded Flash Helper

- `./tools/flash_round_amoled.sh` is the only approved local helper for flashing this branch's round AMOLED firmware.
- `./tools/run_round_amoled_flash_acceptance.sh` is the approved wrapper for an explicitly approved flash + verification pass; it must delegate the actual write to `flash_round_amoled.sh`.
- The helper defaults to dry-run and must remain that way.
- Do not run it with `--execute --yes-i-understand-this-writes-flash` unless the human explicitly approves flashing `/dev/cu.usbmodem101`.
- The helper must keep checking:
  - branch `codex/round-amoled-printsphere`
  - protected port `/dev/cu.usbmodem101`
  - expected MAC `28:84:85:91:ed:00`
  - accepted firmware SHA
  - pre-flash backup SHA manifest
  - app partition rollback backup SHA manifest

## Acceptance

- Build success is not physical acceptance.
- Any future flash must record device identity, firmware version, and camera evidence.
- `./tools/verify_round_amoled.sh` defaults to HTTP-only to avoid USB-CDC serial-open resets and transient UI mutations; use `--capture-serial` only when boot/version logs are needed, and `--simulate-touch` only for acceptance runs.
- `./tools/run_round_amoled_flash_acceptance.sh` must generate `acceptance_checks.json`; round AMOLED physical acceptance requires all checks to pass, including serial project `bambustat_idf` and version `v1.6-beta1`.
- Touch validation should use software/debug endpoints first before asking the human to tap manually.
