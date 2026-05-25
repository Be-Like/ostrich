# Ostrich — Workflow & Layout

This document describes **how a developer interfaces with ostrich** —
the happy-path interaction flows and the high-level structure of the
UI layout. It sits beneath `context/design.md` (the statement of
intent) and above the per-project PRDs/ARDs/implementation plans under
`context/projects/`. Those downstream documents should trace back to
the flows and layout fixed here.

It deliberately captures **product flow and layout**, not
architecture or task breakdown. Where a decision touches something
`design.md` explicitly defers (e.g. device-log filtering, config
persistence mechanism), the *UX intent* is recorded here and the
*mechanism* is left to a per-project doc.

## Mental model

Three ideas frame everything below:

- **One Mac, one active run configuration.** At any moment ostrich is
  centered on a single connected Mac and a single *active* run
  configuration. Multiple configurations can be *saved*, but only one
  is live at a time. A configuration captures *what* and *how* to
  build (project, scheme, configuration, bundle id); *where* to run —
  the target device or simulator — is chosen separately at run time
  and is **not** part of the saved configuration. There is no
  multi-Mac, multi-project, or multi-window workspace.
- **Phases are states, not screens.** The Connect → Configure → Play →
  Observe phases from `design.md` are *states of persistent panels*,
  not steps in a wizard. The user lives in one docked console rather
  than advancing through pages.
- **Concurrency means parallel streams, not parallel targets.** The
  one thing that genuinely runs in parallel is *output*: the Device
  Log streams the running app continuously while the Build Log
  streams in parallel. The clean simultaneous case is a build-only
  **Build/COMPILE** — it never redeploys, so the running app's
  Device Log stays live while a fresh build streams. A full Play
  rebuild instead **cuts the Device Log over** to the new instance
  (terminate-first) for clean per-build output. The log panels are
  never torn down either way.

## Layout

ostrich is a **single window** built on the ImGui docking branch. All
containers are **locked to their docked locations** (no free-floating
or user-rearranged panels in MVP), with one deliberate exception: the
**Build Log / Live Feed divider is user-resizable** (see below).

On launch the user is met with a **connection overlay** (modal). Once
connected, the overlay is dismissed and the connection collapses into
a thin top bar; the docked working area is revealed.

```
LAUNCH (overlay, modal):
  +-----------------------------+
  |  Connect to Mac             |
  |  host ____  port __  user _ |
  |  auth: ( ) ssh-agent        |
  |        ( ) password ____    |
  |        [ ] remember password|
  |  -- saved connections --    |
  |   o studio-mac              |
  |   o mac-mini                |
  |        [Connect]            |
  +-----------------------------+

CONNECTED (overlay dismissed):
  +----------------------------------------------+
  | user@mac  * connected      [update] [close]  |  <- thin top bar
  +----------------------------------------------+
  | XCODE PROJECT CONFIGURATION                  |
  |  project [App.xcwsp             v]+          |
  |  preset  [my-app v]+/-  scheme ___           |
  |  target  [iPhone 15 v]↻   config ___         |
  |  bundle ___             * READY              |
  |  ▶ EXECUTE  COMPILE  ■ ABORT                 |
  |  BUILD ▷ INSTALL ▷ LAUNCH   PAYLOAD STALE    |
  +------------------------+---------------------+
  | Build Log              |  Live Feed          |  <- resizable split
  |  (xcodebuild, full-    |  (launched app      |
  |   chain output)        |   stdout/stderr;    |
  |  ...fills vert...      |   demarcated hist)  |
  +----------------------------------------------+
  | ostrich  *  60 FPS                           |  <- slim footer
  +----------------------------------------------+
```

Containers, top to bottom:

- **Connection bar** (thin, full width, top) — connection identity and
  live status, plus controls to update or close the connection.
- **XCODE PROJECT CONFIGURATION** (full-width, directly below the
  connection bar) — one merged container: the run-config form
  (project, preset, scheme, config, bundle id; target picker) on its
  left/center, and the run-control cluster (EXECUTE / COMPILE / ABORT,
  phase, progression, READY, PAYLOAD STALE) on its right.
- **Build Log** (lower-left) and **Live Feed** (lower-right) — split
  the full width below the config band and **fill the vertical space**
  between it and the footer; the divider is **user-resizable**
  (default 50/50) — a deliberate exception to the locked-panels rule.
- **Footer** (slim, full width, bottom) — ostrich-related info such as
  FPS and app status (not target/run output).

## Containers in detail

### Connection overlay → connection bar

The overlay collects: **host, port (default 22), user**, and an
**auth method**:

- **ssh-agent** — uses the running ssh-agent; no secret entered.
- **password** — an explicit password field, with an opt-in
  **"remember password"** checkbox.

It also lists **saved connections** for one-click loading. On launch
the overlay is always shown with the **most-recently-used connection
pre-selected**, so connecting is a single click or Enter; ostrich
never silently reaches for a Mac that may be off.

Once connected the overlay is dismissed and the **connection bar**
shows `user@host`, live status (`connected / reconnecting… /
disconnected`), and controls to **update** (re-open the overlay) or
**close** the connection.

### XCODE PROJECT CONFIGURATION

This container merges the former **Run Configuration** form and
**Control / Status** cluster into one full-width band directly below
the connection bar.

**Run configuration form (left/center)**

A **preset selector** dropdown (choose / new / rename / delete) sits
at the top. Run configurations are **named presets bound to a
connection**; the active preset drives Play. A preset holds **four
essential fields**:

1. **Project/workspace path** — a hand-editable path field with a
   **`[v]` picker button** that opens a popup containing the scan
   root, the `⌖ SCAN HOST` / `■ ABORT SCAN` button, and the scanned
   blueprints (with `BLUEPRINTS RECOVERED`, `// NO BLUEPRINTS`,
   `XCODE NOT FOUND`, and `COULD NOT READ INVENTORY` states).
   Selecting a blueprint prefills the path (and triggers the
   scheme/config/bundle read). Typing a path by hand remains fully
   supported and is never silently re-corrected — the hand-typed path
   is the escape hatch when scanning does not surface the project.
2. **Scheme** — a prefilled editable input with a discovered-set hint.
3. **Build configuration** (default `Debug`) — a prefilled editable
   input with a discovered-set hint.
4. **Bundle ID** — a prefilled editable input with a discovered-set
   hint.

The **target selector** is a **dropdown picker** fed by a `↻ SWEEP`
action, listing all devices and simulators in range in one unified set
(each labeled device vs. simulator, with booted state for simulators).
The selection is **session-sticky** and remembered separately from any
preset; the last target is silently re-selected on connect when still
in range. An empty or stale result shows a clear "no targets in range"
state. The target is **not** part of the saved configuration.

The `.app` output path needed for install is resolved by ostrich and
is **not** a user-facing field. Advanced inputs (launch arguments,
environment variables, extra `xcodebuild` flags) are deferred.

**Control / Status cluster (right region)**

- **Readiness** — when the active preset is complete (project, scheme,
  config, bundle id) and a target is selected, ostrich shows the
  configuration is **ready** to Play.
- **▶ EXECUTE** — runs the full chain `build → install → launch`;
  requires a complete preset and a locked target. A not-booted
  simulator is auto-booted as part of the chain (headless — observed
  via the Live Feed). While a run is in progress, becomes **■ ABORT**
  (abort run and terminate running app, back to idle).
- **COMPILE** — build-only; compiles without installing or launching
  (also abortable). **Needs no target** (builds for a generic
  destination when none is locked) and **leaves a running app alive**,
  so the Live Feed keeps streaming while the build streams in parallel.
  Because it produces a build newer than what is deployed, ostrich
  then flags the running app as **stale** (behind the latest build)
  until the next Play.
- **Phase / progression** — shows the current phase and a
  `BUILD ▷ INSTALL ▷ LAUNCH` progression.
- **PAYLOAD STALE** — flag shown when the running app is behind the
  latest build; cleared on the next Play.

### Build Log

The build/deploy operation's raw tooling output — `xcodebuild` as it
compiles, then the install and launch commands (`devicectl` /
`simctl`) — with no error parsing in MVP. **Cleared at the start of
each build.** Auto-scrolls to the bottom, **pausing when the user
scrolls up**. Provides clear/copy affordances. A build failure and a
deploy (install/launch) failure surface as *distinct* states (see
the run-state machine); both render their output here.

When the panel has no content it shows the centered ostrich wordmark
with the `// NO PAYLOAD COMPILED` empty-state caption; the art
disappears the instant any content arrives.

### Live Feed

The launched app's own output (stdout/stderr), captured by running
the app attached (`devicectl … process launch --console` on a
device, `simctl launch --console` on a simulator), so it is
inherently scoped to the app. It streams continuously while the app
runs. A full Play rebuild is **terminate-first**: the old instance
is killed before the build, so the Live Feed goes briefly dark
during the rebuild and then resumes on the new instance — by design,
so each build's output is unambiguous and never cross-poisoned.
History is **preserved with a run demarcation** (`> ── NEW PAYLOAD
── `) at each launch rather than cleared, within a **bounded buffer**
so a long session never bloats memory. The panel is never torn down.
Auto-scroll behaves like the Build Log.

When the panel has no content it shows the centered ostrich wordmark
with the `// NO SIGNAL — TARGET DARK` empty-state caption; the art
disappears the instant any content arrives.

The vertical divider between Build Log and Live Feed is
**user-resizable** (default 50/50, session-only, clamped to a sane
minimum for each panel). This is a deliberate soft exception to the
"no user rearrange in MVP" rule; all other panel boundaries remain
locked.

### Footer

A slim strip for ostrich's own diagnostics (FPS, app-level status) —
distinct from build/device output.

## Run-state machine

```
idle ──Play──> building ──> installing ──> launching ──> running
  ^                                                         |
  |                                          re-Play: terminate, build
  |                                                         v
  +<── failed (build or deploy; shown in the Build Log)  building ...
  +<── aborted (Stop pressed, or SSH drop mid-run)
```

- **Play** from `idle` (or while `running`) drives the full chain;
  it requires a complete preset and a locked target.
- **re-Play while running** is **terminate-first**: it kills the
  current app instance *before* rebuilding, so the **Device Log goes
  briefly dark during the build** and then resumes on the fresh
  instance (with a `NEW PAYLOAD` demarcation) — keeping each build's
  device output unambiguous. The simultaneous build+device streaming
  payoff instead lives in a build-only **Build** (see below).
- **Build** runs `idle → building → idle` without install/launch;
  while `running` it builds *without* terminating the app, so the
  Device Log keeps streaming the running instance while the build
  streams in parallel — and the running app is flagged **stale**
  afterward.
- **failed** is terminal for the run: a **build** failure reads
  `EXPLOIT FAILED`, a **deploy** (install/launch) failure reads a
  distinct `DEPLOYMENT FAILED`, both with output in the Build Log.
- **aborted** results from Stop or an unexpected disconnect mid-run;
  Stop while `running` terminates the app back to `idle`.

## Happy paths

### 1. First run (cold start)

1. Launch ostrich → connection overlay (no saved connections yet).
2. Enter host/port/user, pick **ssh-agent** or **password**, click
   **Connect**. Optionally save the connection.
3. Overlay dismisses; the connection bar and docked area appear.
4. **Scan** the Mac for projects and pick one (or type its path);
   accept the prefilled scheme / config / bundle id (or edit them),
   and **save it as a named preset**. **Sweep** for targets and pick
   a device or simulator.
5. Press **Play**. Build Log streams `xcodebuild` output → install →
   launch.
6. The app launches on the target; the Device Log streams its output.

### 2. Returning user (warm / daily)

1. Launch → overlay with the **last-used connection pre-selected**.
2. Press **Enter / Connect**.
3. The **last-active preset is restored** for that connection, and
   the **last target is re-selected** if it is still in range.
4. Press **Play** → observe. (Everyday path is essentially
   *connect → Play*.)

### 3. Iterate loop (rebuild while running)

1. Edit source in Neovim (outside ostrich; ostrich never edits code).
2. In ostrich, press **Play** again.
3. **Terminate-first**: the running app is killed, then the rebuild
   streams into the Build Log, reinstall, relaunch. The Device Log
   goes briefly dark during the build and resumes on the new
   instance with a `NEW PAYLOAD` demarcation — so its output is
   never cross-poisoned between builds.
4. For a quick check *without losing the running app*, press
   **Build** (build-only) instead: it leaves the app running, so the
   **Device Log keeps streaming live while the Build Log streams the
   compile in parallel** — and ostrich flags the running app as
   **stale** until the next Play.

### 4. Build-only

1. Press **Build** to compile and surface errors without installing or
   launching (e.g. a quick "does it still build?").

### 5. Switch target or preset

1. Pick a different **target** from the swept list (a device or a
   simulator), or switch the active **preset**.
2. Press **Play**.

### 6. Recover from a drop

1. SSH link blips (Mac sleeps, Wi-Fi drops); the connection bar shows
   **reconnecting…**.
2. ostrich auto-retries with backoff; local state (active preset,
   layout) is preserved. Any in-flight run is treated as **aborted**.
3. On reconnect, streams resume; the user re-Plays if needed.

## Persistence model

ostrich persists, in local configuration:

- **Connections** — `label + host + port + user + auth-method`, and an
  **opt-in stored password** (plaintext, single-user local tool;
  ssh-agent connections store no secret).
- **Named run-config presets per connection** — each connection owns a
  set of named presets (the **four** fields each — project, scheme,
  config, bundle id) plus the **last-active** preset.
- **Last-used target per connection** — the most recently selected
  device/simulator, remembered **separately** from any preset (it is
  re-validated against a fresh sweep on connect, not blindly trusted).

The *intent* to persist is fixed here; the concrete on-disk format and
config-persistence mechanism remain design-deferred and belong in a
per-project ARD/impl.

## Deferred / open

Recorded so downstream docs can pick them up; intentionally **out of
MVP scope**:

- **Unified-system-log firehose.** The Device Log is the app's own
  process console (inherently app-scoped); the full-system firehose
  toggle is **dropped** for MVP and recorded as a future item (it
  would likely be simulator-only or need non-Xcode tooling on a
  device).
- **Build-error parsing** (structured errors / jump-to) — raw output
  only in MVP.
- **Keychain-backed password storage** (macOS Keychain / libsecret) as
  the hardened replacement for opt-in plaintext.
- **Advanced run-config inputs** — launch arguments, environment
  variables, extra `xcodebuild` flags.
- **Multiple named presets switching UX** richness beyond MVP basics.
- **Reattach to a running remote build** across a reconnect (currently
  an in-flight run aborts on drop).
- **Multi-Mac / multi-window** — explicitly out of scope per
  `design.md`.
- **Debugger and SSH port-forwarding** — future goals in `design.md`.

## Note on `design.md`

`design.md` was updated in the session that produced this document so
that core functional goal #1 reflects **two** authentication methods —
**ssh-agent** or **explicit user/host/port/password** — rather than
ssh-agent only.

## Note on the discovery revision

This document was revised when the **discovery** project (recon the
Mac for build inputs) promoted discovery from a post-MVP deferral to
a delivered capability. The revision records four product changes
resolved in that project's PRD/ARD:

1. The **target is removed from the persisted preset** — it is a
   build-time, session-sticky selection remembered separately and
   re-validated on connect, not one of the saved fields.
2. The separate **device/simulator "target type" switch is dropped**
   in favor of one unified target list, with the type inferred from
   the pick.
3. **Scheme, config, and bundle id are prefilled editable inputs**
   (with a discovered-set hint), while the **project is a dropdown**
   of scanned results plus manual entry.
4. The persisted run configuration is therefore **four** fields
   (project, scheme, config, bundle id), not six.

See `context/projects/discovery/prd.md` and `ard.md` for the full
flow and the mechanism behind it.

## Note on the xcode-project-build-and-deploy revision

This document was revised when the
**xcode-project-build-and-deploy** project (the Play/Observe core
loop) resolved how the build → install → launch chain and the two
logs actually behave. The revision records:

1. **The Live Feed source is the launched app's process console**
   (`devicectl … process launch --console` / `simctl launch
   --console`) — the app's own stdout/stderr, inherently app-scoped.
2. **Re-Play is terminate-first.** The running app is killed before
   a rebuild, so the Live Feed goes dark during the build (then
   resumes on the new instance with a `NEW PAYLOAD` demarcation),
   trading a moment of silence for clean, un-cross-poisoned per-build
   output. This **amends `design.md` #7**: the live feed does *not*
   stay live across a Play rebuild.
3. **The genuine simultaneous-streams concurrency** lives in the
   running phase and in a **build-only Build/COMPILE**, which leaves
   the running app alive (no redeploy) so the Live Feed keeps
   streaming while the build streams in parallel.
4. **A stale-build indicator** marks the running app as behind the
   latest build (e.g. after a build-only) until the next Play.
5. **The full-system firehose toggle is dropped**; the Live Feed
   keeps history with a run demarcation (not cleared) within a
   bounded buffer.
6. **The Build Log carries the whole chain's tooling output**
   (xcodebuild, then install/launch), not xcodebuild alone, and
   build vs deploy failures are distinct states.
7. **Build/COMPILE needs no target** (generic destination when none
   is locked); Play requires a target. A not-booted simulator is
   auto-booted (headless; observed via the Live Feed).

See `context/projects/xcode-project-build-and-deploy/prd.md` for
the full rationale.

## Note on the ui-layout revision

This document was revised when the **ui-layout** project reorganized
the ONLINE working area into a cleaner four-band layout. The revision
records:

1. The **Run Configuration form and Control / Status cluster** are
   merged into a single full-width **XCODE PROJECT CONFIGURATION**
   band directly below the connection bar, replacing the former
   side-by-side upper panels.
2. **Project, preset, and target** are now **dropdown pickers**:
   project keeps a hand-editable path field with a `[v]` picker
   popup as the manual escape hatch (honoring the manual-fallback
   value); scheme, config, and bundle id remain prefilled editable
   inputs.
3. **"Device Log"** is renamed to **"Live Feed"** throughout, to
   reflect its role as the streaming live output panel.
4. Each log panel shows the centered ostrich wordmark plus its
   empty-state caption (`// NO PAYLOAD COMPILED` for Build Log,
   `// NO SIGNAL — TARGET DARK` for Live Feed) when it has no
   content.
5. The vertical **Build Log / Live Feed divider is user-resizable**
   (default 50/50, session-only) — a deliberate soft exception to
   the "no user rearrange in MVP" rule; all other panel boundaries
   remain locked.
