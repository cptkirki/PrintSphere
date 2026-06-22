#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="/dev/cu.usbmodem101"
HOST="192.168.31.196"
WS200_HOST="192.168.31.10"
OUTPUT_ROOT="/Users/seanlee/Documents/camera-checks/bambu-display-2026-06-07/round-amoled-acceptance"
EXECUTE=0
WRITE_CONFIRM=0
CAPTURE_CAMERA=1
EXPECTED_PROJECT="bambustat_idf"
EXPECTED_VERSION="v1.6-beta1"
HTTP_WAIT_SECONDS=90
VALIDATE_EXISTING=""

usage() {
  cat <<'USAGE'
Usage: tools/run_round_amoled_flash_acceptance.sh [options]

Orchestrate the approved round AMOLED flash and acceptance sequence.

Default behavior:
  - DRY RUN ONLY. It prints the full sequence.
  - Does not write flash unless BOTH --execute and
    --yes-i-understand-this-writes-flash are supplied.
  - Delegates the actual write to tools/flash_round_amoled.sh, which has its
    own branch, port, MAC, image SHA, and backup checks.

Options:
  --port PORT       Round AMOLED serial port. Default: /dev/cu.usbmodem101.
  --host HOST       Round AMOLED IP/host after boot. Default: 192.168.31.196.
  --ws200 HOST      WS200 host for regression check. Default: 192.168.31.10.
  --output-dir DIR  Acceptance evidence directory root.
  --http-wait N     Seconds to wait for round HTTP health. Default: 90.
  --validate-existing DIR
                  Re-run acceptance validation on an existing evidence dir.
                  This does not flash or touch devices.
  --no-camera       Skip imagesnap camera capture during verification.
  --execute         Run the sequence.
  --yes-i-understand-this-writes-flash
                    Required together with --execute to flash.
  -h, --help        Show this help.

Safety:
  This script is a wrapper. It only writes flash through flash_round_amoled.sh,
  and only when the same explicit confirmation flags are present.
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
    --ws200)
      WS200_HOST="${2:?--ws200 requires a value}"
      shift
      ;;
    --output-dir)
      OUTPUT_ROOT="${2:?--output-dir requires a value}"
      shift
      ;;
    --http-wait)
      HTTP_WAIT_SECONDS="${2:?--http-wait requires a value}"
      shift
      ;;
    --validate-existing)
      VALIDATE_EXISTING="${2:?--validate-existing requires a value}"
      shift
      ;;
    --no-camera)
      CAPTURE_CAMERA=0
      ;;
    --execute)
      EXECUTE=1
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

timestamp="$(date +%Y%m%d-%H%M%S)"
OUT="$OUTPUT_ROOT/$timestamp"

VERIFY_ARGS=(--port "$PORT" --host "$HOST" --output-dir "$OUT/round_verify" --capture-serial --simulate-touch)
if [[ "$CAPTURE_CAMERA" -eq 1 ]]; then
  VERIFY_ARGS+=(--capture-camera)
fi

wait_for_round_http() {
  local deadline=$((SECONDS + HTTP_WAIT_SECONDS))
  while [[ "$SECONDS" -le "$deadline" ]]; do
    if curl --silent --show-error --max-time 5 "http://$HOST/api/health" >"$OUT/round_health_poll.json" 2>"$OUT/round_health_poll.err"; then
      return 0
    fi
    sleep 3
  done
  echo "Round HTTP health did not recover within ${HTTP_WAIT_SECONDS}s." >&2
  return 1
}

validate_acceptance() {
  local round_dir="$OUT/round_verify"
  local serial_log
  serial_log="$(find "$round_dir" -maxdepth 2 -name serial_capture.log -print -quit 2>/dev/null || true)"

  python3 - "$OUT" "$round_dir" "$serial_log" "$EXPECTED_PROJECT" "$EXPECTED_VERSION" <<'PY'
import json
import re
import sys
from pathlib import Path

out = Path(sys.argv[1])
round_dir = Path(sys.argv[2])
serial_log = Path(sys.argv[3]) if sys.argv[3] else None
expected_project = sys.argv[4]
expected_version = sys.argv[5]

checks = []

def record(name, ok, detail):
    checks.append({"name": name, "ok": bool(ok), "detail": str(detail)})

health_files = sorted(round_dir.glob("*/api_health.http")) or sorted(round_dir.glob("api_health.http"))
health_text = health_files[-1].read_text(encoding="utf-8", errors="replace") if health_files else ""
record("round_api_health_http_200", "HTTP/1.1 200" in health_text or "HTTP/1.0 200" in health_text, health_files[-1] if health_files else "missing api_health.http")

touch_files = sorted(round_dir.glob("*/api_touch_simulate.http")) or sorted(round_dir.glob("api_touch_simulate.http"))
touch_text = touch_files[-1].read_text(encoding="utf-8", errors="replace") if touch_files else ""
record("round_touch_simulate_http_200", "HTTP/1.1 200" in touch_text or "HTTP/1.0 200" in touch_text, touch_files[-1] if touch_files else "missing api_touch_simulate.http")

health_after_files = sorted(round_dir.glob("*/api_health_after_touch.http")) or sorted(round_dir.glob("api_health_after_touch.http"))
health_after_text = health_after_files[-1].read_text(encoding="utf-8", errors="replace") if health_after_files else ""
health_after_compact = health_after_text.replace(" ", "").replace("\n", "")
record("round_touch_long_press_pin_active", '"portal_pin_active":true' in health_after_compact, health_after_files[-1] if health_after_files else "missing api_health_after_touch.http")

if serial_log and serial_log.exists():
    serial_text = serial_log.read_text(encoding="utf-8", errors="replace")
    record("round_serial_project", expected_project in serial_text, expected_project)
    record("round_serial_version", expected_version in serial_text, expected_version)
else:
    record("round_serial_project", False, "missing serial_capture.log")
    record("round_serial_version", False, "missing serial_capture.log")

camera_files = list(round_dir.glob("*/round_amoled.jpg")) + list(round_dir.glob("round_amoled.jpg"))
valid_camera_files = [path for path in camera_files if path.exists() and path.stat().st_size > 0]
record("round_camera_evidence", bool(valid_camera_files), valid_camera_files[-1] if valid_camera_files else "missing or empty round_amoled.jpg")

ws200_path = out / "ws200_debug.json"
try:
    ws200 = json.loads(ws200_path.read_text(encoding="utf-8"))
    slots = ws200.get("printers") or ws200.get("slots") or []
    slot_checks = []
    for slot in slots:
        name = slot.get("name") or slot.get("serial") or slot.get("slot")
        rc = slot.get("last_rc", slot.get("mqtt_last_rc"))
        slot_checks.append((name, rc))
    ok = bool(slot_checks) and any(rc == 0 for _, rc in slot_checks)
    record("ws200_primary_live_regression", ok, slot_checks)
except Exception as exc:
    record("ws200_primary_live_regression", False, f"{type(exc).__name__}: {exc}")

ok = all(item["ok"] for item in checks)
(out / "acceptance_checks.json").write_text(json.dumps({"ok": ok, "checks": checks}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
(out / "acceptance_summary.txt").write_text("\n".join(f"{'PASS' if c['ok'] else 'FAIL'} {c['name']}: {c['detail']}" for c in checks) + "\n", encoding="utf-8")
if not ok:
    sys.exit(20)
PY
}

if [[ -n "$VALIDATE_EXISTING" ]]; then
  OUT="$VALIDATE_EXISTING"
  if [[ ! -d "$OUT" ]]; then
    echo "Existing acceptance directory not found: $OUT" >&2
    exit 9
  fi
  validate_acceptance
  echo "Acceptance validation re-run for: $OUT"
  exit 0
fi

cat <<PLAN
Round AMOLED approved flash acceptance plan
  root:       $ROOT
  port:       $PORT
  host:       $HOST
  ws200:      $WS200_HOST
  output:     $OUT
  camera:     $CAPTURE_CAMERA
  http_wait:  $HTTP_WAIT_SECONDS

Sequence:
  1. Preflight dry-run flash helper checks.
  2. If explicitly confirmed, write v1.6-beta1 to $PORT.
  3. Wait for reboot/network HTTP health.
  4. Run read-only round verification into $OUT/round_verify.
  5. Capture WS200 /debug regression into $OUT/ws200_debug.json.
  6. Validate serial version, HTTP health, simulated touch/PIN, camera evidence, and WS200 regression health.
  7. Write acceptance manifest.
PLAN

"$ROOT/tools/flash_round_amoled.sh" --port "$PORT" >/tmp/round-acceptance-flash-plan.txt
sed -n '1,120p' /tmp/round-acceptance-flash-plan.txt

if [[ "$EXECUTE" -ne 1 ]]; then
  echo
  echo "Dry run only. Add --execute --yes-i-understand-this-writes-flash after explicit approval to flash and verify."
  exit 0
fi

if [[ "$WRITE_CONFIRM" -ne 1 ]]; then
  echo "Refusing acceptance run: --execute requires --yes-i-understand-this-writes-flash." >&2
  exit 8
fi

mkdir -p "$OUT"

{
  echo "generated_at=$timestamp"
  echo "root=$ROOT"
  echo "port=$PORT"
  echo "host=$HOST"
  echo "ws200=$WS200_HOST"
  echo "expected_project=$EXPECTED_PROJECT"
  echo "expected_version=$EXPECTED_VERSION"
  echo "http_wait_seconds=$HTTP_WAIT_SECONDS"
  echo "phase=starting"
} > "$OUT/manifest.txt"

"$ROOT/tools/flash_round_amoled.sh" \
  --port "$PORT" \
  --execute \
  --yes-i-understand-this-writes-flash \
  | tee "$OUT/flash.log"

wait_for_round_http

"$ROOT/tools/verify_round_amoled.sh" "${VERIFY_ARGS[@]}" | tee "$OUT/round_verify.log"

curl --silent --show-error --max-time 8 "http://$WS200_HOST/debug" > "$OUT/ws200_debug.json" 2>"$OUT/ws200_debug.err" || true

validate_acceptance

{
  echo "generated_at=$timestamp"
  echo "root=$ROOT"
  echo "port=$PORT"
  echo "host=$HOST"
  echo "ws200=$WS200_HOST"
  echo "expected_project=$EXPECTED_PROJECT"
  echo "expected_version=$EXPECTED_VERSION"
  echo "phase=completed"
  echo "round_verify_dir=$OUT/round_verify"
  echo "ws200_debug=$OUT/ws200_debug.json"
  echo "acceptance_checks=$OUT/acceptance_checks.json"
} > "$OUT/manifest.txt"

(
  cd "$OUT"
  shasum -a 256 * 2>/dev/null | sort > SHA256SUMS
)

echo "Round AMOLED acceptance evidence written: $OUT"
