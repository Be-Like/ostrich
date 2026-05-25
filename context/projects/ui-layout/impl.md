# Implementation Plan - Application Layout (ui-layout)

This plan reorganizes ostrich's ONLINE working area into a clean
four-band layout: the connection-status header, one **full-width
Xcode project configuration** container, then the **Build Log
(left) + Live Feed (right)** filling the remaining vertical space,
and the footer. It is a presentational refactor of the existing UI
— it reuses the current view-models, intents, and `app.c` wiring
unchanged, and lives almost entirely in `src/ui/ui.cpp`.

It traces to `context/workflow.md` (Layout / Containers — the
sovereign source for panel structure) and `context/theme.md`
(empty-state copy, lexicon, palette). There is no PRD/ARD for this
project; Task 1 ratifies the new layout into `workflow.md` so the
code tasks have an updated source-of-truth to trace to.

## Summary of Tasks

1. **Ratify layout in workflow.md** — amend the Layout and
   Containers sections to the full-width merged config container,
   condensed dropdown pickers, and the resizable log divider.
2. **Relayout skeleton** — fixed four-band geometry: header /
   full-width config band (run controls relocated into its right
   region) / Build Log + Live Feed split filling vertical / footer.
3. **Compact config internals** — condense blueprint/preset/target
   lists into dropdown pickers and lay the form out in columns so
   the band shrinks to ~3 rows and the logs gain vertical space.
4. **Per-panel empty-state wordmark** — both logs show the centered
   ostrich wordmark + themed caption while empty.
5. **Resizable log divider** — a draggable splitter between the two
   logs, default 50/50, ratio held in UI-internal state.

## Task Dependency Relationships

```
T1  Ratify layout in workflow.md (docs)
 |
 v
T2  Relayout skeleton (four-band geometry)
 |
 +--> T3  Compact config internals (dropdowns + columns)
 |
 +--> T4  Per-panel empty-state wordmark
 |
 +--> T5  Resizable log divider

T3, T4, T5 each depend only on T2 and are independent of one
another (any order once T2 lands).
```

## Detailed Tasks

### Task 1 - Ratify layout in workflow.md

- **Status**: pending
- **Blocked by**: none
- **User stories covered**: n/a (no PRD; design ratification)

#### What to build

Amend `context/workflow.md` so the documented layout matches the
new design before any code is written. The current Layout section
describes two side-by-side upper panels (Run Configuration
upper-left, Control/Status strip upper-right); the new design
merges them into a single full-width container and condenses the
discovery lists into dropdowns.

Update, in `workflow.md`:

- The **Layout** ASCII diagram and the "Containers, top to bottom"
  list: the upper region becomes one **full-width XCODE PROJECT
  CONFIGURATION** container sitting between the connection bar and
  the logs, holding the run-config form on the left/center and the
  Control/Status cluster on its right.
- **Containers in detail** (Run Configuration, Control/Status
  strip): note they are now one container; the project picker, the
  preset selector, and the target selector are **dropdown pickers**
  (project keeps a hand-editable path with a picker popup — see
  the manual-fallback value), with scan-root + SCAN HOST living
  inside the project picker popup.
- **Build Log / Device Log**: record that the divider between them
  is **user-resizable** (default 50/50), a deliberate soft
  exception to the "panels locked, no user rearrange in MVP" rule,
  and that each panel shows the ostrich wordmark + its empty-state
  caption when it has no content.

#### Technical Details

Doc-only change. Keep within `workflow.md`'s authority (structure
and flow); do not restate `theme.md` copy or `design.md`
constraints. Preserve the existing hard-wrap width and section
style. No change to `theme.md` is required — its empty-state copy
and lexicon already cover the wordmark captions; layout structure
is `workflow.md`'s alone.

#### Acceptance criteria

- [ ] `workflow.md` Layout diagram shows header / full-width config
      / Build Log + Live Feed / footer, with controls inside the
      config container.
- [ ] Containers-in-detail text describes the merged full-width
      config container and the dropdown pickers (project keeps a
      manual path escape hatch).
- [ ] The resizable Build/Live divider is recorded as a deliberate
      exception to the locked-panels MVP rule.
- [ ] No structural claim contradicts `design.md`; no `theme.md`
      copy is duplicated.

### Task 2 - Relayout skeleton (four-band geometry)

- **Status**: pending
- **Blocked by**: T1
- **User stories covered**: n/a; realizes `workflow.md` Layout

#### What to build

Replace the current ONLINE panel arrangement (centered-floating
recon panel + right-side run panel that stacks controls over both
logs) with a fixed four-band layout that fills the window and
resizes with it:

- **Header** — the existing connection bar, thin, full width, top
  (unchanged).
- **Config band** — one full-width container directly below the
  header. This task renders the existing recon form into it as-is
  (still a vertical stack, just full width) and **relocates the
  run-control cluster** — phase label, `BUILD ▷ INSTALL ▷ LAUNCH`
  progression, `▶ EXECUTE` / `COMPILE` / `■ ABORT`, and the
  `PAYLOAD STALE` flag — out of the run panel and into the **right
  region** of this band.
- **Build Log (left) + Live Feed (right)** — two separate
  side-by-side containers that split the full width below the
  config band and fill the vertical space down to the footer. Each
  keeps its header and COPY/CLR controls. Fixed 50/50 this task.
- **Footer** — the existing slim diagnostics strip, full width,
  bottom (unchanged).

Internal redesign of the config form (dropdowns/columns), the
per-panel empty art, and the resizable divider are explicitly out
of scope here — they land in T3/T4/T5. The current single empty
caption per log is retained for now.

#### Technical Details

All changes are in `src/ui/ui.cpp`. The existing immediate-mode,
manually-positioned `Begin`-window approach (compute pos/size from
`io.DisplaySize`) is kept — no actual ImGui docking is used. Add a
single geometry step in `ui_frame` that derives the four rects
(header height from text + frame padding as today; config band a
fixed compact height; logs filling the gap to the footer; build vs
live a 50/50 horizontal split).

- Split `draw_run_panel` into the run-control cluster (moves into
  the config band) and two log panels (`draw_build_log` /
  `draw_live_feed`, or one helper called twice). Reuse `UiRunView`
  (`build_log`, `device_log`, `phase`, `stale`, `readiness`) and
  `UiRunIntents` unchanged.
- Reposition `draw_recon_panel` to the full-width config band.
- Keep the full-window resting wordmark as the pre-connect
  backdrop (it is covered by the bands once ONLINE).
- **No changes to `include/ui.h`** (same view-models/intents) and
  **no changes to `src/app/app.c`** — this is presentation only.
- Per `coding_standards.md`: `ui` stays `libui.a` with C++
  internals behind the pure-C `ui.h`; no new arenas or allocations
  (geometry uses stack locals); UI-thread-confined.

#### Acceptance criteria

- [ ] Header spans full width at top; config band spans full width
      directly below it; Build Log and Live Feed sit side-by-side
      below the config band and fill the vertical space to the
      footer; footer spans full width at the bottom.
- [ ] Build Log is on the left, Live Feed on the right.
- [ ] The run controls (EXECUTE/COMPILE/ABORT, phase,
      progression, STALE) render inside the config band's right
      region; the log panels no longer contain controls.
- [ ] No panels overlap; the layout reflows correctly when the
      window is resized.
- [ ] EXECUTE/COMPILE/ABORT and the recon actions still drive the
      same behavior (intents unchanged).
- [ ] `include/ui.h` and `src/app/app.c` are unchanged.
- [ ] `make` builds and `make test` passes (ui_test headless smoke
      renders the new layout without crashing).

### Task 3 - Compact config internals (dropdowns + columns)

- **Status**: pending
- **Blocked by**: T2
- **User stories covered**: n/a; realizes `workflow.md`
  Containers-in-detail + the discovery dropdown intent

#### What to build

Condense the always-visible scrolling lists in the config band into
dropdown pickers and arrange the fields in columns so the band
shrinks to ~3 short rows, maximizing log height:

- **PROJECT** — a hand-editable path field plus a `[v]` picker
  button. The picker popup is the whole "find a project" surface:
  the SCAN ROOT input, the `⌖ SCAN HOST` / `■ ABORT SCAN` button,
  and the scanned blueprints (with the `BLUEPRINTS RECOVERED`,
  `// NO BLUEPRINTS`, `XCODE NOT FOUND`, and
  `COULD NOT READ INVENTORY` states). Selecting a blueprint
  prefills the path (and triggers the existing scheme/config/
  bundle read). Typing a path by hand remains fully supported —
  honoring the recorded manual-fallback value.
- **PRESET** — a dropdown selector plus new / rename / delete
  controls (the existing popups), with the `// NO OPERATION
  CONFIGURED` empty state.
- **TARGET** — a picker fed by `↻ SWEEP`, listing swept devices/
  simulators with their booted markers, plus the `// NO TARGETS IN
  RANGE`, `SWEEPING…`, and sweep-error states.
- **SCHEME / CONFIG / BUNDLE ID** — kept as prefilled-editable text
  inputs (not dropdowns) with the discovered-set hint tooltips, laid
  out in a column.
- **Control / Status** — EXECUTE/COMPILE/ABORT, phase label,
  progression, READY indicator, and STALE flag grouped in the
  band's right region.

#### Technical Details

`src/ui/ui.cpp` only. This reuses every existing recon/run intent —
`scan`, `abort_scan`, `pick_blueprint`, `pick_preset`,
`preset_new/rename/delete`, `sweep`, `pick_target`,
`scheme_edited`, `config_edited`, `bundle_id_edited`,
`execute`/`compile`/`abort_run` — so **no changes to `ui.h` or
`app.c`** are required; only the widgets that emit them change
(inline `Selectable` lists become `BeginCombo`/popup pickers).

- Use `ImGui::BeginCombo`/popups for preset and target; for project
  use a text `InputText` + a small button opening a popup that
  contains the scan controls and the blueprint `Selectable` list.
- Lay fields across the width with `ImGui::SameLine`/column groups
  rather than a single vertical stack; keep label colors/styles per
  the existing palette usage.
- Per `coding_standards.md`: still no new arenas/allocations
  (the COPY-to-clipboard `new[]/delete[]` already present in
  ui.cpp is untouched); pure-C `ui.h` boundary preserved.

#### Acceptance criteria

- [ ] PROJECT is a typable path field with a `[v]` picker popup
      containing scan-root + SCAN HOST + the scanned blueprints;
      hand-typed paths work without being overwritten.
- [ ] PRESET and TARGET are dropdown pickers; preset new/rename/
      delete and `↻ SWEEP` still work; empty/error states render
      with the correct lexicon copy.
- [ ] SCHEME/CONFIG/BUNDLE remain editable inputs with discovered-
      set hint tooltips.
- [ ] READY and the run controls are grouped in the band's right
      region; the config band is ~3 rows tall and the logs visibly
      gain vertical space.
- [ ] All existing recon and run intents fire exactly as before
      (verified via the app and ui_test); `ui.h` and `app.c`
      unchanged.
- [ ] `make` builds and `make test` passes.

### Task 4 - Per-panel empty-state wordmark

- **Status**: pending
- **Blocked by**: T2
- **User stories covered**: n/a; realizes the requested empty-log
  art + `theme.md` empty-state copy

#### What to build

When a log panel has no content, render the ostrich wordmark
(`LEX_WORDMARK`) centered in the panel with its themed empty-state
caption beneath it — independently per panel:

- **Build Log** empty → wordmark + `// NO PAYLOAD COMPILED`
  (`LEX_RUN_BUILD_EMPTY`).
- **Live Feed** empty → wordmark + `// NO SIGNAL — TARGET DARK`
  (`LEX_RUN_DEVICE_EMPTY`).

Each panel's art is replaced by its log lines the instant that
panel has any content; the other panel is unaffected (one may show
art while the other streams).

#### Technical Details

`src/ui/ui.cpp` only, in the two log-panel render paths from T2.
Gate on `logbuf_count(...) == 0`. Center the multi-line
`LEX_WORDMARK` horizontally (and roughly vertically) within the
panel's content region using `CalcTextSize` + `SetCursorPos`, in
`C_CYAN` (matching the resting wordmark), with the caption in the
existing dim style. Reuse the existing lexicon strings; add no new
copy. No `ui.h`/`app.c` changes.

#### Acceptance criteria

- [ ] An empty Build Log shows the centered wordmark with
      `// NO PAYLOAD COMPILED` beneath it.
- [ ] An empty Live Feed shows the centered wordmark with
      `// NO SIGNAL — TARGET DARK` beneath it.
- [ ] As soon as a panel receives content its art disappears and
      log lines render; the other panel's empty art is unaffected.
- [ ] Wordmark uses the existing `LEX_WORDMARK` and palette; no new
      strings introduced.
- [ ] `make` builds and `make test` passes (ui_test exercises both
      the empty and populated render paths).

### Task 5 - Resizable log divider

- **Status**: pending
- **Blocked by**: T2
- **User stories covered**: n/a; realizes the resizable-divider
  decision recorded in T1

#### What to build

Make the vertical divider between the Build Log (left) and Live
Feed (right) user-draggable. It defaults to 50/50, updates both
panels live as the user drags, and the chosen ratio persists across
frames for the session. The ratio is clamped so neither panel can
be dragged below a sane minimum width.

#### Technical Details

`src/ui/ui.cpp`. Hold the split ratio as UI-internal state (a
`float` in the `Ui` struct, e.g. `log_split = 0.5f`, set in
`ui_init`) rather than a file-backed setting — session-only is
sufficient and avoids touching `store`/`app.c`. Drive the drag with
an ImGui splitter handle (an `InvisibleButton` + `IsItemActive`
delta, or `ImGui::Splitter`-style helper) between the two log
windows; clamp the ratio to a min/max and recompute the two log
rects from it each frame in the T2 geometry step. No `ui.h` public
contract change is needed (the field is private to the `Ui`
struct). Per `coding_standards.md`: no allocation; UI-thread state.

#### Acceptance criteria

- [ ] A draggable divider sits between Build Log and Live Feed;
      dragging it resizes both panels live.
- [ ] The split defaults to 50/50 and the dragged ratio persists
      across frames within the session.
- [ ] The ratio is clamped so neither log collapses below a usable
      minimum width.
- [ ] No public `ui.h` change and no `app.c`/`store` change (ratio
      is UI-internal session state).
- [ ] `make` builds and `make test` passes.

## Conformance checklist (coding_standards.md)

- [x] **Arenas named + lifetimes stated** — no new arenas; this is
      a presentational refactor that reuses existing buffers/view-
      models. (N/A: no new allocating code.)
- [x] **Allocation is caller-controlled** — no new allocating
      functions introduced.
- [x] **Thread-confinement respected** — all changes run on the UI
      thread; no new cross-thread data (existing log handoff is
      untouched).
- [x] **Non-arena allocations flagged + justified** — none added;
      the pre-existing clipboard `new[]/delete[]` in ui.cpp is not
      modified.
- [x] **Module → library decisions made** — no new module; `ui`
      remains `build/libui.a`.
- [x] **Library layout specified** — unchanged: `include/ui.h`
      public, `src/ui/*.cpp` private, `build/libui.a` archive.
- [x] **C/C++ seam identified** — all work is inside the C++
      `src/ui/ui.cpp`; `include/ui.h` stays pure C and is
      unchanged.
- [x] **Error handling shape confirmed** — no new fallible API; no
      hidden error state introduced.
- [x] **Test approach per library** — `tests/ui_test.c` (black-box
      via `ui.h`) stays a headless smoke test, extended to render
      the new layout's representative states; `make test` must pass
      for every task.
