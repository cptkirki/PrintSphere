# Round AMOLED Firmware-Side Redaction Policy

Lane: `bambustat`
Worktree: `/Users/seanlee/Documents/printsphere-round-amoled`
Branch: `codex/round-amoled-printsphere` (untracked sidecar policy doc)
Companion to: `WO-WEB-034` (server-side redaction counterpart)
Status: source-tree-local policy doc only; no firmware source edit in this slice
Scope: every value the round AMOLED PrintSphere LVGL firmware pushes to any
UI control (label text, bar fill, icon, toast, log line, JSON serialisation
into setup portal, MQTT publish, OTA status, debug dump) MUST be filtered
through this policy before render or transmit.

## 1. Purpose

This file enumerates every field the round AMOLED firmware MUST strip,
truncate, hash, or replace before the value reaches a user-visible control
or an outgoing transport. It exists because:

- The web/API layer (`/api/monitor-summary`, `/api/hardware-safe-fields`,
  `/api/compact-status`) redacts on the server. Firmware MUST also redact so
  that anything not coming through those endpoints (direct MQTT echo,
  cached NVS read, local log line, OTA JSON, debug dump) cannot leak a
  forbidden value even if the web/API gate were missing or bypassed.
- Defence in depth: server-side redaction is necessary but not sufficient.
  The screen cannot be allowed to display `access_code=***`, raw MQTT JSON,
  a printer `192.168.x.x:8883` pair, or a Tasmota `User:Pass@host` token.
- This is the BambuStat-side counterpart to `WO-WEB-034`. The two policies
  MUST stay aligned; if a field is forbidden here, the server MUST also
  strip it; if a field is forbidden on the server, the firmware MUST also
  strip it.

## 2. Forbidden Field Inventory

The firmware MUST treat the following field names as `REDACT` whenever
they appear in any path that can reach a UI control, a log line, or an
outgoing serialised payload. Field matching is case-insensitive and
applies to JSON keys, C struct member names by stringification, NVS
keys, header names, and substring matches in opaque blobs.

| # | Forbidden field / token | Why it is forbidden | Where it can leak from |
|---|---|---|---|
| F1 | `access_code` | Bambu printer LAN access code (32-char) — grants local MQTT control. | Bambu MQTT pushall, NVS key `bambu_access_code`, setup-portal payload, JSON log. |
| F2 | `token` (any of `auth_token`, `bearer_token`, `refresh_token`, `cloud_token`, `access_token`, `sso_token`) | Long-lived credential used to talk to Bambu Cloud. | Cloud login response, NVS namespace `bambu_cloud`, HTTP `Authorization` header, OTA status JSON. |
| F3 | `raw MQTT echo` (full pushall / `message` field / `print` object verbatim) | Carries access_code, serial, IP, and HMS detail verbatim. | Bambu MQTT subscribe handler, debug log, OTA status dump. |
| F4 | `printer IP+port` (e.g. `192.168.31.42:8883`, `10.0.0.5:1883`) | Reveals LAN topology and lets an attacker reach the Bambu MQTT port. | Bambu MQTT config, NVS key `bambu_host`, setup-portal payload, `/status` JSON, debug log. |
| F5 | `NVS value` (full opaque blob from `nvs_get_str` / `nvs_get_blob`) | May contain access_code, token, password, or printer IP. | Any `nvs_get_*` call whose key is in the `bambu_*` or `cloud_*` namespace. |
| F6 | `Bambu cookie` (any `bambu_*` / `BB-Token` / `Set-Cookie: bambucloud_session=*`) | Session cookie for Bambu Cloud web; valid for hours. | Cloud HTTPS response, NVS `cloud_cookie`, log of HTTP redirect. |
| F7 | `Tasmota auth string` (e.g. `User:Pass` in `http://User:Pass@192.168.x.x/cm?`, `user=admin&password=***` in `cm?` query) | Device admin credential for the Tasmota power plug if any outlet is integrated. | Tasmota HTTP request, NVS `tasmota_auth`, setup portal. |
| F8 | `customer` / `customer_id` / `customer_name` / `customer_email` / `customer_phone` / `customer_address` | PII; outside BambuStat's hardware-monitor scope. | Any future web/API payload, future intake JSON, log of an upstream exception. |
| F9 | `order` / `order_id` / `order_number` / `order_status` / `order_total` | Commerce entity; outside hardware monitor. | Web/API response, setup portal future import, debug log. |
| F10 | `quote` / `quote_id` / `quote_total` / `quote_valid_until` | Commerce entity. | Web/API response, future intake path. |
| F11 | `cost` / `unit_cost` / `material_cost` / `print_cost` / `cost_basis` | Commercial figure; outside hardware monitor. | Web/API response, future invoice field. |
| F12 | `profit` / `margin` / `markup` | Commercial figure; never belongs on a status screen. | Web/API response, future commerce summary. |
| F13 | `secret` / `client_secret` / `api_secret` / `app_secret` | Generic credential; never belongs on a status screen. | Cloud config, log of failed auth. |
| F14 | `raw media` (raw image / video / file bytes, base64 of media, `media_url`, `thumbnail_url`, `screenshot_url`, `image_base64`, `file_data`) | Media bytes are outside the hardware-monitor scope; never belong on a 1.75" status screen. | Web/API response, intake response, debug log, future media ingestion. |

The regex catalog for matching these names is in section 4.

## 3. Replacement Strategy

The firmware MUST apply the following transformations to any forbidden
field BEFORE the value reaches a render path or transport.

### 3.1 Per-class replacement

| Class | Rule | Replacement literal |
|---|---|---|
| `access_code` (F1) | replace whole value | `***` |
| `token` family (F2) | replace whole value; preserve first 4 chars only if user is debugging and an explicit `BMS_DEBUG_TOKEN=1` build flag is set | `***` (default); `tok_***` (debug build) |
| Raw MQTT echo (F3) | NEVER pass raw `message` field through; replace with an empty object `{}` plus a single `mqtt_summary_ok: true` flag | `{"mqtt_summary_ok": true}` |
| Printer IP+port (F4) | replace last two octets of IP with `*`; replace port with `0` | `192.168.*.*:0` |
| NVS opaque blob (F5) | if namespace is in the deny-list (`bambu_*`, `cloud_*`, `tasmota_*`, `*token*`, `*secret*`, `*password*`, `*auth*`), the whole blob is `***` | `***` |
| Bambu cookie (F6) | replace whole cookie | `***` |
| Tasmota auth (F7) | replace user and password; preserve scheme/host | `http://***:***@host/cm?` |
| Customer / order / quote / cost / profit / secret (F8-F13) | replace whole value | `***` |
| Raw media (F14) | replace whole URL or bytes | `***`; for URLs preserve only the `host` part if it is a known Bambu CDN, otherwise `***` |

### 3.2 Universal replacement helpers (proposed API surface, not yet compiled)

The implementation MUST expose, at minimum:

```cpp
// returns true if `key` (case-insensitive) is in the deny-list
bool bmsIsForbiddenKey(const char* key);

// returns a static string for the replacement of `value` when `key` is forbidden
const char* bmsRedactValue(const char* key, const char* value);

// truncates / redacts an opaque blob
std::string bmsRedactNvsBlob(const char* ns, const char* key, size_t blob_len);

// strips IP last-two-octets and zeros the port
std::string bmsRedactHostPort(const char* host, int port);

// collapses a JSON object to {"<key>_present": true} with all leaves replaced by ***
std::string bmsRedactJsonObject(const char* json_body, size_t len);
```

These helpers are a SOURCE-LEVEL spec only; they MUST be implemented in a
future build-only slice. Until then, every call site that prints a value
MUST be guarded manually using the regex catalog below.

## 4. Regex Catalog (case-insensitive, JSON-key AND substring)

The firmware MUST treat any of the following patterns, when found as a JSON
key or as a substring of a printable string, as a `REDACT` trigger. Match
position is anywhere in the string; length is full token length; the whole
value is replaced with `***` (or the per-class replacement above).

| Pattern | Applies to | Class |
|---|---|---|
| `(?i)\baccess[_-]?code\b` | JSON key, NVS key, header | F1 |
| `(?i)\bauth[_-]?token\b` | JSON key, header | F2 |
| `(?i)\bbearer[_-]?token\b` | JSON key, header | F2 |
| `(?i)\brefresh[_-]?token\b` | JSON key | F2 |
| `(?i)\baccess[_-]?token\b` | JSON key, header | F2 |
| `(?i)\bcloud[_-]?token\b` | JSON key | F2 |
| `(?i)\bsso[_-]?token\b` | JSON key | F2 |
| `(?i)\b(bambu|bb)[_-]?token\b` | JSON key, header, cookie | F2/F6 |
| `(?i)\bmqtt[_-]?(message|payload|raw|pushall)\b` | JSON key (whole value replaced with empty obj) | F3 |
| `(?i)\bprint\b\s*:\s*\{` (only when value is an opaque JSON dump; safer: never print the whole `print` object) | JSON value | F3 |
| `(?i)\b(bambu[_-]?host|printer[_-]?ip|host|port)\b` (host/port pair) | JSON key + value (transform IP+port) | F4 |
| `(?i)\bnvs\b|\bnvs_[a-z_]+\b` | any log line containing an NVS namespace + value | F5 |
| `(?i)\b(set-?cookie|cookie)\s*[:=]\s*bambu` | header / log line | F6 |
| `(?i)\bbambucloud[_-]?session\b` | JSON key, cookie | F6 |
| `(?i)\b(user|password|passwd|pwd)\s*[:=]\s*[^&\s]+` | Tasmota HTTP query | F7 |
| `(?i)\bhttp(s?)://[^/\s:]+:[^/\s@]+@` | Tasmota URL | F7 |
| `(?i)\bcustomer[_-]?(id|name|email|phone|address)?\b` | JSON key | F8 |
| `(?i)\border[_-]?(id|number|status|total)?\b` | JSON key | F9 |
| `(?i)\bquote[_-]?(id|total|valid[_-]?until)?\b` | JSON key | F10 |
| `(?i)\b(unit|material|print)[_-]?cost\b` | JSON key | F11 |
| `(?i)\bcost[_-]?basis\b` | JSON key | F11 |
| `(?i)\b(profit|margin|markup)\b` | JSON key | F12 |
| `(?i)\bsecret\b|\bclient[_-]?secret\b|\bapp[_-]?secret\b|\bapi[_-]?secret\b` | JSON key, header, log line | F13 |
| `(?i)\b(media[_-]?url|thumbnail[_-]?url|screenshot[_-]?url|image[_-]?base64|file[_-]?data|raw[_-]?media)\b` | JSON key, log line, URL string | F14 |

## 5. Where This Policy Applies In The Source Tree

The policy MUST be honoured by every one of the following call sites
(if present) in the round AMOLED `main/src/` tree:

- `setup_portal.cpp` — JSON serialisation of `/setup`, `/api/devices`,
  `/api/health`. MUST NOT leak NVS values into the portal HTML/JSON.
- `printer_client.cpp` — printer config struct printer (debug log path).
- `bambu_cloud_client.cpp` — Cloud HTTPS response log, `Set-Cookie`
  handling, token cache write.
- `application.cpp` — `lv_label_set_text` calls with values that came
  from any source other than the server-redacted endpoint.
- `ui.cpp` — every LVGL label, bar, icon, and toast.
- `mqtt_bridge.cpp` / `bambu_mqtt.cpp` (if present) — raw MQTT payload
  echo MUST be replaced with `mqtt_summary_ok: true` before any log.
- Any future `debug_dump.cpp` or `diagnostics.cpp` — full system state
  MUST pass through `bmsRedactJsonObject` first.
- OTA status payload — MUST be filtered for F1, F2, F3, F4, F5, F6, F13.

## 6. What This Policy Does NOT Permit

This policy is a **read-side / pre-render filter only**. It does NOT:

- Authorise any firmware source edit, build, flash, upload, erase,
  restore, serial monitor, `/dev/cu.*` access, NVS read or write.
- Authorise any secret / token / cookie / access-code / NVS blob read
  or write. The forbidden-field list exists so the firmware AVOIDS
  printing those values; it does NOT authorise the firmware to read
  them for any other purpose.
- Authorise any `git push`, `git rebase`, `git reset`, `git clean`,
  `git stash drop/pop`, or branch deletion.
- Authorise any external send, publish, deploy.
- Cover the WS200 line (see `firmware/notes/redaction-policy.md` in the
  `bambuhelper-ws700-ws200-port` tree) or the WS700 preservation line.
- Override `WO-WEB-034`. The server-side redaction is the primary gate;
  this firmware-side policy is the defence-in-depth companion.

## 7. Cross-References

- Web/API counterpart: `WO-WEB-034` server-side redaction (web lane).
- Slice plan: `product-delivery/2026-06-22/claude-evidence/bambustat-next-slice/next-5-source-port-slices.md` slice 1.
- Lane checkpoint: `lanes/bambustat/{progress.md,handoff.md,worklog.jsonl}`.
- Evidence bundle: `agent-supervision/evidence/WO-BMB-034/`.
- WS700 screen contract: not in scope (preservation branch only).
- PrintSphere v1.5.1 / v1.6-beta1 upstream: this policy is a BambuStat
  contract layer; PrintSphere upstream may add its own redaction, but
  BambuStat MUST NOT rely on it.