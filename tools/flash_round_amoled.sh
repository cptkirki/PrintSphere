#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPECTED_BRANCH="codex/round-amoled-printsphere"
PORT="/dev/cu.usbmodem101"
BAUD="460800"
EXPECTED_MAC="28:84:85:91:ed:00"
IMAGE="$ROOT/release/firmware.bin"
EXPECTED_IMAGE_SHA="1c157bde292c02a7b8b6008cfe4191ed6257faa1e1c5b24ca57a959bf0f4a004"
BACKUP_DIR="/Users/seanlee/Documents/data/device-backups/printsphere-round-amoled/20260607-134549"
APP_BACKUP_DIR="/Users/seanlee/Documents/data/device-backups/printsphere-round-amoled/20260607-014242-app-partitions"
DRY_RUN=1
WRITE_CONFIRM=0

usage() {
  cat <<'USAGE'
Usage: tools/flash_round_amoled.sh [options]

Guarded flash helper for the Waveshare ESP32-S3-Touch-AMOLED-1.75
round bambustat device.

Default behavior:
  - DRY RUN ONLY. It prints the planned checks and write-flash command.
  - Refuses to run unless branch is codex/round-amoled-printsphere.
  - Defaults to protected port /dev/cu.usbmodem101.
  - Requires the pre-flash backup SHA manifest to verify.
  - Requires firmware SHA to match the accepted v1.6-beta1 package.

Options:
  --port PORT       Serial port. Default: /dev/cu.usbmodem101.
  --baud BAUD       Baud rate. Default: 460800.
  --image FILE      Merged flash image. Default: release/firmware.bin.
  --backup-dir DIR  Pre-flash backup directory.
  --app-backup DIR  App partition backup directory for rollback readiness.
  --execute         Leave dry-run mode.
  --yes-i-understand-this-writes-flash
                    Required together with --execute to actually write flash.
  -h, --help        Show this help.

Safety:
  This script writes flash only when BOTH --execute and
  --yes-i-understand-this-writes-flash are supplied.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT="${2:?--port requires a value}"
      shift
      ;;
    --baud)
      BAUD="${2:?--baud requires a value}"
      shift
      ;;
    --image)
      IMAGE="${2:?--image requires a value}"
      shift
      ;;
    --backup-dir)
      BACKUP_DIR="${2:?--backup-dir requires a value}"
      shift
      ;;
    --app-backup)
      APP_BACKUP_DIR="${2:?--app-backup requires a value}"
      shift
      ;;
    --execute)
      DRY_RUN=0
      ;;
    --yes-i-understand-this-writes-flash)
      WRITE_CONFIRM=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

cd "$ROOT"

branch="$(git branch --show-current 2>/dev/null || true)"
if [[ "$branch" != "$EXPECTED_BRANCH" ]]; then
  echo "Refusing flash: expected branch $EXPECTED_BRANCH, got ${branch:-unknown}" >&2
  exit 3
fi

if [[ ! -e "$PORT" ]]; then
  echo "Refusing flash: port not found: $PORT" >&2
  exit 4
fi

if [[ ! -f "$IMAGE" ]]; then
  echo "Refusing flash: image not found: $IMAGE" >&2
  exit 5
fi

actual_sha="$(shasum -a 256 "$IMAGE" | awk '{print $1}')"
if [[ "$actual_sha" != "$EXPECTED_IMAGE_SHA" ]]; then
  echo "Refusing flash: image SHA mismatch" >&2
  echo "  expected: $EXPECTED_IMAGE_SHA" >&2
  echo "  actual:   $actual_sha" >&2
  exit 6
fi

if [[ ! -f "$BACKUP_DIR/SHA256SUMS" ]]; then
  echo "Refusing flash: state backup SHA256SUMS not found under $BACKUP_DIR" >&2
  exit 7
fi
if [[ ! -f "$APP_BACKUP_DIR/SHA256SUMS" ]]; then
  echo "Refusing flash: app backup SHA256SUMS not found under $APP_BACKUP_DIR" >&2
  exit 7
fi

(
  cd "$BACKUP_DIR"
  shasum -a 256 -c SHA256SUMS >/dev/null
)
(
  cd "$APP_BACKUP_DIR"
  shasum -a 256 -c SHA256SUMS >/dev/null
)

if command -v esptool.py >/dev/null 2>&1; then
  ESPTOOL=(esptool.py)
elif command -v esptool >/dev/null 2>&1; then
  ESPTOOL=(esptool)
else
  ESPTOOL=(python3 -m esptool)
fi

FLASH_CMD=(
  "${ESPTOOL[@]}"
  --chip esp32s3
  --port "$PORT"
  --baud "$BAUD"
  --before default-reset
  --after hard-reset
  write-flash
  --flash-mode dio
  --flash-size 16MB
  --flash-freq 80m
  0x0 "$IMAGE"
)

print_plan() {
  cat <<PLAN
bambustat round AMOLED guarded flash plan
  root:       $ROOT
  branch:     $branch
  port:       $PORT
  expected:   $EXPECTED_MAC
  image:      $IMAGE
  image_sha:  $actual_sha
  state bkp:  $BACKUP_DIR
  app bkp:    $APP_BACKUP_DIR
  esptool:    ${ESPTOOL[*]}
  write cmd:  ${FLASH_CMD[*]}

Post-flash acceptance:
  1. Verify serial project/version: bambustat_idf v1.6-beta1.
  2. Verify round 466x466 AMOLED lights and does not boot-loop.
  3. Verify Bambu status, touch/long-press unlock, and Web Config.
  4. Capture focused camera evidence.
  5. Re-check WS200 at http://192.168.31.10 remains online.
PLAN
}

print_plan

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo
  echo "Dry run only. Add --execute --yes-i-understand-this-writes-flash after explicit approval to write flash."
  exit 0
fi

if [[ "$WRITE_CONFIRM" -ne 1 ]]; then
  echo "Refusing flash: --execute requires --yes-i-understand-this-writes-flash." >&2
  exit 8
fi

read_mac_output="$("${ESPTOOL[@]}" --chip esp32s3 --port "$PORT" --baud "$BAUD" --before default-reset --after hard-reset read-mac 2>&1)"
echo "$read_mac_output"
if ! grep -Eqi "MAC:[[:space:]]+$EXPECTED_MAC" <<<"$read_mac_output"; then
  echo "Refusing flash: device MAC did not match $EXPECTED_MAC." >&2
  exit 9
fi

echo "Writing flash to $PORT..."
"${FLASH_CMD[@]}"
