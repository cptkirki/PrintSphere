# HARDWARE-003 Round AMOLED Compact Status Consumer — Evidence

Status: implemented-awaiting-codex-review
Build verification status: blocked (Docker Hub `registry-1.docker.io`
TLS-handshake outage, 2026-07-07 pull-and-build run, exit 1)
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
  includes the 14 keys from `contract.json:forbidden_payload_fields`
  plus representative round-AMOLED redaction-policy keys. The
  classifier also applies bounded family matching for policy patterns
  such as token-family keys, `nvs_*`, customer/order/quote families,
  cost families, host/port, and short password aliases. Matching is
  case-insensitive and dash-insensitive to stay aligned with the
  policy's key catalog (e.g. `Access-Code` matches `access_code`).
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

### Last command output (2026-07-07, HARDWARE-003 build-acceptance run)

Docker Desktop was started via `open -a Docker` and the daemon reached
`docker info` READY before the build command was re-run. The daemon
blocker from the prior run is resolved; the remaining blocker is the
missing IDF image, which this work order forbids resolving with
`--pull`.

```
$ ./tools/build_only.sh
Missing Docker image: espressif/idf:release-v6.0
Run again with --pull, or pull it explicitly after accepting the download cost.
EXIT=127
```

Image cache check on the running daemon:

```
$ docker image inspect espressif/idf:release-v6.0   # -> not found
$ docker images 'espressif/idf'                      # -> no cached tags
```

### Interpretation

- `idf.py` is not on the host `PATH` (native ESP-IDF lane unavailable).
- Docker daemon is now RUNNING (started this run), so the earlier
  "Docker daemon is not running" blocker is cleared.
- The required image `espressif/idf:release-v6.0` is not present in the
  local Docker cache, and no `espressif/idf` tag is cached at all. The
  script's Docker lane therefore returned exit code `127` with
  `Missing Docker image`.
- Completing the build would require `docker pull` / `--pull`, which the
  HARDWARE-003 build-acceptance work order explicitly forbids.
- The script did not write to flash, serial, `/dev/cu.*`, NVS, or any
  external transport.

This is the exact allowed-blocker path the build-acceptance work order
defines: "required Docker image is missing and would require `--pull`
... Evidence note records exact command output and keeps status
`implemented-awaiting-build-acceptance`."

### Last command output (2026-07-07 01:48 CST, HARDWARE-003 pull-and-build run)

Follow-up work order `HARDWARE-003-round-amoled-pull-image-build-acceptance.md`
explicitly authorizes a single `--pull` invocation. Pre-flight:

```
$ git branch --show-current
codex/round-amoled-printsphere
$ git status --short
(empty — clean)
$ command -v docker
/usr/local/bin/docker
$ docker info | grep Server Version
 Server Version: 29.6.1     # daemon READY (started in prior run)
$ docker image inspect espressif/idf:release-v6.0 >/dev/null 2>&1
                                  # IMAGE_MISSING from local cache
```

`./tools/build_only.sh --pull` invocation (full transcript captured to
`/tmp/hardware-003-pull-build.log`):

```
2026-07-06T17:48:57Z RUN_START
Error response from daemon: failed to resolve reference
"docker.io/espressif/idf:release-v6.0": failed to do request: Head
"https://registry-1.docker.io/v2/espressif/idf/manifests/release-v6.0":
Service Unavailable
2026-07-06T17:49:02Z RUN_END exit=1
```

The build script exited `1` after the image pull failed; the rest of the
script's pipeline (no native `idf.py`, so the docker lane was the only
option) never executed.

### Why the pull failed — registry backend diagnostics

Layered evidence captured to `/tmp/hardware-003-registry-diag.log`
(2026-07-07 01:48 CST):

| Probe | Result | Interpretation |
|---|---|---|
| `docker info` | Server Version 29.6.1 | Docker daemon is RUNNING, API reachable |
| `nslookup registry-1.docker.io` | resolves to `198.18.0.159` | DNS works |
| `ping -c 2 -W 5 registry-1.docker.io` | 0% loss, avg 0.27 ms | ICMP / L3 reachability is fine |
| `curl -v` to `https://registry-1.docker.io/v2/espressif/idf/manifests/release-v6.0` | `Connected to ... port 443`, then `SSL_connect: SSL_ERROR_SYSCALL in connection to registry-1.docker.io:443` → `Closing connection` | TCP succeeds (port 443), but the TLS handshake aborts |
| `openssl s_client -connect registry-1.docker.io:443 -servername registry-1.docker.io` | `error:0A000126:SSL routines::unexpected eof while reading` after ClientHello | Server closes the TLS connection mid-handshake; "0 bytes read, 1558 bytes written" confirms server never finished the handshake |
| Repeated `curl` 3x to the manifest URL | HTTP=000 t≈0.06s each (3/3) | Persistent, not transient; the registry frontend is not even returning a 5xx — it's hanging up at TLS |
| `curl https://hub.docker.com/` | 200 | Other Docker Hub host works |
| `curl https://registry.hub.docker.com/v2/` | 401 (expects auth) | Reachable, returns expected auth challenge |
| `curl https://github.com/` | 200 | Non-Docker hosts unaffected |
| `docker pull` (re-run via build script) | same `Service Unavailable` from daemon, exit 1 | Reproducible; not a transient flake |

### Interpretation (pull-and-build run)

- Network stack (DNS, ICMP, TCP/443) reaches `registry-1.docker.io`
  without issue.
- The TLS server at that endpoint accepts the TCP connection but then
  abruptly closes the socket (`SSL_ERROR_SYSCALL` / `unexpected eof`)
  before completing the handshake. Docker's daemon reports this as
  `Service Unavailable`.
- The same exact pattern reproduces 3/3 attempts with sub-second
  timeouts, so it is not a transient retry candidate inside this run.
- Other Docker Hub hosts (`hub.docker.com`, `registry.hub.docker.com`)
  and unrelated public hosts (`github.com`, `anthropic.com`) are
  unaffected, ruling out a Mac-side firewall / proxy / TLS-MITM.
- This is therefore a **Docker Hub v2 registry backend outage**
  affecting the `registry-1.docker.io` endpoint, not a local or branch
  defect. There is no safe fix this work order can apply: the only
  way to obtain the image is to wait for the registry to recover.
- The pull run did not write to flash, serial, `/dev/cu.*`, NVS,
  partitions, or any external transport. No source file was edited.
  No alternate Docker image was pulled (the work order permits exactly
  `./tools/build_only.sh --pull`'s normal `docker pull`).

This matches the pull-build work order's `failure/blocker` clause:
*"Pull fails due to network/provider/docker registry issue, ...
Evidence note records exact command output and first concrete failure.
This work order status becomes `blocked` or `implemented-awaiting-fix`
as appropriate."* The correct status is `blocked` (third-party outage),
not `implemented-awaiting-fix` (no code change is required).

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
| Count and suppress forbidden keys (PMD-007 + redaction policy) | DONE — `suppressed_forbidden_keys` counts the PMD-007 deny-list plus bounded redaction-policy key families |
| Keep read-only / parser-focused | DONE — no transport, no fetch, no storage |
| Pre-existing HTTP helper not required | N/A — no fetch in this slice |
| Match local ESP-IDF / C++ style | DONE — free functions in `printsphere` namespace, cJSON, cstdint, std::string, `-Wall -Wextra` clean |
| `./tools/build_only.sh` runs | PARTIAL — script ran; environment blocker recorded above |
| `./tools/build_only.sh --pull` runs (follow-up pull-and-build work order) | BLOCKED — Docker Hub `registry-1.docker.io` returned `Service Unavailable` with TLS-handshake EOF (3/3 reproducible); exit `1`. See "Last command output (2026-07-07 01:48 CST, HARDWARE-003 pull-and-build run)" above. |
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

- Confirm the forbidden-key classifier matches what
  `docs/bambustat-compact-status-contract.json` plus
  `main/notes/redaction-policy.md` together require. The 18 safe
  fields are the full contract field set; the 41 exact forbidden keys
  do not overlap with safe fields, and family matching covers the
  policy catalog's token, NVS, customer/order/quote, cost, host/port,
  secret and password-alias key patterns.
- Confirm the future integration slice (HARDWARE-004 family) will
  consume `BambustatCompactStatus` without needing changes here.
- Re-run `./tools/build_only.sh --pull` when the docker daemon is
  available to upgrade the verification result from
  `implemented-awaiting-codex-review` (build env blocked) to
  full-acceptance.
