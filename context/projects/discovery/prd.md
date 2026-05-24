# PRD — Discovery (Recon the Mac for build inputs)

## Problem Statement

ostrich can now connect to a Mac and keep the link alive, but the
moment I want to actually build something I am back to typing. To
drive `xcodebuild` I must know — and hand-enter — the exact path to
my `.xcodeproj`/`.xcworkspace` on the Mac, the precise scheme name,
the build configuration, my app's bundle identifier, and the UDID of
the device or simulator I want to run on. These live on the Mac, not
in my head: the project path is wherever the repo sits on that
machine, scheme and bundle id come straight out of the project, and a
device UDID is an opaque string that changes per device and only
exists while the device is plugged in.

As the operator I live in Linux/Neovim and treat the Mac as a
headless appliance, so I have no Finder, no Xcode scheme dropdown, no
"Devices and Simulators" window to read these values off of. Today
that forces me back to the macOS GUI just to *look something up* —
exactly the context switch ostrich exists to remove. Hand-typing a
UDID or a deep absolute path is also error-prone, and a single wrong
character produces a confusing `xcodebuild` failure later rather than
an obvious "you picked the wrong thing" now.

There is also nothing to build a run against. ostrich has no run
configuration surface at all: no place to hold the project, scheme,
config, and bundle id, no notion of saving a setup so I do not
reassemble it every day, and no way to choose which device I am
targeting. The connection project deliberately stopped before any of
this. So between "connected" and "press Play" there is a gap: I
cannot tell ostrich *what* to build, *how* to build it, or *where* to
run it — and I cannot do it without leaving for the Mac's GUI.

## Solution

ostrich gains **recon**: once breached, it can interrogate the Mac
for everything a build needs and let me assemble a run configuration
from selectable, discovered inputs instead of memorized strings.

I point ostrich at a directory on the Mac (defaulting to my home,
remembered per connection) and trigger **SCAN HOST**. Off the UI
thread and cancelable, ostrich sweeps that root for buildable Xcode
projects and presents a **curated** list of **BLUEPRINTS RECOVERED** —
the noise pruned out (the throwaway `project.xcworkspace` inside every
`.xcodeproj`, plus `Pods/`, `Carthage/`, `.build/`, `DerivedData`,
`node_modules`), recursion depth-capped, and the `.xcworkspace`
surfaced as the build target when a workspace and project sit
together (because that is the one that actually builds). I pick a
project from this list — or, if discovery never surfaces it, I type
the path myself; manual entry is always open.

Picking a project reads its blueprint. ostrich runs
`xcodebuild -list` against that specific project and **prefills** the
**scheme**, **build configuration** (defaulting to `Debug`), and a
best-effort **bundle id** (resolved from the chosen scheme/config's
build settings). These are editable input fields, not locked
dropdowns: ostrich fills in its best guess and shows the full
discovered set beside the field as a non-blocking hint
(`> discovered: MyApp, MyApp-Staging, …`), so I can accept the
prefill, type a different value, or override entirely. The run
configuration that results — project, scheme, config, bundle id — is
saved as a **named preset bound to the connection**, with full
new / rename / delete / choose, and the last-active preset is
restored automatically next time so the daily path needs no re-scan.

The **target** is treated differently, because devices come and go.
It is **not** part of the saved preset. Instead a target selector
sits with the run controls: I **SWEEP FOR TARGETS** and ostrich lists,
in one unified set, the physical devices and simulators currently in
range (`devicectl` and `simctl`, queried concurrently). I pick one;
ostrich infers device-vs-simulator from the choice, and the selection
sticks for the session. My last target is remembered (separately from
any preset) and silently re-selected on connect when it is still in
range, so a warm start lands on a ready config without my re-picking;
if that device is gone, ostrich just shows `// NO TARGETS IN RANGE`
and I sweep again.

All of this is best-effort and forgiving. Every discovery call runs
off the UI thread so the window never freezes, independent queries run
in parallel over the existing multi-channel session (no second
connection), and a slow scan can be aborted. When the Xcode CLI is
absent, a command fails, its JSON does not parse (a different Xcode
version), or a list comes back empty, ostrich says so in distinct,
plain-language, themed terms and falls back to manual entry — recon
never blocks me from configuring a run by hand.

This project stops cleanly at **READY**: when a preset is complete
(project, scheme, config, bundle id) and a target is selected, ostrich
shows the configuration is ready for the Play project to consume. It
builds no `build → install → launch` chain, no run-state machine, and
no EXECUTE/COMPILE action — only the discovered, selectable,
persistable run configuration that Play will later act on.

## User Stories

1. As the operator, I want to scan the Mac for my Xcode projects, so
   that I do not have to remember and type a deep absolute path.

2. As the operator, I want to point the scan at a root directory
   (defaulting to my home), so that I control where ostrich looks
   instead of it crawling the whole disk.

3. As the operator, I want my scan root remembered per connection, so
   that I do not re-enter it every time I want to look for projects.

4. As the operator, I want discovery to run only when I ask
   (SCAN HOST / SWEEP FOR TARGETS), so that ostrich never reaches into
   the Mac on its own — consistent with how it never auto-connects.

5. As the operator, I want the scanned project list curated to my
   real buildable projects, so that internal `project.xcworkspace`
   files, `Pods/`, `Carthage/`, `.build/`, `DerivedData`, and
   `node_modules` do not bury the list in noise.

6. As the operator, I want recursion depth-capped during the scan, so
   that a deep tree does not make the scan crawl forever over SSH.

7. As the operator, when a project has both a `.xcworkspace` and a
   `.xcodeproj`, I want the workspace surfaced as the build target, so
   that I pick the one that actually builds (pods/SPM live there).

8. As the operator, I want to pick my project from the
   BLUEPRINTS RECOVERED list, so that selecting it is a click, not a
   typed path.

9. As the operator, when discovery does not surface a project, I want
   to type the project/workspace path manually, so that an unscanned
   or unusual location never blocks me.

10. As the operator, I want a themed empty state (`// NO BLUEPRINTS`)
    when a scan finds nothing, so that the result reads intentionally
    rather than as a blank panel.

11. As the operator, I want a long scan to be cancelable
    (`■ ABORT SCAN`), so that pointing at a huge tree by mistake never
    strands me.

12. As the operator, once I select a project I want ostrich to read
    its blueprint automatically, so that scheme, config, and bundle id
    fill in without a second action.

13. As the operator, I want the scheme prefilled with a best-guess
    primary, so that the common single-scheme case needs no typing.

14. As the operator, I want the full discovered scheme set shown as a
    non-blocking hint beside the field, so that I know what to type
    when I want a scheme other than the prefill.

15. As the operator, I want the scheme field to stay a free text
    input (not a locked dropdown), so that I can always override it
    with exactly the value I want.

16. As the operator, I want the build configuration prefilled
    (defaulting to `Debug`) with the discovered configs shown as a
    hint, so that the usual case is one keystroke and the alternatives
    are visible.

17. As the operator, I want a best-effort bundle id resolved from the
    chosen scheme/config's build settings and prefilled, so that I do
    not have to look it up in the project.

18. As the operator, I want the bundle id to remain an editable input,
    so that I can correct or override ostrich's best guess.

19. As the operator, I want every configuration field to accept manual
    entry as a fallback, so that a discovery miss or failure never
    traps me — I can always type the value and proceed.

20. As the operator, I want my project chosen via a dropdown/select
    while scheme, config, and bundle id are prefilled inputs, so that
    the surface matches how each value behaves (a finite scanned set
    vs. a prefilled-but-editable string).

21. As the operator, I want to save the assembled configuration as a
    named preset, so that I do not reassemble project/scheme/config/
    bundle id every day.

22. As the operator, I want presets bound to the connection, so that
    each Mac carries its own set of configurations.

23. As the operator, I want to create, rename, delete, and choose
    presets, so that I can keep several configurations (e.g. app vs.
    staging) without retyping.

24. As the operator, I want my last-active preset restored on
    reconnect/relaunch, so that the daily path is connect → ready
    with no re-scan.

25. As the operator, I want a themed empty state
    (`// NO OPERATION CONFIGURED`) when no preset exists yet, so that
    first use reads intentionally.

26. As the operator, I want to sweep the Mac for available targets, so
    that I can pick a device or simulator instead of typing a UDID.

27. As the operator, I want physical devices and simulators in one
    unified target list, so that I choose where to run in a single
    place.

28. As the operator, I want each target labeled (device vs.
    simulator, and booted state for sims), so that I can tell them
    apart at a glance.

29. As the operator, I want ostrich to infer device-vs-simulator
    (`devicectl` vs. `simctl`) from my pick, so that I never set a
    separate "target type" by hand.

30. As the operator, I want the target chosen at build time and kept
    out of the saved preset, so that a configuration does not rot when
    a device is unplugged.

31. As the operator, I want my selected target to stick for the
    session, so that I do not re-pick it on every action within a
    session.

32. As the operator, I want my last target remembered separately and
    re-selected on connect when it is still in range, so that a warm
    start reaches a ready configuration without a manual pick.

33. As the operator, when my remembered target is gone, I want a clear
    `// NO TARGETS IN RANGE` (or unselected) state, so that I know to
    sweep and pick again rather than building against nothing.

34. As the operator, I want to re-sweep targets on demand, so that a
    device I just plugged in (or a simulator I just booted) shows up
    without reconnecting.

35. As the operator, I want devices and simulators queried
    concurrently, so that a sweep returns quickly rather than serially.

36. As the operator, I want all discovery to run off the UI thread, so
    that the window never freezes while ostrich talks to the Mac.

37. As the operator, I want discovery to reuse the existing
    multi-channel session, so that recon opens no second connection
    and adds no new login.

38. As the operator, when the Xcode command-line tools are missing, I
    want a distinct themed message (e.g. `> XCODE NOT FOUND`), so that
    I fix the Mac's setup instead of guessing why nothing lists.

39. As the operator, when a discovery command fails, I want a clear
    themed reason rather than a silent empty result, so that I can
    tell failure from genuinely-empty.

40. As the operator, when discovery output does not parse (a different
    Xcode version), I want ostrich to say so and fall back to manual
    entry, so that version drift degrades gracefully instead of
    crashing or showing garbage.

41. As the operator, I want empty discovery results to render as
    themed empty states rather than errors, so that "no devices
    plugged in" reads as a normal state.

42. As the operator, I want discovery failures to never block me from
    configuring a run by hand, so that a flaky CLI does not stop me
    from working.

43. As the operator, I want a READY indicator when my preset is
    complete and a target is selected, so that I know the
    configuration is good before the Play project ever exists.

44. As the operator, I want themed hints for what is missing
    (`// NO OPERATION CONFIGURED`, no target selected), so that I can
    see exactly what stands between me and READY.

45. As the operator, I want the recon surfaces in ostrich's voice and
    lexicon (SCAN HOST, BLUEPRINTS RECOVERED, SWEEP FOR TARGETS,
    TARGETS IN RANGE), sourced from the centralized strings table, so
    that they stay on-theme and a future straight-mode is a no-UI
    swap.

46. As the operator, I want recon to honor the palette discipline
    (decorative cyan/magenta for chrome, semantic green/red/amber only
    for meaning), so that a real discovery failure still reads
    unmistakably.

47. As the operator, I want recon to stay zero-cost whimsy — a scan
    dwells only for its real duration, with no fake "scanning…" delay
    and no gate — so that discovery never slows the loop.

48. As the operator, I want the recon surfaces keyboard-drivable, so
    that I stay in the keyboard consistent with my Neovim workflow.

49. As the operator, I want discovered values to never be silently
    "corrected" by ostrich after I edit them, so that my manual
    overrides stick.

50. As the developer, I want the discovery JSON parsing tolerant of
    fields it does not recognize, so that a newer/older Xcode does not
    break the parse over an additive change.

51. As the developer, I want `make test` to stay meaningful and green
    for discovery — state-based tests over the list-shaping/curation,
    the JSON parsing, the configuration/preset serialize-deserialize,
    and the readiness logic — with real-Mac calls gated behind host
    availability (SKIP when absent), so that the gate stays
    trustworthy.

52. As the developer, I want discovery to build and run on both Linux
    and macOS hosts, so that both supported platforms work.

53. As the developer, I want discovery to consume the connection's
    multi-channel, off-thread session model rather than re-architect
    it, so that recon is the first real payoff of that design.

## Out of Scope

Deferred to later projects or fixed as non-goals:

- **Play / Build orchestration and the run-state machine.** The
  `build → install → launch` chain, EXECUTE/COMPILE actions, run-state
  labels (`COMPILING EXPLOIT…`, etc.), and abort are the next project.
  Discovery stops at READY: a validated, target-selected configuration
  for Play to consume.

- **The `.app` output-path resolution for install.** Resolving the
  built product path is a Play-time concern (and not a user-facing
  field). Discovery resolves the bundle id, not the artifact path.

- **Whole-disk project crawl.** The scan is rooted at a directory the
  operator points at; ostrich never crawls the entire filesystem.

- **Importing Xcode's own recent-projects / workspace state.**
  Discovery scans the filesystem; it does not read Xcode's internal
  recents.

- **Auto-discovery on connect.** Recon is on-demand. The lone
  exception is a single lightweight *target* re-validation sweep on
  connect (see Further Notes); project scanning and blueprint reading
  never run unprompted.

- **Live device hotplug / continuous target refresh.** Targets update
  when the operator sweeps; ostrich does not watch for devices being
  plugged or simulators booting in the background.

- **Scheme/config/bundle-id as locked dropdowns.** These stay
  prefilled, editable inputs with a discovered-set hint; only the
  project (a finite scanned set) and the target are true selectors.

- **Advanced run-config inputs** — launch arguments, environment
  variables, extra `xcodebuild` flags — remain deferred per
  `workflow.md`.

- **Test plans, multiple targets within a scheme, and extension /
  watch / widget sub-apps.** Discovery resolves a single app build
  configuration; richer multi-target handling is deferred.

- **The persistence mechanism.** *That* presets and the remembered
  target persist is required here; the concrete on-disk format and
  store implementation are ARD decisions (and should reuse the
  connection project's config store).

- **Strict Xcode version pinning / a version-mismatch gate.**
  Discovery degrades gracefully to manual on unparseable output rather
  than hard-blocking on an unrecognized Xcode version.

- **The threading / arena / module mechanism.** The PRD fixes the
  *behaviors* (off-thread, cancelable, concurrent, never-block); the
  worker model, arenas, parser placement, and library boundaries are
  the ARD's.

## Further Notes

- **Traceability.** This project realizes `design.md` core goal #2
  (discover schemes, build configurations, destinations, connected
  physical devices, and simulators by querying the Mac, and present
  them as selectable inputs) and promotes the discovery that
  `workflow.md` and the connection PRD repeatedly marked "post-MVP"
  into a real project. It builds the **Run Configuration** container
  from `workflow.md` (now discovery-fed) and its **named-preset
  persistence model**, and serves happy paths 1 (cold start, now with
  recon instead of typing), 2 (warm/daily, last-active preset +
  re-validated target → ready), and 5 (switch target/preset). It
  traces to `theme.md` for voice, palette discipline, zero-cost
  whimsy, and the centralized lexicon, and depends directly on the
  connection project's multi-channel, off-thread, kept-alive session.

- **Deliberate refinements to `workflow.md` (to be reconciled
  upstream).** `workflow.md` should be updated to match the model
  resolved here, the way the connection project amended upstream docs:
  (1) the **target is removed from the persisted preset** — it is a
  build-time, session-sticky selection remembered separately, not one
  of the saved fields; (2) the separate **"target type" device/sim
  radio is dropped** in favor of one unified target list with the type
  inferred from the pick; (3) **scheme, config, and bundle id are
  prefilled editable inputs** (with a discovered-set hint), not
  dropdowns, while the **project is a dropdown** plus manual entry;
  (4) the persisted run configuration is therefore **four** fields
  (project, scheme, config, bundle id), not six. These change product
  flow/structure (`workflow.md`'s domain), so they are recorded here
  and should be folded back into `workflow.md`.

- **New camp copy (to be added to `theme.md`).** Discovery needs
  lexicon the theme does not yet name; like the connection project, it
  should be added to `theme.md`'s canonical lexicon as deliberate
  entries under a **Discovery / recon** group: the scan action
  `⌖ SCAN HOST` (with `■ ABORT SCAN` while scanning), the project
  results header `BLUEPRINTS RECOVERED`, the target action
  `↻ SWEEP FOR TARGETS`, the target results header `TARGETS IN RANGE`,
  the empty states `// NO BLUEPRINTS` and `// NO TARGETS IN RANGE`,
  and the recon failure lines in the `>` voice (`XCODE NOT FOUND`,
  `COULD NOT READ INVENTORY` for a parse/command failure). No
  structural change is implied; the strings table simply gains these
  keys.

- **Decisions resolved while scoping this PRD.** (1) Scope is the full
  discovery — project/workspace *and* schemes, configs, devices,
  simulators — with the project found by scanning a pointed-at root,
  not a whole-disk crawl. (2) Schemes, configs, and bundle id are
  project-gated (read from the chosen project); devices and simulators
  are Mac-global and listable independently. (3) This project delivers
  the run-configuration surface itself plus full per-connection
  named-preset CRUD and last-active restore. (4) Discovery is
  on-demand with inputs remembered. (5) The project list is curated
  (noise pruned, depth-capped, workspace preferred) with manual path
  entry always available. (6) Every field has a manual fallback;
  project is a select, scheme/config/bundle id are prefilled inputs
  with a hint. (7) The target is decoupled from the preset: a unified
  device+sim selector, sticky for the session, type inferred,
  remembered-and-re-validated across connects. (8) Discovery runs
  off-thread, concurrently, and cancelably over the existing session.
  (9) Failure handling is best-effort with distinct themed reasons and
  graceful degrade to manual. (10) The project stops at READY and
  builds no Play orchestration.

- **Resolved tension — the one auto sweep.** "On-demand, never
  auto-discover" and "remember + re-validate the last target on a warm
  start" pull against each other. The resolution: on connect, ostrich
  runs a **single, lightweight target sweep** for the sole purpose of
  re-validating the remembered target (targets are the cheap,
  Mac-global query). This is the *only* unprompted discovery; project
  scanning and blueprint reading remain strictly on-demand. If even
  this one sweep is later judged to violate the no-auto ethos, the
  fallback is to defer re-validation to the operator's first manual
  sweep — at the cost of one extra step on the warm path.

- **Bundle id is best-effort.** It is resolved from the chosen
  scheme/config's primary app target build settings
  (`PRODUCT_BUNDLE_IDENTIFIER`). Projects with multiple targets or
  unusual settings may resolve ambiguously; ostrich prefills its best
  guess, shows it as editable, and the operator's override wins. The
  field is never blocked on a confident resolution.

- **Two scans, two meanings.** "Scan" here is the **filesystem
  project scan** on the Mac (find buildable projects under a root).
  It is unrelated to the connection project's host-key handling or any
  `known_hosts` notion; do not conflate recon with the SSH layer.

- **Architectural seams for the ARD (flagged, not designed).** A
  **discovery library behind a pure-C header** that issues the Xcode
  CLI commands over the session and parses their JSON; a tolerant
  **JSON parsing** approach resilient to additive Xcode-version
  changes; **list curation** logic (prune/depth/workspace-preference)
  that is unit-testable without a Mac; the **run-configuration +
  preset store**, reusing the connection project's config store and
  adding per-connection presets plus the separately-remembered target;
  the **off-thread, concurrent discovery jobs** over multiple exec
  channels with cross-thread handoff into UI memory (the pattern the
  connection project established); and the **readiness** computation.
  Arenas (e.g. a per-scan arena, a per-session discovery cache) are
  named in the ARD.

- **Keeping `make test` meaningful.** Real discovery needs a live Mac
  with Xcode, so the gate stays trustworthy by black-box testing the
  display-free, network-free parts — project-list curation, JSON
  parsing against captured fixtures (including a drifted/odd sample),
  configuration/preset serialize-deserialize, the
  remembered-target re-validation logic, and the readiness rule — and
  gating any real-Mac call behind host availability (printing SKIP
  when absent), mirroring the connection/app-shell pattern.

- **Whimsy stays zero-cost.** SCAN HOST and SWEEP FOR TARGETS dwell
  only for the real command duration; there are no artificial
  "scanning…" pauses and no gates. Recon copy lives only on ostrich's
  own surfaces in the `>`/magenta voice and never recolors or
  fabricates real Xcode output.
