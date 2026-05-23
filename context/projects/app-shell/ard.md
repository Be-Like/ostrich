# ARD — Application Shell (GUI Entry Point)

## PRD

This ARD is the architecture for the project described in
`context/projects/app-shell/prd.md` — replacing the console
hello-world entry point with the real ostrich application shell:
one native GLFW + OpenGL 3 + Dear ImGui (docking) window, wearing
the full cyberpunk theme from the first frame, running a smooth
~60 FPS loop with a centered ASCII wordmark, a diagnostics footer,
and a clean teardown. It is a *shell*: it stands up the window,
rendering, theme, docking host, and lifecycle, but builds no
functional panels and integrates no SSH.

The PRD traces to `context/design.md`, `context/workflow.md`, and
`context/theme.md`. This ARD additionally conforms to
`context/coding_standards.md`; its conformance checklist is
addressed in "Further Notes".

## Explanation of Architectural Components

### Where we start

The repository today is the bootstrap from the `project-setup`
project:

- `src/main.c` — a hello-world that prints `ostrich: ready.` and
  exits 0.
- `Makefile` — plain Make, knows only `CC`, compiles the single
  `src/main.c` into `build/ostrich`.
- `tests/smoke_test.c` — launches `build/ostrich`, reads stdout,
  asserts the greeting and a clean exit.
- `third_party/` — declared in `.gitmodules` (`imgui` on the
  `docking` branch, `glfw`, `libssh2`) but **not yet checked
  out**. No vendored code is compiled or linked.

Everything below replaces the hello-world program with a windowed
application while keeping `make` / `make test` as the build
surface.

### Target module decomposition

The shell is split into one deep module compiled as a static
library and three shallow pure-C utility modules left as plain
source, all wired together by a plain-source app layer. Per
`coding_standards.md`, a `.a` library is reserved for a deep
module with real implementation to hide; shallow code stays plain
source.

```
                    +------------------------+
   src/main.c  ---> | app / composition root |
   src/app/*.c      |  (plain C, orchestrates)|
                    +-----------+------------+
                                |
        +-----------+-----------+-----------+
        v           v           v           v
   include/     include/    include/   include/ui.h
   arena.h      lexicon.h   framestats.h    |   (pure C)
     |             |            |           v
   src/arena.c  src/        src/      build/libui.a
   (plain)      lexicon.c   framestats.c  (C++ seam:
                (plain)     (plain)        src/ui/*.cpp:
                                           GLFW + GL3 +
                                           ImGui + theme)
                                              |
                                   borrows / links
                                              v
                              build/libglfw.a (GLFW from
                              vendored source) + ImGui TUs
                              (imgui*.cpp, backends) +
                              system OpenGL
```

- **`ui` (deep module, `build/libui.a`)** — the single C/C++
  seam and the only library. Its `.cpp` internals under
  `src/ui/` own the GLFW window, the OpenGL 3 context, the Dear
  ImGui context and its GLFW/OpenGL3 backends, the theme
  (palette, JetBrains Mono atlas, ImGui style), the docking host,
  the resting-view ASCII wordmark, the static scanline/vignette
  overlay, and the diagnostics footer. All of this is hidden
  behind the pure-C contract `include/ui.h`
  (`ui_init` / `ui_frame` / `ui_shutdown` / `ui_status_str`).
  The library compiles ImGui (a C++ dependency) and so is C++
  internally; nothing outside it sees a C++ type.

- **`arena` (plain source)** — `include/arena.h` +
  `src/arena.c`. A minimal linear/bump allocator: create, alloc
  (aligned), reset, destroy. Pure C, no display, fully testable.
  This is ostrich's first arena implementation; it exists because
  `ui_init` takes an `Arena *` (see "Memory" below).

- **`lexicon` (plain source)** — `include/lexicon.h` +
  `src/lexicon.c`. The single centralized strings table that the
  theme requires: the footer copy, the `OSTRICH //
  infiltration console` identity line, the ASCII wordmark, and
  the `>` magenta voice signature, all behind enum→string
  lookups. Keeping every camp string here (rather than as
  scattered literals in `src/ui/`) is what makes a future
  "straight mode" a no-UI swap. Pure C, testable.

- **`framestats` (plain source)** — `include/framestats.h` +
  `src/framestats.c`. The rolling FPS smoothing that backs the
  footer's live frame-rate readout, plus the trivial centering
  arithmetic for the resting view. Pulling this out of the GL
  code is what lets the FPS logic be tested without a display.
  Pure C, testable.

- **App / composition root (plain source, not a library)** —
  `src/main.c` plus `src/app/*.c`. Its only job is
  orchestration: create the app arena, populate `UiOptions`, call
  `ui_init`, run `while (ui_frame(ui)) {}`, call `ui_shutdown`,
  map a `UiStatus` to a process exit code, and tear the arena
  down. Per the standards this layer hides no complexity and is
  exempt from the library rule; if real logic ever accumulates
  here it is pushed down into a library.

### The C / C++ seam

ostrich is C-first (C11). The **only** C++ in the shell lives
inside `libui.a`: `src/ui/*.cpp` `#include`s Dear ImGui and the
`imgui_impl_glfw` / `imgui_impl_opengl3` backends (all C++).
GLFW exposes a C API, but because the ImGui backend that binds to
the `GLFWwindow*` is C++ and is tightly coupled to ImGui, the
window/GL/ImGui stack is kept together inside this one library
rather than split — a single deep module with one seam.

`include/ui.h` is pure C: `extern "C"` guarded, no C++ types in
the signature, only opaque handles and POD option/status types.
`src/main.c`, `src/app/*.c`, and all of `arena` / `lexicon` /
`framestats` are pure C11 and never touch ImGui directly — they
reach the UI only through `include/ui.h`.

### Build integration

A real window cannot compile until the vendored submodules are
initialized and wired into plain Make. This project does that;
`libssh2` stays uninitialized and unlinked.

- **Submodules.** `git submodule update --init` brings in
  `third_party/imgui` (docking branch) and `third_party/glfw`.
  `third_party/libssh2` is intentionally left uninitialized.

- **C++ toolchain.** The Makefile gains `CXX` / `CXXFLAGS`
  (defaulting to a C++ compiler with the same warning posture and
  a fixed `-std=c++17` for ImGui) alongside `CC` / `CFLAGS`. The
  ImGui translation units compile as C++; the final `build/ostrich`
  link is driven by the C++ compiler (or otherwise links the C++
  runtime) because `libui.a` pulls in libstdc++. `CC` / `CFLAGS` /
  `CXX` / `CXXFLAGS` overrides are honored so the build stays
  portable across Linux and macOS, and the build stays
  warning-clean for our own code.

- **Dear ImGui.** Compiled into `libui.a` from
  `third_party/imgui`: `imgui.cpp`, `imgui_draw.cpp`,
  `imgui_tables.cpp`, `imgui_widgets.cpp`, plus the backends
  `backends/imgui_impl_glfw.cpp` and
  `backends/imgui_impl_opengl3.cpp`. The `imgui_impl_opengl3`
  backend uses its own bundled GL loader, so **no GLAD/GL3W** is
  vendored or linked.

- **GLFW.** Compiled from vendored source by our own Makefile
  into `build/libglfw.a` (no GLFW CMake, no system GLFW). The
  host platform is detected with `uname`:
  - Linux: define `_GLFW_X11`, link `-lX11 -ldl -lpthread -lm`
    (and `-lGL`). The X11 backend also runs under XWayland;
    a Wayland backend (`_GLFW_WAYLAND`) is left as a build-time
    switch, not the default.
  - macOS: define `_GLFW_COCOA`, compile the Objective-C (`.m`)
    sources, link `-framework Cocoa -framework IOKit
    -framework CoreFoundation` (and OpenGL).
  The exact GLFW source-file set per platform is an
  implementation detail of the Makefile rule.

- **OpenGL.** Request an OpenGL **3.2 core** profile with GLSL
  `#version 150` — the floor for the ImGui core-profile backend
  and the ceiling that macOS supports — so one code path serves
  both hosts.

- **Fonts.** JetBrains Mono (regular + bold, OFL) is vendored as
  committed files under `assets/fonts/` (the `third_party/` tree
  is reserved for submodules). They are loaded at runtime (see
  "Memory").

### Memory — arenas

The shell has very little ostrich-owned dynamic memory, so it
introduces exactly one arena.

- **App-lifetime arena (`app`).** Created in `src/main.c` before
  `ui_init` and destroyed after `ui_shutdown`; its lifetime is
  the whole process. Freed by `arena_destroy` (reset semantics),
  never object-by-object. It is passed into `ui_init`, which uses
  it to allocate the opaque `Ui` handle and to hold the
  JetBrains Mono TTF bytes.

- **Caller-controlled allocation.** `ui_init(Arena *, UiOptions,
  Ui **)` takes the arena explicitly; `libui` holds no hidden
  static/global arena and allocates none of its own ostrich-owned
  memory behind the caller's back.

- **Font ownership.** At init, `ui` reads each vendored TTF into
  the `app` arena and registers it with ImGui via
  `AddFontFromMemoryTTF` with `FontDataOwnedByAtlas = false`, so
  ImGui *borrows* the arena bytes and never frees them; the arena
  owns them for the process lifetime. If a font file is missing
  or unreadable, `ui_init` returns a distinct status rather than
  silently rendering a fallback.

- **Library-owned memory (flagged, out of arena scope).** Dear
  ImGui and GLFW allocate internally on their own terms — the
  ImGui context, draw lists, font atlas texture, and GLFW's
  window/X11/Cocoa state. Per the standards this library-owned
  memory follows each library's own model and is outside ostrich's
  arena rules. The shell uses **no** `malloc`/`free` of its own;
  the only transient per-frame string (the footer's
  `… // NN FPS // …`) is formatted into a small stack buffer.

- **Per-frame arena: deliberately deferred.** The shell has zero
  ostrich-owned per-frame heap allocations, so no per-frame arena
  is introduced. It earns its place with **log streaming** (the
  build/device logs are unbounded transient per-frame text), which
  is a later project; this ARD records that as the intended home
  for a per-frame/scratch arena.

### Threading

The shell is **single-threaded** — the UI thread runs the entire
init → frame loop → shutdown. No worker threads, no cross-thread
data handoff, and therefore no shared-arena-across-threads
concern yet. The arena thread-confinement rule is satisfied
trivially (one thread owns the one arena). The concurrent worker
model and its explicit copy/handoff across threads arrive with
the SSH / log-streaming work.

### Render loop and frame pacing

`ui_frame` performs one full frame: poll GLFW events, start the
ImGui frame, build the docking host + resting view + overlay +
footer, render, and swap buffers. It returns `false` when the
window should close (close button or Ctrl-Q) and `true`
otherwise; the app loops on it.

Pacing uses **vsync** (`glfwSwapInterval(1)`): the swap blocks on
the display refresh, which both prevents tearing and keeps an idle
ostrich from spinning a hot CPU core. On a 60 Hz panel this yields
~60 FPS; on a high-refresh panel the loop runs at the panel rate
and the footer reports the **measured** rate honestly (the
theme's `60 FPS` is illustrative copy, not a literal clamp). An
optional fixed 60 FPS cap is noted as a later refinement, not
built here. No boot animation, splash, or gate: the first
`ui_frame` paints the themed resting view immediately.

### HiDPI

At init, `ui` reads the GLFW window content scale
(`glfwGetWindowContentScale`) and scales the base font pixel size
and the ImGui style (`style.ScaleAllSizes`) by it so the shell is
legible on a HiDPI display. Exact DPI-scaling polish is left to
later UI work, per the PRD.

### The `make test` rework

The current smoke test asserts the `ostrich: ready.` stdout
greeting, which a windowed event-loop app no longer emits. It is
replaced by a **two-tier**, state-based suite, consistent with the
existing harness, run by `make test`:

1. **Pure tier (always runs, no display).** Black-box tests for
   the display-free modules: `tests/arena_test.c`,
   `tests/lexicon_test.c`, `tests/framestats_test.c`. These
   `#include` only the module's public header and compile its
   plain source directly. This tier is the trustworthy core gate —
   it asserts real behavior (arena alloc/align/reset invariants,
   every lexicon key resolving to non-empty copy with the correct
   `>` voice signature, FPS smoothing producing a sane positive
   readout, centering arithmetic) and is meaningful even where no
   display exists.

2. **UI smoke (display-gated).** `tests/ui_test.c` links
   `build/libui.a` and exercises the public contract black-box:
   `ui_init` (with a **headless** option that hides the window),
   a few `ui_frame` calls, then `ui_shutdown`, asserting `UI_OK`
   and a clean teardown. Because GLFW needs a display server even
   for a hidden window, `ui_init` returns the distinct
   `UI_ERR_NO_DISPLAY` when it cannot reach one; the test then
   prints `SKIP: no display` and exits 0. Any **other** outcome
   (partial init, non-`OK` status, teardown crash) fails the test
   hard. This keeps the gate green and portable on a headless
   RALPH/CI box while still fully exercising init/frame/shutdown
   wherever a display is present.

No `--smoke` flag on the binary is needed: the library seam we
chose (`init`/`frame`/`shutdown` behind `ui.h`) is what `ui_test`
drives directly. The black-box approach means no white-box peeking
into `src/ui/` internals is required.

### Theme and lexicon governance

Per `theme.md`'s authority limits, the shell makes **no**
structural change (adds no panels, alters no flow). The only
theme-owned wording it introduces — the footer copy and the
identity/wordmark text — is recorded as deliberate and lives
solely in the `lexicon` module so a future straight-mode is a
no-UI swap. Palette discipline (decorative cyan/magenta for
chrome; semantic green/red/amber reserved for meaning; off-white
log/body text) is applied in the `ui` theme setup even though no
semantic states exist yet, so the discipline holds from the
start. The wordmark is the single bright element in the resting
view ("brightness = attention"). The scanline/vignette is static
and faint, drawn behind chrome only.

## Interfaces

All public headers are C-includable and follow the standards'
two conventions: allocating functions take a caller-supplied
`Arena *`, and every fallible function returns a status enum with
results via out-parameters plus a status→string companion. There
is no hidden `errno`-style or global error state, and no failure
via exceptions.

### `include/arena.h` (pure C)

```c
typedef struct Arena Arena;

/* Reserve `cap` bytes up front. Returns NULL on OOM. */
Arena *arena_create(size_t cap);

/* Bump-allocate `size` bytes aligned to `align`.
   Returns NULL if the arena is exhausted. */
void *arena_alloc(Arena *a, size_t size, size_t align);

/* Roll the arena back to empty; memory is reused, not freed. */
void arena_reset(Arena *a);

/* Release the whole arena. */
void arena_destroy(Arena *a);
```

### `include/lexicon.h` (pure C)

The single source of truth for the shell's camp copy. Lookups are
infallible (an unknown key returns a stable non-NULL placeholder),
so no status type is needed.

```c
typedef enum {
    LEX_IDENTITY,       /* "OSTRICH // infiltration console" */
    LEX_WORDMARK,       /* the static ASCII ostrich banner   */
    LEX_FOOTER_NAME,    /* "ostrich"                          */
    LEX_FOOTER_ONLINE,  /* "ONLINE"                           */
    LEX_VOICE_PREFIX,   /* ">" (magenta voice signature)      */
    LEX__COUNT
} LexKey;

const char *lex(LexKey key);
```

### `include/framestats.h` (pure C)

```c
typedef struct {
    double accum;   /* smoothed frame time, seconds */
    /* ... rolling state ... */
} FrameStats;

void frame_stats_init(FrameStats *fs);

/* Feed one frame's delta-time (seconds); returns the current
   smoothed frames-per-second as an integer for the footer. */
int frame_stats_update(FrameStats *fs, double dt_seconds);

/* Centering arithmetic for the resting view: the left/top
   offset to center `content` within `avail`, clamped >= 0. */
float center_offset(float avail, float content);
```

### `include/ui.h` (pure C contract over the C++ UI library)

```c
#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ui Ui;  /* opaque handle */

typedef enum {
    UI_OK = 0,
    UI_ERR_NO_DISPLAY,  /* no reachable display server      */
    UI_ERR_GLFW,        /* glfwInit / window / GL ctx failed */
    UI_ERR_GL,          /* GL/loader init failed             */
    UI_ERR_FONT,        /* a vendored TTF was missing/bad    */
    UI_ERR_OOM          /* arena exhausted during init       */
} UiStatus;

typedef struct {
    const char *title;     /* window title ("ostrich")       */
    int width, height;     /* initial window size            */
    const char *font_dir;  /* dir holding JetBrainsMono TTFs */
    bool headless;         /* hidden window (for ui_test)    */
} UiOptions;

/* Stand up window + GL + ImGui + theme + fonts. Allocates the
   Ui handle and font bytes from `a`. */
UiStatus ui_init(Arena *a, UiOptions opts, Ui **out);

/* Render exactly one frame. Returns false when the window
   should close (close button or Ctrl-Q), true otherwise. */
bool ui_frame(Ui *ui);

/* Tear down ImGui, the GL context, and GLFW cleanly. */
void ui_shutdown(Ui *ui);

/* Human-readable reason for a UiStatus, for the UI/CLI. */
const char *ui_status_str(UiStatus st);

#ifdef __cplusplus
}
#endif
```

### App / composition root (illustrative)

```c
int main(void) {
    Arena *app = arena_create(APP_ARENA_BYTES);
    if (!app) return EXIT_FAILURE;

    UiOptions opts = {
        .title = "ostrich",
        .width = 1280, .height = 800,
        .font_dir = "assets/fonts",
        .headless = false,
    };

    Ui *ui = NULL;
    UiStatus st = ui_init(app, opts, &ui);
    if (st != UI_OK) {
        fprintf(stderr, "ostrich: %s\n", ui_status_str(st));
        arena_destroy(app);
        return EXIT_FAILURE;
    }

    while (ui_frame(ui)) { }

    ui_shutdown(ui);
    arena_destroy(app);
    return EXIT_SUCCESS;
}
```

### Build surface (Makefile contract)

The existing targets keep their meaning; the rules grow.

- `make` (default) → `build/ostrich`, linking `build/libui.a`
  and `build/libglfw.a` plus the platform GL/window libs.
- `make test` → builds and runs `build/arena_test`,
  `build/lexicon_test`, `build/framestats_test`, and
  `build/ui_test`; exits non-zero if any assertion fails.
  `ui_test` exits 0 on `SKIP: no display`.
- `make clean` → removes `build/` (unchanged).

Roughly (illustrative, not prescriptive):

```make
build/libglfw.a: $(GLFW_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(GLFW_DEFS) -c ...
	ar rcs $@ ...

build/libui.a: $(UI_CPP) $(IMGUI_CPP) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Iinclude -I<imgui> -c ...
	ar rcs $@ ...

build/ostrich: src/main.c $(APP_SRC) src/arena.c \
               src/lexicon.c src/framestats.c \
               build/libui.a build/libglfw.a | $(BUILD)
	$(CXX) $(CFLAGS) -Iinclude -o $@ $^ $(PLATFORM_LIBS)
```

## Out of Scope

Inherited from the PRD (all functional panels; the connection
overlay/launch screen; all SSH/`libssh2`; discovery, run config,
the build→install→launch orchestration and log streaming; the
concurrent worker model; persistence; the run-state machine and
its labels; a populated lexicon and straight-mode UI; motion/FX
beyond the static scanline/vignette/glow; audio and bitmap/SVG
icons; multi-window/multi-Mac, the debugger, and SSH
port-forwarding).

Additional architecture-level exclusions fixed by this ARD:

- **The per-frame / scratch arena** — deferred to the
  log-streaming project (see "Memory").
- **ImGui multi-viewport** (`ViewportsEnable`) — the docking
  flag is enabled, but the shell stays a single OS window per the
  design; multi-viewport platform windows are off.
- **Shader-based bloom / post-processing** — the "glow" look is
  approximated with ImGui draw-list layering; a real GL shader
  pass for bloom is out of scope.
- **A second OpenGL loader (GLAD/GL3W)** — the ImGui backend's
  bundled loader is used.
- **GLFW Wayland backend** — X11 (XWayland-compatible) is the
  Linux default; Wayland is left as a build switch.
- **A `--smoke` CLI mode** — testing goes through the `libui`
  seam, not a binary flag.
- **`libssh2` build integration** — its submodule stays
  uninitialized.
- **Worker threads / cross-thread handoff** — single-threaded
  shell only.
- **Window size/position persistence** — none.

## Further Notes

### coding_standards.md conformance checklist

- [x] **Arenas named + lifetimes stated** — one `app` arena,
  process lifetime, created in `main` and destroyed after
  `ui_shutdown`; freed by reset/destroy, never per-object. The
  per-frame arena is explicitly deferred with its rationale.
- [x] **Allocation is caller-controlled** — `arena_create` in
  `main`; `ui_init(Arena *, …)` receives it; `libui` keeps no
  hidden/global allocator.
- [x] **Thread-confinement respected** — single-threaded shell;
  the one arena is owned by the one (UI) thread; no cross-thread
  data, so no shared-arena handoff. The copy/handoff rule applies
  to the future worker model, not here.
- [x] **Non-arena allocations flagged + justified** — ostrich
  itself does no `malloc`/`free`. ImGui and GLFW allocate
  internally (library-owned memory, out of arena scope); the
  borrowed font bytes live in the `app` arena via
  `FontDataOwnedByAtlas = false`; the per-frame footer string
  uses a stack buffer.
- [x] **Module → library decisions made** — `ui` is the only
  `.a` (deep, hides the GLFW+GL+ImGui+theme stack and the C++
  seam); `arena`, `lexicon`, `framestats` are intentionally plain
  source (shallow); the app layer is plain orchestration.
- [x] **Library layout specified** — `include/ui.h` public
  contract, `src/ui/*.cpp` private, `build/libui.a` archive.
  Other modules `#include` only `include/ui.h`.
- [x] **C/C++ seam identified** — ImGui (and the GLFW/GL
  backends) are sealed inside `libui` behind the pure-C
  `include/ui.h` (`extern "C"`, no C++ types); everything else,
  including `src/app/`, is C11 and never touches ImGui directly.
- [x] **Error handling shape confirmed** — `UiStatus` enum
  return + `Ui **` out-param + `ui_status_str`; `arena` reports
  failure by returning `NULL`; `lexicon` lookups are infallible
  (stable non-NULL). No hidden/global error state; no exceptions
  across the C headers.
- [x] **Test approach per library** — `ui_test` links
  `build/libui.a` and tests black-box via `ui.h`;
  `arena`/`lexicon`/`framestats` tests compile their plain source
  and test via their public headers. No white-box access is
  needed, so none is taken.

### Other notes

- **Font-missing behavior.** A missing/unreadable TTF yields
  `UI_ERR_FONT` from `ui_init` rather than a silent fallback, so
  a broken/incomplete vendor of `assets/fonts/` fails loudly
  instead of shipping an off-theme window.
- **High-refresh displays.** vsync ties the loop to the panel
  refresh; the footer reports the measured rate, so a 144 Hz
  panel honestly reads ~144, not a forced 60. A fixed 60-cap is a
  noted future refinement.
- **`libssh2` stays dark.** Its submodule remains uninitialized
  and unlinked; the footer's `ONLINE` means "ostrich is running,"
  not a live SSH link, exactly as the PRD scopes it.
- **Traceability.** Window/GL/ImGui-docking choices and the
  Linux+macOS target trace to `context/design.md`; the
  single-locked-window layout and the footer-as-own-diagnostics
  trace to `context/workflow.md`; palette discipline, JetBrains
  Mono, the static scanline/vignette, the ASCII wordmark, and the
  footer/identity copy trace to `context/theme.md`. The
  centralized `lexicon` is the structural hook the theme document
  asks for so a future straight-mode needs no UI rework.
