# Implementation Plan - Keybinds

Application-wide keyboard shortcuts for the most common ostrich
actions. This project has no PRD or ARD; the requirements are stated
directly below and were resolved through a grilling session.

## Resolved requirements

These five keybinds are in scope:

1. **Ctrl+Enter (global) -> EXECUTE.** From anywhere in the app,
   run the full build -> install -> launch chain. Mirrors the
   "> EXECUTE" button exactly (full chain, not COMPILE).
2. **Keychain modal: Enter -> submit, Escape -> skip.** "The
   keychain form" is the mid-execute KEYCHAIN passphrase modal
   (`draw_kc_modal`), NOT the BREACH connection overlay. Enter sets
   `kc_submit`; Escape sets `kc_skip` (the existing "proceed without
   passkey" path).
3. **`v` -> toggle the project dropdown.** Full toggle (opens when
   closed, closes when open). Only fires when no textbox is focused
   (`!io.WantTextInput`).
4. **Ctrl+Backspace (global) -> clear the Device Log.** Clears the
   Device Log / Live Feed (the existing destructive `device_log_clear`
   -> `logbuf_clear`). "The actual logs" means the Build Log, which is
   left untouched.
5. **Ctrl+Escape (global) -> CLOSE the current connection.** Mirrors
   the CLOSE button exactly (no confirmation), valid only when
   ONLINE / REACQUIRING.

Key design decisions from grilling:

- **No new intents, no new app/session/library code.** Every keybind
  maps onto an intent that already exists
  (`execute`, `close`, `device_log_clear`, `kc_submit`, `kc_skip`)
  or is a pure-UI popup toggle (`v`). All changes land in
  `src/ui/ui.cpp`.
- **Silent no-op when unavailable.** Each chord respects its button's
  guard; if the button would be disabled/absent, the chord does
  nothing (no toast, no error).
- **Modal owns the keyboard.** While the KEYCHAIN modal is open, the
  three global chords (Ctrl+Enter, Ctrl+Escape) and `v` are
  suppressed so the modal's Enter/Escape take precedence.
- **Edge-triggered chords.** Use `ImGui::IsKeyChordPressed` (fires
  once per press), not held-key polling, evaluated after
  `ImGui::NewFrame()`.

## Coding-standards conformance

- **Arenas / allocation / threads:** Not applicable. No ostrich-owned
  allocation, no new threads, no cross-thread handoff is introduced;
  the feature only sets pre-existing intent flags read by the app
  loop within the same frame.
- **Module -> library:** No new modules. All edits are inside the
  existing UI library (`src/ui/ui.cpp`, sealed behind the pure-C
  `include/ui.h`). No public-header changes are required because no
  new intents are added.
- **C/C++ seam:** Respected. The keybind logic lives entirely in the
  C++ ImGui internals of the UI library; `src/app/` and `include/`
  are untouched.
- **Error handling:** Not applicable; no fallible functions are
  added.
- **Testing:** Verified manually by running the app (UI-only,
  ImGui-input behavior). The repo has no ImGui input-simulation
  harness and none is added; `make test` must continue to pass as a
  non-regression gate for every task.

## Summary of Tasks

1. Global chord handler + Ctrl+Enter (EXECUTE), Ctrl+Escape (CLOSE),
   Ctrl+Backspace (clear Device Log).
2. KEYCHAIN modal: Enter -> submit, Escape -> skip.
3. `v` toggles the project dropdown, focus-guarded.

## Task Dependency Relationships

```
        +---------------------------------------------+
        | Task 1: global chord handler + 3 chords     |
        | (Ctrl+Enter / Ctrl+Escape / Ctrl+Backspace) |
        +---------------------------------------------+

        +-----------------------------+
        | Task 2: KEYCHAIN Enter/Esc  |   (independent)
        +-----------------------------+

        +-----------------------------+
        | Task 3: `v` dropdown toggle |   (independent)
        +-----------------------------+
```

All three tasks are independent and can be done in any order. Task 1
groups the three global Ctrl-chords because they share a single new
handler and the same modal-suppression guard.

## Detailed Tasks

### Task 1 - Global chord handler + the three global Ctrl-chords

- **Status**: done
- **Blocked by**: none
- **User stories covered**: requirements 1, 4, 5 (Ctrl+Enter
  EXECUTE, Ctrl+Backspace clear Device Log, Ctrl+Escape CLOSE)

#### What to build

A single application-wide keyboard handler, invoked once per frame
after `ImGui::NewFrame()` (so ImGui key state is current), that wires
three global chords onto already-existing intents:

- **Ctrl+Enter -> EXECUTE.** Set `run_intents->execute = true` only
  when the EXECUTE precondition holds (connection ONLINE,
  `readiness == READY_OK`, not already in a run chain) -- i.e. the
  same condition that enables the button at `src/ui/ui.cpp:833`. If
  the precondition is false, do nothing. Accept both `ImGuiKey_Enter`
  and `ImGuiKey_KeypadEnter`. This is the full chain (EXECUTE), never
  COMPILE.
- **Ctrl+Backspace -> clear Device Log.** Set
  `run_intents->device_log_clear = true` (the same intent the
  `CLR##dev` button sets at `src/ui/ui.cpp:1059`). The Build Log is
  left untouched.
- **Ctrl+Escape -> CLOSE connection.** Set `conn_intents->close =
  true` (the same intent the CLOSE button sets at
  `src/ui/ui.cpp:1243`) only when the connection phase is
  `CONN_ONLINE` or `CONN_REACQUIRING`. No confirmation.

All three chords are **suppressed while the KEYCHAIN modal is open**
so the modal keeps ownership of Enter/Escape.

#### Technical Details

- New static helper in `src/ui/ui.cpp` (e.g.
  `handle_global_keys(...)`) taking the connection view, run view,
  and the connection + run intent out-structs. Call it from
  `ui_frame()` after `ImGui::NewFrame()` and after the intent structs
  are zeroed (`src/ui/ui.cpp:1366-1375`), before/around the panel
  draws.
- Use `ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | <key>)` for
  edge-triggered detection; `ImGuiMod_Ctrl` covers both Ctrl keys.
- Modal-open detection: `ImGui::IsPopupOpen("##kc_vault")` (the
  KEYCHAIN modal id from `src/ui/ui.cpp:1113/1123`). When open, return
  early from the handler.
- Ctrl-chords need no `WantTextInput` guard -- a Ctrl modifier means
  they are safe to fire even while a text field is focused, matching
  "no matter where I am."
- This reuses the existing app-side intent processing
  (`src/app/app.c` execute/close/device_log_clear handlers); no
  app-layer changes.

#### Acceptance criteria

- [ ] Ctrl+Enter from any panel/field starts a full EXECUTE when (and
      only when) the EXECUTE button would be enabled; it is a no-op
      otherwise.
- [ ] Ctrl+Enter never triggers COMPILE.
- [ ] Ctrl+Backspace clears the Device Log feed and leaves the Build
      Log unchanged.
- [ ] Ctrl+Escape closes the connection exactly like the CLOSE
      button, only when ONLINE/REACQUIRING, with no confirmation.
- [ ] All three chords are inert while the KEYCHAIN modal is open.
- [ ] Chords are edge-triggered (one action per physical press, no
      auto-repeat while held).
- [ ] `make test` passes; behavior confirmed by running the app.

### Task 2 - KEYCHAIN modal: Enter submits, Escape skips

- **Status**: pending
- **Blocked by**: none
- **User stories covered**: requirement 2 (keychain form Enter/Escape)

#### What to build

Inside the KEYCHAIN passphrase modal (`draw_kc_modal`,
`src/ui/ui.cpp:1111`), add keyboard equivalents for its two buttons:

- **Enter -> submit.** Same effect as the ENTER button
  (`run_intents->kc_submit = true` + close popup,
  `src/ui/ui.cpp:1163-1166`).
- **Escape -> skip.** Same effect as the SKIP button
  (`run_intents->kc_skip = true` + close popup,
  `src/ui/ui.cpp:1169-1172`).

#### Technical Details

- The passkey `InputText` already auto-focuses on appearing
  (`SetKeyboardFocusHere`, `src/ui/ui.cpp:1147`). Add
  `ImGuiInputTextFlags_EnterReturnsTrue` to the passkey field (or
  detect `ImGui::IsKeyPressed(ImGuiKey_Enter)` /
  `ImGuiKey_KeypadEnter` within the open modal) to fire `kc_submit`.
- Detect `ImGui::IsKeyPressed(ImGuiKey_Escape)` within the open modal
  to fire `kc_skip`.
- On either, call `ImGui::CloseCurrentPopup()` so the modal dismisses
  exactly as the button paths do.
- Pure UI; reuses existing `kc_submit` / `kc_skip` intents handled in
  `src/app/app.c`.

#### Acceptance criteria

- [ ] With the KEYCHAIN modal open, Enter submits the passphrase
      (identical to the ENTER button).
- [ ] With the KEYCHAIN modal open, Escape skips (identical to the
      SKIP button).
- [ ] The modal dismisses on both, and the corresponding intent is
      delivered to the app exactly once.
- [ ] `make test` passes; behavior confirmed by running the app.

### Task 3 - `v` toggles the project dropdown (focus-guarded)

- **Status**: pending
- **Blocked by**: none
- **User stories covered**: requirement 3 (`v` toggles project
  dropdown when no textbox focused)

#### What to build

Make the `v` key toggle the project picker popup that is opened today
only by the `[v]` button (`ImGui::OpenPopup("##project_picker")`,
`src/ui/ui.cpp:463`). Full toggle:

- If the popup is closed and no textbox is focused, `v` opens it.
- If the popup is open, `v` closes it.

The key only fires when **no ImGui text field is capturing input**
(`!ImGui::GetIO().WantTextInput`) and is suppressed while the
KEYCHAIN modal is open. Because the recon panel (and thus the picker)
only exists when ONLINE, `v` is naturally inert before connection.

#### Technical Details

- Edit the recon panel region around `src/ui/ui.cpp:450-537`.
- Open: when `v` is pressed and `!io.WantTextInput` and the picker is
  not already open, call `ImGui::OpenPopup("##project_picker")`.
- Close: detect the toggle while
  `ImGui::IsPopupOpen("##project_picker")` is true and request a
  close -- e.g. carry a one-frame "close requested" flag consumed
  inside the `BeginPopup("##project_picker")` block via
  `ImGui::CloseCurrentPopup()` (close can only be issued from inside
  the popup scope).
- Use `ImGui::IsKeyPressed(ImGuiKey_V)` with no Ctrl/Alt/Super held
  so it is a plain `v`, not part of another chord.
- Guard against the KEYCHAIN modal:
  `ImGui::IsPopupOpen("##kc_vault")` -> do nothing.
- Pure UI; no intent or app change (the picker open/close is entirely
  UI-local; selecting a blueprint still uses the existing
  `pick_blueprint` intent).

#### Acceptance criteria

- [ ] Pressing `v` with no textbox focused opens the project
      dropdown; pressing `v` again closes it.
- [ ] Typing `v` into any text field (BREACH fields, scheme/config/
      bundle-id, scan-root, keychain passkey) inserts the character
      and does NOT toggle the dropdown.
- [ ] `v` is inert while the KEYCHAIN modal is open and before the
      connection is ONLINE.
- [ ] `make test` passes; behavior confirmed by running the app.
