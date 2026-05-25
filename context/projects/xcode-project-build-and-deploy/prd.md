# PRD — Build & Deploy (the core build → install → launch loop)

## Problem Statement

ostrich can now connect to a Mac and assemble a complete run
configuration — project, scheme, build configuration, bundle id,
and a chosen device or simulator. The discovery project ends at
READY // ARMED. But READY is a promise ostrich cannot yet keep:
pressing EXECUTE does nothing, because the entire
build → install → launch chain does not exist. Everything up to
this point has been preparation; the payoff — actually running my
app on the target and watching it — is missing.

So today, to turn that ready configuration into a running app, I
still have to leave ostrich for the macOS GUI: open Xcode, pick
the scheme and destination I already chose in ostrich, press Run,
and watch Xcode's build log and the device console in windows I
came to ostrich to escape. The one thing ostrich exists to do —
keep the build/run/observe loop inside my Linux/Neovim world — is
the one thing it still cannot do. I am stranded one step short of
the goal.

I also cannot *observe*. Even if the build ran, I would be blind:
there is no live build log to watch xcodebuild compile (and to
read the errors when it fails), and no device log to watch my app
actually do something once it launches. Observing is half the
loop — a build I cannot watch and an app whose output I cannot
read is barely better than firing blind. And the iteration that
makes any of this worthwhile — edit in Neovim, re-run, watch
again — has nowhere to happen.

## Solution

ostrich gains its reason to exist: a single action that drives the
whole chain on the Mac and streams both halves of the result back
live.

With a preset READY and a target locked, I press **▶ EXECUTE** and
ostrich runs, on the Mac over the existing session, `xcodebuild`
(build) → install (`devicectl device install` for a device,
`simctl install` for a simulator) → launch the app on the target.
The Control/Status strip narrates the real phases as they happen —
`COMPILING EXPLOIT…` while xcodebuild runs, `DEPLOYING PAYLOAD…`
during install, `EXECUTING PAYLOAD…` at launch — each dwelling for
its genuine duration with no fake delay, resolving to
`TARGET ACQUIRED // LIVE` once the app is up. ostrich resolves the
built `.app` path itself; it is never a field I fill. A build-only
**COMPILE** runs just the compile step (for a quick "does it still
build?") and needs no target. While the chain is in flight EXECUTE
becomes **■ ABORT** and I can stop it; ABORT also stops a running
app and returns me to STANDBY.

I watch it happen. The **Build Log** streams the chain's raw
tooling output live — xcodebuild as it compiles, then the
install/launch commands — cleared at the start of each build so it
shows only the current run, auto-scrolling but pausing when I
scroll up to read, with copy and clear. When xcodebuild fails the
errors are right there (`EXPLOIT FAILED`); when the device rejects
the install or the launch fails, that is a *distinct* failure
(`DEPLOYMENT FAILED // PAYLOAD REJECTED`) so I know the build was
fine and the device step broke — a different fix entirely.

The **Device Log** streams my app's own output — its
stdout/stderr, captured by launching the process attached
(`devicectl … process launch --console` on a device, `simctl
launch --console` on a simulator). This is inherently scoped to my
app, which is exactly what I want to read. It keeps a continuous,
demarcated history: each new launch drops a `> ── NEW PAYLOAD ──`
separator so I can always tell which run a line came from, and the
buffer is bounded so an hours-long session never bloats memory.

The iteration loop is the point. After I edit source in Neovim I
press ▶ EXECUTE again. ostrich **terminates the running instance
first**, then rebuilds, reinstalls, and relaunches —
terminate-first on purpose, so that every device-log line after
the relaunch belongs unambiguously to the new build, with no
cross-build poisoning. The device log goes briefly dark during
that rebuild (the old app is gone); that is expected, and is the
price of clean, trustworthy output.

The genuinely-parallel case is **COMPILE while an app is
running**. A build-only does not redeploy, so ostrich leaves the
running app alive: its Device Log keeps streaming live while the
Build Log streams the new compile in parallel — two live streams
at once, safely, because nothing is being swapped underneath the
running instance. Because I have now built code newer than what is
deployed, ostrich shows a clear **stale** indicator (the running
PAYLOAD is behind the latest EXPLOIT), nudging me to EXECUTE when
I am ready to deploy it.

All of this is forgiving and never freezes the window: the chain
and both streams run off the UI thread over concurrent SSH
channels on the one existing session — no second connection. A
not-booted simulator is booted automatically (no Simulator.app;
the Mac is headless and I observe the sim purely through its Device
Log). A build failure, a deploy failure, an ABORT, and an SSH drop
mid-run each resolve to a clear terminal state with my local state
preserved, and a command that hangs degrades to a terminal failure
rather than an infinite spinner.

This project closes the core loop `design.md` describes —
connect → discover → **play → observe** — and is the first time
ostrich does the job it was built for. It stops there: no error
parsing or jump-to-error, no launch arguments or environment
variables, no debugger, no unified-system-log firehose — only the
build → install → launch chain and the two live logs that make it
observable.

## User Stories

1. As the operator, I want one EXECUTE action to drive the whole
   build → install → launch chain on the Mac, so that running my
   app is a single press, not three manual steps.

2. As the operator, I want EXECUTE enabled only when the
   configuration is READY (preset complete and a target locked),
   so that I never fire a chain that cannot succeed.

3. As the operator, I want a build-only COMPILE action, so that I
   can ask "does it still build?" without installing or launching.

4. As the operator, I want COMPILE to need no target — building
   for a generic destination when none is locked — so that a quick
   compile-check works with nothing plugged in.

5. As the operator, I want COMPILE to build for my locked target's
   destination when one is selected, so that the compile-check
   matches where I will actually run.

6. As the operator, I want ostrich to install to a physical device
   via `devicectl`, so that my primary target just works.

7. As the operator, I want ostrich to install to a simulator via
   `simctl`, so that simulators are a first-class target too.

8. As the operator, I want ostrich to infer device-vs-simulator
   tooling from the target I picked, so that I never set a
   "target type" by hand.

9. As the operator, I want a not-booted simulator booted
   automatically as part of the chain, so that picking a cold
   simulator just works.

10. As the operator, I want simulators observed through the Device
    Log with no Simulator.app GUI, so that the headless-Mac premise
    holds and I get logs rather than a window I cannot see anyway.

11. As the operator, I want ostrich to resolve the built `.app`
    path itself, so that install is automatic and the artifact path
    is never a field I must know or type.

12. As the operator, I want the app launched on the target after a
    successful install, so that EXECUTE ends with my app actually
    running.

13. As the operator, I want the run-state label to narrate the
    current phase — STANDBY, COMPILING EXPLOIT…, DEPLOYING
    PAYLOAD…, EXECUTING PAYLOAD…, TARGET ACQUIRED // LIVE — so that
    I always know where in the chain I am.

14. As the operator, I want a build ▷ install ▷ launch progression
    indicator, so that the chain's shape is visible at a glance.

15. As the operator, I want each phase label to dwell for its real
    duration with no artificial pause, so that the narration is
    honest and never slows me down.

16. As the operator, I want EXECUTE to toggle to ■ ABORT while a
    run is in flight, so that there is one obvious way to stop it.

17. As the operator, I want ABORT to cancel an in-flight chain —
    killing the remote xcodebuild process or aborting the current
    step — so that a bad or stuck run never traps me.

18. As the operator, I want ABORT to also terminate a running app
    and return me to STANDBY, so that one control stops whatever is
    happening, not just builds.

19. As the operator, I want to press EXECUTE again to iterate
    (rebuild → reinstall → relaunch), so that the edit-run-observe
    loop is a single repeated key.

20. As the operator, I want a re-EXECUTE to terminate the running
    instance first, then rebuild, so that every device-log line
    after the relaunch belongs unambiguously to the new build.

21. As the operator, I accept the Device Log going briefly dark
    during a rebuild, so that I trade a moment of silence for
    guaranteed clean, un-poisoned per-build output.

22. As the operator, I want to watch xcodebuild output stream live
    in the Build Log, so that I can see the build progress and read
    errors as they appear.

23. As the operator, I want the Build Log to also carry the
    install and launch tooling output, so that it is the one
    "operation log" of everything ostrich runs on my behalf.

24. As the operator, I want the Build Log cleared at the start of
    each build, so that it shows only the current run.

25. As the operator, I want the Build Log to auto-scroll but pause
    when I scroll up, so that I can read earlier output without
    fighting the tail.

26. As the operator, I want copy and clear affordances on the
    Build Log, so that I can grab output or reset the panel.

27. As the operator, I want the Build Log left raw with no error
    parsing, so that I see exactly what xcodebuild said.

28. As the operator, I want a themed empty Build Log
    (`// NO PAYLOAD COMPILED`) before the first build, so that it
    reads intentionally rather than blank.

29. As the operator, I want a build failure shown as EXPLOIT
    FAILED with the errors in the Build Log, so that a compile
    error is obvious and its detail is right there.

30. As the operator, I want an install/launch failure shown as a
    *distinct* DEPLOYMENT FAILED // PAYLOAD REJECTED, so that I
    know the build was fine and the device/install step broke — a
    different fix from a compile error.

31. As the operator, I want my app's own output to stream live in
    the Device Log after it launches, so that I can watch it
    actually run.

32. As the operator, I want the Device Log inherently scoped to my
    app (its process console), so that I read my app's output
    without wading through a system firehose.

33. As the operator, I want the Device Log to work the same way on
    both a physical device and a simulator, so that observing does
    not change shape with the target.

34. As the operator, I want the Device Log to start automatically
    when the app launches, so that observing needs no separate
    action.

35. As the operator, I want the Device Log to preserve history
    with a `> ── NEW PAYLOAD ──` separator at each new launch, so
    that I keep prior-run output while always knowing which build a
    line came from.

36. As the operator, I want the Device Log buffer bounded (oldest
    dropped past a cap), so that an hours-long observe session
    never bloats memory.

37. As the operator, I want copy and clear affordances and the
    same auto-scroll/pause behavior on the Device Log, so that it
    is as readable and grabbable as the Build Log.

38. As the operator, I want a streaming Device Log header
    (`LIVE FEED // INTERCEPTING`) and a themed empty state
    (`// NO SIGNAL — TARGET DARK`), so that its status reads at a
    glance.

39. As the operator, I want the Device Log left raw and never
    recolored, so that it shows the truth of my app's output, not
    ostrich's interpretation of it.

40. As the operator, I want COMPILE to leave a running app alive,
    so that a quick compile-check does not kill the instance I am
    observing.

41. As the operator running a COMPILE while an app is live, I want
    the Device Log to keep streaming while the Build Log streams
    the compile in parallel, so that I get the genuine
    two-streams-at-once payoff safely.

42. As the operator, I want a clear stale indicator when I have
    built code newer than the deployed instance, so that I know the
    running app is behind the latest build and can choose to
    redeploy.

43. As the operator, I want the window to stay smooth during
    builds and while both logs stream, so that ostrich feels
    lightweight even under load.

44. As the operator, I want the chain and both streams to run over
    concurrent channels on the existing session, so that observing
    never opens a second connection or a new login.

45. As the operator, I want distinct, plain-language terminal
    states — build failure, deploy failure, aborted — so that I
    always know what happened and what to change.

46. As the operator, I want a command that hangs to degrade to a
    terminal failure rather than an infinite spinner, so that a
    stuck device or tool never strands me.

47. As the operator, I want an SSH drop mid-run treated as an
    abort with my local state preserved, so that a Wi-Fi blip ends
    the run cleanly instead of corrupting my session.

48. As the operator, I want all new copy — EXECUTE, COMPILE,
    ABORT, the run-state labels, DEPLOYMENT FAILED // PAYLOAD
    REJECTED, the NEW PAYLOAD separator, and the stale indicator —
    sourced from the centralized lexicon, so that a future
    straight-mode stays a no-UI swap.

49. As the operator, I want palette discipline kept — decorative
    cyan/magenta for chrome, semantic green/red/amber only for
    meaning, logs in calm off-white — so that a real failure reads
    unmistakably and dense logs stay readable.

50. As the operator, I want the chain to stay zero-cost whimsy:
    phase labels dwell only for real durations, with no fake
    "deploying…" pauses and no success gate, so that the theme
    never slows the loop.

51. As the operator, I want the Build and Device Logs kept 100%
    raw, with ostrich's `>`/magenta voice only on its own surfaces
    (status strip, log headers, separators), so that tool output is
    never confused with ostrich narration.

52. As the operator, I want the run controls keyboard-drivable, so
    that I stay in the keyboard consistent with my Neovim workflow.

53. As the developer, I want ostrich to instrument its own
    build/deploy internals (command exec, channel lifecycle,
    failures) via the existing `log.h` facility under
    `OSTRICH_DEBUG`, so that I can debug the orchestration without
    adding a user-facing debug surface.

54. As the developer, I want `make test` to stay meaningful and
    green for this project — state-based tests over the run-state
    machine transitions, the failure-code → reason mapping, the
    device/simulator command and destination construction, the log
    ring buffer, and the stale-build computation — with real-Mac
    calls gated behind host availability (SKIP when absent), so
    that the gate stays trustworthy.

55. As the developer, I want build & deploy to build and run on
    both Linux and macOS hosts, so that both supported platforms
    work.

56. As the developer, I want this project to consume the
    connection's multi-channel, off-thread session model rather
    than re-architect it, so that orchestration and dual-stream
    logging are the first real payoff of that concurrency design.

## Out of Scope

Deferred to later projects or fixed as non-goals:

- **Build-error parsing / jump-to-error.** The Build Log is raw
  output only in MVP; structured errors and navigation are
  deferred per `workflow.md`.

- **The unified-system-log firehose.** Process-console makes the
  Device Log inherently app-scoped; a true full-system firehose is
  dropped for MVP (it is not available on a physical device with
  Xcode-only tooling, and would likely be simulator-only or need
  non-Xcode tooling). This amends `workflow.md`'s firehose toggle.

- **Launch arguments, environment variables, and extra
  `xcodebuild` flags.** Advanced run-config inputs remain deferred
  per `workflow.md`.

- **Clean builds / build-setting overrides.** ostrich builds
  incrementally; an explicit clean and setting overrides are
  deferred.

- **The debugger and SSH port-forwarding.** Future goals per
  `design.md`; the chain must not preclude them, but neither is
  built.

- **Simulator screen / device screen mirroring (VNC).** ostrich
  observes through logs, never a remote screen; a simulator gives
  logs, not a visible window. This is consistent with the
  no-remote-desktop premise and is a non-goal.

- **Log persistence to disk.** Both panels are ephemeral in-memory
  ring buffers with copy/clear. (ostrich's own internal
  `OSTRICH_DEBUG` file logger is the separate `logging` project and
  is not the product feature here.)

- **An in-app debug/diagnostics surface for ostrich internals.**
  Explicitly the deferred item of the `logging` project; this
  project builds only the operator-facing Build and Device Log
  panels.

- **Reattaching to an in-flight remote build across a reconnect.**
  An SSH drop mid-run aborts the run; reattach is deferred per
  `workflow.md`.

- **Multiple targets per scheme, test plans, and extension / watch
  / widget sub-apps.** A single app build → install → launch is in
  scope; richer multi-target handling is deferred.

- **New persistence.** Nothing new persists here — the preset and
  remembered target already persist from discovery, and logs are
  ephemeral.

- **The threading / arena / channel mechanism.** The PRD fixes the
  *behaviors* (off-thread, concurrent, cancelable, bounded,
  never-freeze); the worker model, channel allocation, arenas, and
  library boundaries are the ARD's.

- **Multi-Mac / multi-window / multiple simultaneous runs.**
  Non-goal; one Mac, one active run at a time.

## Further Notes

- **Traceability.** This project realizes `design.md` core goals
  #4 (Play — one action orchestrates `xcodebuild` → install →
  launch), #5 (stream build logs live), #6 (stream device logs
  live; physical devices primary, simulators supported), and #7
  (concurrency — multiple live streams), and is the increment where
  the core loop finally closes end-to-end. It implements the
  Control/Status actions and the Build Log / Device Log containers
  from `workflow.md`, drives its run-state machine, and serves
  happy paths 1 (cold start → Play → observe), 3 (iterate loop), 4
  (build-only), 5 (switch target/preset then Play), and 6 (recover
  from a drop mid-run). It traces to `theme.md` for the EXECUTE /
  COMPILE / ABORT actions, the run-state labels, the log empty
  states and the LIVE FEED header, palette discipline, and the
  `>` voice signature. It depends directly on the connection
  project's multi-channel, off-thread, kept-alive session and on
  discovery's READY configuration (preset + target).

- **Deliberate amendment to `design.md` #7 (top-authority — flag
  prominently).** Core goal #7 states the device log "stays live
  continuously, including across rebuilds, while build output
  streams in parallel." The process-console device-log source plus
  the operator's chosen **terminate-first** rebuild contradict the
  "across rebuilds … in parallel" clause: on a Play rebuild the
  running app is killed first (device log dark during the build) so
  that post-relaunch output is unambiguously the new build's. The
  genuine simultaneous-streams concurrency is preserved in two
  places instead: (a) during the **running** phase the Device Log
  streams continuously, and (b) a **COMPILE while running** keeps
  both streams live in parallel (a build-only never redeploys, so
  it is safe). This is a real revision of a core functional goal,
  driven by a data-integrity preference, and should be reconciled
  into `design.md` and `workflow.md` (the connection and discovery
  projects amended upstream docs the same way).

- **Deliberate refinements to `workflow.md` (to reconcile
  upstream).** (1) The Device Log "stays streaming continuously,
  including across rebuilds" becomes: continuous during the running
  phase, but **cut over (terminate-first) on a Play rebuild** with
  a `NEW PAYLOAD` demarcation, dark during that build. (2) The
  "toggle to the full system firehose" is **removed** — the Device
  Log is the app's process console only. (3) Happy-path-3's
  "running app is terminated; rebuild streams … Device Log stays
  live throughout" is corrected to the terminate-first reality.
  (4) The Build Log's "raw `xcodebuild` output" is **broadened** to
  the full chain's raw tooling output (xcodebuild, then
  install/launch). (5) A new **stale-build indicator** and the
  **COMPILE-while-running** concurrency are added. These touch
  product flow/structure (`workflow.md`'s domain) and are recorded
  here to be folded back.

- **New camp copy (to add to `theme.md`).** Like the connection and
  discovery projects, this needs lexicon the theme does not yet
  name, to be added as deliberate entries under a **Build / deploy**
  group: the distinct deploy failure
  `DEPLOYMENT FAILED // PAYLOAD REJECTED` (semantic `fail` red, in
  the `>` voice); the Device Log run separator
  `> ── NEW PAYLOAD // <time> ──`; and a **stale-build** indicator
  (candidate `PAYLOAD STALE // NEW EXPLOIT READY`) shown when the
  deployed instance is behind the latest successful build.
  Optionally a simulator-boot phase label (candidate
  `PRIMING TARGET…`) preceding `DEPLOYING PAYLOAD…`. Final wording
  is `theme.md`'s call; no structural change is implied.

- **Decisions resolved while scoping this PRD.** (1) Scope is
  all-in: Play orchestration + run-state machine + Build Log +
  Device Log + concurrency in one project (Build Log is
  inseparable from Play; the Device Log and concurrency are the
  headline payoff and ship together). (2) The Device Log source is
  the launched app's **process console** (`devicectl … process
  launch --console` / `simctl launch --console`) — Xcode-only, no
  extra Mac deps, symmetric across device and simulator, inherently
  app-scoped. (3) Re-Play is **terminate-first** for clean,
  un-poisoned per-build output; the device log goes dark during the
  rebuild, by design. (4) **COMPILE leaves a running app alive**
  (the safe two-streams-at-once case) and surfaces a **stale**
  indicator afterward. (5) The Device Log **preserves history with
  a `NEW PAYLOAD` demarcation** at each launch (not cleared). (6)
  The **firehose toggle is dropped**. (7) Both panels are
  **bounded ring buffers** with copy/clear, no disk persistence.
  (8) **ABORT is a universal stop** — cancels an in-flight chain
  and terminates a running app. (9) A not-booted **simulator is
  auto-booted** (headless; observe via the Device Log). (10)
  **COMPILE needs no target** (generic destination when none is
  locked); Play requires a target. (11) Failures are **distinct**:
  build (`EXPLOIT FAILED`) vs deploy
  (`DEPLOYMENT FAILED // PAYLOAD REJECTED`) vs abort/drop
  (`OPERATION ABORTED`). (12) Install/launch tooling output goes
  **into the Build Log** (the "operation log"); the Device Log
  carries only the app's own output. (13) "Logging within the
  product" means the two **operator-facing panels**; internals are
  instrumented via the existing `log.h`/`OSTRICH_DEBUG` facility,
  not a new in-app surface.

- **Consequences of the process-console choice.** The Device Log
  is the app's **stdout/stderr**, not the os_log/unified stream, so
  it shows `print`/stderr output and the launch tool's interleaved
  process chatter rather than structured system logging. The launch
  step runs **attached** (`--console`), so the launch command stays
  open for the life of the app and *is* the device-log stream;
  reaching `TARGET ACQUIRED // LIVE` means the process is up and
  its console is streaming. A target unplugged or a simulator shut
  down mid-run ends that stream and resolves the run accordingly.

- **Architectural seams for the ARD (flagged, not designed).** A
  new **run command/event family** on the existing session worker
  (alongside the connection and discovery families) carrying
  EXECUTE / COMPILE / ABORT and streaming build- and device-log
  line events with cross-thread handoff into UI memory; a
  **dedicated build channel** and a **dedicated device-console
  channel** that can be live at once (honoring the libssh2
  one-open-at-a-time rule — **serialize channel-opens**, read
  concurrently — and the `devicectl --json-output -` deadlock
  lesson where any JSON-emitting query is used); a **liveness model
  for long / infinite streams** (a build may run far longer than
  discovery 60s watchdog, and the device console runs indefinitely,
  so liveness must be **output/heartbeat-based**, not a fixed
  total-time cap); **`.app` path resolution** (via
  `xcodebuild -showBuildSettings` or a pinned `-derivedDataPath`,
  ostrich-resolved, never user-facing); **destination
  construction** for device vs simulator; **app termination** via
  `devicectl`/`simctl`; the **run-state machine**; the **bounded
  log ring buffers**; and the **stale-build computation** (latest
  successful build vs deployed instance). Arenas (e.g. a per-run
  arena, per-stream buffers) are named in the ARD.

- **Keeping `make test` meaningful.** A real build/install/launch
  needs a live Mac with Xcode and a target, so the gate stays
  trustworthy by black-box testing the display-free, network-free
  parts — the run-state machine transitions (including
  terminate-first re-Play, COMPILE-while-running, and every failure
  edge), the failure-code → reason mapping, the device/simulator
  command and `-destination` construction, the log ring-buffer
  behavior (bounded, demarcation insertion), and the stale-build
  rule — and gating any real-Mac call behind host availability
  (printing SKIP when absent), mirroring the connection / discovery
  / app-shell pattern.

- **Whimsy stays zero-cost.** `COMPILING EXPLOIT…`, `DEPLOYING
  PAYLOAD…`, and `EXECUTING PAYLOAD…` dwell only for their real
  command durations — a build genuinely takes seconds, so the label
  dwells naturally — with no artificial pauses and no success gate;
  `TARGET ACQUIRED // LIVE` lands the instant the app is up. Both
  log panels stay raw; ostrich's voice appears only on its own
  surfaces (status strip, log headers, the `NEW PAYLOAD`
  separator), never recoloring real tool or app output.
