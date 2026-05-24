# Implementation Plan - Discovery (Recon the Mac for build inputs)

This plan realizes `context/projects/discovery/prd.md` and
`context/projects/discovery/ard.md`, and conforms to
`context/coding_standards.md`. It is sequenced strictly bottom-up:
the pure core and library boundaries first (each proven through
`make test` and the `discovery_smoke` dev tool), then the recon UI
delivered as vertical slices, UI last. Every task is a single
buildable, testable, releasable increment; backend tasks are purely
additive and consumed by nothing user-facing until the slices land,
so each is safe to ship as-is.

## Summary of Tasks

1. **libdiscovery core** — shell-safe command builders, blueprint
   curation, the readiness predicate, and `disc_status_str`; no
   jsmn. New library + `discovery_test`.
2. **libdiscovery parsers (jsmn)** — vendor `third_party/jsmn`; add
   the four tolerant JSON parsers with inline canonical + drifted
   fixtures.
3. **libssh channel primitives** — non-blocking
   `ssh_channel_exec/read/eof/exit/close` so the worker can run
   commands and read output over the one session.
4. **libstore presets + target + scanroot** — `store_conn_key` and
   three per-connection record families (presets, remembered
   target, scan root); `store_test` round-trips.
5. **lexicon recon keys** — all recon `LexKey`s + strings, asserted
   in `lexicon_test`.
6. **session 5a — engine + SCAN + ABORT** — the discovery ring
   pair, job table, job-arena pool, read loop, parse-on-worker
   streaming, timeout refinement; SCAN_HOST + ABORT_SCAN; create
   `tools/discovery_smoke.c`.
7. **session 5b — READ_BLUEPRINT + RESOLVE_BUNDLE_ID** — `-list`
   and `-showBuildSettings` jobs streaming schemes/configs and the
   bundle id.
8. **session 5c — SWEEP** — devicectl + simctl as two grouped
   concurrent jobs merged into one target stream.
9. **slice A — project discovery** — extend `ui_frame`; scan-root
   field, SCAN/ABORT, BLUEPRINTS list + select, manual path, empty
   / error states; scan-root restore on connect.
10. **slice B — scheme/config/bundle-id** — prefilled editable
    inputs with discovered-set hints, per-field user-edited flags,
    and the bundle-id resolve flow.
11. **slice C — presets** — selector + new/rename/delete/choose,
    last-active restore on connect, the empty state.
12. **slice D — targets + READY** — sweep, unified target list +
    select, session-sticky selection, auto-sweep + remembered-
    target re-validation on connect, the READY indicator.

## Task Dependency Relationships

```
 1 ─┬───────────────▶ 6 ──┬────────▶ 7 ───────────▶ 10
    │                 ▲    │                          ▲
    ▼                 │    └────────▶ 8 ──────┐       │
 2 ─┴──────────────┐  │                       ▼       │
                   └──┼────────────────────▶ (8,7)    │
 3 ────────────────▶ 6                         │      │
                                               ▼      │
 4 ───────────────────────────┬─────────────▶ 12     │
                              ├─────────────▶ 11      │
 5 ───────────────────────────┼──┐            ▲      │
                              │  │            │      │
                              └─▶ 9 ──────────┴──────┘
                                 (9 precedes 10,11,12)
```

Reading it: 1 feeds 2 and 6; 2 feeds 7 and 8; 3 feeds 6; 6 feeds 7
and 8; 4 + 5 feed the slices. Slice A (9) is the base every later
slice builds on; 10 also needs 7, 11 also needs 4, 12 also needs 8
and 4.

## Detailed Tasks

### Task 1 - libdiscovery core

- **Status**: done
- **Blocked by**: none
- **User stories covered**: 5, 6, 7, 43, 44, 52

#### What to build

The pure, host-free core of discovery with no JSON and no I/O:
the remote command strings, the blueprint curation that shapes the
SCAN HOST result, and the readiness predicate behind the READY
indicator. Realizes the curation and depth/workspace rules of PRD
Solution and stories 5–7, plus the readiness rule of stories
43–44. Establishes `libdiscovery.a` as a new library so later
tasks extend an existing boundary.

#### Technical Details

Per ARD §"New: `libdiscovery.a`" and §"Interfaces
(`include/discovery.h`)": create `include/discovery.h` and
`src/discovery/`, build `build/libdiscovery.a`, and wire it into
the Makefile mirroring the existing `lib<module>.a` pattern. Land
the shell-safe builders `disc_scan_cmd` (find with the
heavy-directory `-prune` set + `-maxdepth`), `disc_list_cmd`,
`disc_build_settings_cmd`, `disc_simctl_cmd`, `disc_devicectl_cmd`
— single-quote-escaping all paths/roots. Land
`disc_curate_blueprints` (operates on the plain-text `find`
output: dedup the inner `project.xcworkspace`, prefer the
workspace when co-located, depth-respecting) and the pure
`disc_readiness` over `RunConfig` + a target-selected bool, plus
`disc_status_str`. All allocating calls take a caller `Arena *`;
all fallible calls return `DiscStatus`. Pure C11; black-box
tested through `discovery.h` only (coding_standards "Testing").

#### Acceptance criteria

- [ ] `include/discovery.h` declares the builders, curation,
      readiness, the result structs, `DiscStatus`, and
      `disc_status_str` exactly as ARD §"Interfaces" specifies.
- [ ] `disc_scan_cmd` quotes the root and emits the documented
      `-prune` set (`Pods`, `Carthage`, `.build`, `DerivedData`,
      `node_modules`) and `-maxdepth`; a root with a space is
      escaped safely.
- [ ] `disc_curate_blueprints` drops the inner
      `project.xcworkspace`, prefers the `.xcworkspace` when a
      workspace and project share a directory, and honors the
      depth cap — verified by `discovery_test` cases.
- [ ] `disc_readiness` returns the specific missing-field enum
      (no project / scheme / config / bundle id / target) for
      each gap and `READY_OK` when complete.
- [ ] `build/libdiscovery.a` builds clean on Linux and macOS
      (`CC`/`CFLAGS` honored), `tests/discovery_test.c` links the
      archive, and `make test` passes.

### Task 2 - libdiscovery parsers (jsmn)

- **Status**: done
- **Blocked by**: 1
- **User stories covered**: 28, 29, 40, 50

#### What to build

The JSON half of the core: parse `xcodebuild -list` into schemes
and configurations, `-showBuildSettings` into the bundle id, and
`simctl` + `devicectl` into unified `Target` records (device vs.
simulator, booted state). Parsing must tolerate fields it does not
recognize so a newer/older Xcode never breaks it (stories 40, 50),
which is also what lets targets be labeled and inferred
(stories 28, 29).

#### Technical Details

Per ARD §"New: `libdiscovery.a`" (Parsing) and §"New submodule:
`third_party/jsmn`": vendor jsmn as a git submodule alongside the
others and compile it into `libdiscovery`. Add `disc_parse_list`,
`disc_parse_bundle_id`, `disc_parse_simctl`, `disc_parse_devicectl`
using a jsmn token-walk that reads only the keys it needs and
ignores the rest (tolerant by construction). The jsmn token array
is bump-allocated from the caller arena (ARD §"Arenas"); jsmn
allocates nothing of its own. Exhausted token/arena space returns
`DISC_ERR_PARSE`/`DISC_ERR_OOM`. Per the test decision, fixtures
are hand-authored inline string literals in `discovery_test.c`
(canonical + a drifted/odd sample) matching the harness's
in-memory style; real captures from smoke runs can harden them
later.

#### Acceptance criteria

- [ ] `third_party/jsmn` is a registered submodule and compiles
      into `build/libdiscovery.a`; `.gitmodules` updated.
- [ ] The four parsers populate their out-structs from canonical
      fixtures and ignore unknown/additive fields in the drifted
      fixture without error.
- [ ] `disc_parse_simctl`/`disc_parse_devicectl` set
      `is_simulator` and `booted` correctly so targets are
      distinguishable (stories 28, 29).
- [ ] Malformed/truncated JSON returns `DISC_ERR_PARSE` (never a
      crash or garbage), exercised by a fixture.
- [ ] `make test` passes with the new parser cases; the test
      remains host-free (no live Mac required).

### Task 3 - libssh channel primitives

- **Status**: done
- **Blocked by**: none
- **User stories covered**: 36, 37, 53

#### What to build

The low-level, non-blocking channel calls the worker needs to run
a command on the existing session and read its output — several
channels at once, none blocking — so discovery can run off the UI
thread over the one multi-channel session (stories 36, 37, 53).

#### Technical Details

Per ARD §"Extended: `libssh.a` — channel exec primitives" and
§"Interfaces (`include/ssh.h`)": add `ssh_channel_exec`,
`ssh_channel_read` (into a caller buffer), `ssh_channel_eof`,
`ssh_channel_exit` (exit code), and `ssh_channel_close` to
`include/ssh.h` and `src/ssh/ssh.c`. `ssh_channel_open` already
exists. The worker owns the read loop and accumulation; libssh
holds no output buffer and stays a thin wrapper at the existing
`*_step` granularity. All channels multiplex over the single
session socket fd. `SSH_AGAIN` is returned while a request would
block. libssh has no automated test binary; this is validated by
extending a `tools/*_smoke.c` against a live Mac.

#### Acceptance criteria

- [ ] The five primitives are declared in `include/ssh.h` with the
      exact signatures and `SSH_AGAIN` semantics of ARD
      §"Interfaces", and implemented in `src/ssh/ssh.c`.
- [ ] No output buffer is added to libssh; reads write only the
      caller's buffer.
- [ ] `build/libssh.a` builds clean and `make test` still passes
      (no regressions; libssh keeps no test binary).
- [ ] Manually, against a live Mac via a smoke tool: open a
      channel, exec `xcrun xcodebuild -version`, read to EOF, and
      read back exit code 0; exec a missing command and read back
      exit code 127.

### Task 4 - libstore presets + target + scanroot

- **Status**: done
- **Blocked by**: none
- **User stories covered**: 3, 21, 22, 23, 24, 30, 32

#### What to build

Per-connection persistence for everything recon remembers: named
run-config presets (project/scheme/config/bundle id) with a
last-active marker, the separately-remembered target, and the
scan root — each bound to the connection so every Mac carries its
own (PRD Solution; stories 3, 21–24, 30, 32).

#### Technical Details

Per ARD §"Extended: `libstore.a`" and §"Interfaces
(`include/store.h`)" + §"On-disk formats": add `store_conn_key`
(the stable `user@host:port` identity used to key all three
families) and reuse libstore's existing private atomic-write and
`mkdirs_for` helpers; the connection `Conn` struct and format are
untouched. Add `Preset`/`PresetList` with `preset_load`/
`preset_save` over `~/.config/ostrich/presets` (records carry
`conn`, `name`, the four fields, and `active=1` on the last-active
preset; `preset_save` read-modify-writes, replacing one
`conn_key`'s records and preserving the rest). Add
`RememberedTarget` with `target_load`/`target_save` over
`~/.config/ostrich/targets` (`udid` + cached `name`; kind is not
persisted — re-derived on a fresh sweep). Per the scan-root
decision, add a dedicated `~/.config/ostrich/scanroots` family
keyed by `conn_key` with its own load/save, independent of target
state. Unknown keys are ignored for forward compatibility.
`preset_load` allocates into a caller `Arena *`; all return
`StoreStatus`. Extend `tests/store_test.c` with
serialize/deserialize round-trips, redirecting `XDG_CONFIG_HOME`
to a temp dir as the existing tests do.

#### Acceptance criteria

- [ ] `store_conn_key` produces `user@host:port` matching how the
      app already compares connection identity.
- [ ] Preset round-trip preserves all four fields, the `name`, and
      the `active` marker; `preset_save` for one connection leaves
      other connections' records intact.
- [ ] Remembered-target round-trip preserves `udid` + `name` and
      stores no kind.
- [ ] Scan-root round-trip restores the per-connection root; a
      connection with no saved root yields the documented default
      handling.
- [ ] Files are written atomically at `0600`; unknown keys in a
      hand-written record are ignored on load. `make test` passes
      with the new `store_test` cases.

### Task 5 - lexicon recon keys

- **Status**: done
- **Blocked by**: none
- **User stories covered**: 45, 46

#### What to build

All recon copy as centralized lexicon keys so every recon surface
draws its wording from the single strings table and a future
straight-mode stays a no-UI swap (stories 45, 46). `theme.md`
already specifies the strings; this lands them in code.

#### Technical Details

Per ARD §"Extended: `libui.a` and the lexicon" and §"Interfaces
(`include/lexicon.h`)": add the recon `LexKey`s under a
Discovery / recon group to `include/lexicon.h` and their strings
to `src/lexicon.c` — `LEX_REC_SCAN_HOST` (`⌖ SCAN HOST`),
`LEX_REC_ABORT_SCAN` (`■ ABORT SCAN`), `LEX_REC_BLUEPRINTS`
(`BLUEPRINTS RECOVERED`), `LEX_REC_NO_BLUEPRINTS`
(`// NO BLUEPRINTS`), `LEX_REC_SWEEP` (`↻ SWEEP FOR TARGETS`),
`LEX_REC_TARGETS` (`TARGETS IN RANGE`), `LEX_REC_NO_TARGETS`
(`// NO TARGETS IN RANGE`), `LEX_REC_NO_OP`
(`// NO OPERATION CONFIGURED`), `LEX_REC_READY` (`READY`),
`LEX_REC_ERR_XCODE` (`XCODE NOT FOUND`), `LEX_REC_ERR_INVENTORY`
(`COULD NOT READ INVENTORY`), plus the scan-root / scheme /
config / bundle-id / preset field labels. Keep the strings
verbatim from `theme.md`'s canonical lexicon. Extend
`tests/lexicon_test.c` to assert each new key resolves to its
non-empty string.

#### Acceptance criteria

- [ ] Every recon key above exists in `include/lexicon.h` and maps
      to its `theme.md` string in `src/lexicon.c`.
- [ ] `lex()` returns the exact themed strings (no `(?)`) for all
      new keys; UTF-8 glyphs (`⌖`, `■`, `↻`, `//`) are preserved.
- [ ] `tests/lexicon_test.c` asserts the new keys and `make test`
      passes.

### Task 6 - session 5a: engine + SCAN + ABORT

- **Status**: pending
- **Blocked by**: 1, 3
- **User stories covered**: 1, 11, 36, 37, 47, 53

#### What to build

The discovery job engine inside the existing worker, proven with
the SCAN HOST path: submit a scan, stream curated blueprints back
progressively, and abort a long scan — all off the UI thread over
the one session, with no fake delay (stories 1, 11, 36, 37, 47,
53). Also creates the `discovery_smoke` dev tool so the engine is
verifiable before any UI exists.

#### Technical Details

Per ARD §"Extended: `libsession.a` — the discovery job engine",
§"End-to-end data flow (SCAN HOST)", §"Interfaces
(`include/session.h`)", and §"Arenas": add the second ring pair
(`SessionDiscCmd` / `SessionDiscEvent`) created via `spsc_create`
at `session_open` (flagged non-arena, like the existing rings),
plus `session_disc_submit` / `session_disc_poll`. The connection
`SessionCmd`/`SessionEvent` rings and `connstate` are untouched
(purely additive). Add the fixed-capacity job table
(`DISC_MAX_JOBS`), each job owning one `SshChannel*`, a kind, an
accumulation buffer, and a bound job-arena from a pool reset and
returned on completion (separate from the connection arena). One
worker drains both inboxes and drives every in-flight job's read
loop each wakeup over the one fd; on channel EOF it calls
`libdiscovery` to curate in the job arena and pushes one
fixed-size POD per blueprint (`DEV_BLUEPRINT`) terminated by
`DEV_SCAN_COMPLETE{count}` or `DEV_SCAN_FAILED{DiscStatus}`.
Refine `compute_timeout_ms` so that while any job is in flight the
poll is bounded and watches the ssh fd for `POLLIN|POLLOUT` while
keepalive still fires; discovery commands are accepted only in
`SUB_ONLINE`, and a mid-job link drop fails in-flight jobs with
`*_FAILED`. `ABORT_SCAN` closes the scan channel, resets its
arena, and emits a terminal scan event. Map exit 127 →
`DISC_ERR_XCODE_MISSING`, other non-zero → `DISC_ERR_COMMAND_FAILED`
(ARD §"Failure mapping"). Create `tools/discovery_smoke.c` to
submit SCAN, print streamed blueprints, and exercise ABORT,
mirroring `tools/session_smoke.c`. No automated test binary
(ARD §"Out of Scope").

#### Acceptance criteria

- [ ] `session.h` declares `SessionDiscCmd`, `SessionDiscEvent`,
      `session_disc_submit`, `session_disc_poll` per ARD
      §"Interfaces"; the existing connection rings/types and
      `connstate` are unchanged and their tests still pass.
- [ ] Only fixed-size PODs cross the discovery rings; no arena
      pointer is shared across the thread boundary.
- [ ] Job arenas are reset and returned to the pool on completion;
      a scan in flight is undisturbed by connection churn and vice
      versa.
- [ ] `make test` passes (no new test binary; connection,
      spsc_ring, and connstate suites green).
- [ ] Manually via `discovery_smoke` against a live Mac: SCAN HOST
      on a real root streams curated BLUEPRINTS progressively and
      ends with a completion count; ABORT during a deep scan halts
      it and emits a terminal event; a root that errors yields the
      mapped `DEV_SCAN_FAILED` status; the worker never blocks
      (keepalive continues during a scan).

### Task 7 - session 5b: READ_BLUEPRINT + RESOLVE_BUNDLE_ID

- **Status**: pending
- **Blocked by**: 6, 2
- **User stories covered**: 12, 13, 16, 17, 38, 39, 40

#### What to build

The blueprint-read jobs: given a chosen project, run
`xcodebuild -list` and stream its schemes and configurations, then
resolve a best-effort bundle id from the chosen scheme/config's
build settings — each off-thread so a slow `-showBuildSettings`
never blocks scheme/config display (stories 12, 13, 16, 17, 38–40).

#### Technical Details

Per ARD §"Extended: `libsession.a`" (the remaining job kinds) and
the data-flow note that READ_BLUEPRINT streams
`DEV_SCHEME`/`DEV_CONFIG` then a bundle-id follow-up: add
`DCMD_READ_BLUEPRINT` (builds via `disc_list_cmd`, parses with
`disc_parse_list`, streams `DEV_SCHEME`/`DEV_CONFIG`, terminates
with completion/`DEV_BLUEPRINT_FAILED`) and `DCMD_RESOLVE_BUNDLE_ID`
(builds via `disc_build_settings_cmd`, parses with
`disc_parse_bundle_id`, emits `DEV_BUNDLE_ID`). The bundle-id
resolve is its own job so it never blocks scheme/config display
(ARD §"Risks"). Reuse the task-6 engine, job table, and arena
pool unchanged. Extend `tools/discovery_smoke.c` to drive both
against a chosen project path.

#### Acceptance criteria

- [ ] `DCMD_READ_BLUEPRINT` and `DCMD_RESOLVE_BUNDLE_ID` reuse the
      existing engine/job-table/arena-pool with no re-architecture.
- [ ] `make test` passes (parser cases from task 2 cover the parse
      logic; no new session test binary).
- [ ] Manually via `discovery_smoke` against a live Mac: selecting
      a real project streams its schemes and configurations, then a
      resolved bundle id arrives separately; a project missing
      Xcode tooling yields `XCODE NOT FOUND`
      (`DISC_ERR_XCODE_MISSING`); unparseable output yields the
      parse failure and does not crash.

### Task 8 - session 5c: SWEEP

- **Status**: pending
- **Blocked by**: 6, 2
- **User stories covered**: 26, 27, 34, 35, 41

#### What to build

The target sweep: query physical devices and simulators
concurrently and merge them into one unified, re-sweepable target
stream so a sweep returns quickly and on demand (stories 26, 27,
34, 35), with an empty result reading as a normal empty state
(story 41).

#### Technical Details

Per ARD §"Extended: `libsession.a`" (Sweep merge) and the
data-flow note: add `DCMD_SWEEP_TARGETS`, which opens `devicectl`
and `simctl` as two jobs in one group (built via
`disc_devicectl_cmd`/`disc_simctl_cmd`, parsed via
`disc_parse_devicectl`/`disc_parse_simctl`). Each streams its
`DEV_TARGET` records tagged device/sim as it finishes; the worker
emits one `DEV_SWEEP_COMPLETE` only after both group members
finish (per-group completion count), or `DEV_SWEEP_FAILED` on
failure. Two channels in flight at once is the concurrency the PRD
requires; the job pool caps the maximum. Reuse the task-6 engine.
Extend `tools/discovery_smoke.c` to drive a sweep.

#### Acceptance criteria

- [ ] A sweep opens devicectl and simctl as two grouped jobs in
      flight simultaneously over the one fd; `DEV_SWEEP_COMPLETE`
      fires only after both finish.
- [ ] Targets stream tagged device vs. simulator with booted state
      where applicable; an empty sweep yields
      `DEV_SWEEP_COMPLETE{count=0}` (not a failure).
- [ ] `make test` passes (no new session test binary).
- [ ] Manually via `discovery_smoke` against a live Mac: a sweep
      lists both real devices and simulators in one stream and
      returns promptly; re-running the sweep reflects a
      newly-booted simulator without reconnecting.

### Task 9 - slice A: project discovery

- **Status**: pending
- **Blocked by**: 6, 5, 4
- **User stories covered**: 2, 4, 8, 9, 10, 19, 20, 42, 44, 48

#### What to build

The first recon surface end-to-end: point at a root, SCAN HOST,
see BLUEPRINTS RECOVERED stream in, pick a project or type a path
manually, with themed empty/error states — and the scan root
restored per connection. On-demand only (story 4), keyboard-
drivable (story 48), with manual entry always open so a discovery
miss never blocks (stories 9, 19, 20, 42). This task extends the
`ui_frame` signature that later slices build on.

#### Technical Details

Per ARD §"Extended: `libui.a`" (the `UiReconView`/`UiReconIntents`
contract and the extended `ui_frame`) and §"Extended: the app
layer — orchestration": extend `ui_frame` to take the recon view,
the working `RunConfig`, and the recon intents alongside the
connection ones, so one ImGui frame builds every panel. libui adds
the scan-root field + SCAN HOST/ABORT SCAN, the BLUEPRINTS list +
select + manual path entry, and the `// NO BLUEPRINTS` /
`XCODE NOT FOUND` / `COULD NOT READ INVENTORY` states, drawing all
wording from the task-5 lexicon and honoring palette discipline
(decorative chrome vs. semantic failure colors). The app gains the
mutable `RunConfig` working struct (fixed inline buffers, like
`ConnForm`) and a blueprints arena reset at the start of each
SCAN; it translates SCAN/ABORT intents into `DCMD_SCAN_HOST`/
`DCMD_ABORT_SCAN`, drains `DEV_BLUEPRINT`/`DEV_SCAN_COMPLETE`/
`DEV_SCAN_FAILED`, copying streamed records into the blueprints
arena. On the transition to `CONN_ONLINE` it loads the
connection's saved scan root via `store` (task 4) and prefills the
field. Failure events render the matching themed reason and leave
manual entry open. `ui_test.c` keeps passing in headless mode.

#### Acceptance criteria

- [ ] `ui_frame`'s extended signature (recon view + `RunConfig` +
      recon intents) matches ARD §"Interfaces"; the app builds the
      recon view-model each frame and the existing connection
      panels are unaffected.
- [ ] SCAN streams blueprints into the list progressively;
      `// NO BLUEPRINTS` renders on an empty success and the
      themed failure lines render on `DEV_SCAN_FAILED`, with manual
      path entry always available.
- [ ] The scan root is restored from `store` on connect and
      editable; the surface is keyboard-drivable and uses semantic
      colors only for failure.
- [ ] `build/ostrich` builds clean and `make test` passes
      (`ui_test` headless still green).
- [ ] Manually against a live Mac: connect → SCAN a real root →
      curated projects appear and one can be selected, or a path
      typed manually; ABORT works; the window never freezes.

### Task 10 - slice B: scheme/config/bundle-id

- **Status**: pending
- **Blocked by**: 9, 7
- **User stories covered**: 14, 15, 18, 19, 20, 49

#### What to build

Picking a project fills the scheme, build configuration, and
bundle id as prefilled-but-editable inputs, each with the full
discovered set shown as a non-blocking hint, and never silently
re-corrected after a manual edit (stories 14, 15, 18–20, 49).

#### Technical Details

Per ARD §"Extended: the app layer" (the blueprint-read flow and
per-field user-edited flags) and §"Extended: `libui.a`": libui
renders the three editable inputs each with a
`> discovered: …` hint beside it. The app adds per-field
"user-edited" flags so prefill fills only empty/untouched fields
and never overrides a manual edit. Selecting a project submits
`DCMD_READ_BLUEPRINT`, prefills scheme (best-guess primary) and
config (defaulting to `Debug`) from the streamed sets, then
submits `DCMD_RESOLVE_BUNDLE_ID` to prefill the bundle id; editing
scheme or config re-submits `RESOLVE_BUNDLE_ID` unless the bundle
id was manually edited. All three remain free-text inputs (not
locked dropdowns); discovered sets are shown only as hints.

#### Acceptance criteria

- [ ] Selecting a project prefills scheme + config and, after the
      resolve job, the bundle id; the discovered sets show as
      non-blocking hints beside each field.
- [ ] Editing scheme/config re-resolves the bundle id only while
      the bundle-id field is untouched; once edited, the manual
      value sticks (story 49).
- [ ] Every field accepts manual entry and is never reverted by a
      later discovery result; `build/ostrich` builds and
      `make test` passes.
- [ ] Manually against a live Mac: pick a real multi-scheme
      project → the primary scheme + `Debug` + a plausible bundle
      id prefill; typing a different scheme updates the resolved
      bundle id, and hand-editing the bundle id is preserved.

### Task 11 - slice C: presets

- **Status**: pending
- **Blocked by**: 9, 4
- **User stories covered**: 25

#### What to build

Saving the assembled four-field configuration as a named preset
bound to the connection, with full new/rename/delete/choose, the
last-active preset restored on connect, and a themed empty state
before any preset exists (PRD Solution; stories 21–25, restore per
story 24). (Stories 21–24 are realized jointly with the task-4
store; 25 is the empty state delivered here.)

#### Technical Details

Per ARD §"Extended: the app layer" (preset CRUD and
last-active/persistence translation) and §"Extended: `libui.a`":
libui renders the preset selector + new/rename/delete/choose and
the `// NO OPERATION CONFIGURED` empty state. The app translates
those intents into `preset_load`/`preset_save` (task 4), keeping
the active marker current, and on the transition to `CONN_ONLINE`
loads the connection's last-active preset and applies it to the
working `RunConfig` so the daily path is connect → ready with no
re-scan. Presets hold exactly the four fields; the target is not
among them.

#### Acceptance criteria

- [ ] New/rename/delete/choose all work and persist via `store`;
      switching presets repopulates the four fields.
- [ ] The last-active preset is restored and applied on connect;
      `// NO OPERATION CONFIGURED` renders when none exists.
- [ ] Presets are connection-scoped — a second connection shows
      its own set, not the first's.
- [ ] `build/ostrich` builds and `make test` passes.
- [ ] Manually: assemble a config, save it, relaunch/reconnect,
      and confirm it is restored without re-scanning.

### Task 12 - slice D: targets + READY

- **Status**: pending
- **Blocked by**: 9, 8, 4
- **User stories covered**: 28, 31, 33, 43

#### What to build

The target selector and the READY gate: SWEEP FOR TARGETS lists
devices and simulators in one labeled set, the pick is
session-sticky with the type inferred, the last target is
remembered separately and silently re-selected on connect when in
range (else `// NO TARGETS IN RANGE`), and READY lights when the
preset is complete and a target is selected (stories 26–35, 43–44;
the single auto-sweep per PRD "Resolved tension"). (Sweep/parse
behaviors land in tasks 8/2; this slice delivers selection,
stickiness, re-validation, and the READY surface.)

#### Technical Details

Per ARD §"Extended: the app layer" (auto-sweep on connect,
readiness) and §"Extended: `libui.a`": libui renders SWEEP FOR
TARGETS + the unified `TargetList` with device/sim + booted
labels + select, the `// NO TARGETS IN RANGE` state, and the
READY indicator with themed hints for what is missing. The app
adds a targets arena reset at the start of each SWEEP, drains
`DEV_TARGET`/`DEV_SWEEP_COMPLETE`/`DEV_SWEEP_FAILED` into it,
holds the session-sticky selection (type inferred from the pick,
not persisted), and on the transition to `CONN_ONLINE` submits the
single lightweight SWEEP — the only unprompted discovery — then
matches the remembered udid from `store` (task 4) and re-selects
it or falls to the unselected/`NO TARGETS IN RANGE` state.
Readiness is computed each frame via `disc_readiness` (task 1)
over the working `RunConfig` + target-selected and surfaced in the
view-model.

#### Acceptance criteria

- [ ] A sweep renders devices and simulators in one labeled list;
      picking one infers device vs. simulator and the selection
      sticks for the session.
- [ ] The remembered target is re-selected on connect when in
      range via the single auto-sweep; when absent,
      `// NO TARGETS IN RANGE` (or unselected) renders. The target
      is never written into a preset.
- [ ] READY lights only when project, scheme, config, bundle id
      are present and a target is selected; otherwise the specific
      missing-field hint shows.
- [ ] `build/ostrich` builds and `make test` passes.
- [ ] Manually against a live Mac: sweep, pick a target, complete
      a preset → READY lights; unplug/remove the target and
      reconnect → it is not re-selected and the empty state shows;
      re-sweeping after booting a simulator surfaces it.
