# Implementation Plan - Build & Deploy (the Play/Observe core loop)

This plan realizes
`context/projects/xcode-project-build-and-deploy/prd.md` and
`context/projects/xcode-project-build-and-deploy/ard.md`, and
conforms to `context/coding_standards.md`. It is sequenced strictly
bottom-up, mirroring the discovery and logging plans: the three pure
deep libraries and the lexicon first (each proven through
`make test`), then the worker run subsystem in three cohesive
increments (each proven through `session_run_test.c` and the new
`tools/run_smoke.c`), then the UI delivered as two vertical slices,
UI last.

Every task is a single buildable, testable, releasable increment.
The worker tasks (T5–T7) are purely additive — a new run
command/event family and a `drive_run` hook alongside the existing
discovery engine — and are consumed by nothing user-facing until the
UI slices land, so each is safe to ship dormant. The first UI slice
(T8) is the first user-facing landing and is releasable on its own
(Play + watch the Build Log); the second (T9) closes the loop with
the Device Log and the stale indicator.

Verification model (settled by interview): automated coverage is the
black-box library tests plus a stub-SSH worker test
(`session_run_test.c`) that asserts observable state and sequencing;
the genuine live-Mac build/install/launch path is exercised by
`tools/run_smoke.c` and the running app and recorded as an unchecked
manual acceptance criterion per worker/UI task, exactly as discovery
did with `discovery_smoke`. `make test` stays host-free and green on
both Linux and macOS.

## Summary of Tasks

1. **librunstate** — pure run-state machine: `runstate_step`
   transitions, the build-generation/`deployed_gen` stale rule, and
   failure → `LexKey` classification. New library +
   `runstate_test`.
2. **libbuilddeploy** — pure shell-safe command construction for
   every step, device-vs-simulator `-destination`, the
   `setsid`/PID-marker launch wrapper, the `-showBuildSettings`
   product-path parser, the PID-marker parser, and the
   code → reason mapping. New library + `builddeploy_test`.
3. **liblogbuf** — pure bounded line buffer: incremental line
   assembly across chunk boundaries, drop-oldest bounds, the
   demarcation insert, clear, and copy-all. New library +
   `logbuf_test`.
4. **lexicon Build/deploy keys** — all run copy as centralized
   `LexKey`s, asserted in `lexicon_test`.
5. **worker: forward chain + DevConsole + watchdog** — the run
   ring family, `RunChain` (settings → build → prime → install →
   launch), the `DevConsole` channel handoff and indefinite stream,
   the output-progress watchdog, and distinct build/deploy
   resolution. EXECUTE happy path + COMPILE build-only. Creates
   `tools/run_smoke.c` and `session_run_test.c`.
6. **worker: ABORT + terminate-first + drop teardown** — the
   two-pronged kill, terminate-first re-EXECUTE, and the
   `disconnect_ssh` teardown of an in-flight run.
7. **worker: COMPILE-while-running + build-gen/stale** — a second
   channel for COMPILE while the DevConsole streams, the build
   generation bump, and the `REV_STALE` emission.
8. **UI slice A: wiring + run controls + Build Log** — drain the
   run-event ring, mirror phase + append chunks to the Build Log
   buffer, submit run intents; the EXECUTE↔ABORT toggle, COMPILE,
   the run-state label, the build ▷ install ▷ launch progression,
   and the Build Log panel.
9. **UI slice B: Device Log + stale indicator** — the Device Log
   panel with the NEW PAYLOAD demarcation at the launch→running
   edge, and the stale indicator. Closes the loop.
10. **Build Log surfacing fix** — production bug fix: merge libssh2
    extended-data (stderr) into stdout so xcodebuild's stderr-bound
    warnings/diagnostics actually reach the Build Log instead of
    silently filling libssh2's window and stalling the build channel;
    render the EXPLOIT FAILED / DEPLOYMENT FAILED header even when the
    line buffer is empty so a pre-output failure is no longer invisible.
11. **Per-step command demarcation in the Build Log** — emit an
    ostrich-voice demarcation line (the same `> ── … ──` pattern the
    Device Log already uses for `NEW PAYLOAD`) into the Build Log at
    the start of each chain step, carrying the exact command that was
    dispatched. Fills the otherwise-blank SETTINGS dwell, makes
    "what ostrich actually ran" visible without breaking the
    raw-output contract, and gives every subsequent block of tool
    output an unambiguous header. New event kind on the run ring,
    one new lexicon group, no changes to `runstate`/`builddeploy`.

## Task Dependency Relationships

```
 T1 runstate ──┐
 T2 builddeploy┼──▶ T5 forward chain ──┬──▶ T6 abort /        ──┐
 T3 logbuf ────┤    + DevConsole       │    terminate-first    │
 T4 lexicon ───┤    + watchdog         │    + drop teardown    │
               │    (+ run_smoke)      │                       │
               │                       └──▶ T7 compile-while-  ─┼─┐
               │                            running + stale     │ │
               │                                                │ │
   T1,T3,T4 ───┴──────────────────────────▶ T8 wiring +  ◀─────┘ │
                                             controls +           │
                                             Build Log            │
                                                  │               │
                                                  ▼               │
                                          T9 Device Log + ◀───────┘
                                          stale indicator
```

Reading it: T1–T4 are independent pure foundations. T5 needs the
run-state machine (T1) and the command/parse module (T2). T6 and T7
each extend the worker T5 built. T8 (the first UI slice and the
EXECUTE↔ABORT toggle) needs the forward path (T5) and a working
ABORT (T6), plus the phase machine (T1), the line buffer (T3), and
the lexicon (T4). T9 needs the controls/Build-Log slice (T8) and the
stale logic (T7). The strict landing order is T1 → T2 → T3 → T4 →
T5 → T6 → T7 → T8 → T9.

T10 is an independent follow-up fix landing after the original nine
ship. It touches `libssh` (one libssh2 call after channel open) and
`libui` (the empty-state branch of `draw_build_log`); it does not
depend on, and is not depended on by, any of T1–T9, and can be picked
up at any time once a real-Mac symptom has been observed.

T11 builds on T5 (the worker run subsystem, which owns the per-step
command construction site) and on T8 (the UI's Build Log drain and
`logbuf_mark` call site). It is a UX increment, not a bug fix, and is
sequenced after T10 because the demarcation only pays off once the
underlying tool output it labels is actually reaching the panel.

## Detailed Tasks

### Task 1 - librunstate

- **Status**: done
- **Blocked by**: none
- **User stories covered**: 13, 16, 17, 18, 19, 20, 42, 45, 54

#### What to build

The pure, host-free heart of the feature: the run-state machine
that narrates the chain's phases, decides what the worker does next
at each event, classifies terminal failures, and computes
staleness. It owns the behavior behind the run-state label
(story 13), the EXECUTE↔ABORT toggle and ABORT semantics as state
(stories 16–18), the terminate-first re-EXECUTE transition
(stories 19, 20), the stale rule (story 42), and the distinct
terminal states (story 45). Establishes `librunstate.a` so later
tasks extend an existing boundary; it is the testable centre of
story 54.

#### Technical Details

Per ARD §"What changes" item 3 ("`librunstate`") and
§"Interfaces (`runstate.h`)": create `include/runstate.h` and
`src/runstate/`, build `build/librunstate.a`, and wire it into the
Makefile mirroring the existing `lib<module>.a` pattern. Land
`RunPhase`, `RunEvent`, `RunAction`, and `RunState` exactly as the
ARD specifies, plus `runstate_init`, `runstate_step`,
`runstate_stale`, `runstate_phase_lex`, `runstate_reason_lex`, and
`runstate_phase_str`. The machine drives
`idle → building → (priming) → installing → launching → running`
on the success events, routes `*_FAIL` events to `build_failed`
vs `deploy_failed`, folds `RUN_EV_ABORT` and `RUN_EV_DROP` to
`aborted`, and resolves `RUN_EV_CONSOLE_EOF` to a clean idle. The
`built_gen` counter increments on each successful build;
`deployed_gen` is set on a Play launch; `runstate_stale` is
`running && built_gen > deployed_gen`. `target_is_sim` gates whether
`RUN_ACT_PRIME` is emitted. Pure C11; depends only on `lexicon.h`
for `LexKey` and `builddeploy.h` for `BdStatus` in the reason
lookup (forward-declared / included as a pure header — no link
dependency on T2's `.c`). Black-box tested through `runstate.h`
only (coding_standards "Testing").

#### Acceptance criteria

- [x] `include/runstate.h` declares `RunPhase`, `RunEvent`,
      `RunAction`, `RunState`, and the seven functions exactly as
      ARD §"Interfaces (`runstate.h`)" specifies.
- [x] Every documented transition is covered by `runstate_test`,
      including terminate-first re-EXECUTE (running + EXECUTE →
      terminate-first action → building), COMPILE-while-running
      (running + COMPILE → build on, phase stays running), all
      failure edges (build/prime/install/launch), `CONSOLE_EOF` →
      idle, and `DROP` → aborted.
- [x] The build-generation stale rule is exercised: a successful
      build bumps `built_gen`; a Play launch sets `deployed_gen`;
      `runstate_stale` is true only while running with
      `built_gen > deployed_gen`, and a COMPILE-while-running makes
      it true.
- [x] `runstate_phase_lex` and `runstate_reason_lex` return the
      correct `LexKey` for each phase and each failure status (no
      stray `(?)`).
- [x] `build/librunstate.a` builds clean on Linux and macOS
      (`CC`/`CFLAGS` honored), `tests/runstate_test.c` links the
      archive, and `make test` passes.

### Task 2 - libbuilddeploy

- **Status**: done
- **Blocked by**: none
- **User stories covered**: 4, 5, 6, 7, 8, 9, 11, 30, 54

#### What to build

The pure command/parse module the worker drives: every remote
command string for the chain, the device-vs-simulator destination
and tooling inference, the `.app` path resolution, and the
failure-code → reason mapping. It encodes installing to a device
via `devicectl` (story 6) and a simulator via `simctl` (story 7),
inferring the tooling from the picked target (story 8), the
simulator boot command (story 9), the no-target-vs-target COMPILE
destination (stories 4, 5), ostrich-resolved `.app` path
(story 11), and the distinct deploy-failure classification
(story 30). Establishes `libbuilddeploy.a`; it is the
command-construction half of story 54.

#### Technical Details

Per ARD §"What changes" item 4 ("`libbuilddeploy`") and
§"Interfaces (`builddeploy.h`)": create `include/builddeploy.h`
and `src/builddeploy/`, build `build/libbuilddeploy.a`, and wire it
into the Makefile. It `#include`s `discovery.h` (for `RunConfig`,
`Target`, `Str`) and `lexicon.h`. Land `BdStatus` and all the
command builders — `bd_settings_cmd`, `bd_build_cmd`,
`bd_boot_cmd`, `bd_bootstatus_cmd`, `bd_install_cmd`,
`bd_launch_cmd`, `bd_terminate_cmd`, `bd_kill_cmd`,
`bd_destination` — each single-quote-escaping every path/identifier
exactly like `disc_*_cmd`. `bd_build_cmd` wraps `xcodebuild` in
`setsid` and emits a PID/PGID marker the worker can parse;
`bd_launch_cmd` builds the attached `--console` launch
(`devicectl … process launch --console` for a device, `simctl
launch --console` for a simulator). `bd_destination` builds the
device-id destination when a target is locked and a generic
destination when none is (the COMPILE no-target case). Land the
parsers `bd_parse_product_path` (token-walk over
`xcodebuild -showBuildSettings -json` for the built `.app`) and
`bd_parse_pid_marker`, plus `bd_reason_lex` / `bd_status_str`. All
pure C11; allocation is into caller buffers (`char*, size_t`) and
caller arenas where needed. Reuse jsmn (already vendored for
discovery) for the settings parse. Black-box tested through
`builddeploy.h` only.

#### Acceptance criteria

- [ ] `include/builddeploy.h` declares `BdStatus`, the nine command
      builders, the two parsers, and the two classification calls
      exactly as ARD §"Interfaces (`builddeploy.h`)" specifies.
- [ ] Device vs. simulator command and `-destination` construction
      diverge correctly by `Target` kind (devicectl vs simctl;
      device-id vs simulator-id destination); a no-target COMPILE
      yields the generic destination (stories 4, 5, 8).
- [ ] All paths/identifiers are single-quote-escaped; a value with a
      space or quote is shell-safe — verified by `builddeploy_test`.
- [ ] `bd_build_cmd` produces a `setsid`-wrapped command with a
      parseable PID marker, and `bd_parse_pid_marker` recovers the
      pgid from a representative marker line.
- [ ] `bd_parse_product_path` extracts the `.app` path from a
      canonical `-showBuildSettings -json` fixture and returns
      `BD_ERR_PARSE` (never a crash) on malformed input.
- [ ] `bd_reason_lex` maps each `BdStatus` to its `LexKey`,
      distinguishing a build failure from a deploy failure
      (story 30).
- [ ] `build/libbuilddeploy.a` builds clean on Linux and macOS,
      `tests/builddeploy_test.c` links the archive, and `make test`
      passes host-free.

### Task 3 - liblogbuf

- **Status**: done
- **Blocked by**: none
- **User stories covered**: 24, 35, 36, 54

#### What to build

The pure, UI-side bounded line buffer used for both the Build Log
and the Device Log. It assembles raw byte chunks into whole lines,
bounds memory by dropping the oldest lines (story 36), supports the
cleared-per-build Build Log (story 24) and the demarcation insert
for the Device Log's NEW PAYLOAD separator (story 35), and exposes
the lines for rendering and a flatten-for-copy. Establishes
`liblogbuf.a`; it is the log-ring half of story 54.

#### Technical Details

Per ARD §"What changes" item 5 ("`liblogbuf`") and
§"Interfaces (`logbuf.h`)": create `include/logbuf.h` and
`src/logbuf/`, build `build/liblogbuf.a`, and wire it in. It
`#include`s `arena.h`. Land the opaque `LogBuf` and
`logbuf_init(Arena*, byte_cap, max_lines)`, `logbuf_append`,
`logbuf_mark`, `logbuf_clear`, `logbuf_count`, `logbuf_line`, and
`logbuf_copy_all`. `logbuf_append` carries a partial line across
calls (chunk boundaries fall anywhere), splitting on newlines; the
store is a bounded byte buffer + a line index that drops whole
oldest lines when either the byte cap or the line cap is hit, never
truncating a surviving line. `logbuf_mark` flushes any pending
partial line, then inserts a complete demarcation line.
`logbuf_line` returns a borrowed NUL-terminated view for rendering;
`logbuf_copy_all` flattens every line into a caller buffer and
returns the bytes needed (may exceed cap). Backed entirely by the
caller-supplied app arena (allocated once at startup, never reset);
this module is UI-thread-confined text storage, distinct from the
cross-thread `spsc_ring` transport. Pure C11; black-box tested
through `logbuf.h` only.

#### Acceptance criteria

- [x] `include/logbuf.h` declares the opaque `LogBuf` and the seven
      functions exactly as ARD §"Interfaces (`logbuf.h`)"
      specifies; `logbuf_init` takes an `Arena *` (no hidden
      allocator).
- [x] Incremental assembly is verified: bytes split mid-line across
      two `logbuf_append` calls produce one correct line; multiple
      lines in one chunk produce multiple lines.
- [x] Bounding is verified for both caps: exceeding the byte cap and
      exceeding the line cap each drop whole oldest lines, and no
      surviving line is ever truncated.
- [x] `logbuf_mark` flushes a pending partial line, then inserts the
      demarcation line as its own complete line; `logbuf_clear`
      empties the store.
- [x] `logbuf_copy_all` flattens all lines and reports the required
      size, handling the cap-exceeded case.
- [x] `build/liblogbuf.a` builds clean on Linux and macOS,
      `tests/logbuf_test.c` links the archive, and `make test`
      passes.

### Task 4 - lexicon Build/deploy keys

- **Status**: done
- **Blocked by**: none
- **User stories covered**: 48

#### What to build

All new run copy as centralized lexicon keys so every run surface
draws its wording from the single strings table and a future
straight-mode stays a no-UI swap (story 48). `theme.md` owns the
final strings; this lands them in code under a Build/deploy group
so `runstate` and `builddeploy` can return `LexKey`s the UI looks
up with no logic.

#### Technical Details

Per ARD §"What changes" item 7 ("New lexicon copy") and
§"Interfaces": add the Build/deploy `LexKey`s to
`include/lexicon.h` and their strings to `src/lexicon.c` — the
action labels (EXECUTE, COMPILE, ABORT), the run-state labels
(STANDBY, COMPILING EXPLOIT…, PRIMING TARGET…, DEPLOYING PAYLOAD…,
EXECUTING PAYLOAD…, TARGET ACQUIRED // LIVE), the failure lines
(EXPLOIT FAILED, the distinct DEPLOYMENT FAILED // PAYLOAD
REJECTED, OPERATION ABORTED), the Device Log NEW PAYLOAD separator,
the stale indicator, and the log empty states / LIVE FEED header.
Keep the strings verbatim from `theme.md`'s canonical lexicon.
Extend `tests/lexicon_test.c` to assert each new key resolves to
its non-empty themed string. This is the same shape discovery's
task 5 used.

#### Acceptance criteria

- [x] Every Build/deploy key above exists in `include/lexicon.h`
      and maps to its `theme.md` string in `src/lexicon.c`.
- [x] `lex()` returns the exact themed strings (no `(?)`) for all
      new keys; UTF-8 glyphs and the `//` separators are preserved.
- [x] `runstate_phase_lex`/`runstate_reason_lex` and `bd_reason_lex`
      resolve through these keys end-to-end (the key set is
      complete — no phase or failure status lacks a string).
- [x] `tests/lexicon_test.c` asserts the new keys and `make test`
      passes.

### Task 5 - worker: forward chain + DevConsole + watchdog

- **Status**: done
- **Blocked by**: 1, 2
- **User stories covered**: 1, 3, 9, 10, 11, 12, 15, 31, 32, 33,
  34, 44, 46, 53, 55, 56

#### What to build

The run subsystem's forward path inside the existing worker, proven
end-to-end before any UI exists: submit EXECUTE and ostrich drives
`xcodebuild` → install → launch over the one session, streaming the
chain's raw output and then the launched app's own stdout/stderr;
submit COMPILE and it runs build-only. This realizes the one-action
chain (story 1), build-only COMPILE (story 3), simulator auto-boot
(story 9) observed headlessly through logs (story 10),
ostrich-resolved `.app` path (story 11), launch-after-install
(story 12), honest real-duration phases (story 15), the app's own
output streaming via process-console (stories 31–34), and the
concurrent off-thread channels on the existing session (stories 44,
56). A hung step degrades to a terminal failure via the
output-progress watchdog (story 46). Creates the `run_smoke` dev
tool so the chain is verifiable before any UI exists.

#### Technical Details

Per ARD §"What changes" items 1–2 ("A new run subsystem inside the
worker", "A new run command/event family"), §"Control + data flow
for one EXECUTE", §"Interfaces (`session.h`)", and §"Liveness,
termination, and failure mapping": add the run ring pair
(`run_cmd_ring`, `run_event_ring`) created via `spsc_create` at
`session_open` (flagged non-arena, like the existing rings), plus
`session_run_submit` / `session_run_poll` and the `SessionRunCmd` /
`SessionRunEvent` types. Add `RunChain` and `DevConsole` to
`WorkerCtx` and a per-run arena `arena_create`d at worker init
beside the existing `disc_arenas`. Add `drive_run(ctx)` called after
`drive_disc_jobs` each loop iteration; all channel-opens (disc and
run) go through the single shared `open_owner` seam. `RunChain`
steps `settings → build → (prime) → install → launch`, reusing one
channel across steps; `settings` accumulates
`-showBuildSettings -json` to EOF and parses the product path with
`bd_parse_product_path` into the per-run arena; `build` streams raw
bytes as `REV_BUILD_LOG` chunks (copied into the fixed-size record —
no worker pointer crosses the boundary); `prime` runs boot +
bootstatus for a simulator only; `install` and `launch` stream into
the Build Log. On a successful `launch`, the channel is **handed
off** from `RunChain` to `DevConsole`, which holds the running app's
identity in fixed struct fields (independent of the per-run arena)
and streams `REV_DEVICE_LOG` chunks indefinitely. The worker does
**no** line-splitting — that is the UI's job. The output-progress
watchdog replaces the disc 60 s total cap: each chain step's
deadline **resets on every byte**; a step silent past a generous
stall window (~120 s) fails terminally. The `DevConsole` is exempt
(an idle app is silent for hours); its only liveness is the existing
SSH keepalive, and app exit EOFs the channel (`RUN_EV_CONSOLE_EOF` →
clean idle). Distinct failures: a non-zero `build` exit drives
`RUN_EV_BUILD_FAIL` (→ `RUN_BUILD_FAILED`); a non-zero
`boot/install/launch` exit drives the deploy-fail events (→
`RUN_DEPLOY_FAILED`); `builddeploy` owns the code→`BdStatus`
mapping carried on the `REV_PHASE` event. Run commands are accepted
only in `SUB_ONLINE`. Instrument the new exec/stream cycle via the
existing `log.h`/`OSTRICH_DEBUG` facility (story 53), reusing the
command-exec logging shape the logging project established. Create
`tools/run_smoke.c` (mirroring `tools/discovery_smoke.c`) to connect
and drive EXECUTE/COMPILE against a real Mac, and
`tests/session_run_test.c` with a stub SSH (mirroring
`session_exec_test` and reusing the `ssh_stub` pattern) for the
host-free automated path.

#### Acceptance criteria

- [x] `session.h` declares `SessionRunCmd`, `SessionRunEvent`,
      `session_run_submit`, and `session_run_poll` exactly as ARD
      §"Interfaces (`session.h`)" specifies; the existing
      connection and discovery rings/types are unchanged and their
      tests still pass.
- [x] Only fixed-size records cross the run rings; log bytes are
      copied into the `chunk[RUN_CHUNK_CAP]` field and no per-run
      arena pointer is shared across the thread boundary.
- [x] `session_run_test.c` (stub SSH) drives an EXECUTE through
      `settings → build → install → launch → running`, asserting
      the `REV_PHASE` sequence, that build/install bytes arrive as
      `REV_BUILD_LOG` chunks and post-launch bytes arrive as
      `REV_DEVICE_LOG` chunks, and that the launch channel is handed
      to the DevConsole (it keeps streaming after the chain
      completes).
- [x] In the stub test, a non-zero `build` exit resolves to
      `RUN_BUILD_FAILED` and a non-zero `install`/`launch` exit
      resolves to the distinct `RUN_DEPLOY_FAILED`; a COMPILE runs
      `settings`+`build` only and stops (no install/launch, no
      target required).
- [x] The output-progress watchdog is exercised: a step whose byte
      stream stalls past the window resolves to a terminal failure,
      while a long step that keeps emitting bytes does not — proving
      reset-on-byte; the DevConsole is exempt and a `CONSOLE_EOF`
      resolves cleanly to idle.
- [x] The worker never blocks: keepalive and the connection/disc
      machines continue while a run is in flight (the run path only
      adds a non-blocking `drive_run` and serializes opens through
      `open_owner`).
- [x] `build/librunstate.a`, `build/libbuilddeploy.a`,
      `build/libsession.a`, `tools/run_smoke`, and the full app
      build clean on Linux and macOS, and `make test` passes.
- [ ] Manually via `run_smoke` against a live Mac: EXECUTE on a real
      preset+target builds, installs, launches, and streams the
      app's stdout/stderr; a simulator target is auto-booted and
      observed purely through its console; COMPILE builds without a
      target; a build error and a deploy error each surface as the
      distinct terminal failure; the window/keepalive never
      freezes.

### Task 6 - worker: ABORT + terminate-first + drop teardown

- **Status**: done
- **Blocked by**: 5
- **User stories covered**: 17, 18, 19, 20, 21, 47, 53, 55

#### What to build

The stop/restart half of the run subsystem: ABORT becomes a
universal stop that cancels an in-flight chain and terminates a
running app (stories 17, 18), pressing EXECUTE again iterates by
terminating the running instance first and then rebuilding
(stories 19, 20) — accepting the Device Log going briefly dark
during the rebuild (story 21) — and an SSH drop mid-run is folded
into a clean abort with local state preserved (story 47). Built on
the T5 forward path so re-EXECUTE and ABORT operate on a real
`RunChain` + `DevConsole`.

#### Technical Details

Per ARD §"Control + data flow" (the re-EXECUTE / ABORT / drop
paragraphs) and §"Liveness, termination, and failure mapping"
("Two-pronged kill"): handle `RCMD_ABORT` with the two-pronged
kill — terminate the running app with
`bd_terminate_cmd` (`devicectl/simctl terminate <bundle>`) and kill
the in-flight build's process group with `bd_kill_cmd`
(`kill -- -<pgid>`, using the pgid the worker parsed from the T5
build marker) on a fresh channel — then close the local channels
and drive `RUN_EV_ABORT` → `RUN_ABORTED`. Implement terminate-first
re-EXECUTE: when EXECUTE arrives while `RUN_RUNNING`, run
`terminate <bundle>` first; the `DevConsole` channel EOFs and
closes (device log dark, by design) before a fresh `RunChain`
starts. Extend the existing `disconnect_ssh` teardown to also tear
down an in-flight `RunChain` and the `DevConsole` and emit
`RUN_EV_DROP` → `RUN_ABORTED`, so a transport drop ends the run
cleanly with the app's local state intact. Channel-opens for the
kill/terminate channels go through the shared `open_owner` seam.
Instrument the terminate/kill/teardown via `log.h` (story 53).
Extend `session_run_test.c` with stub assertions for these edges.
No new `ssh.h` primitive — every action is an ordinary exec.

#### Acceptance criteria

- [x] `RCMD_ABORT` mid-build runs the two-pronged kill: the stub
      test asserts a `terminate <bundle>` exec and a
      `kill -- -<pgid>` exec are issued (the pgid matching the
      parsed build marker), the local channels close, and the state
      resolves to `RUN_ABORTED`.
- [x] An EXECUTE while `RUN_RUNNING` issues `terminate <bundle>`
      **before** opening the next `RunChain`'s first channel (stub
      test asserts the ordering), and the DevConsole channel EOFs /
      closes in between.
- [x] An SSH drop mid-run (stub-simulated transport failure) tears
      down the `RunChain` + `DevConsole` and resolves to
      `RUN_ABORTED` via `RUN_EV_DROP`; no run-event pointer dangles
      and the connection-machine teardown is otherwise unchanged.
- [x] The existing connection/discovery teardown behavior and tests
      are unaffected; `make test` passes.
- [ ] Manually via `run_smoke` / a live Mac: ABORT during a build
      stops `xcodebuild` (no orphaned process holds the build dir)
      and ABORT during the running phase terminates the app and
      returns to STANDBY; a re-EXECUTE terminates the old instance,
      the Device Log goes briefly dark, then the new build's output
      appears; pulling the network mid-run ends the run cleanly.

### Task 7 - worker: COMPILE-while-running + build-gen/stale

- **Status**: done
- **Blocked by**: 5
- **User stories covered**: 40, 41, 42, 43, 53, 55

#### What to build

The genuinely-parallel headline case: a build-only COMPILE while an
app is running leaves the running instance alive (story 40) and
streams the new compile in the Build Log while the Device Log keeps
streaming the live app — two live streams at once (story 41) —
because nothing is being swapped underneath the running instance.
Having built code newer than what is deployed, the worker flags the
deployed instance stale (story 42). The dual stream runs over
concurrent channels without freezing the worker (story 43). Built on
the T5 forward path / `DevConsole`.

#### Technical Details

Per ARD §"Control + data flow" (the COMPILE-while-running
paragraph) and §"What changes" item 1 ("at most ~2 run channels
live at once"): when `RCMD_COMPILE` arrives while `RUN_RUNNING`,
run `settings`+`build` on a **second** channel (opened through the
shared `open_owner` seam) while the `DevConsole` keeps its channel
and continues streaming — the engine already reads multiple
channels concurrently (as the discovery sweep does with two). The
`DevConsole`'s fixed-field identity is independent of the per-run
arena, so the COMPILE's per-run arena reset does not disturb the
running app. On a successful COMPILE build, increment the run-state
`built_gen`; the worker emits `REV_STALE` whenever `runstate_stale`
flips true (running with `built_gen > deployed_gen`). The phase
stays `RUN_RUNNING` throughout (a COMPILE never redeploys).
Instrument the second-channel lifecycle via `log.h` (story 53).
Extend `session_run_test.c` with stub assertions for this edge.

#### Acceptance criteria

- [x] `session_run_test.c` asserts that a COMPILE submitted while
      `RUN_RUNNING` opens a second channel while the DevConsole
      channel stays open, the phase remains `RUN_RUNNING`, and the
      DevConsole keeps emitting `REV_DEVICE_LOG` chunks during the
      compile.
- [x] A successful COMPILE-while-running increments `built_gen` and
      the worker emits a `REV_STALE` event reflecting
      `runstate_stale` going true; a subsequent EXECUTE that
      redeploys clears it.
- [x] The COMPILE's per-run arena reset does not disturb the
      running app's DevConsole identity (bundle/udid/sim flag held
      in fixed fields, verified by the DevConsole continuing to
      stream).
- [x] `make test` passes (no real Mac required for the stub
      assertions).
- [ ] Manually via the app / a live Mac: with an app running, a
      COMPILE streams the new build in the Build Log while the
      Device Log keeps streaming the live app, and afterwards the
      stale flag is set; the window stays smooth with both streams
      active.

### Task 8 - UI slice A: wiring + run controls + Build Log

- **Status**: done
- **Blocked by**: 5, 6, 1, 3, 4
- **User stories covered**: 2, 13, 14, 16, 22, 23, 24, 25, 26, 27,
  28, 29, 30, 49, 50, 51, 52, 55

#### What to build

The first user-facing landing and a complete vertical slice: the
composition root drains the run-event ring and the run controls
drive the chain, with the Build Log streaming the whole chain's raw
output. EXECUTE is enabled only when READY (story 2); the run-state
label narrates the live phase (story 13) with a build ▷ install ▷
launch progression (story 14); EXECUTE toggles to ABORT in flight
(story 16). The Build Log streams xcodebuild live (story 22) and
carries the install/launch tooling too (story 23), cleared per
build (story 24), auto-scrolling with scroll-up pause (story 25),
with copy/clear (story 26), left raw (story 27), with a themed
empty state (story 28) and the distinct EXPLOIT FAILED / DEPLOYMENT
FAILED lines (stories 29, 30). Palette discipline and zero-cost
whimsy hold (stories 49–51) and the controls are keyboard-drivable
(story 52). After this task ostrich can Play and watch the build —
shippable without the Device Log.

#### Technical Details

Per ARD §"What changes" item 6 ("UI + composition-root wiring") and
§"Interfaces (`ui.h`)": extend `ui_frame` to take a read-only
`UiRunView` (mirrored `RunPhase`, progression, `disc_readiness`,
and the two `LogBuf*`s) and a `UiRunIntents` (execute, compile,
abort, build-log copy/clear), following the existing
`UiReconView`/`UiReconIntents` pattern; `ui.h` gains
`#include "logbuf.h"`. `app.c` owns two `LogBuf` instances
(`logbuf_init` from the app arena at startup) and a mirrored
`RunPhase` + stale + generation, drains `run_event_ring` each frame
(appending `REV_BUILD_LOG` chunks into the Build Log buffer,
applying `REV_PHASE` transitions, clearing the Build Log at each
build start), and submits run commands from the Control/Status
intents. EXECUTE/COMPILE/ABORT enablement combines the mirrored
phase with `disc_readiness` (EXECUTE requires READY + a target;
COMPILE needs no target; ABORT only in flight). `ui.cpp` gains the
EXECUTE↔ABORT toggle, COMPILE, the run-state label, the
progression indicator, and the Build Log panel (auto-scroll with
scroll-up pause, copy via `logbuf_copy_all`, clear, themed empty
state, raw rendering), drawing wording from the T4 lexicon and
honoring palette discipline (decorative chrome vs. semantic
green/red/amber for meaning; logs in calm off-white, never
recolored). `app` stays pure C; ImGui stays sealed behind `ui.h`.
The Device Log buffer exists and the worker streams to the
DevConsole, but no Device Log panel renders yet (deferred to T9).
`ui_test.c` keeps passing in headless mode.

#### Acceptance criteria

- [x] `ui_frame`'s extended signature (the `UiRunView` +
      `UiRunIntents`) matches ARD §"Interfaces (`ui.h`)"; the app
      builds the run view-model each frame and the existing
      connection/recon panels are unaffected.
- [x] EXECUTE is enabled only when `disc_readiness` is READY and a
      target is locked; COMPILE is enabled without a target; in
      flight the control reads ABORT; the run-state label and the
      build ▷ install ▷ launch progression track the mirrored
      phase.
- [x] The Build Log streams `REV_BUILD_LOG` chunks live (build,
      then install/launch), is cleared at each build start,
      auto-scrolls but pauses on scroll-up, and offers copy + clear;
      it renders raw with no recoloring and a themed empty state
      before the first build.
- [x] A build failure renders EXPLOIT FAILED and a deploy failure
      the distinct DEPLOYMENT FAILED // PAYLOAD REJECTED, with the
      errors in the Build Log; semantic colors are used only for
      meaning.
- [x] The controls are keyboard-drivable; `build/ostrich` builds
      clean on Linux and macOS and `make test` passes (`ui_test`
      headless still green).
- [ ] Manually against a live Mac: with a READY preset+target,
      EXECUTE drives the chain, the label narrates real phases with
      no fake delay, the Build Log shows the whole chain's output
      live, and ABORT stops it; the window never freezes.

### Task 9 - UI slice B: Device Log + stale indicator

- **Status**: done
- **Blocked by**: 8, 7
- **User stories covered**: 31, 33, 34, 35, 36, 37, 38, 39, 41, 42,
  43, 55

#### What to build

The loop closes: the Device Log panel streams the launched app's own
output (story 31), identically on a device and a simulator
(story 33), starting automatically at launch (story 34), preserving
history with a `> ── NEW PAYLOAD ──` separator at each new launch
(story 35), bounded so a long session never bloats memory
(story 36), with copy/clear and the same auto-scroll/pause
(story 37), a streaming LIVE FEED header and a themed empty state
(story 38), left raw and never recolored (story 39). The
COMPILE-while-running two-streams payoff is now visible (story 41),
and the stale indicator surfaces when the deployed instance is
behind the latest build (story 42), all while the window stays
smooth (story 43).

#### Technical Details

Per ARD §"What changes" item 6 and §"Control + data flow"
(the demarcation note): in `app.c`, drain `REV_DEVICE_LOG` chunks
into the Device Log `LogBuf` and apply `REV_STALE` to the mirrored
stale flag; insert the NEW PAYLOAD demarcation via `logbuf_mark`
at the launch→running edge (the first device output of a run), so
each run's lines are unambiguously separated while history is
preserved. `ui.cpp` gains the Device Log panel — bounded rendering
of the `LogBuf` lines, auto-scroll with scroll-up pause, copy via
`logbuf_copy_all`, clear, the LIVE FEED // INTERCEPTING header, the
`// NO SIGNAL — TARGET DARK` empty state, raw never-recolored
output — and the stale indicator (drawing the T4 lexicon stale
string) shown when the mirrored stale flag is set. The Device Log
behaves identically for a device and a simulator because both come
from the same process-console stream. Wording is the T4 lexicon;
palette discipline holds. `ui_test.c` keeps passing headless.

#### Acceptance criteria

- [x] The Device Log panel renders `REV_DEVICE_LOG` lines live,
      bounded (oldest dropped past the cap), with copy + clear and
      auto-scroll/scroll-up pause matching the Build Log.
- [x] A new launch inserts the `> ── NEW PAYLOAD ──` demarcation at
      the launch→running edge while preserving prior-run lines; the
      panel shows the LIVE FEED header when streaming and the themed
      empty state before any output.
- [x] The Device Log is rendered raw and never recolored; it behaves
      identically for a device and a simulator target.
- [x] The stale indicator appears when the mirrored stale flag is
      set (after a COMPILE-while-running) and clears on a redeploy;
      the COMPILE-while-running case shows both logs streaming at
      once.
- [x] `build/ostrich` builds clean on Linux and macOS and
      `make test` passes (`ui_test` headless still green).
- [ ] Manually against a live Mac: a launched app's output streams
      in the Device Log on both a device and a simulator; iterating
      shows a NEW PAYLOAD separator per run; a COMPILE while running
      shows two live streams and lights the stale indicator; an
      hours-long session stays bounded and the window stays smooth.

### Task 10 - Build Log surfacing fix

- **Status**: done
- **Blocked by**: 5, 8
- **User stories covered**: 22, 23, 27, 29, 30, 43, 46

#### What to build

A production bug fix surfaced by an operator running EXECUTE against
a real Mac: the run-state label sits on `COMPILING EXPLOIT…` while
xcodebuild appears to do real work on the remote, but the Build Log
panel stays on its empty-state wordmark indefinitely — story 22 (watch
xcodebuild stream live) is silently broken. The root cause is two
independent gaps in the path bytes take from the remote process to the
panel, plus one display invariant that hides the resulting failure:

1. **libssh2 extended-data stalls the build channel.** `ssh_channel_read`
   is `libssh2_channel_read(ch->channel, ...)`, which reads stream 0
   (stdout) only. libssh2's default extended-data mode is
   `LIBSSH2_CHANNEL_EXTENDED_DATA_NORMAL` — stderr is buffered in a
   separate per-channel queue that nothing in ostrich ever drains. Once
   that queue fills libssh2's flow-control window (~32 KB on a typical
   build), the server stops sending the stdout side too and the remote
   xcodebuild blocks on its next stderr write. The Build Log stays
   empty, the chain never progresses, and the only thing that ends it
   is the 120 s stall watchdog (story 46) firing as `EXPLOIT FAILED`.
   Stories 22, 23 (build/install/launch tooling output in the log),
   27 (raw output of what xcodebuild *said*), and 43 (window stays
   smooth) all depend on stderr being captured.
2. **Pre-output failures vanish.** `draw_build_log` (`src/ui/ui.cpp`)
   renders the EXPLOIT FAILED / DEPLOYMENT FAILED header only inside
   the `count > 0` branch. A failure that resolves before any chunk
   reaches `logbuf` — e.g. a SETTINGS-step `xcodebuild -showBuildSettings`
   non-zero exit, a `BD_ERR_PARSE` on truncated JSON, or the stall
   watchdog firing during the symptom above — leaves the panel on its
   themed empty state with no failure indication, contradicting
   stories 29 and 30.

This task fixes both. It does not change the PRD's "raw, never
recolored" contract for tool output, and it does not add error
parsing — it only makes the bytes that are already produced actually
reach the panel, and makes a no-output failure visible.

#### Technical Details

**(1) Merge extended-data on every opened channel.** In
`src/ssh/ssh.c`, immediately after `libssh2_channel_open_session` in
`ssh_channel_open` returns a non-NULL channel, call
`libssh2_channel_handle_extended_data2(ch->channel,
LIBSSH2_CHANNEL_EXTENDED_DATA_MERGE)`. This is the standard libssh2
configuration for non-interactive command execution: stderr is folded
into the stdout stream so a single `libssh2_channel_read` drains both.
The MERGE call returns `LIBSSH2_ERROR_EAGAIN` in non-blocking mode if
it would block, so it must be inside the same idempotent-retry block
the recent memory-leak fix introduced — set a "merge applied" flag on
the `SshChannel` once it succeeds, retry on EAGAIN, fail the open on
any hard error. The merge applies to every channel the run subsystem
opens (settings, build, prime, install, launch/DevConsole, terminate,
kill) and every channel discovery opens, so the fix lives in the
single ssh.c seam — no per-call-site changes in `libsession`.
Apply the same `libssh2_channel_handle_extended_data2` to the probe
channel for consistency (the probe is a `true` exec and unlikely to
emit stderr, but the symmetry is worth more than the saved call).

**(2) Render the failure header on an empty Build Log.** In
`src/ui/ui.cpp` `draw_build_log`, when `count == 0` and the mirrored
phase is `RUN_BUILD_FAILED` or `RUN_DEPLOY_FAILED`, render the
lexicon-mapped phase label (`runstate_phase_lex(rv->phase)`) centered
in `C_FAIL` *in place of* the wordmark + themed empty-state caption.
The empty-state branch already centers a two-line block, so the
change is one extra conditional that picks which text to draw. The
non-empty branch is unchanged — it already prints the failure header
above the lines.

No new lexicon keys; no PRD/ARD revisions; no changes to `logbuf`,
`runstate`, or `builddeploy`. The fix is invariant under the existing
acceptance criteria of T5 and T8 and adds two new ones below.

#### Acceptance criteria

- [x] After `ssh_channel_open` returns `SSH_OK`, the channel has
      extended-data MERGE applied (idempotent across EAGAIN retries);
      `ssh_channel_open` fails closed on a hard error from the MERGE
      call rather than silently leaving the channel in NORMAL mode.
- [x] The merge is applied to every channel the worker opens
      (run-chain steps, DevConsole, abort terminate/kill, discovery
      jobs, probe) — verified by inspection of the single shared
      `ssh_channel_open` seam, not per-call-site.
- [x] `draw_build_log` renders the EXPLOIT FAILED /
      DEPLOYMENT FAILED header (in `C_FAIL`) when `count == 0` and the
      phase is `RUN_BUILD_FAILED` or `RUN_DEPLOY_FAILED`, replacing
      the wordmark + empty-state block; the non-empty branch is
      unchanged and the IDLE / running empty state still shows the
      themed wordmark + caption.
- [x] `make test` passes on both Linux and macOS (`session_run_test`
      uses the SSH stub and is unaffected by libssh2 extended-data
      semantics; `ui_test` headless picks up the new conditional but
      asserts no new behavior).
- [ ] Manually via the app against a real Mac that previously
      reproduced the symptom: pressing EXECUTE produces visible Build
      Log output (the PGID marker line and subsequent xcodebuild
      stdout/stderr) instead of staying on the empty state; a build
      whose only diagnostic is an stderr-bound warning surfaces that
      warning in the Build Log rather than hanging the channel; a
      SETTINGS-step failure resolves to `EXPLOIT FAILED` visible in
      the panel rather than the empty state.

### Task 11 - Per-step command demarcation in the Build Log

- **Status**: not started
- **Blocked by**: 5, 8, 10
- **User stories covered**: 22, 23, 51 (extension), 53

#### What to build

A surface improvement on top of T10's bug fix: at the start of every
chain step the worker has just dispatched a real command on the Mac,
and the operator currently has no way to see what got run. SETTINGS
in particular produces zero Build Log output by design (its bytes go
to `settings_buf` for parsing only), so on a slow `-showBuildSettings`
query the panel sits blank for many seconds even though the chain is
working. Subsequent steps (build, install, launch) do stream raw
output but with no visible header telling the operator which command
that block belongs to — a 200-line install/launch interleave can be
hard to read without per-step demarcation.

This task lands a single ostrich-voice demarcation line at the start
of each step, carrying the exact command string. It follows the same
pattern the Device Log already uses for `> ── NEW PAYLOAD // <time> ──`
(story 35) — separator glyphs + `>` voice — extended to the Build Log
so the operator gets the same "you can always tell which run a line
belongs to" guarantee on the build side. Format candidate (theme.md
owns final wording):

```
> ── COMPILING EXPLOIT // xcodebuild -showBuildSettings -json … ──
> ── COMPILING EXPLOIT // xcodebuild -workspace … -scheme … ──
> ── PRIMING TARGET // xcrun simctl boot <udid> ──
> ── DEPLOYING PAYLOAD // xcrun simctl install <udid> <app> ──
> ── EXECUTING PAYLOAD // xcrun simctl launch --console <udid> <bundle> ──
```

The phase label on the left makes it scannable; the command tail on
the right is the debugging payload. The PRD's "logs are raw, ostrich
voice only on separators and headers" contract (stories 27, 51) is
*preserved*: the demarcation is structurally a separator, not a tool
output line, and lives next to the existing NEW PAYLOAD separator
in the lexicon's Build/deploy group.

#### Technical Details

Per ARD §"Interfaces (`session.h`)" — the run event family is the
extension seam, mirroring the existing `REV_PHASE` / `REV_BUILD_LOG`
shape:

1. **New event kind: `REV_BUILD_MARK`.** Add to `SessionRunEventKind`
   alongside the existing four. It carries a fixed-size command-summary
   field (`char cmd[RUN_CMD_SUMMARY_CAP]`, e.g. 256 bytes, copied by
   value — thread-confinement preserved) and the `RunPhase` of the
   step being entered (so the UI can render the phase label
   consistently with the rest of the chain). No new ring; reuses
   `run_event_ring`.
2. **Worker emit site.** In `src/session/session.c` `drive_run_chain`,
   immediately after `build_step_cmd(ctx, cmd, sizeof(cmd))` succeeds
   and before `ssh_channel_exec`, push a `REV_BUILD_MARK` event with
   the command summary (truncate-with-ellipsis if longer than
   `RUN_CMD_SUMMARY_CAP - 1` — never split a UTF-8 sequence in the
   middle; ASCII-only command strings are the actual case so a byte
   truncate is safe). Emit for every step: settings, build, prime-boot,
   prime-status, install, launch. The launch event fires before the
   channel is handed to the DevConsole, so the demarcation lands in
   the Build Log, not the Device Log.
3. **App drain + insertion.** In `src/app/app.c` extend the
   `session_run_poll` switch with a `REV_BUILD_MARK` arm that formats
   the line from the lexicon template and the carried command, then
   calls `logbuf_mark(app->build_log, line)`. The format goes through
   one new lexicon key (a printf-style template like
   `"> ── %s // %s ──"` taking the phase label and the command) so a
   future straight-mode swap stays a no-UI change.
4. **Lexicon group.** One new `LexKey` (e.g. `LEX_RUN_STEP_HEADER_FMT`)
   in `include/lexicon.h` + `src/lexicon.c`, asserted in
   `tests/lexicon_test.c`. No new phase or status keys.
5. **No changes to `librunstate` or `libbuilddeploy`.** Both are
   passive participants — runstate provides `runstate_phase_lex` for
   the formatter, builddeploy already constructs the exact command
   string the worker captures.
6. **`logbuf` contract reuse.** `logbuf_mark` already flushes any
   pending partial line before inserting a complete demarcation line
   (T3 acceptance criterion); calling it from the build-log path is a
   pure reuse of the existing primitive — no `logbuf` changes.
7. **Tests.** Extend `tests/session_run_test.c` to assert that a
   `REV_BUILD_MARK` fires before the first `REV_BUILD_LOG` of every
   step on the happy path, and that it carries the right phase and a
   non-empty command. Extend `tests/lexicon_test.c` to assert the new
   key resolves. `ui_test` stays headless and unaffected.

#### Acceptance criteria

- [ ] `session.h` declares `REV_BUILD_MARK` with a phase field and a
      fixed-size command-summary field; `SessionRunEvent` remains a
      tagged union with no pointers crossing the worker↔UI boundary.
- [ ] The worker emits exactly one `REV_BUILD_MARK` per step, before
      the step's first `REV_BUILD_LOG` chunk and before the SSH exec;
      asserted in `session_run_test.c` for settings/build/install/launch
      on the device-target happy path and for settings/build on the
      compile-only path.
- [ ] The app's `REV_BUILD_MARK` handler calls `logbuf_mark` exactly
      once per event, formatting through the new lexicon template; the
      line is the only Build Log entry that begins with `> ──` (mirrors
      the Device Log's NEW PAYLOAD invariant).
- [ ] A SETTINGS step long enough to be perceptible shows its
      demarcation line in the Build Log immediately on dispatch,
      eliminating the formerly-blank dwell.
- [ ] In a COMPILE-while-running scenario the COMPILE's demarcation
      lands in the Build Log (where the new compile streams) and the
      Device Log keeps streaming with no interference (T7 invariant
      preserved).
- [ ] `make test` passes on both Linux and macOS (`session_run_test`,
      `lexicon_test`, headless `ui_test`).
- [ ] Manually against a live Mac: every chain step on EXECUTE and
      COMPILE shows its `> ── PHASE // cmd ──` demarcation before its
      tool output; the operator can read the panel as a sequence of
      labeled blocks; copy-to-clipboard via the COPY button includes
      the demarcation lines verbatim; the demarcations render in the
      decorative cyan/magenta voice (not semantic green/red), matching
      the existing NEW PAYLOAD line.
