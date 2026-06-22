#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="/dev/cu.usbmodem101"
HOST="192.168.31.196"
SERIAL_SECONDS=12
OUTPUT_ROOT="/Users/seanlee/Documents/camera-checks/bambu-display-2026-06-07/round-amoled-verify"
CAPTURE_CAMERA=0
CAPTURE_SERIAL=0
SIMULATE_TOUCH=0
DRY_RUN=0

usage() {
  cat <<'USAGE'
Usage: tools/verify_round_amoled.sh [options]

Read-only verification helper for the round AMOLED bambustat device.

Options:
  --port PORT           Serial port. Default: /dev/cu.usbmodem101.
  --host HOST           Device host/IP. Default: 192.168.31.196.
  --serial-seconds N    Serial capture duration. Default: 12.
  --output-dir DIR      Evidence directory. Default under camera-checks.
  --capture-serial      Capture serial logs. This may reset USB-CDC devices.
  --capture-camera      Capture one imagesnap photo into the evidence directory.
  --simulate-touch      POST /api/touch/simulate long_press and re-check health.
  --dry-run             Print planned read-only checks without touching device.
  -h, --help            Show this help.

Safety:
  This script is read-only. It does not write flash, erase flash, upload
  firmware, or unlock Web Config. Default mode avoids opening the serial port
  because that can reset USB-CDC devices; pass --capture-serial when serial
  boot/version evidence is required. Touch simulation is opt-in because it
  changes transient UI state by requesting the same unlock-PIN flow as a
  physical long press.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT="${2:?--port requires a value}"
      shift
      ;;
    --host)
      HOST="${2:?--host requires a value}"
      shift
      ;;
    --serial-seconds)
      SERIAL_SECONDS="${2:?--serial-seconds requires a value}"
      shift
      ;;
    --output-dir)
      OUTPUT_ROOT="${2:?--output-dir requires a value}"
      shift
      ;;
    --capture-camera)
      CAPTURE_CAMERA=1
      ;;
    --capture-serial)
      CAPTURE_SERIAL=1
      ;;
    --simulate-touch)
      SIMULATE_TOUCH=1
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

timestamp="$(date +%Y%m%d-%H%M%S)"
OUT="$OUTPUT_ROOT/$timestamp"

print_plan() {
  cat <<PLAN
Round AMOLED read-only verification plan
  root:           $ROOT
  port:           $PORT
  host:           $HOST
  serial_seconds: $SERIAL_SECONDS
  serial_capture: $CAPTURE_SERIAL
  simulate_touch: $SIMULATE_TOUCH
  output:         $OUT
  camera:         $CAPTURE_CAMERA

Checks:
  - HTTP GET /api/health
  - HTTP GET /api/status (raw body, may be 404/locked depending firmware)
  - optional HTTP POST /api/touch/simulate long_press, then re-check /api/health
  - optional serial capture for boot/version/status lines
  - optional imagesnap photo
PLAN
}

print_plan

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "Dry run only."
  exit 0
fi

mkdir -p "$OUT"

{
  echo "generated_at=$timestamp"
  echo "root=$ROOT"
  echo "port=$PORT"
  echo "host=$HOST"
  echo "serial_capture=$CAPTURE_SERIAL"
  echo "simulate_touch=$SIMULATE_TOUCH"
  echo "serial_seconds=$SERIAL_SECONDS"
} > "$OUT/manifest.txt"

curl --silent --show-error --max-time 8 -i "http://$HOST/api/health" > "$OUT/api_health.http" 2>&1 || true
curl --silent --show-error --max-time 8 -i "http://$HOST/api/status" > "$OUT/api_status.http" 2>&1 || true
if [[ "$SIMULATE_TOUCH" -eq 1 ]]; then
  curl --silent --show-error --max-time 8 -i \
    -X POST "http://$HOST/api/touch/simulate" \
    -H "Content-Type: application/json" \
    --data '{"gesture":"long_press"}' \
    > "$OUT/api_touch_simulate.http" 2>&1 || true
  sleep 1
  curl --silent --show-error --max-time 8 -i "http://$HOST/api/health" > "$OUT/api_health_after_touch.http" 2>&1 || true
else
  echo "touch simulation skipped; pass --simulate-touch for acceptance runs" > "$OUT/api_touch_simulate.skipped.txt"
fi

if [[ "$CAPTURE_SERIAL" -eq 1 ]]; then
python3 - "$PORT" "$SERIAL_SECONDS" "$OUT/serial_capture.log" <<'PY' || true
import sys
import time
from pathlib import Path

import serial

port = sys.argv[1]
seconds = float(sys.argv[2])
out = Path(sys.argv[3])

deadline = time.time() + seconds
with out.open("wb") as fh:
    try:
        ser = serial.Serial(port, 115200, timeout=0.2, exclusive=False)
        ser.dtr = False
        ser.rts = False
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                fh.write(chunk)
        ser.close()
    except Exception as exc:
        fh.write(f"serial capture failed: {exc}\n".encode())
PY
else
  echo "serial capture skipped; pass --capture-serial when boot/version logs are required" > "$OUT/serial_capture.skipped.txt"
fi

if [[ "$CAPTURE_CAMERA" -eq 1 ]]; then
  if command -v imagesnap >/dev/null 2>&1; then
    imagesnap -q -w 2 "$OUT/round_amoled.jpg" >/dev/null 2>&1 || true
  else
    echo "imagesnap not found" > "$OUT/round_amoled_camera.txt"
  fi
fi

(
  cd "$OUT"
  shasum -a 256 * 2>/dev/null | sort > SHA256SUMS
)

echo "Verification evidence written: $OUT"
