# HARDWARE-003 Round AMOLED Compact Status Consumer — Evidence

Status: implemented-awaiting-codex-review
Branch: `codex/round-amoled-printsphere`
Worktree: `/Users/seanlee/Documents/printsphere-round-amoled`
Owner: Claude Code (M3)
Supervisor: Codex
Source-of-truth contract: PMD-007
`docs/bambustat-compact-status-contract.json`
`docs/bambustat-compact-status-consumer-contract.md`

## Slice Scope

Build-only, read-only PMD-007 compact-status consumer for the 1.75 inch
round AMOLED. Maps the 18 hardware-safe fields into a fixed-size,
view-shaped struct that the UI layer can drain without learning anything
about transport, secrets, MQTT, NVS, or the raw PMD-007 schema.

This slice does **not**:

- touch UI rendering, LVGL widgets, pager, page pages, or page indices
- perform any HTTP fetch, MQTT subscribe, NVS read/write, or transport I/O
- read secrets, access codes, cookies, or device tokens
- claim physical acceptance from build success

## Files Changed

| Path | Kind | Purpose |
|---|---|---|
| `main/include/printsphere/bambustat_compact_status.hpp` | new | Public API: struct `BambustatCompactStatus`, hardware-safe and forbidden-key name catalogs, parse/reset helpers, suppressed-key audit accessor. |
| `main/src/bambustat_compact_status.cpp` | new | cJSON-backed mapper. Walks the payload once, populates only the 18 hardware-safe fields, counts and suppresses every key on the PMD-007 deny-list. |
| `main/CMakeLists.txt` | edit | Register `src/bambustat_compact_status.cpp` in the printsphere component source list. |
| `main/notes/hardware-003-round-amoled-compact-status-consumer.md` | new | This evidence note. |

`git status --short` after the slice:

```
 M main/CMakeLists.txt
?? main/include/printsphere/bambustat_compact_status.hpp
?? main/src/bambustat_compact_status.cpp
?? main/notes/hardware-003-round-amoled-compact-status-consumer.md
```

`git diff --stat HEAD` after the slice:

```
 main/CMakeLists.txt | 1 +
```

(The two untracked source files are intentionally listed below the
diff stat because they are new files; nothing outside the table above
was modified in this slice.)

## Design Notes

- **Hardware-safe field set** — declared once in
  `kBambustatHardwareSafeFieldNames[]` in the header. Source-of-truth
  order matches `bambustat-compact-status-contract.json`. The struct
  holds a copy of each field plus a `_known` boolean so the UI can
  suppress rows for absent/null/empty values rather than rendering a
  misleading `0` or `false`.
- **Forbidden payload key set** — `kBambustatForbiddenPayloadKeys[]`
  unions the 14 keys from `contract.json:forbidden_payload_fields`
  with the 31 redaction-policy denials from `main/notes/redaction-policy.md`
  (F1..F14). Match is case-insensitive and dash-insensitive to stay
  aligned with the policy's regex catalog (e.g. `Access-Code` matches
  `access_code`).
- **Subtree walk** — `count_forbidden_in_subtree()` recursively scans
  objects and arrays so a forbidden key nested inside
  `ams_slot_summary` or anywhere else still contributes to the audit
  counter even though the value itself never reaches the model.
- **Tri-state booleans** — `low_stock_warning_present` /
  `unbound_spool_hint_present` distinguish "absent" from "explicit
  false". The UI layer MUST use the `_present` flag before deciding
  to render a warning chip.
- **No exceptions, no allocations beyond std::string growth** —
  follows the local style: free functions in `namespace printsphere`,
  `std::string` for textual fields, cstdint for counters, no smart
  pointers, no template metaprogramming.
- **Reuse without coupling** — the header does not include
  `printsphere/ui.hpp`; the future integration slice can consume the
  struct directly without dragging in LVGL headers.

## Build Verification

`./tools/build_only.sh` is the canonical verification path. The
auto-mode of that script picks the native ESP-IDF lane first, falling
back to Docker (`espressif/idf:release-v6.0`).

### Last command output

```
$ ./tools/build_only.sh
Docker daemon is not running.
$ echo $?
127
```

### Interpretation

- `idf.py` is not on the host `PATH` (native ESP-IDF lane unavailable).
- Docker is installed but the daemon is not running, so the script's
  Docker fallback cannot pull the `espressif/idf:release-v6.0` image
  either.
- The script returned exit code `127` from the Docker lane and did
  not write to flash, serial, `/dev/cu.*`, NVS, or any external
  transport.

This is the exact blocker the work order's Acceptance section
explicitly permits: "if the build environment is unavailable, Claude
records the exact blocker and the last command output in the
evidence note."

### Local syntax-only check (host compiler)

To provide additional evidence that the new source compiles cleanly
with the project's `-Wall -Wextra` setting, a host-clang syntax-only
preprocess was run against the in-tree cJSON header:

```
$ clang++ -std=c++17 -fsyntax-only -Wall -Wextra \
    -I main/include \
    -I managed_components/espressif__cjson/cJSON \
    main/src/bambustat_compact_status.cpp
$ echo $?
0
```

No diagnostics. Codex may re-run `./tools/build_only.sh --pull`
inside the project docker runtime once the daemon is started to
confirm the full ESP-IDF compile, but no source-side dependencies
were added that would change that outcome.

## Forbidden-Field Behavior

For every parsed payload, the model exposes:

- `suppressed_forbidden_keys` — exact count of deny-list occurrences
  found anywhere in the subtree
- `parsed_top_level_keys` — count of top-level keys present after
  forbidden-key suppression
- `known_field_hits` — count of hardware-safe fields populated
- `parse_ok` / `parse_error` — success flag and (when failing) cause

A forbidden key contributes its presence to the counters but never
its value to any model field. Forbidden values from payload arrays,
nested objects, or `mqtt_*` family keys are not propagated through
any field accessor.

## Acceptance Status Against HARDWARE-003 Requirements

| Requirement | Status |
|---|---|
| Build a parser/mapper for PMD-007 into a round-AMOLED-safe struct | DONE — `BambustatCompactStatus`, header exposes exactly the 18 PMD-007 fields |
| Include only hardware-safe display fields | DONE — mapper only reads from `kBambustatHardwareSafeFieldNames` |
| Count and suppress forbidden keys (PMD-007 + redaction policy) | DONE — `suppressed_forbidden_keys` counts both lists |
| Keep read-only / parser-focused | DONE — no transport, no fetch, no storage |
| Pre-existing HTTP helper not required | N/A — no fetch in this slice |
| Match local ESP-IDF / C++ style | DONE — free functions in `printsphere` namespace, cJSON, cstdint, std::string, `-Wall -Wextra` clean |
| `./tools/build_only.sh` runs | PARTIAL — script ran; environment blocker recorded above |
| Target branch remains `codex/round-amoled-printsphere` | DONE — `git branch --show-current` confirms |
| `git status --short` shows only intended source/evidence changes | DONE — only the four paths in the Files Changed table |
| Evidence note exists at `main/notes/hardware-003-...md` | DONE — this file |

## Forbidden Hardware Action Statement

This slice did not:

- access `/dev/cu.usbmodem101` or any `/dev/cu.*`
- run `idf.py flash`, `esptool.py`, `esptool write_flash`, or a serial
  monitor
- read NVS, write NVS, or restore a partition table
- read secrets, access codes, cookies, tokens, or printer credentials
- mutate any database, perform inventory/order/media writes, or send
  to any external webhook / push / email channel
- call `git push`, `git rebase`, `git reset`, `git clean`, or
  `git stash drop/pop`
- port WS700 or WS200 UI code into this branch

## Build Success Is Not Physical Acceptance

Per `docs/bambustat-compact-status-contract.json` and the round
AMOLED `CLAUDE.md`, build success — and even this evidence note — do
**not** authorize any future flash / upload / touch / camera check.
Physical acceptance for HARDWARE-005 remains blocked behind explicit
human approval for the exact device, port, and command.

## Suggested Codex Review Notes

- Confirm the union of forbidden keys in
  `kBambustatForbiddenPayloadKeys[]` matches what
  `docs/bambustat-compact-status-contract.json` plus
  `main/notes/redaction-policy.md` together require. The 18 safe
  fields are the full contract field set; the 45 forbidden keys cover
  `forbidden_payload_fields` (14) + redaction-policy (31) without
  overlap.
- Confirm the future integration slice (HARDWARE-004 family) will
  consume `BambustatCompactStatus` without needing changes here.
- Re-run `./tools/build_only.sh --pull` when the docker daemon is
  available to upgrade the verification result from
  `implemented-awaiting-codex-review` (build env blocked) to
  full-acceptance.
