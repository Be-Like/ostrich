# Implementation Plan - Application Shell (GUI Entry Point)

## Summary of Tasks

Tackle in this order; each task is one buildable, testable, and
releasable commit/PR.

1. **Arena module** — `include/arena.h` + `src/arena.c` + test;
   the caller-controlled bump allocator `ui_init` needs.
2. **Lexicon module** — `include/lexicon.h` + `src/lexicon.c` +
   test; the single centralized camp-copy table.
3. **Framestats module** — `include/framestats.h` +
   `src/framestats.c` + test; FPS smoothing and centering math.
4. **Build integration + UI seam skeleton** — init `imgui`/`glfw`
   submodules, vendor fonts, add the C++ toolchain, build
   `libglfw.a` + `libui.a`, and a headless-tested `ui.h` seam
   (`ui_init`/`ui_frame`/`ui_shutdown`); stub binary still ships.
5. **Windowed entry point + docking host + quit** — flip
   `src/main.c` to the composition root, open the real visible
   window with a docking host, wire close-button + Ctrl-Q quit,
   and finish the `make test` rework.
6. **Theme: palette + font styling** — dark base, palette
   discipline, JetBrains Mono applied everywhere.
7. **Resting view: wordmark + identity** — the centered ASCII
   wordmark and identity line, the single bright element.
8. **Diagnostics footer** — the slim `ostrich // NN FPS // ONLINE`
   footer with a live frame-rate readout.
9. **Scanline/vignette overlay** — the faint static atmosphere
   drawn behind the chrome.

## Task Dependency Relationships

```
  1 arena ──────────► 4 build + UI seam ──► 5 windowed shell
                                                  │
                                                  ▼
                                              6 theme
                                                  │
                          ┌───────────────────────┼───────────┐
                          ▼                        ▼           ▼
                    7 resting view           8 footer    9 overlay
                          ▲                        ▲
  2 lexicon ──────────────┤                        │
  3 framestats ───────────┴────────────────────────┘

  (2 lexicon and 3 framestats have no blockers; they are
   consumed first at tasks 7 and 8.)
```

## Detailed Tasks

### Task 1 - Arena module

- **Status**: done
- **Blocked by**: none
- **User stories covered**: foundation for caller-controlled
  memory; underpins US-24 (clean teardown) and US-32 (a
  meaningful `make test`). No directly user-visible story.

#### What to build

The first ostrich arena: a minimal linear/bump allocator
(create, aligned alloc, reset, destroy). It exists because every
allocating interface in this project — beginning with `ui_init` —
takes a caller-supplied `Arena *`. No UI, no display; pure C.
`src/main.c` and the existing `smoke_test` are untouched, so the
console stub keeps building and shipping.

#### Technical Details

Implements the `include/arena.h` contract from the ARD
("Interfaces" → `include/arena.h`) and the memory model in ARD
"Memory — arenas": one process-lifetime `app` arena, freed by
reset/destroy rather than per object. `arena_create` returns
`NULL` on OOM; `arena_alloc` returns `NULL` when exhausted.
Plain source (shallow module), not a library, per
`coding_standards.md` "Modules as compiled libraries". The
Makefile gains a `build/arena_test` rule and runs it from
`make test` as the first member of the ARD's "pure tier"
(ARD "The `make test` rework").

#### Acceptance criteria

- [x] `include/arena.h` matches the ARD signature
      (`arena_create`/`arena_alloc`/`arena_reset`/`arena_destroy`)
      and is pure C.
- [x] `tests/arena_test.c` black-box tests alloc, alignment,
      reset reuse, and OOM/exhaustion returning `NULL`.
- [x] `make test` builds and runs `arena_test` alongside the
      existing `smoke_test`; both pass.
- [x] `make` still builds the console stub; build is
      warning-clean under `-Wall -Wextra`.

### Task 2 - Lexicon module

- **Status**: done
- **Blocked by**: none
- **User stories covered**: US-21 (camp copy from a single
  centralized lexicon, not scattered literals); supplies the
  copy later used by US-16 and US-18/20.

#### What to build

The single source of truth for the shell's camp copy: the
`OSTRICH // infiltration console` identity line, the ASCII
ostrich wordmark, the footer name (`ostrich`) and `ONLINE`
label, and the `>` magenta voice signature — all behind an
enum→string lookup. Centralizing them here is what makes a future
straight-mode a no-UI swap. Pure C; lookups are infallible.

#### Technical Details

Implements `include/lexicon.h` exactly as in the ARD
("Interfaces" → `include/lexicon.h`): the `LexKey` enum ending in
`LEX__COUNT` and `const char *lex(LexKey)`. An unknown key
returns a stable non-`NULL` placeholder, so no status type is
needed. Plain source (shallow), per the standards. Wording is
theme-owned and traces to `context/theme.md`; this task only
houses it (ARD "Theme and lexicon governance"). Adds
`build/lexicon_test` to the pure tier of `make test`.

#### Acceptance criteria

- [ ] `include/lexicon.h` defines the `LexKey` enum (with
      `LEX__COUNT`) and `lex()` as in the ARD.
- [ ] Every key in `0..LEX__COUNT` resolves to non-empty copy;
      the voice key returns the `>` signature.
- [ ] An out-of-range key returns a stable non-`NULL`
      placeholder (no crash).
- [ ] `tests/lexicon_test.c` asserts the above; `make test`
      runs it and it passes.

### Task 3 - Framestats module

- **Status**: done
- **Blocked by**: none
- **User stories covered**: backs US-19 (the footer's live
  frame-rate readout) and US-25 (resting-view centering that
  survives resize). No directly user-visible story on its own.

#### What to build

The display-free arithmetic behind two later visuals: rolling FPS
smoothing for the footer readout, and the centering offset used to
place the resting view. Pulling this out of the GL code is what
lets the FPS and centering logic be tested without a display.
Pure C.

#### Technical Details

Implements `include/framestats.h` from the ARD ("Interfaces" →
`include/framestats.h`): `FrameStats` plus
`frame_stats_init`, `frame_stats_update(fs, dt_seconds)`
returning a smoothed integer FPS, and
`center_offset(avail, content)` clamped `>= 0`. Plain source
(shallow). Adds `build/framestats_test` — the third member of the
pure tier (ARD "The `make test` rework"). With this task the pure
tier is complete; the UI smoke tier arrives in Task 4.

#### Acceptance criteria

- [x] `include/framestats.h` matches the ARD signatures.
- [x] `frame_stats_update` returns a sane positive FPS for a
      stream of plausible deltas and smooths jitter.
- [x] `center_offset` returns `(avail - content)/2` and clamps
      to `0` when `content > avail`.
- [x] `tests/framestats_test.c` asserts the above; `make test`
      runs all three pure-tier tests and they pass.

### Task 4 - Build integration + UI seam skeleton

- **Status**: done
- **Blocked by**: Task 1
- **User stories covered**: US-4 (GLFW + OpenGL 3 context),
  US-5 (ImGui docking context enabled), US-9 (vsync, no CPU
  peg), US-10 (no tearing/flicker), US-14 (JetBrains Mono baked
  into the atlas), US-24 (clean ImGui/GL/GLFW teardown), US-26
  (HiDPI scaling), US-29 (fonts vendored), US-30 (`CC`/`CFLAGS`
  honored, warning-clean), US-31 (builds on Linux + macOS);
  partial US-32 (adds the UI smoke tier).

#### What to build

Make the C/C++ window stack actually compile, link, and tear down
cleanly behind a tested seam — without yet changing what the user
runs. Initialize the vendored `imgui` and `glfw` submodules,
vendor JetBrains Mono, teach plain Make to build C++ and the
GLFW-from-source archive, and stand up the `include/ui.h` library:
`ui_init` opens a (hideable) GLFW window with a GL 3.2-core
context and a Dear ImGui docking context, loads the fonts from the
arena, and scales for HiDPI; `ui_frame` runs one vsynced frame
(clear to a dark background, render an empty UI, swap) and returns
`false` on window-close; `ui_shutdown` tears it all down. This is
the project's biggest integration risk, so it lands behind a
headless test while `src/main.c` stays the console stub and the
shipping binary is unchanged.

#### Technical Details

Per ARD "Build integration": `git submodule update --init` for
`imgui` (docking) and `glfw` only — `libssh2` stays uninitialized.
The Makefile gains `CXX`/`CXXFLAGS` (`-std=c++17`, same warning
posture), builds `build/libglfw.a` from vendored GLFW source with
`uname`-based platform defines/libs (`_GLFW_X11` + `-lX11 -ldl
-lpthread -lm -lGL` on Linux; `_GLFW_COCOA` + the Cocoa/IOKit/
CoreFoundation frameworks and `.m` sources on macOS), and compiles
the ImGui TUs + `imgui_impl_glfw`/`imgui_impl_opengl3` backends
into `build/libui.a`. No GLAD/GL3W (the OpenGL3 backend's bundled
loader is used); request a GL **3.2 core** profile with GLSL
`#version 150`. JetBrains Mono (regular + bold, OFL) is committed
under `assets/fonts/`.

`include/ui.h` is the full pure-C contract from the ARD
("Interfaces" → `include/ui.h`): opaque `Ui`, the `UiStatus`
enum, `UiOptions` (incl. `headless`), and
`ui_init`/`ui_frame`/`ui_shutdown`/`ui_status_str`, all
`extern "C"` with no C++ types. The C++ internals live in
`src/ui/*.cpp` (the only library, the only C/C++ seam — ARD "The
C / C++ seam"). Memory follows ARD "Memory — arenas": `ui_init`
takes the `Arena *`, allocates the `Ui` handle and the TTF bytes
from it, and registers fonts with `FontDataOwnedByAtlas = false`
so ImGui borrows the arena bytes. ImGui multi-viewport stays off
(docking flag only). Pacing is `glfwSwapInterval(1)`. Distinct
statuses: `UI_ERR_NO_DISPLAY` when no display server is
reachable, `UI_ERR_FONT` for a missing/unreadable TTF (no silent
fallback), plus `UI_ERR_GLFW`/`UI_ERR_GL`/`UI_ERR_OOM`.

`tests/ui_test.c` is the UI smoke tier (ARD "The `make test`
rework", tier 2): it links `build/libui.a`, calls `ui_init` with
`headless = true`, runs a few `ui_frame` calls, then
`ui_shutdown`, asserting `UI_OK` and a clean teardown. On
`UI_ERR_NO_DISPLAY` it prints `SKIP: no display` and exits 0; any
other non-`OK` outcome or teardown crash fails hard. The existing
`smoke_test` and `src/main.c` stub are left in place this task, so
`make`/`make test` stay green either way.

#### Acceptance criteria

- [x] Build setup initializes only the `imgui` and `glfw`
      submodules; `libssh2` stays registered in `.gitmodules`
      but is never compiled or linked into any target.
- [x] `make` builds `build/libglfw.a` and `build/libui.a` from
      vendored source on Linux (and the macOS path is wired);
      `CC`/`CFLAGS`/`CXX`/`CXXFLAGS` overrides are honored and
      our own code compiles warning-clean.
- [x] `include/ui.h` is the ARD contract, `extern "C"`, with no
      C++ types; all C++ is confined to `src/ui/*.cpp`.
- [x] On a display, `ui_test` runs `ui_init`(headless) →
      `ui_frame` ×N → `ui_shutdown` returning `UI_OK` with no
      crash or leak; with no display it prints `SKIP: no display`
      and exits 0.
- [x] `ui_init` returns `UI_ERR_FONT` when a vendored TTF is
      absent and requests a GL 3.2-core context with vsync
      enabled.
- [x] `make test` runs the three pure-tier tests plus `ui_test`
      (and the still-present `smoke_test`); all pass/skip; the
      console-stub binary still ships unchanged.

### Task 5 - Windowed entry point + docking host + quit

- **Status**: done
- **Blocked by**: Task 4
- **User stories covered**: US-1 (a native window opens), US-2
  (single window titled `ostrich`), US-3 (resizable/native
  behavior), US-6 (empty docking host laid out for future
  panels), US-7 (paints instantly, no boot animation), US-8
  (~60 FPS, smooth), US-22 (quit by closing the window), US-23
  (keyboard-shortcut quit), US-27 (nothing external on launch),
  US-28 (`make` builds the windowed app with imgui+glfw linked
  in), US-32 (the `make test` rework completed).

#### What to build

Flip ostrich from a console stub into the application you can see:
running `./build/ostrich` opens one resizable native window
titled `ostrich`, paints immediately, presents an empty docking
host ready for future panels, runs a smooth loop, and quits
cleanly via the close button or a keyboard shortcut. This is the
"ostrich is finally an app" checkpoint described in PRD
"Solution".

#### Technical Details

Replace `src/main.c` with the composition root from ARD
("Interfaces" → "App / composition root" and "App / composition
root (plain source, not a library)"): create the process-lifetime
`app` arena, fill `UiOptions` (`title="ostrich"`, 1280×800,
`font_dir="assets/fonts"`, `headless=false`), call `ui_init`, run
`while (ui_frame(ui)) {}`, `ui_shutdown`, map `UiStatus` to the
exit code, then `arena_destroy`. Any orchestration beyond `main`
goes in `src/app/*.c`; this layer is plain C and never touches
ImGui directly. Inside `libui`, `ui_frame` lays out a full-window
ImGui dockspace as the empty host (single OS window;
multi-viewport stays off — ARD "Out of Scope") and returns
`false` on the GLFW close flag **or** Ctrl-Q. The default `make`
target now links `build/libui.a` + `build/libglfw.a` + the
platform GL/window libs (ARD "Build surface").

This task completes the `make test` rework: the windowed app no
longer prints `ostrich: ready.`, so `tests/smoke_test.c` is
removed and its Makefile rule retired; `make test` is now the
ARD's two tiers (the three pure tests + `ui_test`). The shell does
nothing external on launch — no network, no SSH (`libssh2` stays
dark; the footer's `ONLINE` only means "ostrich is running").

#### Acceptance criteria

- [x] `./build/ostrich` opens one resizable native window
      titled `ostrich` that paints immediately (no splash) and
      can be moved/min/maximized via the OS.
- [x] The window presents an empty ImGui docking host; it stays
      a single OS window (multi-viewport off).
- [x] Closing the window **and** pressing Ctrl-Q each quit
      cleanly, tearing down ImGui/GL/GLFW with no crash or hang,
      and `main` returns the `UiStatus`-mapped exit code.
- [x] Launch performs no network/SSH activity.
- [x] `tests/smoke_test.c` is removed; `make test` runs only the
      three pure-tier tests plus `ui_test`, and all
      pass (or `ui_test` skips with no display).

### Task 6 - Theme: palette + font styling

- **Status**: pending
- **Blocked by**: Task 5
- **User stories covered**: US-11 (dark cyberpunk identity),
  US-12 (dark near-black base), US-13 (palette discipline),
  applies US-14 (JetBrains Mono used everywhere).

#### What to build

Dress the live window in ostrich's identity: a dark near-black
base, JetBrains Mono (regular + bold) as the typeface everywhere,
and the palette discipline applied to the ImGui style — decorative
cyan/magenta reserved for chrome, the semantic green/red/amber
held back for meaning, off-white for body/log text — so the visual
language is consistent from the start even before any stateful UI
exists.

#### Technical Details

Per ARD "Theme and lexicon governance": set the ImGui style and
colors in the `ui` theme setup (the font atlas was already baked
in Task 4). Apply `style.ScaleAllSizes` consistent with the
HiDPI font scaling from Task 4. No new structure, panels, or flow
(theme-document authority limit). Colors and typography trace to
`context/theme.md`. Semantic colors are defined but unused here
(no states yet); the discipline is what is being established.

#### Acceptance criteria

- [ ] The window renders on a dark near-black base with the
      cyberpunk style applied from the first painted frame.
- [ ] JetBrains Mono regular and bold are the typeface used for
      all rendered text.
- [ ] The ImGui style uses decorative cyan/magenta for chrome
      only; semantic green/red/amber are defined but reserved
      (unused) and body text is off-white.
- [ ] `make`/`make test` stay green; no panels or flow added.

### Task 7 - Resting view: wordmark + identity

- **Status**: pending
- **Blocked by**: Task 6, Task 2, Task 3
- **User stories covered**: US-16 (centered ASCII wordmark +
  identity line), US-17 (the single bright element), US-25 (stays
  correctly placed on resize).

#### What to build

Give the empty shell a recognizable face: the static ASCII
ostrich wordmark and the `OSTRICH // infiltration console`
identity line, centered in the docking host's empty central area
and remaining centered as the window resizes. The wordmark is the
single bright element in the resting view, honoring the
"brightness = attention" discipline.

#### Technical Details

Draw the resting view inside the empty docking host from Task 5.
Pull both strings from the `lexicon` (`LEX_WORDMARK`,
`LEX_IDENTITY`) rather than literals, and center them using
`framestats`' `center_offset` against the available content
region so placement survives resize (ARD modules `lexicon` and
`framestats`). Static paint only — no animation (PRD "Further
Notes": whimsy is zero-cost). Uses the Task 6 theme so the
wordmark is the brightest element.

#### Acceptance criteria

- [ ] The ASCII wordmark and `OSTRICH // infiltration console`
      line render centered in the empty docking host.
- [ ] Both strings come from `lexicon` lookups (no inline
      literals in `src/ui/`).
- [ ] The view stays centered (via `center_offset`) across
      window resizes.
- [ ] The wordmark is the single brightest element; the paint is
      static (no animation); `make`/`make test` stay green.

### Task 8 - Diagnostics footer

- **Status**: pending
- **Blocked by**: Task 6, Task 2, Task 3
- **User stories covered**: US-18 (slim footer
  `ostrich // 60 FPS // ONLINE`), US-19 (live frame-rate
  readout), US-20 (reads as ostrich's own voice, distinct from
  future tool output).

#### What to build

A slim footer along the bottom showing `ostrich // NN FPS //
ONLINE`, where the frame-rate updates live as the loop runs — the
observable proof of the ~60 FPS goal — styled as ostrich's own
diagnostics so it is never confused with future build/device
output.

#### Technical Details

Compose the footer from `lexicon` (`LEX_FOOTER_NAME`,
`LEX_FOOTER_ONLINE`, `LEX_VOICE_PREFIX`) plus a live FPS from
`frame_stats_update` fed each frame's delta-time. The FPS readout
reports the **measured** rate honestly on high-refresh panels
(ARD "Render loop and frame pacing"); `60 FPS` is illustrative
copy, not a clamp. Format the transient `… // NN FPS // …` into a
small stack buffer — no per-frame heap allocation (ARD "Memory —
arenas"). Style it in ostrich's own voice, visually distinct from
tool output (workflow: the footer is ostrich's diagnostics).
`ONLINE` means "ostrich is running," not an SSH link.

#### Acceptance criteria

- [ ] A slim footer renders `ostrich // NN FPS // ONLINE` with
      all words sourced from `lexicon`.
- [ ] The `NN` reading updates live each frame from `framestats`
      and reflects the measured rate.
- [ ] The footer is styled distinctly as ostrich's own
      diagnostics (not styled like future tool output).
- [ ] The per-frame footer string uses a stack buffer (no heap
      allocation); `make`/`make test` stay green.

### Task 9 - Scanline/vignette overlay

- **Status**: pending
- **Blocked by**: Task 6
- **User stories covered**: US-15 (a faint, static scanline and
  vignette overlay behind the chrome).

#### What to build

Add the cyberpunk atmosphere: a faint, static scanline plus a
vignette drawn behind the chrome, present without any motion that
could distract. This completes the resting shell's look.

#### Technical Details

Draw the scanline/vignette via ImGui draw-list layering behind
the chrome (ARD "Out of Scope": no shader bloom — the glow look is
approximated with draw-list layering; multi-viewport off). Static
and faint, tuned so it never competes with the wordmark and could
be cut if it ever hindered the core loop (PRD "Further Notes":
whimsy is zero-cost). No animation, audio, or bitmap/SVG assets.

#### Acceptance criteria

- [ ] A faint, static scanline and vignette render behind the
      chrome (wordmark and footer stay on top and legible).
- [ ] The overlay has no motion/animation and adds no
      perceptible cost to the frame loop.
- [ ] The overlay is drawn with ImGui draw-list layering only
      (no GL shader pass, no external assets).
- [ ] `make`/`make test` stay green.
