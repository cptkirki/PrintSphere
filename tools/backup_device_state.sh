#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPECTED_BRANCH="codex/round-amoled-printsphere"
PORT="/dev/cu.usbmodem101"
BAUD="460800"
BACKUP_ROOT="/Users/seanlee/Documents/data/device-backups/printsphere-round-amoled"
OUTPUT_DIR=""
DRY_RUN=0
FULL_FLASH=0

usage() {
  cat <<'USAGE'
Usage: tools/backup_device_state.sh [options]

Read-only backup helper for the Waveshare ESP32-S3-Touch-AMOLED-1.75
bambustat device before any future firmware flash.

Default behavior:
  - Refuses to run unless the git branch is codex/round-amoled-printsphere.
  - Uses /dev/cu.usbmodem101 unless --port is supplied.
  - Creates /Users/seanlee/Documents/data/device-backups/printsphere-round-amoled/YYYYmmdd-HHMMSS.
  - Reads only chip/device evidence and flash regions:
      partition_table  0x8000   0x1000
      nvs              0x9000   0x80000
      otadata          0x89000  0x2000
      phy_init         0x8B000  0x1000

Options:
  --port PORT       Serial port to read. Default: /dev/cu.usbmodem101.
  --baud BAUD       Serial baud rate. Default: 460800.
  --output-dir DIR  Explicit backup directory.
  --full-flash      Also read the full 16 MB flash to full_flash_16mb.bin.
  --dry-run         Print the read-only plan without connecting to a device.
  -h, --help        Show this help.

Safety:
  This script never runs write-flash, erase-flash, erase-region, upload,
  flash, monitor, reset-only helper targets, or any command that writes flash.
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
    --output-dir)
      OUTPUT_DIR="${2:?--output-dir requires a value}"
      shift
      ;;
    --full-flash)
      FULL_FLASH=1
      ;;
    --dry-run)
      DRY_RUN=1
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
  echo "Refusing backup: expected branch $EXPECTED_BRANCH, got ${branch:-unknown}" >&2
  exit 3
fi

timestamp="$(date +%Y%m%d-%H%M%S)"
if [[ -z "$OUTPUT_DIR" ]]; then
  OUTPUT_DIR="$BACKUP_ROOT/$timestamp"
fi

if command -v esptool.py >/dev/null 2>&1; then
  ESPTOOL=(esptool.py)
elif command -v esptool >/dev/null 2>&1; then
  ESPTOOL=(esptool)
else
  ESPTOOL=(python3 -m esptool)
fi

BASE_ARGS=(--chip esp32s3 --port "$PORT" --baud "$BAUD" --before default-reset --after hard-reset)

PARTITION_NAMES=(partition_table nvs otadata phy_init)
PARTITION_OFFSETS=(0x8000 0x9000 0x89000 0x8B000)
PARTITION_SIZES=(0x1000 0x80000 0x2000 0x1000)
PARTITION_FILES=(partition_table.bin nvs.bin otadata.bin phy_init.bin)

print_plan() {
  echo "bambustat round AMOLED read-only backup plan"
  echo "  root:       $ROOT"
  echo "  branch:     $branch"
  echo "  port:       $PORT"
  echo "  baud:       $BAUD"
  echo "  output:     $OUTPUT_DIR"
  echo "  esptool:    ${ESPTOOL[*]}"
  echo "  commands:"
  echo "    version"
  echo "    read-mac"
  echo "    chip-id"
  echo "    flash-id"
  echo "    get-security-info"
  echo "    generate device_summary.txt from read-only evidence"
  for i in "${!PARTITION_NAMES[@]}"; do
    echo "    read-flash ${PARTITION_OFFSETS[$i]} ${PARTITION_SIZES[$i]} ${PARTITION_FILES[$i]} (${PARTITION_NAMES[$i]})"
  done
  if [[ "$FULL_FLASH" -eq 1 ]]; then
    echo "    read-flash 0x0 0x1000000 full_flash_16mb.bin (full flash)"
  fi
}

if [[ "$DRY_RUN" -eq 1 ]]; then
  print_plan
  exit 0
fi

mkdir -p "$OUTPUT_DIR"

MANIFEST_TSV="$OUTPUT_DIR/manifest_items.tsv"
: > "$MANIFEST_TSV"

run_capture() {
  local label="$1"
  shift
  local outfile="$OUTPUT_DIR/$label.txt"
  echo "Running read-only command: $label"
  {
    echo "# command_category: read-only-evidence"
    printf "# command:"
    printf " %q" "${ESPTOOL[@]}" "${BASE_ARGS[@]}" "$@"
    printf "\n\n"
    "${ESPTOOL[@]}" "${BASE_ARGS[@]}" "$@"
  } >"$outfile" 2>&1
}

read_region() {
  local name="$1"
  local offset="$2"
  local size="$3"
  local file="$4"
  local path="$OUTPUT_DIR/$file"

  echo "Reading $name: offset=$offset size=$size -> $file"
  "${ESPTOOL[@]}" "${BASE_ARGS[@]}" read-flash "$offset" "$size" "$path" >"$OUTPUT_DIR/${name}_read_flash.log" 2>&1
  local digest
  digest="$(shasum -a 256 "$path" | awk '{print $1}')"
  printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$name" "$offset" "$size" "$file" "$digest" "read-flash" >> "$MANIFEST_TSV"
}

"${ESPTOOL[@]}" version >"$OUTPUT_DIR/esptool_version.txt" 2>&1 || true
run_capture read_mac read-mac
run_capture chip_id chip-id
run_capture flash_id flash-id
run_capture security_info get-security-info

for i in "${!PARTITION_NAMES[@]}"; do
  read_region "${PARTITION_NAMES[$i]}" "${PARTITION_OFFSETS[$i]}" "${PARTITION_SIZES[$i]}" "${PARTITION_FILES[$i]}"
done

if [[ "$FULL_FLASH" -eq 1 ]]; then
  read_region full_flash 0x0 0x1000000 full_flash_16mb.bin
fi

{
  echo "bambustat round AMOLED device evidence summary"
  echo "generated_at=$timestamp"
  echo "branch=$branch"
  echo "port=$PORT"
  echo "baud=$BAUD"
  echo
  for file in esptool_version.txt read_mac.txt chip_id.txt flash_id.txt security_info.txt; do
    echo "## $file"
    sed 's/[[:cntrl:]]//g' "$OUTPUT_DIR/$file" || true
    echo
  done
} > "$OUTPUT_DIR/device_summary.txt"

(
  cd "$OUTPUT_DIR"
  shasum -a 256 *.bin *.txt *.log 2>/dev/null | sort > SHA256SUMS
)

{
  echo "bambustat round AMOLED device backup manifest"
  echo "generated_at=$timestamp"
  echo "root=$ROOT"
  echo "branch=$branch"
  echo "port=$PORT"
  echo "baud=$BAUD"
  echo "output_dir=$OUTPUT_DIR"
  echo "full_flash=$FULL_FLASH"
  echo
  echo "Sensitive-data note: NVS can contain Wi-Fi, Bambu credentials, tokens, printer access codes, and settings."
  echo "Do not commit or share this backup."
  echo
  echo "items:"
  while IFS=$'\t' read -r name offset size file digest category; do
    printf "  - name=%s offset=%s size=%s file=%s sha256=%s command_category=%s\n" \
      "$name" "$offset" "$size" "$file" "$digest" "$category"
  done < "$MANIFEST_TSV"
} > "$OUTPUT_DIR/manifest.txt"

python3 - "$OUTPUT_DIR" "$MANIFEST_TSV" "$timestamp" "$ROOT" "$branch" "$PORT" "$BAUD" "$FULL_FLASH" <<'PY'
import json
import sys
from pathlib import Path

output_dir = Path(sys.argv[1])
tsv_path = Path(sys.argv[2])
generated_at, root, branch, port, baud, full_flash = sys.argv[3:9]

items = []
for line in tsv_path.read_text(encoding="utf-8").splitlines():
    if not line.strip():
        continue
    name, offset, size, file_name, sha256, category = line.split("\t")
    items.append({
        "name": name,
        "offset": offset,
        "size": size,
        "file": file_name,
        "sha256": sha256,
        "command_category": category,
    })

manifest = {
    "generated_at": generated_at,
    "project_root": root,
    "branch": branch,
    "port": port,
    "baud": baud,
    "full_flash": full_flash == "1",
    "sensitive_data_note": "NVS can contain Wi-Fi, Bambu credentials, tokens, printer access codes, and settings. Do not commit or share this backup.",
    "items": items,
}
(output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
PY

rm -f "$MANIFEST_TSV"
echo "Backup complete: $OUTPUT_DIR"
