# ARD — Build & Deploy (the Play/Observe core loop)

## PRD

This ARD evaluates
`context/projects/xcode-project-build-and-deploy/prd.md` — the
Play/Observe core loop. The PRD fixes the *behaviors* (one EXECUTE
drives `xcodebuild` → install → launch over the existing session;
build-only COMPILE; universal ABORT; a Build Log of the whole
chain's raw tooling output; a Device Log of the app's own
stdout/stderr via process-console; terminate-first re-Play;
COMPILE-while-running with a stale indicator; bounded ephemeral log
buffers; distinct build vs deploy failures; off-thread, concurrent,
cancelable, never-freeze, output-based liveness) and explicitly
leaves the *mechanism* — worker model, channel allocation, arenas,
and library boundaries — to this ARD. It closes `design.md` core
goals #4–#7 and amends #7 (see Further Notes).

## Explanation of Architectural Components

The whole feature is built as a new **run/observe capability on the
existing single session worker** — a third command/event family
alongside connection and discovery — plus three new pure libraries
and extensions to the UI. Nothing in the connection or discovery
layers is re-architected; this project is the first real payoff of
the worker's multi-channel, off-thread design.

### Where it sits today

- `libsession` (`src/session/session.c`) owns the one worker
  thread. It runs a non-blocking `poll()` loop over a self-pipe and
  the SSH socket, drains UI→worker command rings, drives the
  connection sub-phase machine, and drives a **discovery job
  engine** (`DiscJob` slots, per-job arenas, an
  `OPEN→EXEC→READ→EXIT→EMIT` state machine, the `open_owner`
  channel-open serialization seam, a 60 s per-job watchdog). It
  emits worker→UI events through SPSC rings.
- `libssh` exposes the channel primitives the engine uses
  (`ssh_channel_open/exec/read/eof/exit/close`, `ssh_keepalive`).
  **No new `ssh.h` primitive is required** — every remote action in
  this project (build, settings, boot, install, launch, terminate,
  kill) is an ordinary exec over these primitives.
- `libconnstate` and `libdiscovery` are the existing **deep, pure,
  black-box-tested modules**: a state machine
  (`connstate_step → action`, reason→lexicon) and a
  command-construction + parse + readiness module
  (`disc_*_cmd`, `disc_parse_*`, `disc_readiness`). The new modules
  mirror this split exactly.
- `src/app/app.c` is the composition root: it drains event rings
  each frame, accumulates streamed results into app-owned arenas,
  builds the read-only view-models, and calls `ui_frame`.
  `libui` (C++/ImGui behind a pure-C `ui.h`) renders.
- `RunConfig` and `Target` already live in `discovery.h` and are the
  exact inputs a run needs; `disc_readiness` already gates READY.

### What changes

**1. A new run subsystem inside the worker** (modifies
`libsession`). The discovery job engine accumulates-to-EOF then
parses (request/response); a run instead needs *sequential
streaming steps* plus an *infinite live stream*. Rather than
overload `DiscJob`, the worker gains two new worker-private
structures in `WorkerCtx`:

- **`RunChain`** — the sequential pipeline state for one EXECUTE or
  COMPILE: a state machine stepping through
  `unlock → settings → build → (prime) → install → launch` (COMPILE
  stops after `build`). The `unlock` step is gated on
  `WorkerCtx.kc_pass != ""`; when empty it is skipped and the chain
  starts at `settings` exactly as before. It owns one channel at a
  time, reused across steps, plus the per-run arena. The `settings`
  step is the only request/response step (accumulate
  `xcodebuild -showBuildSettings -json` to EOF, parse the product
  path); `build` streams; the remaining steps are short streamed execs
  whose exit codes drive the run-state machine.
- **`DevConsole`** — the persistent device-console stream. The
  `launch` step runs `… process launch --console` (device) or
  `simctl launch --console` (simulator); on success its channel is
  **handed off** from the `RunChain` to the `DevConsole`, where it
  streams the app's stdout/stderr indefinitely. The `DevConsole`
  holds the running app's identity (bundle id, udid, simulator
  flag) in **fixed struct fields**, so it is independent of the
  per-run arena and survives a COMPILE-while-running arena reset.

Both are driven non-blocking each loop iteration by a new
`drive_run(ctx)`, called after `drive_disc_jobs`. All channel-opens
— disc *and* run — go through the **single shared `open_owner`
seam** (libssh2 tracks open progress per-session, so opens must
serialize); reads run concurrently across channels, giving the
genuine simultaneous Build + Device streaming. At most ~2 run
channels are live at once (a persistent console + one chain step or
a COMPILE build), plus any in-flight disc channel.

**2. A new run command/event family** (modifies `libsession`,
`session.h`). Two new SPSC rings (`run_cmd_ring`, `run_event_ring`)
and `session_run_submit` / `session_run_poll`, mirroring the
discovery family. Commands carry `RCMD_EXECUTE` / `RCMD_COMPILE` /
`RCMD_ABORT` with the `RunConfig` and (for EXECUTE) the `Target`.
Events are a **tagged union**: phase-transition records (a
`RunPhase` plus a failure status) and **raw output-chunk records**
tagged build|device with a fixed-size byte payload. The worker
reads raw channel bytes and pushes them as chunks; it does **no
line-splitting** — that is the UI side's job. Cross-thread bytes are
**copied into the fixed-size record** (thread-confinement: never a
pointer into worker memory).

**3. `librunstate`** — new pure deep module (mirrors
`libconnstate`). The run-state machine:
`runstate_step(rs, event) → action`, phase/reason lookups, the
**build-generation counter** for staleness, and failure
classification. The worker drives it from chain events (settings
done, build exit, install exit, launch accepted, console up/EOF,
abort, drop) and emits `RunPhase` events; the app mirrors phase +
stale for rendering and combines them with `disc_readiness` for
EXECUTE/COMPILE/ABORT enablement. It is the testable heart of PRD
story #54.

**4. `libbuilddeploy`** — new pure deep module (mirrors
`libdiscovery`), consuming `discovery.h`'s `RunConfig`, `Target`,
and `Str`. Shell-safe command construction for every step
(build, settings, boot, bootstatus, install, launch, terminate,
kill), device-vs-simulator `-destination` construction, the
`setsid`/PID-marker launch wrapper, the `-showBuildSettings`
product-path parser, the PID-marker parser, and the
failure-code→reason (lexicon) mapping. All pure; single-quote
path-escaping like `disc_*_cmd`.

**5. `liblogbuf`** — new pure deep module: the UI-side bounded log
line buffer used for both the Build Log and the Device Log.
`logbuf_append(bytes)` incrementally assembles lines (handling
partial lines across chunk boundaries), the store is a **bounded
byte buffer + line index** that drops whole oldest lines when the
cap is hit (raw, never truncated), with a demarcation-insert
(`> ── NEW PAYLOAD ──`), clear, line-access for rendering, and
copy-all. Allocated once from the app arena at startup
(app-lifetime, never reset). This is distinct from `spsc_ring`
(the cross-thread fixed-record transport); `logbuf` is
UI-thread-confined text storage.

**6. UI + composition-root wiring** (modifies `libui`/`src/app`).
`app.c` owns two `LogBuf` instances and a mirrored `RunPhase` +
stale + generation, drains `run_event_ring` each frame (appending
chunks into the right `LogBuf`, applying phase transitions,
inserting the device-log demarcation at the launch→running edge),
and submits run commands from the Control/Status intents. `ui.cpp`
gains the run controls (EXECUTE↔ABORT toggle, COMPILE, the
run-state label, the `build ▷ install ▷ launch` progression, the
stale indicator) and the Build Log / Device Log panels
(auto-scroll with scroll-up pause, copy, clear, themed empty
states). `app` stays pure C; ImGui stays sealed behind `ui.h`.

**7. New lexicon copy** (modifies `liblexicon`). A Build/deploy
group: EXECUTE / COMPILE / ABORT, the run-state labels
(STANDBY, COMPILING EXPLOIT…, PRIMING TARGET…, DEPLOYING PAYLOAD…,
EXECUTING PAYLOAD…, TARGET ACQUIRED // LIVE), EXPLOIT FAILED, the
distinct DEPLOYMENT FAILED // PAYLOAD REJECTED, OPERATION ABORTED,
the NEW PAYLOAD separator, the stale indicator, and the log empty
states / LIVE FEED header. `runstate` and `builddeploy` return
`LexKey`s (like `connstate_reason_lex`) so the UI stays a no-logic
copy lookup.

### Control + data flow for one EXECUTE

```
UI intent ─session_run_submit(EXECUTE,cfg,target)→ run_cmd_ring
worker: RunChain starts; runstate idle→building
  step settings : open→exec(showBuildSettings -json)→read EOF
                  →parse product .app path (per-run arena)
  step build    : open→exec(setsid xcodebuild, PID marker)
                  →stream bytes ─REV_BUILD_LOG chunks→ UI logbuf
                  exit!=0 → runstate→build_failed (EXPLOIT FAILED)
  step prime    : (simulator only) bootstatus -b (boot-if-needed + wait)
  step install  : exec(devicectl/simctl install app_path)
                  →stream→Build Log; exit!=0 → deploy_failed
  step launch   : exec(launch --console); on accept hand channel
                  to DevConsole; runstate→running (LIVE)
DevConsole: stream app stdout/stderr ─REV_DEVICE_LOG→ UI logbuf
            (app inserts NEW PAYLOAD demarcation at running edge)
```

Re-EXECUTE while running is **terminate-first**: `terminate
<bundle>` runs first, the `DevConsole` channel EOFs/closes (device
log goes dark, by design), then a fresh `RunChain` starts. COMPILE
runs only `settings`+`build` on a **second** channel, leaving the
`DevConsole` streaming in parallel, and increments the build
generation so `runstate` flags the deployed app stale. ABORT runs
the two-pronged kill (terminate the app; `kill` the build's process
group on a fresh channel; close local channels) and resolves to
aborted. An SSH drop mid-run is folded into the existing
`disconnect_ssh` path (extended to tear down `RunChain` +
`DevConsole` and emit aborted), so a Wi-Fi blip ends the run
cleanly with local state preserved.

## Interfaces

### `session.h` — run command/event family (additions)

```c
typedef enum {
    RCMD_EXECUTE,   /* full chain: build → install → launch     */
    RCMD_COMPILE,   /* build-only; no target needed             */
    RCMD_ABORT      /* universal stop (chain and/or running app) */
} SessionRunCmdKind;

typedef struct {
    SessionRunCmdKind kind;
    RunConfig         cfg;        /* project/scheme/config/bundle */
    Target            target;     /* EXECUTE: device or simulator */
    bool              has_target; /* false for a no-target COMPILE */
} SessionRunCmd;

typedef enum {
    REV_PHASE,        /* phase transition (+ reason on failure)  */
    REV_BUILD_LOG,    /* raw output chunk → Build Log            */
    REV_DEVICE_LOG,   /* raw output chunk → Device Log           */
    REV_STALE         /* stale flag changed                      */
} SessionRunEventKind;

typedef struct {
    SessionRunEventKind kind;
    RunPhase            phase;   /* REV_PHASE                     */
    BdStatus            reason;  /* REV_PHASE on a failure phase  */
    bool                stale;   /* REV_STALE                     */
    int                 len;     /* REV_*_LOG: bytes in chunk     */
    char                chunk[RUN_CHUNK_CAP]; /* raw, copied      */
} SessionRunEvent;

bool session_run_submit(Session *s, const SessionRunCmd *cmd);
bool session_run_poll  (Session *s, SessionRunEvent *out);
```

### `runstate.h` — pure run-state machine (new library)

```c
typedef enum {
    RUN_IDLE,          /* STANDBY                                */
    RUN_BUILDING,      /* COMPILING EXPLOIT… (incl. settings)    */
    RUN_PRIMING,       /* PRIMING TARGET… (sim boot)             */
    RUN_INSTALLING,    /* DEPLOYING PAYLOAD…                     */
    RUN_LAUNCHING,     /* EXECUTING PAYLOAD…                     */
    RUN_RUNNING,       /* TARGET ACQUIRED // LIVE                */
    RUN_BUILD_FAILED,  /* EXPLOIT FAILED                         */
    RUN_DEPLOY_FAILED, /* DEPLOYMENT FAILED // PAYLOAD REJECTED  */
    RUN_ABORTED        /* OPERATION ABORTED                      */
} RunPhase;

typedef enum {
    RUN_EV_EXECUTE, RUN_EV_COMPILE, RUN_EV_ABORT,
    RUN_EV_SETTINGS_OK, RUN_EV_BUILD_OK, RUN_EV_BUILD_FAIL,
    RUN_EV_PRIME_OK, RUN_EV_PRIME_FAIL,
    RUN_EV_INSTALL_OK, RUN_EV_INSTALL_FAIL,
    RUN_EV_LAUNCH_OK, RUN_EV_LAUNCH_FAIL,
    RUN_EV_CONSOLE_EOF,   /* app exited / target gone → clean    */
    RUN_EV_DROP           /* SSH drop mid-run → aborted          */
} RunEvent;

typedef enum {
    RUN_ACT_NONE, RUN_ACT_RESOLVE, RUN_ACT_BUILD, RUN_ACT_PRIME,
    RUN_ACT_INSTALL, RUN_ACT_LAUNCH, RUN_ACT_TERMINATE_FIRST,
    RUN_ACT_KILL, RUN_ACT_DONE
} RunAction;

typedef struct {
    RunPhase phase;
    int      built_gen;     /* ++ on each successful build       */
    int      deployed_gen;  /* set when a Play launch succeeds   */
    bool     target_is_sim; /* gates the PRIME step              */
} RunState;

void       runstate_init(RunState *rs);
RunAction  runstate_step(RunState *rs, RunEvent ev);
bool       runstate_stale(const RunState *rs);   /* running &&
                                    built_gen > deployed_gen     */
LexKey     runstate_phase_lex(RunPhase p);
LexKey     runstate_reason_lex(RunPhase p, BdStatus st);
const char *runstate_phase_str(RunPhase p);      /* debug/log    */
```

### `builddeploy.h` — pure commands + parse (new library)

```c
#include "discovery.h"  /* RunConfig, Target, Str */
#include "lexicon.h"    /* LexKey */

typedef enum {
    BD_OK = 0,
    BD_ERR_XCODE_MISSING, /* exit 127                            */
    BD_ERR_BUILD,         /* xcodebuild non-zero  → build fail   */
    BD_ERR_BOOT,          /* boot/bootstatus      → deploy fail  */
    BD_ERR_INSTALL,       /* install non-zero     → deploy fail  */
    BD_ERR_LAUNCH,        /* launch non-zero      → deploy fail  */
    BD_ERR_PARSE,         /* settings did not parse              */
    BD_ERR_OOM
} BdStatus;

/* command construction (shell-safe; single-quote-escaped) */
BdStatus bd_settings_cmd (const RunConfig*, const Target*,
                          bool has_target, char*, size_t);
BdStatus bd_build_cmd    (const RunConfig*, const Target*,
                          bool has_target, char*, size_t);
BdStatus bd_bootstatus_cmd(const Target*, char*, size_t); /* boot-if-needed + wait (-b) */
BdStatus bd_install_cmd  (const Target*, const char *app_path,
                          char*, size_t);
BdStatus bd_launch_cmd   (const Target*, const char *bundle_id,
                          char*, size_t);   /* --console, setsid,
                                               PID marker         */
BdStatus bd_terminate_cmd(const Target*, const char *bundle_id,
                          char*, size_t);
BdStatus bd_kill_cmd     (long pgid, char*, size_t);
BdStatus bd_destination  (const Target*, bool has_target,
                          char*, size_t);

/* parse (raw bytes → values) */
BdStatus bd_parse_product_path(Str settings_json,
                               char *out, size_t cap);
bool     bd_parse_pid_marker(Str chunk, long *out_pgid);

/* classification */
LexKey      bd_reason_lex(BdStatus st);
const char *bd_status_str(BdStatus st);
```

### `logbuf.h` — pure bounded line buffer (new library)

```c
#include "arena.h"

typedef struct LogBuf LogBuf;

/* Backed by the app arena (app-lifetime, never reset). Bounds the
   byte store and the line count; drops whole oldest lines when
   either cap is hit. Lines are never truncated. */
LogBuf *logbuf_init(Arena *a, size_t byte_cap, int max_lines);

/* Append raw bytes; assembles lines incrementally, carrying a
   partial line across calls. */
void    logbuf_append(LogBuf *lb, const char *bytes, size_t n);

/* Insert a complete demarcation line (e.g. the NEW PAYLOAD
   separator) flushing any pending partial line first. */
void    logbuf_mark(LogBuf *lb, const char *line);

void    logbuf_clear(LogBuf *lb);
int     logbuf_count(const LogBuf *lb);
/* Borrowed view of line `i` for rendering (NUL-terminated). */
const char *logbuf_line(const LogBuf *lb, int i, size_t *out_len);
/* Flatten all lines into `out` for the copy affordance; returns
   bytes needed (may exceed cap). */
size_t  logbuf_copy_all(const LogBuf *lb, char *out, size_t cap);
```

### `ui.h` — run view-model + intents (additions)

A read-only `UiRunView` (mirrored `RunPhase`, stale flag,
progression, `disc_readiness`, and the two `LogBuf*`s for the
panels) and a `UiRunIntents` (execute, compile, abort,
build-log copy/clear, device-log copy/clear) threaded through
`ui_frame`, following the existing `UiReconView`/`UiReconIntents`
pattern. `ui.h` gains `#include "logbuf.h"`.

## Out of Scope

- **Connection / discovery internals.** The worker loop, the
  connection sub-phase machine, and the discovery job engine are
  reused as-is; only additive hooks (`drive_run`, the shared
  `open_owner` seam, `disconnect_ssh` teardown) touch them.
- **New `ssh.h` primitives.** Every remote action is an ordinary
  exec over the existing channel API. No PTY, no signal channel, no
  port-forwarding.
- **Build-error parsing / jump-to-error, launch args / env / extra
  flags, clean builds / setting overrides, the debugger, VNC /
  screen mirroring, log persistence to disk, an in-app diagnostics
  surface, reattach-across-reconnect, multi-target / test plans,
  multi-Mac / multiple runs.** All deferred per the PRD.
- **New persistence.** Nothing new persists; presets and the
  remembered target already persist from discovery; logs are
  ephemeral `logbuf` buffers.
- **The unified-system-log firehose.** Dropped; the Device Log is
  the app's process console only.
- **Product copy wording.** Final lexicon strings and any
  `theme.md` entries are that doc's call; this ARD fixes only the
  `LexKey` seams.

## Further Notes

### Non-arena allocations (flagged)

- `run_cmd_ring` / `run_event_ring` use `spsc_create` (internal
  `malloc`) — the same flagged exception as every existing
  cross-thread ring; lifetime is the `Session`'s.
- The per-run arena is `arena_create`d at worker init alongside the
  existing `disc_arenas` and destroyed at worker exit.
- No other new `malloc`. `logbuf` is app-arena-backed; libssh2/ImGui
  own their memory on their own terms.

### Liveness, termination, and failure mapping

- **Output-progress watchdog:** each build/deploy step's deadline
  **resets on every byte** received; a step silent past a generous
  stall window (~120 s) fails terminally — replacing the disc 60 s
  total-time cap, which cannot bound a long build or an infinite
  console. The `DevConsole` is **exempt** (an idle app is silent
  for hours); its only liveness is the existing SSH keepalive. App
  exit EOFs the channel → clean STANDBY (`RUN_EV_CONSOLE_EOF`); a
  transport drop → aborted (`RUN_EV_DROP`).
- **Two-pronged kill:** the running app is stopped with explicit
  `devicectl/simctl terminate <bundle>` (also the terminate-first
  step); an in-flight build is launched under `setsid` emitting a
  PID/PGID marker that the worker parses, so ABORT runs
  `kill -- -<pgid>` on a fresh channel (then closes the local
  channel) — guaranteeing no orphaned `xcodebuild` holds the build
  directory.
- **Distinct failures:** build success is determined by the in-band
  `__OSTRICH_EXIT__<n>` marker emitted at the end of `bd_build_cmd`'s
  `setsid sh -c` script (capturing xcodebuild's real `$?`), because the
  SSH channel reports `setsid`'s own exit code (0) rather than
  `xcodebuild`'s. When the marker is present the worker uses it; when
  absent (setsid itself missing, exit 127) it falls back to the channel
  code. Both `__OSTRICH_PGID__` and `__OSTRICH_EXIT__` lines are
  stripped from the Build Log before reaching `logbuf`. A non-zero
  effective build exit → `RUN_BUILD_FAILED` (EXPLOIT FAILED); a
  non-zero `boot/install/launch` exit → `RUN_DEPLOY_FAILED`
  (DEPLOYMENT FAILED // PAYLOAD REJECTED); ABORT / drop →
  `RUN_ABORTED`. `builddeploy` owns the code→lexicon mapping.

### Doc reconciliation (top-authority flag)

This project **amends `design.md` #7**: the device log is *not*
kept live across a Play rebuild (terminate-first → dark during the
build); the genuine simultaneous-streams concurrency is preserved
in the running phase and in COMPILE-while-running. It also folds the
PRD's `workflow.md`/`theme.md` refinements (process-console source,
terminate-first, dropped firehose, broadened Build Log, stale
indicator, new camp copy). These are recorded in the PRD; the
upstream docs should be reconciled the way connection and discovery
did.

### See also

- `context/projects/setsid-install-help/` — in-app remediation for the
  missing-`setsid` failure mode. When the `setsid` wrapper exits before
  emitting its PID marker (`build_pgid == 0`), the Build Log surfaces the
  `brew install util-linux` command and the exact `ssh user@host`
  invocation for the failing Mac. The `setsid` wrapper design and
  two-pronged kill described in this ARD are unchanged.
- `context/projects/keychain-unlock/` — in-app remediation for the
  locked-`login.keychain` failure mode. The `unlock` step added to the
  front of `RunChain` runs `security unlock-keychain` on the remote Mac
  when the user has supplied a keychain passkey; on failure the Build Log
  surfaces the F1 help block and the chain aborts before any expensive
  step runs.

### Testing approach (per library)

- **`runstate_test.c`** (black-box): every transition incl.
  terminate-first re-Play, COMPILE-while-running, all failure edges,
  console-EOF→idle, drop→aborted, the build-generation stale rule,
  and phase/reason→`LexKey` mapping.
- **`builddeploy_test.c`** (black-box): device vs simulator command
  and `-destination` construction, shell-escaping, the
  `-showBuildSettings` product-path parse, the PID-marker parse, and
  failure classification.
- **`logbuf_test.c`** (black-box): bounded drop-oldest (bytes and
  lines), incremental line assembly across chunk boundaries,
  no truncation, demarcation insert, clear, and copy-all.
- **`session_run_test.c`** (stub SSH, mirrors `session_exec_test`):
  chain sequencing, chunk handoff into the run-event ring,
  ABORT/terminate-first, and console-EOF resolution — no real Mac.
- **Host-gated smoke** (`tools/run_smoke.c`): a real
  build/install/launch against a live Mac, **SKIP when absent**, so
  `make test` stays meaningful and green on both Linux and macOS.

### ARD / IMPL conformance checklist

- [x] **Arenas named + lifetimes stated** — per-run worker arena
      (reset each EXECUTE/COMPILE); `DevConsole` is arena-less
      (fixed fields, survives a COMPILE reset); two `logbuf`
      buffers from the app arena (app-lifetime, never reset).
- [x] **Allocation is caller-controlled** — `logbuf_init` takes an
      `Arena*`; `builddeploy`/`runstate` write into caller buffers;
      no hidden allocators.
- [x] **Thread-confinement respected** — log bytes are **copied**
      into fixed-size run-event records; no shared arena pointers
      cross the worker↔UI boundary.
- [x] **Non-arena allocations flagged + justified** — the two new
      SPSC rings (`malloc`, cross-thread, like existing rings); the
      per-run arena via `arena_create`. No other new `malloc`.
- [x] **Module → library decisions made** — new pure libs
      `librunstate`, `libbuilddeploy`, `liblogbuf`; `libsession`,
      `libui`, `liblexicon` extended; `src/app` stays plain
      composition-root code.
- [x] **Library layout specified** — `include/<m>.h` public,
      `src/<m>/*.c` private, `build/lib<m>.a` archive, linked into
      both `ostrich` and `tests/<m>_test`.
- [x] **C/C++ seam identified** — all three new libs are pure C11;
      ImGui stays sealed inside `libui` behind the pure-C `ui.h`;
      `app` consumes only C headers.
- [x] **Error handling shape confirmed** — `BdStatus` / `RunPhase`
      enums returned, results via out-params, status→`LexKey` /
      status→string companions; no hidden/global error state.
- [x] **Test approach per library** — a black-box
      `tests/<m>_test.c` per new lib + a stub-SSH worker test +
      a host-gated real-Mac smoke tool.
