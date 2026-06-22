#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPECTED_BRANCH="codex/round-amoled-printsphere"
PORT="/dev/cu.usbmodem101"
BAUD="230400"
EXPECTED_MAC="28:84:85:91:ed:00"
STATE_BACKUP_DIR="/Users/seanlee/Documents/data/device-backups/printsphere-round-amoled/20260607-011831"
APP_BACKUP_DIR="/Users/seanlee/Documents/data/device-backups/printsphere-round-amoled/20260607-014242-app-partitions"
DRY_RUN=1
WRITE_CONFIRM=0

usage() {
  cat <<'USAGE'
Usage: tools/restore_round_amoled_apps.sh [options]

Guarded rollback helper for restoring the previously backed-up round AMOLED
application partitions and settings/state partitions.

Default behavior:
  - DRY RUN ONLY.
  - Refuses to run unless branch is codex/round-amoled-printsphere.
  - Verifies backup SHA manifests before any write.
  - Writes flash only with BOTH --execute and
    --yes-i-understand-this-writes-flash.

Options:
  --port PORT          Serial port. Default: /dev/cu.usbmodem101.
  --baud BAUD          Baud rate. Default: 230400.
  --state-backup DIR   Backup dir containing nvs.bin/otadata.bin/phy_init.bin.
  --app-backup DIR     Backup dir containing ota_0.bin/ota_1.bin.
  --execute            Leave dry-run mode.
  --yes-i-understand-this-writes-flash
                       Required with --execute to write flash.
  -h, --help           Show this help.

Safety:
  This restores app and state partitions only. It does not erase flash and
  does not restore a bootloader. Use only after explicit approval.
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
    --state-backup)
      STATE_BACKUP_DIR="${2:?--state-backup requires a value}"
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
  echo "Refusing restore: expected branch $EXPECTED_BRANCH, got ${branch:-unknown}" >&2
  exit 3
fi

for f in "$STATE_BACKUP_DIR/SHA256SUMS" "$APP_BACKUP_DIR/SHA256SUMS"; do
  if [[ ! -f "$f" ]]; then
    echo "Refusing restore: missing SHA manifest $f" >&2
    exit 4
  fi
done

for f in "$STATE_BACKUP_DIR/nvs.bin" "$STATE_BACKUP_DIR/otadata.bin" "$STATE_BACKUP_DIR/phy_init.bin" \
         "$APP_BACKUP_DIR/ota_0.bin" "$APP_BACKUP_DIR/ota_1.bin"; do
  if [[ ! -f "$f" ]]; then
    echo "Refusing restore: missing backup file $f" >&2
    exit 5
  fi
done

(
  cd "$STATE_BACKUP_DIR"
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

RESTORE_CMD=(
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
  0x9000 "$STATE_BACKUP_DIR/nvs.bin"
  0x89000 "$STATE_BACKUP_DIR/otadata.bin"
  0x8B000 "$STATE_BACKUP_DIR/phy_init.bin"
  0x90000 "$APP_BACKUP_DIR/ota_0.bin"
  0x590000 "$APP_BACKUP_DIR/ota_1.bin"
)

cat <<PLAN
Round AMOLED guarded app/settings restore plan
  root:         $ROOT
  branch:       $branch
  port:         $PORT
  expected:     $EXPECTED_MAC
  state backup: $STATE_BACKUP_DIR
  app backup:   $APP_BACKUP_DIR
  esptool:      ${ESPTOOL[*]}
  write cmd:    ${RESTORE_CMD[*]}

This restores NVS, OTA data, PHY init, ota_0, and ota_1.
It does not erase flash and does not restore bootloader.
PLAN

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo
  echo "Dry run only. Add --execute --yes-i-understand-this-writes-flash after explicit approval to restore."
  exit 0
fi

if [[ "$WRITE_CONFIRM" -ne 1 ]]; then
  echo "Refusing restore: --execute requires --yes-i-understand-this-writes-flash." >&2
  exit 8
fi

read_mac_output="$("${ESPTOOL[@]}" --chip esp32s3 --port "$PORT" --baud "$BAUD" --before default-reset --after hard-reset read-mac 2>&1)"
echo "$read_mac_output"
if ! grep -qi "MAC: $EXPECTED_MAC" <<<"$read_mac_output"; then
  echo "Refusing restore: device MAC did not match $EXPECTED_MAC." >&2
  exit 9
fi

echo "Restoring app/settings partitions to $PORT..."
"${RESTORE_CMD[@]}"
