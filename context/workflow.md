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
  is live at a time. There is no multi-Mac, multi-project, or
  multi-window workspace.
- **Phases are states, not screens.** The Connect → Configure → Play →
  Observe phases from `design.md` are *states of persistent panels*,
  not steps in a wizard. The user lives in one docked console rather
  than advancing through pages.
- **Concurrency means parallel streams, not parallel targets.** The
  one thing that genuinely runs in parallel is *output*: the Device
  Log keeps streaming continuously — including across rebuilds — while
  the Build Log streams in parallel. This is the core payoff and the
  reason the log panels are never torn down.

## Layout

ostrich is a **single window** built on the ImGui docking branch. All
containers are **locked to their docked locations** (no free-floating
or user-rearranged panels in MVP).

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
  +---------------------------+------------------+
  | Run Configuration         | Control / Status |
  |  preset: [ my-app  v] +/- |    > Play        |
  |  workspace ____           |    > Build        |
  |  scheme    ____           |  state: idle     |
  |  config    ____           |  build>inst>launch|
  |  target    (dev)(sim)     |                  |
  |  UDID      ____           |                  |
  |  bundle id ____           |                  |
  +---------------------------+------------------+
  | Build Log         | Device Log               |
  |  (xcodebuild...)  |  (device log, live       |
  |                   |   across rebuilds)       |
  |  ...fills vert... |  ...fills vert...        |
  +----------------------------------------------+
  | ostrich  *  60 FPS                           |  <- slim footer
  +----------------------------------------------+
```

Containers, top to bottom:

- **Connection bar** (thin, full width, top) — connection identity and
  live status, plus controls to update or close the connection.
- **Run Configuration** (upper-left) — preset selector and the run
  configuration form.
- **Control / Status strip** (upper-right, beside Run Configuration) —
  the Play / Build / Stop actions and the run-state indicator.
- **Build Log** (lower-left) and **Device Log** (lower-right) — split
  the full width below and **fill the vertical space** between the
  configuration row and the footer.
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

### Run Configuration

A **preset selector** sits at the top (choose / new / rename /
delete). Run configurations are **named presets bound to a
connection**; the active preset drives Play. Below it, the **six
essential fields**:

1. **Project/workspace path** — absolute path on the Mac
   (`.xcodeproj` vs `.xcworkspace` inferred from the extension).
2. **Scheme**
3. **Build configuration** (default `Debug`)
4. **Target type** — physical **device** vs **simulator** (switches
   install/launch between `devicectl` and `simctl`).
5. **Target UDID** — typed by the user (no discovery in MVP).
6. **Bundle ID** — used for install/launch.

Discovery (auto-scanning the Mac for projects, schemes, devices, and
simulators) is **post-MVP**: it would *populate* these fields rather
than replace the form. The `.app` output path needed for install is
resolved by ostrich (e.g. from build settings) and is **not** a
user-facing field. Advanced inputs (launch arguments, environment
variables, extra `xcodebuild` flags) are deferred.

### Control / Status strip

- **Play** — runs the full chain `build → install → launch`. While a
  run is in progress, Play becomes **Stop** (abort).
- **Build** — build-only; compiles without installing or launching
  (also abortable).
- **Run-state indicator** — shows the current phase (see below) and a
  `build ▷ install ▷ launch` progression.

### Build Log

Raw `xcodebuild` output (no error parsing in MVP). **Cleared at the
start of each build.** Auto-scrolls to the bottom, **pausing when the
user scrolls up**. Provides clear/copy affordances.

### Device Log

Live device log that **stays streaming continuously, including across
rebuilds**. Defaults to **filtered to the launched app's process**,
with a **toggle to the full system firehose**. (The *filtering
mechanism* is design-deferred; only the UX intent is fixed here.)
Auto-scroll behaves like the Build Log.

### Footer

A slim strip for ostrich's own diagnostics (FPS, app-level status) —
distinct from build/device output.

## Run-state machine

```
idle ──Play──> building ──> installing ──> launching ──> running
  ^                                                         |
  |                                                  re-Play (rebuild loop)
  |                                                         v
  +<── failed (build error; errors shown in Build Log)   building ...
  +<── aborted (Stop pressed, or SSH drop mid-run)
```

- **Play** from `idle` (or while `running`) drives the full chain.
- **re-Play while running** terminates the current app instance and
  starts a fresh `build → install → launch`. The **Device Log stays
  live across this** while the new build streams into the Build Log —
  the core concurrency behavior.
- **Build** runs `idle → building → idle` without install/launch.
- **failed** is terminal for the run; the Build Log holds the output.
- **aborted** results from Stop or an unexpected disconnect mid-run.

## Happy paths

### 1. First run (cold start)

1. Launch ostrich → connection overlay (no saved connections yet).
2. Enter host/port/user, pick **ssh-agent** or **password**, click
   **Connect**. Optionally save the connection.
3. Overlay dismisses; the connection bar and docked area appear.
4. In Run Configuration, **create a named preset** and fill the six
   fields.
5. Press **Play**. Build Log streams `xcodebuild` output → install →
   launch.
6. The app launches on the target; the Device Log streams its output.

### 2. Returning user (warm / daily)

1. Launch → overlay with the **last-used connection pre-selected**.
2. Press **Enter / Connect**.
3. The **last-active preset is restored** for that connection.
4. Press **Play** → observe. (Everyday path is essentially
   *connect → Play*.)

### 3. Iterate loop (rebuild while running)

1. Edit source in Neovim (outside ostrich; ostrich never edits code).
2. In ostrich, press **Play** again.
3. The running app is terminated; rebuild streams into the Build Log;
   reinstall; relaunch.
4. The **Device Log stays live throughout** — no stream is torn down.

### 4. Build-only

1. Press **Build** to compile and surface errors without installing or
   launching (e.g. a quick "does it still build?").

### 5. Switch target or preset

1. Change the active preset, or edit a field (e.g. flip **target
   type** device → simulator and set the simulator UDID).
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
  set of named presets (the six fields each) plus the **last-active**
  preset.

The *intent* to persist is fixed here; the concrete on-disk format and
config-persistence mechanism remain design-deferred and belong in a
per-project ARD/impl.

## Deferred / open

Recorded so downstream docs can pick them up; intentionally **out of
MVP scope**:

- **Discovery / auto-scan** of schemes, build configs, devices, and
  simulators to populate the form (relies on the JSON-emitting Xcode
  subcommands). MVP uses the manual form.
- **Device-log filtering mechanism** (how the app-process filter is
  implemented; firehose vs filtered) — `design.md`-deferred.
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
