# ARD — Discovery (Recon the Mac for build inputs)

## PRD

This ARD realizes `context/projects/discovery/prd.md`. In short:
once breached, ostrich interrogates the Mac for everything a build
needs and lets the operator assemble a run configuration from
selectable, discovered inputs instead of memorized strings —
SCAN HOST finds buildable Xcode projects under a pointed-at root,
picking one prefills scheme / configuration / bundle id, and
SWEEP FOR TARGETS lists devices and simulators in one unified set.
Run configurations persist as per-connection named presets; the
target is a session-sticky selection kept out of the preset and
re-validated on connect. Everything runs off the UI thread, over
the existing multi-channel session, concurrently and cancelably,
degrades gracefully to manual entry, and stops at READY — no Play
orchestration.

The PRD fixes the *behaviors*; this ARD fixes the *mechanism*: the
worker model, the cross-thread protocol, the parser placement, the
arenas, the library boundaries, and the public interfaces.

## Explanation of Architectural Components

### The architecture discovery inherits

The connection project established a layered, library-per-module
design with a clean functional-core / imperative-shell split:

- **`libssh.a`** — a thin, non-blocking wrapper over libssh2.
  Stepwise primitives (`ssh_handshake_step`, `ssh_auth_step`,
  `ssh_probe_step`, `ssh_channel_open`); caller-supplied arena; no
  hidden buffers. A single libssh2 session is driven from one
  thread only.
- **`libconnstate.a`** — the *pure* connection lifecycle state
  machine. No I/O, no threads. Black-box tested.
- **`libsession.a`** — the *imperative shell*: one worker thread
  owning one `Ssh*`, driven through sub-phases. UI↔worker is two
  SPSC rings of **fixed-size POD records** (`SessionCmd` /
  `SessionEvent`) plus a self-pipe wakeup. No pointer ever crosses
  the thread boundary; data is copied by value.
- **`libstore.a`** — saved connections, persisted as key=value
  records (blank-line separated) with an atomic, 0600 write at
  `~/.config/ostrich/connections`. Unknown keys are ignored for
  forward compatibility.
- **`libui.a`** — Dear ImGui (C++) sealed behind a pure-C header.
  The app builds a read-only **view-model** each frame and receives
  discrete **intents** back (`ui_frame(ui, view, form, intents)`).
- **app layer** (`src/main.c`, `src/app/*.c`) — orchestration glue,
  not a library: drains session events → updates persistent view
  state → builds the view-model → `ui_frame` → translates intents
  into session commands.

Two invariants matter most for discovery and are preserved
unchanged: **one libssh2 session driven from one thread**, and
**only fixed-size PODs cross the ring** (no shared pointers, an
explicit copy/handoff into the consumer's own memory).

### What discovery adds and changes

#### New: `libdiscovery.a` — the pure functional core

The deep module of this project, behind `include/discovery.h`. No
I/O, no threads, no GL — the trustworthy, host-free core of
`make test`. It owns three pure concerns:

- **Command construction.** Given inputs (a scan root + depth, a
  project path, a scheme/config), it produces the exact remote
  command string: the `find` invocation with the heavy-directory
  `-prune` set and `-maxdepth`; `xcodebuild -list -json`;
  `xcodebuild -showBuildSettings -json`; `xcrun simctl list
  devices --json`; `xcrun devicectl list devices --json-output -`.
  These builders are **shell-safe**: project paths and roots are
  single-quote-escaped, because Mac paths routinely contain spaces
  (`~/Developer/My App.xcodeproj`). Building the command here keeps
  it unit-testable ("the scan command for root R, depth D, prunes
  node_modules and emits both project kinds").
- **Parsing**, via the vendored **jsmn** tokenizer. jsmn fills a
  caller-sized token array (zero internal allocation — it lives in
  a caller arena), and the discovery lib walks tokens looking only
  for the keys it needs, ignoring everything else. This makes the
  parse **tolerant of additive Xcode-version changes by
  construction**: an unrecognized field cannot break it. Parsers:
  `-list` → schemes[] + configurations[]; `-showBuildSettings` →
  `PRODUCT_BUNDLE_IDENTIFIER`; `simctl` + `devicectl` → unified
  `Target` records; and the `find` output → candidate paths.
- **Curation** (the semantic half of the scan, per the resolved
  "coarse prune remote, curate local" split): dedup the throwaway
  `project.xcworkspace` inside every `.xcodeproj`, prefer the
  `.xcworkspace` when a workspace and project sit together, and
  shape the final BLUEPRINTS list. The remote `find` already did
  the cheap structural pruning/`-maxdepth`; the semantic rules that
  warrant fixtures live here.
- **Readiness.** A pure predicate over the working run config plus
  "is a target selected" that names exactly what is missing
  (no project / scheme / config / bundle id / target).

All allocating calls take a caller `Arena *`; every fallible call
returns a `DiscStatus` and has a companion `disc_status_str`.

#### Extended: `libssh.a` — channel exec primitives

`libssh` gains low-level, non-blocking channel calls so the worker
can run commands and read their output, several channels at once,
without blocking: `ssh_channel_exec`, `ssh_channel_read` (into a
caller buffer), `ssh_channel_eof`, `ssh_channel_exit` (exit code),
`ssh_channel_close`. The **worker owns the read loop and
accumulation**; libssh holds no output buffer and stays a thin
wrapper, consistent with the existing `*_step` granularity. All
channels multiplex over the **single** session socket fd.

#### Extended: `libsession.a` — the discovery job engine

The worker is extended (not re-architected) into a command engine:

- **Dedicated discovery rings.** A second UI→worker ring
  (`SessionDiscCmd`: SCAN / READ_BLUEPRINT / RESOLVE_BUNDLE_ID /
  SWEEP / ABORT_SCAN) and a second worker→UI ring
  (`SessionDiscEvent`: streamed item records + completion / failure
  events). The connection project's `SessionCmd` / `SessionEvent`
  and their tests are untouched. One worker drains both inboxes and
  feeds both outboxes each wakeup.
- **A job table.** A fixed-capacity set of in-flight jobs, each
  owning one `SshChannel*`, a `DiscJobKind`, an accumulation
  buffer, and (for sweeps) a group id. The worker drives every
  in-flight job's read loop each wakeup, all over the one fd. The
  concurrency the PRD requires — devicectl and simctl queried in
  parallel — is two channels in flight at once; the pool caps the
  maximum (`DISC_MAX_JOBS`, e.g. 4).
- **A job-arena pool.** `DISC_MAX_JOBS` arenas, one bound to each
  in-flight job for its raw-output accumulation and its parsed
  structs. An arena is **reset and returned to the pool when its
  job completes** — freeing by reset, grouped by lifetime, never
  freeing one job's bytes mid-flight. This is *separate* from the
  worker's connection arena (which is reset on disconnect), so a
  scan in flight is never disturbed by connection churn, and vice
  versa.
- **Parse-on-worker, stream records.** When a channel reaches EOF,
  the worker calls `libdiscovery` to parse + curate the
  accumulated bytes in that job's arena, then **pushes one small
  fixed-size POD per result item** over the discovery event ring
  (`DEV_BLUEPRINT`, `DEV_SCHEME`, `DEV_CONFIG`, `DEV_BUNDLE_ID`,
  `DEV_TARGET`), terminated by a `DEV_*_COMPLETE` (carrying a
  count) or a `DEV_*_FAILED` (carrying a `DiscStatus`). This bounds
  cross-thread traffic to distilled records, honors the
  no-pointers-cross-threads invariant, and yields progressive
  display.
- **Sweep merge.** SWEEP opens devicectl and simctl as two jobs in
  one group. Each streams its `DEV_TARGET` records (tagged
  device/sim) as it finishes; the worker emits one
  `DEV_SWEEP_COMPLETE` only after **both** group members finish (a
  per-group completion count). Unification into one list happens
  naturally as the UI appends the streamed records.
- **Cancel.** ABORT_SCAN closes the scan job's channel and resets
  its arena; the remote `find` receives EOF on its next write. The
  worker emits a terminal scan event so the UI leaves the scanning
  state. ABORT_SCAN is a discovery command, distinct from the
  connection project's `CMD_ABORT`/`CMD_CLOSE`.
- **Loop refinement.** `compute_timeout_ms` gains a case: while any
  job is in flight, the poll timeout is bounded (min of the
  keepalive deadline and a short cap) and the loop watches the ssh
  fd for `POLLIN|POLLOUT`, so reads make progress while keepalive
  still fires. Discovery commands are accepted only while
  `SUB_ONLINE`; if the link drops mid-job, in-flight jobs fail and
  emit `*_FAILED`, and the connection rings report the drop as
  before.

#### Extended: `libstore.a` — presets and remembered target

`libstore` grows two new record families, reusing its existing
private atomic-write and `mkdirs_for` helpers (no promotion to a
shared header needed; the connection `Conn` struct and format are
untouched):

- **Presets** in `~/.config/ostrich/presets`: each record carries
  `conn=user@host:port` plus `name` and the four fields
  (`project`, `scheme`, `config`, `bundleid`), with `active=1`
  marking the last-active preset for that connection.
- **Remembered target** in `~/.config/ostrich/targets`: keyed the
  same way, holding `udid` and a cached display `name`. Target
  **kind is not persisted** — it is re-derived when the udid is
  matched in a fresh sweep.

Both files are connection-keyed by the **stable Mac+account
identity** `user@host:port` (matching how `app.c` already compares
connection identity), not the renameable label. `preset_save`
read-modify-writes the shared file, replacing the records for one
`conn_key` and preserving all others.

#### Extended: `libui.a` and the lexicon

`libui` gains a recon read-only view-model (`UiReconView`) and
intents (`UiReconIntents`) covering: the scan-root field and SCAN
HOST / ABORT SCAN; the BLUEPRINTS list + select + manual path
entry; the scheme / config / bundle-id editable inputs each with a
non-blocking discovered-set hint; the preset selector + new /
rename / delete / choose; SWEEP FOR TARGETS + the unified target
list + select; the READY indicator; and the themed empty / error
states. `ui_frame` is extended to take the recon view, the working
`RunConfig`, and the recon intents alongside the connection ones,
so a single ImGui frame builds every panel. The lexicon gains the
recon keys (see Interfaces); the strings table remains the single
source of voice so a future straight-mode is a no-UI swap.

#### Extended: the app layer — orchestration

The app owns discovery's policy and working state, mirroring how it
owns `ConnForm` and the connection view state:

- A mutable **`RunConfig`** working struct (fixed char buffers,
  like `ConnForm`) for the editable fields, plus **per-field
  "user-edited" flags** so prefill fills only empty/untouched
  fields and never silently overrides a manual edit (PRD story 49).
- Two UI-side arenas: a **blueprints arena** reset at the start of
  each SCAN, and a **targets arena** reset at the start of each
  SWEEP. Streamed records are copied out of the ring into these.
- **Auto-sweep on connect.** On observing the transition to
  `CONN_ONLINE`, the app loads the connection's last-active preset
  and remembered target from `libstore`, then submits the single
  lightweight SWEEP — the only unprompted discovery. When sweep
  results arrive it matches the remembered udid and re-selects it,
  or falls to a `// NO TARGETS IN RANGE` / unselected state.
- **Blueprint read flow.** Picking a project submits READ_BLUEPRINT
  (runs `-list`, streams schemes/configs, prefills), then resolves
  bundle id via `-showBuildSettings` for the prefilled
  scheme/config. Editing scheme or config re-submits
  RESOLVE_BUNDLE_ID unless the operator has manually edited the
  bundle-id field.
- **Readiness** is computed each frame by calling
  `libdiscovery`'s pure predicate and surfaced in the view-model.
- Preset CRUD and last-active/target persistence are translated
  from recon intents into `libstore` calls.

#### New submodule: `third_party/jsmn`

A single-header, zero-allocation JSON tokenizer, vendored as a git
submodule alongside the others, compiled into `libdiscovery`.

### End-to-end data flow (SCAN HOST)

```
UI thread                         worker thread
 ──────────                        ─────────────
 SCAN HOST intent
   → session_disc_submit ───────▶ drain disc_cmd ring
   (writes self-pipe wakeup)        take a job + arena from pool
                                     disc_scan_cmd() → find string
                                     ssh_channel_open + _exec
 reset blueprints arena            ◀ DEV_SCAN_STARTED
 ...render "scanning… ■ ABORT"      drive read loop each wakeup,
                                     accumulate into job arena
                                    on channel EOF:
                                     disc_curate_blueprints(arena)
 pop DEV_BLUEPRINT  ◀──────────────  push one POD per blueprint
   copy into blueprints arena       push DEV_SCAN_COMPLETE{count}
 render BLUEPRINTS RECOVERED        reset+return job arena to pool
```

SWEEP is the same with two grouped jobs feeding `DEV_TARGET`;
READ_BLUEPRINT streams `DEV_SCHEME`/`DEV_CONFIG` then a bundle-id
follow-up. Failures (xcode missing, non-zero exit, unparseable
output) arrive as `DEV_*_FAILED{DiscStatus}` and the UI renders the
matching themed reason; an empty-but-successful result arrives as
`*_COMPLETE{count=0}` and renders as a themed empty state, not an
error.

## Interfaces

### `include/discovery.h` (new public contract)

```c
/* A borrowed byte span (house style; may be promoted to a shared
   header later). */
typedef struct { const char *data; size_t len; } Str;

typedef enum {
    DISC_OK = 0,
    DISC_ERR_XCODE_MISSING,  /* xcrun/xcodebuild absent (127)   */
    DISC_ERR_COMMAND_FAILED, /* non-zero exit, not parseable    */
    DISC_ERR_PARSE,          /* output did not parse            */
    DISC_ERR_OOM             /* arena/token space exhausted     */
} DiscStatus;

/* ── command construction (shell-safe; quotes paths) ───────── */
DiscStatus disc_scan_cmd(const char *root, int max_depth,
                         char *buf, size_t cap);
DiscStatus disc_list_cmd(const char *project_path,
                         char *buf, size_t cap);
DiscStatus disc_build_settings_cmd(const char *project_path,
                                   const char *scheme,
                                   const char *config,
                                   char *buf, size_t cap);
DiscStatus disc_simctl_cmd(char *buf, size_t cap);
DiscStatus disc_devicectl_cmd(char *buf, size_t cap);

/* ── results ───────────────────────────────────────────────── */
typedef struct { char path[1024]; bool is_workspace; } Blueprint;
typedef struct { Blueprint *items; int count; } BlueprintList;

typedef struct {
    char name[256];
    char udid[128];
    bool is_simulator;  /* simctl vs devicectl; inferred       */
    bool booted;        /* simulator booted state              */
} Target;
typedef struct { Target *items; int count; } TargetList;

typedef struct { char (*items)[256]; int count; } StrList;

/* ── parse + curate (raw bytes → structs in arena `a`) ─────── */
DiscStatus disc_curate_blueprints(Arena *a, Str find_out,
                                  int max_depth, BlueprintList *out);
DiscStatus disc_parse_list(Arena *a, Str json,
                           StrList *schemes, StrList *configs);
DiscStatus disc_parse_bundle_id(Str json, char *out, size_t cap);
DiscStatus disc_parse_simctl(Arena *a, Str json, TargetList *out);
DiscStatus disc_parse_devicectl(Arena *a, Str json, TargetList *out);

/* ── readiness (pure) ──────────────────────────────────────── */
typedef struct {
    char project[1024];
    char scheme[256];
    char config[128];
    char bundle_id[256];
} RunConfig;

typedef enum {
    READY_OK = 0,
    READY_NO_PROJECT,
    READY_NO_SCHEME,
    READY_NO_CONFIG,
    READY_NO_BUNDLE_ID,
    READY_NO_TARGET
} Readiness;

Readiness   disc_readiness(const RunConfig *rc, bool target_sel);
const char *disc_status_str(DiscStatus st);
```

### `include/ssh.h` (additions)

```c
/* Start running `cmd` on an opened channel. SSH_AGAIN while the
   request is in flight, SSH_OK once accepted. */
SshStatus ssh_channel_exec(SshChannel *ch, const char *cmd);

/* Read available stdout into buf. SSH_AGAIN if it would block;
   SSH_OK with *out_n bytes (*out_n == 0 at EOF — confirm with
   ssh_channel_eof). */
SshStatus ssh_channel_read(SshChannel *ch, char *buf, size_t cap,
                           size_t *out_n);

bool      ssh_channel_eof(SshChannel *ch);
SshStatus ssh_channel_exit(SshChannel *ch, int *out_code);
void      ssh_channel_close(SshChannel *ch);
```

(`ssh_channel_open` already exists. Exit code 127 is mapped by the
worker to `DISC_ERR_XCODE_MISSING`; other non-zero exits to
`DISC_ERR_COMMAND_FAILED`.)

### `include/session.h` (additions)

```c
typedef enum {
    DCMD_SCAN_HOST,        /* arg=root, max_depth             */
    DCMD_READ_BLUEPRINT,   /* arg=project path                */
    DCMD_RESOLVE_BUNDLE_ID,/* arg=project, scheme, config     */
    DCMD_SWEEP_TARGETS,
    DCMD_ABORT_SCAN
} SessionDiscCmdKind;

typedef struct {
    SessionDiscCmdKind kind;
    char arg[1024];        /* root or project path            */
    int  max_depth;
    char scheme[256];
    char config[128];
} SessionDiscCmd;

typedef enum {
    DEV_SCAN_STARTED, DEV_BLUEPRINT, DEV_SCAN_COMPLETE,
    DEV_SCAN_FAILED,
    DEV_SCHEME, DEV_CONFIG, DEV_BUNDLE_ID, DEV_BLUEPRINT_FAILED,
    DEV_TARGET, DEV_SWEEP_COMPLETE, DEV_SWEEP_FAILED
} SessionDiscEventKind;

typedef struct {
    SessionDiscEventKind kind;
    /* flat fixed payload; fields meaningful per kind          */
    char       path[1024]; bool is_workspace;     /* BLUEPRINT */
    char       name[256];  char udid[128];
    bool       is_simulator; bool booted;         /* TARGET    */
    char       value[256];                        /* SCHEME/.. */
    int        count;                             /* COMPLETE  */
    DiscStatus status;                            /* FAILED    */
} SessionDiscEvent;

bool session_disc_submit(Session *s, const SessionDiscCmd *cmd);
bool session_disc_poll (Session *s, SessionDiscEvent *out);
```

### `include/store.h` (additions)

```c
typedef struct {
    char name[64];
    char project[1024];
    char scheme[256];
    char config[128];
    char bundle_id[256];
} Preset;

typedef struct {
    Preset *items;
    int     count;
    int     active_index; /* last-active for this conn; -1 none */
} PresetList;

typedef struct {
    char udid[128];
    char name[256];       /* cached display name                */
} RememberedTarget;

void        store_conn_key(const Conn *c, char *buf, size_t cap);
StoreStatus preset_load(Arena *a, const char *conn_key,
                        PresetList *out);
StoreStatus preset_save(const char *conn_key, const PresetList *l);
StoreStatus target_load(const char *conn_key, RememberedTarget *o);
StoreStatus target_save(const char *conn_key,
                        const RememberedTarget *t);
```

### `include/ui.h` (additions)

`UiReconView` (read-only): scanning flag, `BlueprintList` view +
selected index, the four field strings + their discovered-set hint
arrays, `PresetList` view + active index, `TargetList` view +
selected index, `Readiness`, and discrete empty/error reasons.
`UiReconIntents`: `scan`, `abort_scan`, `pick_blueprint`,
`manual_path`, field-edit flags, `preset_new/rename/delete/choose`,
`sweep`, `pick_target`. `ui_frame` is extended:

```c
bool ui_frame(Ui *ui,
              const UiConnView *cv, ConnForm *cf, UiIntents *ci,
              const UiReconView *rv, RunConfig *rf,
              UiReconIntents *ri);
```

### `include/lexicon.h` (additions)

New `LexKey`s under a Discovery / recon group: `LEX_REC_SCAN_HOST`
(`⌖ SCAN HOST`), `LEX_REC_ABORT_SCAN` (`■ ABORT SCAN`),
`LEX_REC_BLUEPRINTS` (`BLUEPRINTS RECOVERED`),
`LEX_REC_NO_BLUEPRINTS` (`// NO BLUEPRINTS`),
`LEX_REC_SWEEP` (`↻ SWEEP FOR TARGETS`),
`LEX_REC_TARGETS` (`TARGETS IN RANGE`),
`LEX_REC_NO_TARGETS` (`// NO TARGETS IN RANGE`),
`LEX_REC_NO_OP` (`// NO OPERATION CONFIGURED`),
`LEX_REC_READY` (`READY`),
`LEX_REC_ERR_XCODE` (`XCODE NOT FOUND`),
`LEX_REC_ERR_INVENTORY` (`COULD NOT READ INVENTORY`),
plus field labels (scan root, scheme, config, bundle id, preset).

### On-disk formats

```
~/.config/ostrich/presets         ~/.config/ostrich/targets
  conn=jake@studio-mac:22           conn=jake@studio-mac:22
  name=app                          udid=00008110-001A2D...
  active=1                          name=iPhone 15
  project=/Users/jake/App.xcworkspace
  scheme=App
  config=Debug
  bundleid=com.acme.app
                                  (blank line separates records;
  conn=jake@studio-mac:22          unknown keys ignored)
  name=staging
  project=/Users/jake/App.xcworkspace
  ...
```

## Out of Scope

Carries the PRD's out-of-scope list (Play/build orchestration and
the run-state machine; `.app` output-path resolution; whole-disk
crawl; importing Xcode recents; auto-discovery on connect beyond
the one target re-validation sweep; live hotplug; scheme/config/
bundle-id as locked dropdowns; advanced run-config inputs; test
plans / multi-target / sub-apps; strict Xcode version pinning).

ARD-specific exclusions:

- **No new dedicated test binaries beyond `discovery_test` and the
  `store_test` preset/target extension.** Per the resolved test
  posture, the worker job engine and the recon UI/app glue are
  validated manually (a `discovery_smoke`-style dev tool may exist
  outside `make test`, mirroring `session_smoke`, but is not
  required by this project); no headless recon UI test or app-glue
  test binary is mandated here.
- **No re-architecture of the connection rings or `connstate`.**
  Discovery is purely additive: new rings, new libraries, extended
  headers.
- **No second SSH connection or thread for discovery** — rejected
  in favor of multiplexing the one worker/one session.
- **No streaming of raw command output to the UI thread** — parsing
  stays on the worker; only distilled records cross the boundary
  (the build-log streaming pattern is a later project's concern).

## Further Notes

### Arenas — named with lifetimes

| Arena | Owner | Lifetime / reset |
| --- | --- | --- |
| connection arena | worker | per (re)connect; reset on disconnect (existing) |
| job-arena pool (`DISC_MAX_JOBS`) | worker | one arena bound per in-flight job; reset + returned to pool on job completion |
| blueprints arena | UI/app | reset at the start of each SCAN HOST |
| targets arena | UI/app | reset at the start of each SWEEP |
| jsmn token space | caller-supplied | sub-allocated from the active job arena during parse |

The working `RunConfig` (like `ConnForm`) uses fixed inline
buffers, not an arena. jsmn allocates nothing of its own — its
token array is bump-allocated from the job arena.

### Non-arena allocations (flagged)

- The two new SPSC rings (`disc_cmd`, `disc_evt`) are `spsc_create`
  (malloc) at `session_open` — the same flagged cross-thread-
  lifetime pattern as the existing rings.
- The job-arena pool arenas are `arena_create`d at worker start
  (each reserves its backing region via malloc, like the existing
  worker arena) and `arena_destroy`d at worker exit.
- No per-object `malloc`/`free` is introduced anywhere in the
  discovery paths; jsmn and libssh2 own their internal memory on
  their own terms.

### Failure mapping

| Condition | DiscStatus | Themed reason |
| --- | --- | --- |
| exit 127 / command not found | `DISC_ERR_XCODE_MISSING` | `> XCODE NOT FOUND` |
| non-zero exit, not parseable | `DISC_ERR_COMMAND_FAILED` | `> COULD NOT READ INVENTORY` |
| output present, JSON unparsed | `DISC_ERR_PARSE` | `> COULD NOT READ INVENTORY` + fall back to manual |
| parsed OK, zero items | `DISC_OK`, count 0 | `// NO BLUEPRINTS` / `// NO TARGETS IN RANGE` |

Every failure leaves manual entry open; recon never blocks
configuring a run by hand.

### Upstream reconciliations (carried from the PRD)

- **`workflow.md`** should be updated to match the model resolved
  here: the target is removed from the persisted preset (a
  build-time, session-sticky, separately-remembered selection); the
  device/sim "target type" radio is dropped for one unified target
  list with the type inferred; scheme/config/bundle-id are
  prefilled editable inputs (with a discovered-set hint) while the
  project is a dropdown plus manual entry; the persisted run
  configuration is therefore **four** fields, not six.
- **`theme.md`** should gain the recon lexicon entries above under a
  Discovery / recon group (no structural change; the strings table
  simply gains keys).

These touch product flow / voice (those docs' domains) and are
recorded for upstream folding, the way the connection project
amended its upstream docs.

### Coding-standards conformance checklist

- [x] **Arenas named + lifetimes stated** — see the arena table;
  freeing is by reset (job arenas) or destroy at exit.
- [x] **Allocation is caller-controlled** — every `disc_*`
  allocating call takes an `Arena *`; `ssh_channel_read` writes a
  caller buffer; no hidden allocators.
- [x] **Thread-confinement respected** — results cross as
  fixed-size PODs over the discovery rings and are copied into the
  UI's own blueprints/targets arenas; no arena pointer is shared.
- [x] **Non-arena allocations flagged + justified** — the two new
  rings, the job-arena-pool reservations, and library-owned (jsmn /
  libssh2) memory are called out above.
- [x] **Module → library decisions made** — new `libdiscovery.a`
  (deep, pure, real complexity); extend `libssh`/`libsession`/
  `libstore`/`libui`; app glue stays plain source; jsmn vendored.
- [x] **Library layout specified** — `include/discovery.h` public,
  `src/discovery/*.c` private, `build/libdiscovery.a` archive
  (same form as the others).
- [x] **C/C++ seam identified** — `libui` remains the only C++
  (ImGui) behind its pure-C header; `libdiscovery`, `libssh`,
  `libsession`, `libstore`, and jsmn are all pure C11.
- [x] **Error handling shape confirmed** — `DiscStatus` /
  `SshStatus` / `StoreStatus` enums returned, results via
  out-params, `disc_status_str` companion; no hidden error state,
  no exceptions.
- [x] **Test approach per library** — `tests/discovery_test.c`
  links `libdiscovery.a` and exercises parsing (captured + drifted
  fixtures), curation, and readiness black-box through
  `discovery.h`; `tests/store_test.c` is extended with preset/
  target serialize-deserialize round-trips. Worker dispatch and
  recon UI/app glue are validated manually per the resolved test
  posture (white-box access is not required).

### Risks / open notes

- **`-showBuildSettings` latency.** It can take seconds; running it
  off-thread is exactly why the worker model matters. The bundle-id
  resolve is its own job so it never blocks scheme/config display.
- **`SessionDiscEvent` width.** The flat POD is dominated by
  `path[1024]`; at a ring cap of ~64 that is ~80 KB, acceptable.
  If a future need pushes element size up, the result ring can be
  split by record kind without touching the connection rings.
- **`devicectl` output shape** varies by Xcode; the tolerant
  token-walk and the captured-fixtures test (including a drifted
  sample) are the guard, and any miss degrades to manual, not a
  crash.
