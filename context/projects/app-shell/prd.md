# PRD — Application Shell (GUI Entry Point)

## Problem Statement

Today, launching ostrich does nothing useful. `make` produces a
console binary that prints `ostrich: ready.` and exits 0 — proof
that the toolchain compiles and links, and nothing more. There is
no window, no themed surface, and nowhere for the real features
(connecting to a Mac, configuring a run, watching build and device
logs) to live.

As the single developer this tool is built for, I cannot yet *see*
ostrich. I have a build pipeline but not an application. The design
goals describe a lightweight, fast, native desktop window — a
single docked console with a dark cyberpunk identity — but none of
that exists. Every downstream feature assumes that window is
already there to dock into, so until it exists I am blocked from
building any of them.

I need ostrich to actually open as the application the design
describes: one native window, wearing ostrich's look and feel,
running smoothly, that I can launch and quit cleanly — the
foundation everything else is built on.

## Solution

Replace the console hello-world entry point with the real ostrich
application shell. When I run `./build/ostrich`, a single native
window opens immediately, already wearing ostrich's dark cyberpunk
identity, and runs a smooth render loop until I close it.

This first increment is deliberately a *shell*: it stands up the
window, the rendering, the theme, and a clean lifecycle — but no
working panels yet. Concretely, when I launch ostrich I get:

- A single resizable native window titled `ostrich`, built on
  GLFW with an OpenGL 3 context and a Dear ImGui docking context,
  ready for future panels to dock into.
- ostrich's full visual identity from the very first frame: the
  dark cyberpunk background, the JetBrains Mono typeface
  everywhere, the palette discipline (decorative cyan/magenta
  chrome vs. reserved semantic colors), and the faint static
  scanline + vignette overlay behind the chrome.
- A resting view in the empty docking host: the static ASCII
  ostrich wordmark and identity (`OSTRICH // infiltration
  console`) centered in the dark — the brand's recognizable face
  while there is nothing else to show.
- A slim footer reading `ostrich // 60 FPS // ONLINE` — ostrich's
  own diagnostics (live frame rate and an app-alive indicator),
  distinct from any future tool output.
- A clean exit: I can quit by closing the window or with a
  keyboard shortcut, and ostrich tears down ImGui, the GL context,
  and GLFW without crashing or leaking.

The window paints instantly with no boot animation or splash to
click through, runs at roughly 60 FPS without pegging the CPU, and
builds and runs on both Linux and macOS. To make a real window
compile, this project also initializes the vendored `imgui` and
`glfw` submodules and wires them into the plain Make build, and
vendors the JetBrains Mono fonts the theme requires. The libssh2
layer stays deferred.

## User Stories

1. As a developer, I want `./build/ostrich` to open a native
   desktop window, so that ostrich is finally an application I can
   see rather than a console stub.

2. As a developer, I want that window to be a single window titled
   `ostrich`, so that it matches the single-window console model
   the design fixes.

3. As a developer, I want the window to be resizable and to behave
   like a normal native window (minimize, maximize, move via the
   OS), so that it fits naturally into my desktop environment.

4. As a developer, I want the window built on GLFW with an OpenGL
   3 context, so that it conforms to the fixed technical
   constraints and renders efficiently.

5. As a developer, I want a Dear ImGui context with the docking
   branch enabled, so that the window is a docking host that later
   panels can be docked into without re-architecting the shell.

6. As a developer, I want the empty docking host laid out so
   future panels (connection bar, run configuration, control /
   status, build log, device log, footer) can be added by docking
   in, so that the workflow layout drops onto this shell cleanly
   later.

7. As a developer, I want the window to paint instantly on launch
   with no boot animation, splash, or gate, so that the app is
   usable the moment it appears.

8. As a developer, I want ostrich to render at roughly 60 FPS, so
   that the UI feels smooth and responsive.

9. As a developer, I want the render loop to be lightweight and
   not peg a CPU core while idle, so that ostrich stays true to
   its "lightweight and fast" promise.

10. As a developer, I want rendering free of tearing and flicker,
    so that the window looks crisp and stable while I use it.

11. As the operator, I want the window to wear ostrich's dark
    cyberpunk identity from the first frame, so that the tool feels
    like ostrich immediately rather than a generic ImGui window.

12. As a developer, I want the dark near-black background applied
    as the base, so that the cyberpunk neon skin reads correctly.

13. As a developer, I want the palette discipline applied —
    decorative cyan/magenta reserved for chrome and the semantic
    green/red/amber reserved for meaning — so that the visual
    language is consistent from the start, even before there are
    states to color.

14. As a developer, I want JetBrains Mono (regular and bold) baked
    into the ImGui font atlas and used everywhere, so that ostrich
    has its unified terminal/console feel.

15. As a developer, I want a faint, static scanline and vignette
    overlay behind the chrome, so that the cyberpunk atmosphere is
    present without any motion that could distract.

16. As the operator, I want the static ASCII ostrich wordmark and
    `OSTRICH // infiltration console` identity centered in the
    otherwise empty window, so that the resting shell has a
    recognizable face.

17. As a developer, I want the wordmark to be the single bright
    element in the resting view, so that the "brightness =
    attention" discipline holds even in an empty shell.

18. As a developer, I want a slim footer showing
    `ostrich // 60 FPS // ONLINE`, so that I can see ostrich's own
    diagnostics at a glance.

19. As a developer, I want the footer's frame-rate readout to
    update live, so that the 60-FPS goal is something I can
    actually observe.

20. As a developer, I want the footer to read as ostrich's own
    voice and diagnostics, visually distinct from future build and
    device output, so that I never confuse app status with tool
    output.

21. As a developer, I want any camp copy this shell shows (the
    footer and the identity text) sourced from a single centralized
    lexicon rather than scattered string literals, so that a future
    straight-mode is a no-UI swap.

22. As a developer, I want to quit ostrich by closing the window,
    so that it behaves like any native app.

23. As a developer, I want a keyboard shortcut to quit (e.g. Esc
    or Ctrl-Q), so that I can stay in the keyboard, consistent with
    my Neovim-centric workflow.

24. As a developer, I want quitting to tear down ImGui, the GL
    context, and GLFW cleanly, so that ostrich exits without
    crashing, hanging, or leaking resources.

25. As a developer, I want the resting view (wordmark and footer)
    to stay correctly placed when I resize the window, so that the
    shell looks intentional at any size.

26. As a developer, I want the shell to be legible on a HiDPI
    display, so that ostrich is usable on a modern high-resolution
    monitor.

27. As a developer, I want the shell to do nothing external on
    launch — no network, no reaching for a Mac — so that opening
    ostrich is always safe and instant.

28. As a developer, I want `make` to build the windowed app with
    the `imgui` and `glfw` submodules compiled and linked in, so
    that a real GUI binary is produced by the existing build
    surface.

29. As a developer, I want the JetBrains Mono fonts vendored into
    the repository, so that the themed build is self-contained and
    reproducible.

30. As a developer, I want the build to honor `CC` / `CFLAGS`
    overrides and stay warning-clean, so that it remains portable
    across Linux and macOS.

31. As a developer, I want ostrich to build and run on both Linux
    and macOS, so that it works on both supported host platforms.

32. As a developer, I want `make test` to remain meaningful for
    the new GUI entry point, so that the project's automated check
    (and the RALPH loop's gate) stays green and trustworthy even
    though ostrich is no longer a console program.

## Out of Scope

This project is the shell only. The following are explicitly out
of scope and belong to later projects:

- **All functional panels.** The connection bar, run configuration
  form, control / status strip, build log, and device log are not
  built here — only the empty docking host they will later occupy.

- **The connection overlay / launch screen.** The modal `BREACH`
  form, `KNOWN HOSTS`, and auth inputs are deferred; the resting
  shell shows only the wordmark, not the overlay.

- **All SSH connectivity.** libssh2 is not integrated and no
  connection, handshake, or auth happens. The `ONLINE` in the
  footer means "ostrich is running," not a live SSH link.

- **The core build/run/observe features.** Discovery, run
  configuration, the build → install → launch orchestration, log
  streaming, and the concurrent worker model are all later work.

- **Persistence.** No window size/position is remembered across
  launches, and no connections or presets are stored.

- **Run-state machine and its labels.** The themed run-state
  copy (`COMPILING EXPLOIT…`, `TARGET ACQUIRED // LIVE`, etc.) and
  the state transitions arrive with the control/status work.

- **A populated lexicon and straight-mode UI.** Only the minimal
  copy this shell shows is introduced; the theme stays fixed with
  no runtime theme picker.

- **Motion/FX beyond the static overlay.** Granted/denied
  flashes, pulses on `* ONLINE` / `LIVE`, and the blinking cursor
  belong to surfaces that do not exist yet; only the static
  scanline/vignette and glow look are in scope here.

- **Audio and bitmap/SVG icon assets** — out of scope per the
  theme document.

- **Multi-window / multi-Mac, the debugger, and SSH
  port-forwarding** — out of scope or future per the design goals.

## Further Notes

- **Traceability.** This shell traces to `context/design.md`
  (GUI: Dear ImGui + GLFW, OpenGL 3, docking branch; lightweight;
  single user; Linux + macOS), to `context/workflow.md` (single
  locked-dock window; the footer is ostrich's own diagnostics, not
  tool output), and to `context/theme.md` (palette discipline,
  JetBrains Mono, static scanline/vignette, the ASCII wordmark,
  and the footer copy).

- **Theme governance.** The footer and identity copy are
  theme-owned wording and are recorded as deliberate. This project
  introduces no structural change — it adds no panels and alters no
  flow — consistent with the theme document's authority limits.
  The centralized strings table is set up here so future copy and a
  possible straight-mode require no UI rework.

- **Whimsy is zero-cost.** Per the theme's first principle, the
  window paints instantly with no fake delay or gate; the wordmark
  is a static paint, not an animation. Per the second principle,
  the scanline/vignette is tuned faint and would be cut if it ever
  hindered the core loop (no logs exist yet, so it is chrome-only
  for now).

- **Submodules and fonts.** Initializing `imgui` and `glfw` and
  wiring them into plain Make is part of this project — the
  bootstrap deferred it, and a real window cannot compile without
  it. `libssh2` stays uninitialized. JetBrains Mono (regular +
  bold, OFL) is vendored so the themed build is self-contained.

- **C / C++ seam (for the ARD).** Dear ImGui is C++, so per the
  coding standards it must be sealed inside a library whose
  internals are `.cpp` but whose public header is pure C, with
  `src/main.c` / the app layer consuming it through that C header.
  The exact module/library boundary is an ARD decision; this PRD
  only flags that the seam exists.

- **`make test` must be reworked (important).** The current smoke
  test launches `build/ostrich`, reads its stdout, and asserts the
  `ostrich: ready.` greeting and a clean exit. A windowed app runs
  an event loop and emits no such greeting, so that test no longer
  applies. The ARD / implementation plan must define how the shell
  stays verifiable through `make test` — for example a
  headless/offscreen smoke run, a small `--smoke`-style check that
  initializes and tears down without opening a visible window, or
  factoring init/teardown behind a testable seam. Keeping the gate
  green is a requirement of this project (user story 32), but the
  mechanism is left to the ARD/impl.

- **Lightweight rendering.** "Lightweight" contrasts with heavy
  remote-desktop streaming, not with local GPU rendering; still,
  the loop should use vsync or a frame cap so an idle ostrich does
  not spin a hot CPU loop.

- **HiDPI.** The shell should be legible on a HiDPI display;
  exact DPI-scaling polish can be refined in later UI work.
