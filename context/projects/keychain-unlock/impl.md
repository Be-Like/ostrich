# Implementation Plan — Keychain unlock for codesign

## Summary of Tasks

1. **T1 — Data plumbing (libbuilddeploy + librunstate +
   liblexicon).** Add the new `BdStatus` value, three pure
   text/command helpers in `libbuilddeploy`, the new
   `LEX_REC_ERR_KC_UNLOCK` lexicon key, the two new
   `RunEvent`s and their state-machine transitions, and the
   matching `bd_status_str` / `bd_reason_lex` /
   `runstate_reason_lex` mappings. Ships dead-but-safe: no
   caller produces `BD_ERR_UNLOCK_FAILED` and no producer
   fires `RUN_EV_UNLOCK_OK` / `RUN_EV_UNLOCK_FAIL` until T3.
2. **T2 — libstore: `Conn` record extension.** Add
   `kc_remember` and `kc_passkey[256]` to `Conn`; extend the
   line-based serializer/deserializer; keep backward-compat
   via a hardcoded legacy-file fixture. Ships dead-but-safe:
   the app does not yet read or write the new fields. Adds
   the persisted-state bullet to `workflow.md`.
3. **T3 — libsession: `RCMD_SET_KC_PASS` + unlock chain
   step.** Add the new command kind, the `kc_pass[256]` field
   on `SessionRunCmd`, the `session_set_kc_pass` public API,
   the `WorkerCtx.kc_pass` slot, the front-of-`RunChain`
   `unlock` step (gated on non-empty `kc_pass`), the
   success/failure emission paths, and the `setsid`+PID-marker
   envelope so the ABORT path still works. Extend
   `ssh_stub_run.c` so `session_run_test.c` can script the
   unlock channel. Ships dead-but-safe in the running app:
   nothing enqueues `RCMD_SET_KC_PASS` yet, so the worker
   never enters the unlock step. Updates the
   `xcode-project-build-and-deploy/ard.md` RunChain shape
   note + adds a "See also" pointer.
4. **T4 — libui: keychain passkey modal.** Add `KcForm`, the
   `show_kc_prompt` view bit, the `kc_submit` / `kc_skip`
   intents, the `ui_frame` signature extension, the ImGui
   modal in `src/ui/`, and the modal-label lexicon keys. Ships
   dead-but-safe: the app does not set `show_kc_prompt`, so
   the modal never opens. Adds the modal-label keys to
   `theme.md`.
5. **T5 — app: gating cascade + lifecycle wiring.** Add
   `app.kc_pass_cache[256]`, wire the four-branch cascade on
   the EXECUTE/COMPILE intent, the modal `ENTER` and `SKIP`
   handlers, the `REV_PHASE(RUN_BUILD_FAILED,
   BD_ERR_UNLOCK_FAILED)` cache-clear, and the disconnect
   `memset`. Builds on T2/T3/T4. **First end-to-end working
   state**: a non-simulator EXECUTE against a Mac with a
   locked `login.keychain` now opens the modal, accepts a
   passkey, runs the unlock step at the front of the chain,
   and proceeds through the build cleanly.
6. **T6 — H2 codesign hint emission + README.** Add the
   structural H2 hint emission in the BUILD-fail path
   (`!is_simulator && kc_pass == "" && BD_ERR_BUILD`); land
   the negative-case tests; add the README "Remote Mac" and
   "Known issues" entries. Ships last so the hint text
   ("press EXECUTE; the keychain passkey modal will appear")
   is honest the moment it appears.

## Task Dependency Relationships

```
T1 ──┐
T2 ──┤
T3 ──┼──→ T5 ──→ T6
T4 ──┘            ↑
                  └── also depends on T1
                      (uses bd_codesign_hint_block)

(T3 also depends on T1: the unlock step calls
 bd_unlock_cmd / bd_unlock_help_block and passes
 BD_ERR_UNLOCK_FAILED into fail_run_chain.)
```

T1, T2, and T4 are pairwise independent and can ship in any
order. T3 is blocked by T1 because the unlock step's
emission path consumes `bd_unlock_cmd`,
`bd_unlock_help_block`, the `BD_ERR_UNLOCK_FAILED` enum
value, and the `RUN_EV_UNLOCK_*` events. T5 is the first
end-to-end-firing task and is blocked by T2 (needs the
`Conn` fields it reads/writes), T3 (needs
`session_set_kc_pass`), and T4 (needs `show_kc_prompt` and
the `kc_submit` / `kc_skip` intents). T6 is blocked by T5
(modal must exist before the hint that references it
becomes visible) and by T1 (uses
`bd_codesign_hint_block`).

## Detailed Tasks

### Task 1 — Data plumbing (libbuilddeploy + librunstate + liblexicon)

- **Status**: done
- **Blocked by**: none
- **User stories covered**: n/a (no PRD; goal sourced from
  ARD `## PRD` section's one-line goal)

#### What to build

The dead-but-safe foundation layer the rest of the sequence
builds on: three pure helpers in `libbuilddeploy`, a new
`BdStatus` value, a new lexicon key, two new `RunEvent`s
with state-machine transitions, and the full
`bd_status_str` / `bd_reason_lex` / `runstate_reason_lex`
coverage so downstream tasks can wire failures into the
existing UI lexicon path without touching the data layer
again.

End-to-end behavior after T1: callers of `libbuilddeploy`
can construct the keychain-unlock SSH command, render the
F1 unlock-rejected help block, and render the H2 codesign
hint block — all into caller-supplied buffers. `BdStatus`
exposes `BD_ERR_UNLOCK_FAILED` with matching status/reason
companions. `librunstate` accepts `RUN_EV_UNLOCK_OK`
(no-op, stays in `RUN_BUILDING`) and `RUN_EV_UNLOCK_FAIL`
(transitions to `RUN_BUILD_FAILED`, returns `RUN_ACT_DONE`).
Nothing in the running application fires these yet — they
sit unused but fully tested. The app remains buildable,
all existing tests pass, and shipping the binary in this
state is safe (no runtime behavior change).

#### Technical Details

See ARD `## Explanation of Architectural Components` →
"What changes" → item 3 (the three helpers, including the
exact text contracts for the F1 and H2 blocks) and item 4
(the two new `RunEvent`s and their phase semantics). See
ARD `## Interfaces` → `include/builddeploy.h` for the
prototypes, and `include/runstate.h` for the event/phase
shape. See ARD `### Failure-detection semantics` for the
structural-signal rationale that both helpers serve.

Files touched:

- `include/builddeploy.h` — add `BD_ERR_UNLOCK_FAILED` to
  the `BdStatus` enum (after `BD_ERR_SETSID_MISSING`,
  before `BD_ERR_OOM`); add the three prototypes
  (`bd_unlock_cmd`, `bd_unlock_help_block`,
  `bd_codesign_hint_block`).
- `src/builddeploy/builddeploy.c` — implement the three
  helpers. `bd_unlock_cmd` mirrors `bd_build_cmd`'s
  envelope: `setsid sh -c '…'` wrapping the
  `printf "__OSTRICH_PGID__%d\n" $$` PID-marker prelude,
  then the `security unlock-keychain` invocation with the
  passkey passed through an `__BD_KC_PASS=` env-var
  assignment that the inner shell expands under double
  quotes. The passkey is single-quote-escaped via the
  existing `bd_quote` helper. The keychain path is the
  literal `"$HOME/Library/Keychains/login.keychain-db"`
  per ARD `## Out of Scope` (other keychain paths are out
  of scope). `bd_unlock_help_block` and
  `bd_codesign_hint_block` are single-render `snprintf`-
  style functions branching on `port == 22` for the two
  `ssh` line shapes; the rule strings and headers match
  ARD `### Help-block content (illustrative)` verbatim.
  Add `BD_ERR_UNLOCK_FAILED` cases to `bd_status_str`
  (returns `"keychain unlock failed"`) and
  `bd_reason_lex` (returns `LEX_REC_ERR_KC_UNLOCK`).
- `include/lexicon.h` — add `LEX_REC_ERR_KC_UNLOCK` enum
  member next to `LEX_REC_ERR_SETSID`.
- `src/lexicon.c` — add the matching table entry; preserve
  the existing index-by-position layout. The modal-label
  lexicon keys (`KEYCHAIN PASSKEY`, `REMEMBER KEYCHAIN`,
  modal title, etc.) are **not** added here — they are
  T4's concern, since they only have a consumer once the
  modal exists.
- `include/runstate.h` — add `RUN_EV_UNLOCK_OK` and
  `RUN_EV_UNLOCK_FAIL` to the `RunEvent` enum (positioned
  near `RUN_EV_BUILD_OK` / `RUN_EV_BUILD_FAIL` for
  proximity to their semantic peers; appending at the
  end is acceptable if test enum-coverage is positional).
- `src/runstate/runstate.c` — extend `runstate_step` so
  `RUN_EV_UNLOCK_OK` from `RUN_BUILDING` returns
  `RUN_ACT_NONE` and keeps `RUN_BUILDING`;
  `RUN_EV_UNLOCK_FAIL` from `RUN_BUILDING` transitions
  `rs->phase = RUN_BUILD_FAILED` and returns
  `RUN_ACT_DONE` (same shape as the existing
  `RUN_EV_BUILD_FAIL` branch). Extend
  `runstate_reason_lex` so
  `(RUN_BUILD_FAILED, BD_ERR_UNLOCK_FAILED)` returns
  `LEX_REC_ERR_KC_UNLOCK`. All other transitions and
  reason mappings are unchanged.
- `tests/builddeploy_test.c` — new test cases per ARD
  `### Testing approach`:
    - `bd_unlock_cmd`: shell escaping of the passkey
      (single quotes inside the passkey, backslashes,
      spaces, empty string returning a non-empty command
      that the inner shell will interpret as an empty
      passkey); `BD_ERR_OOM` when `cap` is too small; the
      constructed command wraps the unlock in the
      `setsid sh -c '…'` envelope with the PID-marker
      prelude; the `security unlock-keychain` invocation
      references the
      `~/Library/Keychains/login.keychain-db` path.
    - `bd_unlock_help_block`: presence of the literal
      `KEYCHAIN UNLOCK REJECTED.` header; presence of
      `ssh <user>@<host>` for default-port and
      `ssh -p <port> <user>@<host>` for non-default;
      presence of the
      `security unlock-keychain ~/Library/Keychains/login.\
      keychain-db` verify command; presence of the
      `── REMEDIATION ──` opener rule and
      `── END REMEDIATION ──` closer rule;
      NUL-termination; `BD_ERR_OOM` on undersized `cap`.
    - `bd_codesign_hint_block`: presence of
      `errSecInternalComponent` and
      `keychain may be locked` phrases; presence of the
      `── HINT ──` / `── END HINT ──` rules;
      NUL-termination; `BD_ERR_OOM` on undersized `cap`.
    - `BD_ERR_UNLOCK_FAILED`:
      `bd_status_str(BD_ERR_UNLOCK_FAILED)` returns a
      non-empty, non-`unknown` string; `bd_reason_lex`
      returns `LEX_REC_ERR_KC_UNLOCK`.
- `tests/runstate_test.c` — assertions per ARD
  `### Testing approach`:
    - `RUN_EV_UNLOCK_OK` from `RUN_BUILDING` keeps
      `RUN_BUILDING` and returns `RUN_ACT_NONE`.
    - `RUN_EV_UNLOCK_FAIL` from `RUN_BUILDING` transitions
      to `RUN_BUILD_FAILED` and returns `RUN_ACT_DONE`.
    - `runstate_reason_lex(RUN_BUILD_FAILED,
      BD_ERR_UNLOCK_FAILED)` returns
      `LEX_REC_ERR_KC_UNLOCK`.
    - Spot-check that existing transitions are
      unchanged (e.g. `RUN_EV_BUILD_OK` from
      `RUN_BUILDING` still returns the same action).

Conformance with `context/coding_standards.md`:

- **Arenas / allocation:** all three helpers take
  `char *buf, size_t cap` — caller-controlled, no hidden
  allocators. No new arenas. Matches existing
  `libbuilddeploy` precedent (`bd_build_cmd`,
  `bd_setsid_help_block`).
- **Thread confinement:** pure functions; no threading
  concerns introduced.
- **Module → library:** change is contained to existing
  libraries (`libbuilddeploy`, `librunstate`,
  `liblexicon`-equivalent plain source); no new
  libraries; no module reorganization.
- **C/C++ seam:** all touched code is pure C11; no C++
  surface added.
- **Error handling:** new functions return `BdStatus`
  (`BD_OK` / `BD_ERR_OOM`) per the project's status-enum
  convention; `bd_status_str` and `bd_reason_lex`
  companions cover the new enum value.
- **Testing:** black-box, through the public headers,
  added to the existing `tests/builddeploy_test.c` and
  `tests/runstate_test.c` binaries.

#### Acceptance criteria

- [ ] `include/builddeploy.h` `BdStatus` enum contains
      `BD_ERR_UNLOCK_FAILED`.
- [ ] `include/builddeploy.h` declares
      `bd_unlock_cmd(const char *kc_pass, char *buf,
      size_t cap)`,
      `bd_unlock_help_block(const char *user,
      const char *host, int port, char *buf,
      size_t cap)`, and
      `bd_codesign_hint_block(const char *user,
      const char *host, int port, char *buf,
      size_t cap)`, all returning `BdStatus`.
- [ ] `bd_unlock_cmd` wraps the `security
      unlock-keychain` invocation in the same
      `setsid sh -c '…'` + PID-marker envelope as
      `bd_build_cmd`; the passkey is passed via
      `__BD_KC_PASS=` env-var assignment; the keychain
      path is literal
      `"$HOME/Library/Keychains/login.keychain-db"`.
- [ ] `bd_unlock_help_block` renders the F1 text contract
      from ARD `### Help-block content (illustrative)`
      verbatim (header, two cause bullets, verify ssh
      line, retry instruction, both rules);
      default-port path emits `ssh <user>@<host>`,
      non-default emits `ssh -p <port> <user>@<host>`.
- [ ] `bd_codesign_hint_block` renders the H2 text
      contract verbatim (hint header, the
      `errSecInternalComponent` mention, the
      "keychain may be locked" sentence, both rules).
- [ ] All three helpers return `BD_ERR_OOM` when `cap` is
      undersized; on `BD_OK` they NUL-terminate the
      buffer.
- [ ] `bd_status_str(BD_ERR_UNLOCK_FAILED)` returns a
      non-empty, non-`unknown` string (e.g.
      `"keychain unlock failed"`).
- [ ] `bd_reason_lex(BD_ERR_UNLOCK_FAILED)` returns
      `LEX_REC_ERR_KC_UNLOCK`.
- [ ] `include/lexicon.h` declares
      `LEX_REC_ERR_KC_UNLOCK`; `src/lexicon.c` has the
      matching table entry; the lexicon table index
      ordering is preserved.
- [ ] `include/runstate.h` `RunEvent` enum contains
      `RUN_EV_UNLOCK_OK` and `RUN_EV_UNLOCK_FAIL`.
- [ ] `runstate_step(rs, RUN_EV_UNLOCK_OK)` from
      `RUN_BUILDING` keeps `rs->phase == RUN_BUILDING`
      and returns `RUN_ACT_NONE`.
- [ ] `runstate_step(rs, RUN_EV_UNLOCK_FAIL)` from
      `RUN_BUILDING` transitions `rs->phase =
      RUN_BUILD_FAILED` and returns `RUN_ACT_DONE`.
- [ ] `runstate_reason_lex(RUN_BUILD_FAILED,
      BD_ERR_UNLOCK_FAILED)` returns
      `LEX_REC_ERR_KC_UNLOCK`.
- [ ] `tests/builddeploy_test.c` covers the new
      assertions enumerated in *Technical Details*
      above.
- [ ] `tests/runstate_test.c` covers the new event
      transitions and the reason-lex mapping.
- [ ] `make` builds cleanly; `make test` is all-green
      (existing assertions carry over unchanged).
- [ ] The application binary, built at this commit, has
      no observable behavior change from the previous
      commit (nothing produces `BD_ERR_UNLOCK_FAILED`,
      fires `RUN_EV_UNLOCK_*`, or calls the three new
      helpers yet).

### Task 2 — libstore: `Conn` record extension

- **Status**: done
- **Blocked by**: none
- **User stories covered**: n/a (no PRD; goal sourced from
  ARD `## PRD` section's one-line goal)

#### What to build

The on-disk and in-memory `Conn` record gains two fields
(`kc_remember`, `kc_passkey[256]`) so the app cascade in T5
has somewhere durable to persist the opt-in keychain
passkey. The format is extended positionally so legacy
files deserialize to safe defaults without migration code.

End-to-end behavior after T2: `store_load` and `store_save`
round-trip the two new fields; legacy files written before
the feature deserialize with `kc_remember = false`,
`kc_passkey = ""`; the saved file is still mode `0600` and
still written atomically. The app does not yet read or
write the new fields, so user-facing behavior is unchanged.

#### Technical Details

See ARD `## Explanation of Architectural Components` →
"What changes" item 6 (the `Conn` extension and the
backward-compat rationale) and ARD `## Interfaces` →
`include/store.h` for the field order. See ARD
`### Doc reconciliation` for the `workflow.md` bullet.

Files touched:

- `include/store.h` — add `bool kc_remember` and
  `char kc_passkey[256]` to the end of the `Conn`
  struct, after the existing `passkey[256]`.
- `src/store/store.c` (or whichever TU houses the
  `Conn` line-encoder/decoder; mirror the existing
  field-emit order) — extend the line writer to emit
  the two new fields at the end of the record; extend
  the line reader to consume them and to default
  `kc_remember = false`, `kc_passkey = ""` when the
  trailing fields are absent. No format version bump;
  no migration helper; defaults are positional-additive.
  Keep the atomic-write + `0600` permission path
  unchanged.
- `tests/store_test.c` — new test cases per ARD
  `### Testing approach`:
    - Serialize/deserialize round-trip for a `Conn` with
      `kc_remember = true` and a non-empty `kc_passkey`:
      bytes survive verbatim across save/load.
    - Serialize/deserialize for `kc_remember = false`,
      `kc_passkey = ""`: empty-by-default invariant
      survives.
    - An ssh-agent `Conn` (`auth == SSH_AUTH_AGENT`)
      with `kc_remember = true` and a non-empty
      `kc_passkey` round-trips correctly (keychain
      passkey is independent of SSH auth method).
    - Backward-compat: a hardcoded legacy-file fixture
      string (the exact bytes a pre-T2 ostrich would
      have written for one `Conn`) is written to a temp
      file via `fwrite`/`fclose`, then loaded via
      `store_load`; the loaded `Conn` has
      `kc_remember == false` and
      `kc_passkey[0] == '\0'`, with the other fields
      matching the fixture.
    - The `0600` permission assertion on the written
      file (assuming the existing test infrastructure
      asserts this) is re-run after writing the
      extended record to confirm the format change did
      not regress the permission policy.
- `context/workflow.md` — append a fourth bullet to the
  persisted-state list per ARD `### Doc reconciliation`
  → `workflow.md`: "**Per-connection opt-in remembered
  keychain passkey** — paired with the existing SSH
  `REMEMBER PASSKEY` opt-in; off by default; plaintext
  with `0600`." Broaden the deferred-list entry on
  "Keychain-backed password storage (macOS Keychain /
  libsecret) as the hardened replacement for opt-in
  plaintext" to cover the keychain passkey in addition
  to the SSH passkey.

Conformance with `context/coding_standards.md`:

- **Arenas / allocation:** `store_load` continues to
  allocate `ConnList.items` from the caller-supplied
  arena — unchanged. The new `Conn` fields are fixed-
  size POD members of an already-allocated struct; no
  new allocator surface.
- **Thread confinement:** unchanged. `store_load` /
  `store_save` run on the UI thread.
- **Module → library:** contained to existing
  `libstore`; no public-header changes beyond the two
  new struct fields.
- **C/C++ seam:** pure C11.
- **Error handling:** existing `StoreStatus` shape and
  `store_status_str` companion cover this path; no
  new failure modes.
- **Testing:** black-box, through the existing
  `tests/store_test.c` binary.

#### Acceptance criteria

- [ ] `include/store.h` `Conn` struct contains
      `bool kc_remember` and `char kc_passkey[256]`,
      positioned after the existing `passkey[256]`
      field.
- [ ] `store_save` followed by `store_load` round-trips
      `kc_remember = true` and a non-empty `kc_passkey`
      verbatim (bytewise equal).
- [ ] Same round-trip with `kc_remember = false`,
      `kc_passkey = ""` preserves the empty-by-default
      shape.
- [ ] An ssh-agent `Conn` (`auth == SSH_AUTH_AGENT`)
      with `kc_remember = true` and a non-empty
      `kc_passkey` round-trips correctly.
- [ ] A hardcoded legacy-file fixture (one `Conn`'s
      worth of bytes in the pre-T2 format) written to a
      temp file and loaded via `store_load` yields a
      `Conn` with `kc_remember == false` and
      `kc_passkey[0] == '\0'`; other fields match the
      fixture.
- [ ] Files written by `store_save` retain mode `0600`
      after the format extension (existing assertion
      pattern, re-exercised).
- [ ] `context/workflow.md` persisted-state list has the
      new keychain-passkey bullet; the deferred-list
      entry on keychain-backed password storage covers
      both the SSH passkey and the keychain passkey.
- [ ] `make` builds cleanly; `make test` is all-green.
- [ ] The application binary, built at this commit, has
      no observable behavior change from the previous
      commit (no production caller reads or writes the
      new fields yet).

### Task 3 — libsession: `RCMD_SET_KC_PASS` + unlock chain step

- **Status**: done
- **Blocked by**: Task 1
- **User stories covered**: n/a (no PRD; goal sourced from
  ARD `## PRD` section's one-line goal)

#### What to build

The session-layer machinery to (a) receive a keychain
passkey from the app via the existing run-command ring,
(b) hold it as worker-confined state for the lifetime of
the session, and (c) run a new `unlock` step at the front
of every `RunChain` when the passkey is non-empty.
Success emits a one-line confirmation to the Build Log
and proceeds to the existing `settings` step; failure
emits the F1 help block and transitions to
`RUN_BUILD_FAILED` with `BD_ERR_UNLOCK_FAILED`. Nothing
in the running app submits `RCMD_SET_KC_PASS` yet, so
the unlock step is never entered in production until T5
lands.

End-to-end behavior after T3: the public session API
exposes `session_set_kc_pass(Session *, const char *)`,
which enqueues `RCMD_SET_KC_PASS` on the run command
ring. The worker drains it before the next `EXECUTE` or
`COMPILE` (FIFO ordering guarantee) and stores the bytes
in `WorkerCtx.kc_pass`. The `RunChain` driver checks
`WorkerCtx.kc_pass` at the top of each chain; non-empty
runs the unlock step first, empty skips it (today's
behavior). The unlock step opens a fresh channel, runs
`bd_unlock_cmd`, tees output through the existing
`emit_build_log` path, and reads the channel's exit
status. On success: `emit_build_log("> KEYCHAIN
UNLOCKED\n", …)`, fire `RUN_EV_UNLOCK_OK`, proceed to
the existing settings step. On failure: render
`bd_unlock_help_block` into a stack-local buffer, emit
via `emit_build_log`, fire `RUN_EV_UNLOCK_FAIL` →
`fail_run_chain(BD_ERR_UNLOCK_FAILED)`, abort the chain
before any expensive step runs. `WorkerCtx.kc_pass` is
zeroed in `session_close` alongside the existing
per-session teardown.

#### Technical Details

See ARD `## Explanation of Architectural Components` →
"What changes" items 1, 2, and 4 (the new chain step,
the new command kind, the new `RunEvent`s); ARD
`### Control + data flow for an unlock-required EXECUTE`
for the precise sequence; ARD `### Worker-side kc_pass
lifetime` for the zeroing rules; ARD
`### Why RCMD_SET_KC_PASS rather than a field on
SessionRunCmd` for the separation-of-concerns rationale.
See ARD `## Interfaces` → `include/session.h` for the
exact prototype shapes.

Files touched:

- `include/session.h` — add `RCMD_SET_KC_PASS` to
  `SessionRunCmdKind`; add `char kc_pass[256]` to
  `SessionRunCmd` (valid only for `RCMD_SET_KC_PASS`);
  add the `session_set_kc_pass` prototype.
- `src/session/session.c` —
    - In the worker's run-command drain, dispatch
      `RCMD_SET_KC_PASS` by copying `cmd.kc_pass`
      into `WorkerCtx.kc_pass` (fixed 256-byte
      member; one `memcpy` + NUL-terminate). The
      handler returns immediately; no chain side
      effects.
    - Add `char kc_pass[256]` to `WorkerCtx` (or
      whichever struct holds the worker-confined
      run-state today). Zero-initialize at session
      start; `memset` to zero in `session_close`
      alongside the existing teardown.
    - Introduce a new `RCHAIN_STEP_UNLOCK` enum value
      (or whichever naming convention the existing
      `RCHAIN_STEP_SETTINGS` family uses) as the
      first step of the chain. The chain driver
      consults `WorkerCtx.kc_pass`: if empty, skip
      directly to `RCHAIN_STEP_SETTINGS` (today's
      starting point); if non-empty, open a fresh
      channel and exec `bd_unlock_cmd(kc_pass, …)`.
    - Reuse the existing channel-stdout pumping so
      unlock-step bytes are teed to `emit_build_log`
      while accumulating to EOF (mirrors the T3 fix
      from setsid-help that put SETTINGS bytes onto
      the Build Log). On channel close, read the
      exit code via the existing `handle_step_exit`
      dispatch.
    - In `handle_step_exit`'s new
      `RCHAIN_STEP_UNLOCK` case:
        - `exit_code == 0`: `emit_build_log(ctx, "> "
          "KEYCHAIN UNLOCKED\n", strlen(...))`;
          `runstate_step(rs, RUN_EV_UNLOCK_OK)` (no
          public `REV_PHASE` emit — phase stays
          `RUN_BUILDING`); advance the chain to
          `RCHAIN_STEP_SETTINGS`.
        - `exit_code != 0`: render
          `bd_unlock_help_block(ctx->cfg.user,
          ctx->cfg.host, ctx->cfg.port, block,
          sizeof(block))` into a stack-local
          `char block[2048]`; on `BD_OK`,
          `emit_build_log(ctx, block, strlen(block))`;
          `fail_run_chain(ctx, RUN_EV_UNLOCK_FAIL,
          BD_ERR_UNLOCK_FAILED)`. The `fail_run_chain`
          helper already emits the
          `REV_PHASE(RUN_BUILD_FAILED, reason)` event
          and tears down the chain.
    - The unlock command must be wrapped in the same
      `setsid` + PID-marker envelope `bd_build_cmd`
      uses (this is `bd_unlock_cmd`'s contract, set
      in T1) so the two-pronged ABORT path also kills
      a stuck unlock. Capture `build_pgid` from the
      marker the same way the existing BUILD step
      does, so ABORT mid-unlock works.
- `tests/ssh_stub_run.c` — extend the configurable stub
  so a scripted exec whose command string contains
  `security unlock-keychain` returns a configurable
  exit code and a configurable stdout payload (mirror
  the existing pattern used to script the SETTINGS /
  BUILD / INSTALL / LAUNCH steps).
- `tests/session_run_test.c` — new test cases per ARD
  `### Testing approach` (the unlock-related subset;
  H2 hint tests land in T6):
    - `RCMD_SET_KC_PASS` with non-empty passkey
      followed by `RCMD_EXECUTE`: drains until
      `RUN_BUILD_FAILED` or `RUN_RUNNING`; the first
      stub-exec recorded must contain
      `security unlock-keychain` (assertion on the
      captured exec string).
    - `RCMD_SET_KC_PASS` with empty passkey (or no
      `RCMD_SET_KC_PASS` at all) followed by
      `RCMD_EXECUTE`: the first stub-exec is the
      existing `xcodebuild -showBuildSettings -json`
      command — the unlock step was skipped.
    - Stub unlock-step exits 0: the chain proceeds to
      SETTINGS; the drained `REV_BUILD_LOG` payload
      contains `> KEYCHAIN UNLOCKED`; no
      `REV_PHASE(RUN_BUILD_FAILED)` is emitted.
    - Stub unlock-step exits non-zero: the drained
      `REV_BUILD_LOG` payload contains
      `KEYCHAIN UNLOCK REJECTED.` and `ssh ` *before*
      the test observes
      `REV_PHASE(RUN_BUILD_FAILED, BD_ERR_UNLOCK_FAILED)`;
      the chain does not advance to SETTINGS
      (`g_exec_next` reflects only the unlock exec).
    - `session_close` after a `SET_KC_PASS` zeroes
      the worker's `kc_pass` (covered indirectly by
      "a fresh session does not inherit kc_pass from
      a prior session" — black-box, observable via
      the unlock-step skip).
- `context/projects/xcode-project-build-and-deploy/ard.md`
  — update the `RunChain` step list (currently
  `settings → build → … → launch`) to
  `unlock → settings → build → … → launch`, with the
  inline note that the new `unlock` step is gated on
  `WorkerCtx.kc_pass != ""`. Add a Further Notes "See
  also" pointer to this project as the in-app
  remediation for the locked-keychain failure mode.

Conformance with `context/coding_standards.md`:

- **Arenas / allocation:** no new arenas. The
  cross-thread `kc_pass[256]` field lives inside the
  existing `SessionRunCmd` record (the previously-
  flagged cross-thread allocation per the
  xcode-build-and-deploy ARD). `WorkerCtx.kc_pass`
  lives inside the existing `WorkerCtx`. The
  help-block buffer is stack-local
  (`char block[2048]`) with function-scoped lifetime
  — same pattern as the setsid-help emit.
- **Thread confinement:** the keychain passkey crosses
  the UI→worker boundary by **copy** into the fixed-
  size POD `SessionRunCmd.kc_pass[256]` field. No
  shared pointer or arena reference crosses the
  boundary. The worker's copy in `WorkerCtx.kc_pass`
  is worker-thread-confined.
- **Module → library:** contained to existing
  `libsession`; public-header surface grows by one
  enum value, one struct field, and one prototype.
- **C/C++ seam:** pure C11 throughout.
- **Error handling:** existing `BdStatus` /
  `SshStatus` enum returns and `fail_run_chain`
  helper cover the new code paths. The unlock step
  emits its failure through the existing
  `REV_PHASE(RUN_BUILD_FAILED, BD_ERR_UNLOCK_FAILED)`
  shape with no new event kinds.
- **Testing:** black-box, through the public session
  API and the configurable SSH stub
  (`tests/ssh_stub_run.c`). No white-box reach-ins.

#### Acceptance criteria

- [ ] `include/session.h` `SessionRunCmdKind` enum
      contains `RCMD_SET_KC_PASS`.
- [ ] `include/session.h` `SessionRunCmd` struct
      contains `char kc_pass[256]`.
- [ ] `include/session.h` declares
      `bool session_set_kc_pass(Session *s,
      const char *kc_pass)`.
- [ ] Calling `session_set_kc_pass(s, "secret")`
      enqueues one `RCMD_SET_KC_PASS` record on the
      run command ring with `kc_pass` bytes equal to
      `"secret"` (asserted indirectly via the worker
      observing the value on the next chain).
- [ ] After the worker drains `RCMD_SET_KC_PASS`, a
      subsequent `RCMD_EXECUTE` triggers a chain that
      starts with the `security unlock-keychain` exec
      (first captured stub exec contains
      `security unlock-keychain`).
- [ ] After the worker observes
      `RCMD_SET_KC_PASS` with `kc_pass = ""` (or no
      `RCMD_SET_KC_PASS` at all), a subsequent
      `RCMD_EXECUTE` triggers a chain whose first exec
      is the existing
      `xcodebuild -showBuildSettings -json`
      invocation.
- [ ] On a stub unlock-step exit 0, the drained
      `REV_BUILD_LOG` payload contains
      `> KEYCHAIN UNLOCKED`; the chain proceeds (next
      exec is the SETTINGS command); no
      `REV_PHASE(RUN_BUILD_FAILED)` is emitted before
      the BUILD step's natural exit.
- [ ] On a stub unlock-step exit non-zero, the
      drained `REV_BUILD_LOG` payload contains both
      `KEYCHAIN UNLOCK REJECTED.` and `ssh ` *before*
      `REV_PHASE(RUN_BUILD_FAILED,
      BD_ERR_UNLOCK_FAILED)` is observed; the chain
      does not advance to SETTINGS (only the unlock
      exec ran).
- [ ] `bd_unlock_cmd`'s `setsid` envelope makes the
      ABORT-during-unlock path observable: aborting
      mid-unlock results in the worker captured pgid
      being non-zero before the channel terminates
      (sanity-check via the existing two-pronged
      ABORT machinery; reuse the existing
      ABORT-during-build assertion pattern).
- [ ] `WorkerCtx.kc_pass` is zeroed in
      `session_close` (observable via a follow-on
      `session_open` + `RCMD_EXECUTE` showing the
      unlock step skipped).
- [ ] `context/projects/xcode-project-build-and-deploy/
      ard.md` `RunChain` description updated to
      `unlock → settings → build → … → launch` with
      the `WorkerCtx.kc_pass != ""` gating note, and
      gains a Further Notes "See also" pointer to
      this project.
- [ ] `make` builds cleanly; `make test` is all-green.
- [ ] The application binary, built at this commit,
      has no observable behavior change from the
      previous commit — nothing in production submits
      `RCMD_SET_KC_PASS`, so `WorkerCtx.kc_pass`
      stays empty and the unlock step is never
      entered.

### Task 4 — libui: keychain passkey modal

- **Status**: done
- **Blocked by**: none
- **User stories covered**: n/a (no PRD; goal sourced from
  ARD `## PRD` section's one-line goal)

#### What to build

The themed ImGui modal the app opens when it needs a
keychain passkey, plus the public C surface the app uses
to drive it. Ships dead-but-safe: the modal renders only
when the app sets `UiRunView.show_kc_prompt = true`, and
no caller does that until T5.

End-to-end behavior after T4: when the app sets
`show_kc_prompt = true` and passes a `KcForm *` into
`ui_frame`, the modal renders with a masked passkey
input field bound to `form->passkey`, a `REMEMBER
KEYCHAIN` checkbox bound to `form->remember`, and two
buttons (`ENTER` and `SKIP`). User keystrokes mutate
`form->passkey` / `form->remember` in place. Pressing
`ENTER` writes `intents.kc_submit = true`; pressing
`SKIP` writes `intents.kc_skip = true`. The modal
follows the existing BREACH overlay's neon palette and
masked-input idiom. While `show_kc_prompt = false` the
modal does not render and the intents stay false.

#### Technical Details

See ARD `## Explanation of Architectural Components` →
"What changes" item 5 (modal layout, intents,
view-bit); ARD `## Interfaces` → `include/ui.h` for
the struct/intent additions; ARD `### Theme & security
posture` for the visual discipline (BREACH-style
masked input, REMEMBER off by default). See ARD
`### Doc reconciliation` → `theme.md` for the modal-
label lexicon keys.

Files touched:

- `include/ui.h` — add the `KcForm` struct
  (`char passkey[256]; bool remember;`); add
  `bool show_kc_prompt` to `UiRunView` (at the end,
  to keep existing field offsets stable for tests
  reading the struct positionally); add
  `bool kc_submit` and `bool kc_skip` to
  `UiRunIntents`; extend `ui_frame`'s signature to
  accept `KcForm *kf` (positioned alongside the
  existing form pointers; the precise parameter
  slot is determined by `libui`'s composite-form
  pattern at implementation time but must be a
  non-NULL pointer the app owns).
- `src/ui/run.cpp` (or whichever TU houses the run
  panel — mirror the BREACH overlay's location in
  `libui`) — implement the modal as a small ImGui
  popup. Use `ImGui::OpenPopup` keyed on
  `show_kc_prompt`; render with `ImGui::BeginPopup`
  modal helpers; bind the masked field via
  `ImGui::InputText(..., ImGuiInputTextFlags_Password)`
  to `kf->passkey`; bind the checkbox to
  `kf->remember` (default `false`); two buttons
  write the corresponding intents and call
  `ImGui::CloseCurrentPopup` (the app is the source
  of truth for `show_kc_prompt`, so it must clear
  the view bit on its side; the popup `Close` is a
  visual nicety). All visible strings flow through
  the existing lexicon API
  (`lexicon_get(LEX_…)`) so theme changes don't
  touch this TU.
- `include/lexicon.h` — add the modal-label keys
  enumerated in ARD `### Doc reconciliation` →
  `theme.md`: a title key (e.g.
  `LEX_KC_MODAL_TITLE`), `LEX_KC_FIELD_PASSKEY`,
  `LEX_KC_CHECKBOX_REMEMBER`, `LEX_KC_BUTTON_ENTER`,
  `LEX_KC_BUTTON_SKIP`. The exact key names are
  this task's call (they had no consumer in T1);
  pick names consistent with the existing
  `LEX_CONN_*` family.
- `src/lexicon.c` — add the table entries with
  placeholder strings (the theme owner's actual
  copy decisions land via `theme.md` and ostrich's
  existing lexicon-edit flow; this task just opens
  the seam). Use the ARD-suggested phrasings
  (`KEYCHAIN VAULT // PASSKEY`, `KEYCHAIN PASSKEY`,
  `REMEMBER KEYCHAIN`, `ENTER`, `SKIP`) as the
  initial values — `theme.md` is the long-term
  source of truth but a placeholder ships now.
- `tests/ui_test.c` — new test cases:
    - With `show_kc_prompt = false`, `ui_frame`
      returns true and `intents.kc_submit` /
      `intents.kc_skip` are false after one frame.
    - With `show_kc_prompt = true` and a scripted
      "ENTER pressed" input, `ui_frame` returns
      true and `intents.kc_submit` is true after one
      frame. (Use whatever scripted-input mechanism
      `ui_test.c` already uses for the BREACH
      modal's tests.)
    - With `show_kc_prompt = true` and a scripted
      "SKIP pressed" input, `intents.kc_skip` is
      true after one frame.
    - The `KcForm.passkey` is rendered with the
      password mask (assertion against the
      `ImGuiInputTextFlags_Password` flag if the
      test harness exposes it; otherwise a smoke
      test that the rendered glyph count for a
      typed-in passkey matches the password-dot
      shape rather than the raw characters).
    - With `kf->remember = false` initially, after
      one frame the value is unchanged
      (default-off invariant).
- `context/theme.md` — add the new lexicon-key
  entries in the existing keys table (or wherever
  `theme.md` records the lexicon-key inventory):
  the title, field label, checkbox label, two
  button labels. Note that the canonical strings
  are subject to the theme owner's final call (per
  ARD `## Out of Scope` → "Theme/lexicon final
  wording") — the placeholder values shipped in
  `src/lexicon.c` are the seam, not the contract.

Conformance with `context/coding_standards.md`:

- **Arenas / allocation:** the `KcForm` is owned by
  the app (UI-thread-confined, app-arena lifetime
  or app-state-struct member). The UI mutates it
  in place per frame; no allocator surface in
  `libui` is added.
- **Thread confinement:** the modal renders on the
  UI thread; `KcForm` lives on the UI thread; no
  cross-thread data is introduced.
- **Module → library:** contained to existing
  `libui` + `liblexicon`. The C/C++ seam is
  respected: the modal renderer is C++ inside
  `src/ui/*.cpp`; the public `include/ui.h` stays
  pure C (new structs are POD, intents are
  `bool`).
- **C/C++ seam:** unchanged. `KcForm`,
  `show_kc_prompt`, `kc_submit`, and `kc_skip` are
  POD types declared in pure-C `include/ui.h`.
- **Error handling:** the modal has no fallible
  operation; rendering errors are absorbed by the
  existing `ui_frame` return-value semantics.
- **Testing:** black-box through `include/ui.h`,
  added to the existing `tests/ui_test.c` binary.

#### Acceptance criteria

- [ ] `include/ui.h` declares the `KcForm` struct
      (`char passkey[256]; bool remember;`).
- [ ] `include/ui.h` `UiRunView` contains
      `bool show_kc_prompt`.
- [ ] `include/ui.h` `UiRunIntents` contains
      `bool kc_submit` and `bool kc_skip`.
- [ ] `ui_frame`'s signature accepts a `KcForm *kf`
      parameter the app passes; the field is
      mutated in place by the modal while the
      modal is open.
- [ ] With `show_kc_prompt = false`, no modal
      renders and both `kc_submit` and `kc_skip`
      stay false across a single `ui_frame` call.
- [ ] With `show_kc_prompt = true` and a scripted
      ENTER click, `kc_submit == true` after the
      frame and the typed passkey appears in
      `kf->passkey`.
- [ ] With `show_kc_prompt = true` and a scripted
      SKIP click, `kc_skip == true` after the
      frame.
- [ ] The passkey input field uses the password-
      mask flag (`ImGuiInputTextFlags_Password`)
      so typed characters render masked.
- [ ] `REMEMBER KEYCHAIN` checkbox defaults to
      `false` and binds to `kf->remember`.
- [ ] `include/lexicon.h` declares the modal-label
      keys (title, field, checkbox, two buttons);
      `src/lexicon.c` ships placeholder strings
      per ARD-suggested phrasings; the existing
      table-index ordering is preserved.
- [ ] `context/theme.md` records the new lexicon-
      key entries.
- [ ] `make` builds cleanly; `make test` is all-
      green.
- [ ] The application binary, built at this
      commit, has no observable behavior change
      from the previous commit (no caller sets
      `show_kc_prompt = true` yet).

### Task 5 — app: gating cascade + lifecycle wiring

- **Status**: done
- **Blocked by**: Task 2, Task 3, Task 4
- **User stories covered**: n/a (no PRD; goal sourced from
  ARD `## PRD` section's one-line goal)

#### What to build

The app-layer composition that makes T2/T3/T4 a real
user feature. Adds `app.kc_pass_cache[256]` to the
app state, wires the four-branch gating cascade on
`EXECUTE` / `COMPILE` intent, handles the modal `ENTER`
and `SKIP` intents, clears the cache on an unlock-
rejected failure, and zeros the cache on disconnect.
This is the first task that produces user-observable
behavior change: a non-simulator EXECUTE against a Mac
with a locked keychain pops the modal, accepts a
passkey, and proceeds through the unlock step into the
existing build chain.

End-to-end behavior after T5:

- On EXECUTE / COMPILE intent against a simulator
  target: app calls `session_set_kc_pass(s, "")`
  then `session_run_submit(EXECUTE/COMPILE, …)` —
  the worker observes empty `kc_pass` and skips the
  unlock step (today's flow, preserved).
- On EXECUTE / COMPILE against a device with a
  populated `app.kc_pass_cache`: app re-sends the
  cache via `session_set_kc_pass`, then submits.
  Worker runs the unlock step with the cached
  passkey.
- On EXECUTE / COMPILE against a device with an empty
  cache but `active_conn.kc_remember &&
  active_conn.kc_passkey != ""`: app promotes the
  persisted passkey into the cache, then sends and
  submits.
- On EXECUTE / COMPILE against a device with neither
  cache nor persistence: app sets
  `view.show_kc_prompt = true` and defers the submit;
  the modal opens.
- On modal `ENTER`: app fills the cache from the
  form, optionally persists onto the active `Conn`
  (when `form.kc.remember`) via `store_save`, sends
  the cache via `session_set_kc_pass`, submits the
  deferred EXECUTE / COMPILE, clears
  `show_kc_prompt`.
- On modal `SKIP`: app calls
  `session_set_kc_pass(s, "")`, submits the deferred
  EXECUTE / COMPILE (worker skips unlock step),
  clears `show_kc_prompt`. Cache stays empty so the
  next non-sim EXECUTE re-pops the modal.
- On worker event `REV_PHASE(RUN_BUILD_FAILED,
  BD_ERR_UNLOCK_FAILED)`: app zeroes
  `app.kc_pass_cache`, calls
  `session_set_kc_pass(s, "")` (defensive sync), so
  the next EXECUTE re-pops the modal naturally. The
  persisted `Conn.kc_passkey` is untouched — the
  user may have a different passkey now and the
  modal can capture it; persistence is updated on
  the next successful unlock chain that opts to
  REMEMBER.
- On any transition into `CONN_DISCONNECTED` or
  `CONN_SEVERED`: app calls
  `memset(app.kc_pass_cache, 0,
  sizeof app.kc_pass_cache)`. Worker memory dies
  with the session naturally.

#### Technical Details

See ARD `## Explanation of Architectural Components` →
"What changes" item 7 (the cascade, in full pseudocode);
ARD `### Worker-side kc_pass lifetime` and
`### App-cache zeroing on disconnect` for the lifecycle
rules. The cascade lives in `src/app/` as composition-
root glue per `context/coding_standards.md` ("If real
logic accumulates in the app layer, push it down into a
library" — branchy glue that consults multiple library
APIs to make a routing decision is canonical app-layer
work, not library work).

Files touched:

- `src/app/app.c` (or wherever the app state struct +
  EXECUTE/COMPILE intent handler live; mirror the
  existing intent-dispatch pattern) —
    - Add `char kc_pass_cache[256]` to the app state
      struct.
    - Add a `KcForm kc_form` field (or wire one in
      whichever way the app composes its form
      structs into `ui_frame`'s call) so the new
      `KcForm *` parameter has a stable backing
      store.
    - In the EXECUTE/COMPILE intent handler,
      replace the existing direct
      `session_run_submit` call with the four-
      branch cascade described above. Use the
      existing `Target.is_simulator` field (or
      whatever the existing field is called — read
      from the discovery `Target` struct in
      context) for the G2 simulator gate.
    - On modal `kc_submit` intent: copy
      `kc_form.passkey` into `kc_pass_cache`; if
      `kc_form.remember`, copy into
      `active_conn->kc_remember = true` /
      `active_conn->kc_passkey` and call
      `store_save(connlist)`; call
      `session_set_kc_pass(s, kc_pass_cache)`; call
      `session_run_submit` with the deferred
      command; set `view.show_kc_prompt = false`;
      zero `kc_form.passkey` (defensive — don't
      leave the typed passkey hanging in form
      memory).
    - On modal `kc_skip` intent: call
      `session_set_kc_pass(s, "")`;
      `session_run_submit` with the deferred
      command; set `view.show_kc_prompt = false`;
      zero `kc_form.passkey`. Cache untouched.
    - In the session-event handler, on
      `REV_PHASE` with `phase == RUN_BUILD_FAILED`
      and `reason == BD_ERR_UNLOCK_FAILED`: zero
      `kc_pass_cache`; call
      `session_set_kc_pass(s, "")` (defensive — the
      worker has already failed and torn down the
      chain, but a stale `WorkerCtx.kc_pass` is
      surface area best cleared explicitly).
    - In the connection-event handler, on phase
      transition into `CONN_DISCONNECTED` or
      `CONN_SEVERED`: `memset(kc_pass_cache, 0,
      sizeof kc_pass_cache)`. The persisted
      `Conn.kc_passkey` is left intact (its
      lifetime is the store, not the session); on
      reconnect to the same `Conn`, the cascade's
      persisted-hit branch repopulates the cache.
    - Deferred-submit machinery: when the cascade
      falls through to "open the modal," the app
      must remember which command (EXECUTE vs
      COMPILE) was requested and against which
      `Target` so the eventual ENTER/SKIP handler
      can submit the right command. Implementation
      detail: a tiny `DeferredRunCmd` struct (kind
      + target snapshot) inside the app state, set
      when the modal opens and consumed on
      ENTER/SKIP. Cleared on disconnect.
- `tests/app_test.c` — new test cases:
    - Simulator EXECUTE: cascade calls
      `session_set_kc_pass(s, "")` then
      `session_run_submit` once; `show_kc_prompt`
      stays false. (Assert via a session-API spy or
      the existing test-double pattern used by
      `app_test.c`.)
    - Device EXECUTE with `kc_pass_cache` non-
      empty: cascade calls
      `session_set_kc_pass(s, "cache")` then
      `session_run_submit`; `show_kc_prompt` stays
      false.
    - Device EXECUTE with empty cache and
      `active_conn.kc_remember == true,
      .kc_passkey == "persisted"`: cascade
      promotes `"persisted"` into the cache and
      submits; `show_kc_prompt` stays false.
    - Device EXECUTE with empty cache and empty
      persistence: `show_kc_prompt` becomes true;
      no `session_run_submit` is called yet.
    - Following the previous case, modal `ENTER`
      with `kc_form.passkey = "typed",
      .remember = true`: `kc_pass_cache` becomes
      `"typed"`; `active_conn.kc_remember == true`;
      `active_conn.kc_passkey == "typed"`;
      `store_save` was called once;
      `session_set_kc_pass(s, "typed")` was called;
      `session_run_submit` was called once with the
      deferred command kind; `show_kc_prompt`
      becomes false; `kc_form.passkey` is zeroed.
    - Following the same setup, modal `SKIP`:
      `kc_pass_cache` stays empty;
      `session_set_kc_pass(s, "")` was called;
      `session_run_submit` was called once;
      `show_kc_prompt` becomes false.
    - Inject a `REV_PHASE(RUN_BUILD_FAILED,
      BD_ERR_UNLOCK_FAILED)` event: `kc_pass_cache`
      is zeroed; `session_set_kc_pass(s, "")` was
      called.
    - Inject a `ConnPhase` transition into
      `CONN_DISCONNECTED`: `kc_pass_cache` is
      zeroed; `active_conn.kc_passkey` (if
      `kc_remember == true`) is **not** zeroed
      (persisted state survives).

Conformance with `context/coding_standards.md`:

- **Arenas / allocation:** no new arenas.
  `kc_pass_cache[256]` and `kc_form` are POD
  members of the existing app-state struct (app-
  arena lifetime). `DeferredRunCmd` is a POD
  member too. No `malloc` introduced.
- **Thread confinement:** the cascade runs on the
  UI thread. The cache is read on the UI thread
  and written on the UI thread. The cross-thread
  handoff to the worker uses the existing
  `session_set_kc_pass` API (which copies bytes
  into a `SessionRunCmd` record on the run command
  ring — already a flagged cross-thread
  allocation).
- **Module → library:** the cascade stays in
  `src/app/` per coding standards (branchy glue
  routing between `libsession` / `libstore` / view
  state; no real reusable logic to encapsulate).
  If the cascade ever grows real internal
  complexity (e.g. multi-conn precedence rules,
  TTL on cached values), that's the trigger to
  push it down into a library.
- **C/C++ seam:** pure C11 throughout `src/app/`.
- **Error handling:** existing `SshStatus` /
  `StoreStatus` enums and `store_status_str`
  companion cover the new code paths (store_save
  failures are handled the same way they are
  today for SSH-passkey REMEMBER).
- **Testing:** black-box through the app's intent-
  dispatch API and the session/store test-double
  shims `tests/app_test.c` already uses.

#### Acceptance criteria

- [ ] `app` state struct contains
      `char kc_pass_cache[256]`.
- [ ] `app` state struct contains a `KcForm kc_form`
      (or composes one into the existing form-
      struct slot passed to `ui_frame`).
- [ ] EXECUTE / COMPILE intent against a simulator
      target results in
      `session_set_kc_pass(s, "")` followed by
      `session_run_submit` without opening the
      modal.
- [ ] EXECUTE / COMPILE against a device with non-
      empty `kc_pass_cache` results in
      `session_set_kc_pass(s, kc_pass_cache)`
      followed by `session_run_submit` without
      opening the modal.
- [ ] EXECUTE / COMPILE against a device with empty
      cache and `active_conn.kc_remember == true,
      kc_passkey != ""` promotes the persisted
      passkey into `kc_pass_cache` and submits
      without opening the modal.
- [ ] EXECUTE / COMPILE against a device with
      empty cache and empty persistence sets
      `view.show_kc_prompt = true` and defers the
      session submit until the modal resolves.
- [ ] Modal `kc_submit` with `kc_form.remember ==
      true` populates `kc_pass_cache`, mutates
      `active_conn.kc_remember` and
      `active_conn.kc_passkey`, calls
      `store_save`, calls `session_set_kc_pass`
      with the cache, calls the deferred
      `session_run_submit`, clears
      `show_kc_prompt`, and zeros
      `kc_form.passkey`.
- [ ] Modal `kc_submit` with `kc_form.remember ==
      false` populates `kc_pass_cache` but does
      **not** mutate `active_conn` or call
      `store_save`.
- [ ] Modal `kc_skip` calls
      `session_set_kc_pass(s, "")`, calls the
      deferred `session_run_submit`, clears
      `show_kc_prompt`, zeros `kc_form.passkey`,
      and leaves `kc_pass_cache` empty.
- [ ] A `REV_PHASE(RUN_BUILD_FAILED,
      BD_ERR_UNLOCK_FAILED)` event zeroes
      `kc_pass_cache` and calls
      `session_set_kc_pass(s, "")`.
- [ ] Transition into `CONN_DISCONNECTED` (or
      `CONN_SEVERED`) zeroes `kc_pass_cache`;
      `active_conn.kc_passkey` is untouched.
- [ ] `tests/app_test.c` covers each cascade branch
      and each lifecycle event listed above.
- [ ] `make` builds cleanly; `make test` is all-
      green.
- [ ] Manual smoke: on a Mac with a locked
      `login.keychain`, pressing EXECUTE against a
      device target pops the modal; entering the
      correct passkey and pressing ENTER causes the
      Build Log to show `> KEYCHAIN UNLOCKED`
      followed by the normal SETTINGS / BUILD /
      INSTALL / LAUNCH output; subsequent EXECUTEs
      in the same session do not re-pop the modal.

### Task 6 — H2 codesign hint emission + README

- **Status**: done
- **Blocked by**: Task 1, Task 5
- **User stories covered**: n/a (no PRD; goal sourced from
  ARD `## PRD` section's one-line goal)

#### What to build

The closing-the-loop task: when a non-simulator build
fails at the actual BUILD step and the user did not
supply a keychain passkey for this run, the Build Log
shows the H2 codesign hint footer so the user knows
the next EXECUTE will pop the modal. By landing after
T5, the hint text ("press EXECUTE; the keychain
passkey modal will appear") is honest the moment it
appears in production. Also lands the README doc
updates that describe the now-complete feature.

End-to-end behavior after T6: any non-simulator BUILD
failure where the unlock step was skipped (i.e.
`WorkerCtx.kc_pass == ""`) emits the
`bd_codesign_hint_block` text into the Build Log
before the `REV_PHASE(RUN_BUILD_FAILED, BD_ERR_BUILD)`
event. Simulator BUILD failures, unlock-step failures
(handled by T3's F1 path), and BUILD failures where
the user did supply a passkey (the unlock step ran;
codesign is not the issue) do not emit the hint.

#### Technical Details

See ARD `## Explanation of Architectural Components` →
"What changes" item 3 (the H2 hint helper) and the
control-flow box under `### Control + data flow` →
"On the H2 path"; ARD `### Failure-detection
semantics` for the precise structural gate
(`!target.is_simulator && kc_pass == "" &&
RUN_BUILD_FAILED from the build step (BD_ERR_BUILD)`).
The hint must **not** fire on `BD_ERR_UNLOCK_FAILED`
(its own help block already fired in T3), on
`BD_ERR_SETSID_MISSING` (setsid-help already fired),
or on `BD_ERR_PARSE` (a SETTINGS-step failure, not a
BUILD-step failure).

Files touched:

- `src/session/session.c` — in `handle_step_exit`'s
  `RCHAIN_STEP_BUILD` case, **after** the existing
  setsid-help block (the `exit_code != 0 &&
  rc->build_pgid == 0` branch from setsid-help T2)
  but **before** the existing
  `(exit_code == 127) ? BD_ERR_XCODE_MISSING :
  BD_ERR_BUILD` mapping, insert:

  ```c
  if (exit_code != 0
      && ctx->run.target_is_sim == false
      && ctx->kc_pass[0] == '\0') {
      char hint[1024];
      if (bd_codesign_hint_block(ctx->cfg.user,
                                 ctx->cfg.host,
                                 ctx->cfg.port,
                                 hint, sizeof(hint))
          == BD_OK) {
          emit_build_log(ctx, hint, strlen(hint));
      }
      /* fall through to the existing reason
         mapping so the final reason is BD_ERR_BUILD
         (or BD_ERR_XCODE_MISSING for exit 127),
         not a new value */
  }
  ```

  The hint is **additive**: it emits the help block
  but does not change the resulting `BdStatus` or
  the resulting `RUN_EV_*` event. The existing
  setsid-help branch above takes precedence (the
  setsid-help branch returns before reaching the
  hint check, so the two blocks never both fire).
  The exact source of `ctx->run.target_is_sim` and
  `ctx->kc_pass` may need adjusting based on what
  the WorkerCtx struct actually exposes — the
  intent is "read the same `kc_pass` slot T3
  populated and the same `target.is_simulator`
  field the existing chain consults."
- `tests/session_run_test.c` — new test cases per
  ARD `### Testing approach` (the H2-hint subset):
    - Stub channel for BUILD step exits non-zero,
      `WorkerCtx.kc_pass == ""`, target is **not**
      a simulator: the drained `REV_BUILD_LOG`
      payload contains the H2 hint phrases
      (`errSecInternalComponent`,
      `keychain may be locked`) **before** the
      observed
      `REV_PHASE(RUN_BUILD_FAILED, BD_ERR_BUILD)`.
    - Same scenario with `target.is_simulator =
      true`: the hint text is **not** present in
      the drained payload.
    - Same scenario with `WorkerCtx.kc_pass`
      non-empty (set via prior
      `RCMD_SET_KC_PASS`): the hint text is
      **not** present (user already chose unlock;
      failure isn't a keychain issue).
    - Cross-feature interaction check: a BUILD-
      step setsid-missing failure (T2 of setsid-
      help) with empty `kc_pass` and non-sim
      target still fires the **setsid** help
      block, not the H2 hint (the setsid branch
      returns before the hint check).
- `README.md` — extend the "Remote Mac (SSH
  target)" section with a brief paragraph about
  keychain unlock alongside the existing `setsid`
  paragraph: name the lazy modal, the in-session
  cache, and the opt-in `REMEMBER KEYCHAIN`. Add
  an entry to the "Known issues" list for the
  locked-`login.keychain` failure mode, tightened
  in the same style as the `setsid` entry — the
  failure self-documents in-app, the entry exists
  to document the underlying remote-Mac fact.

Conformance with `context/coding_standards.md`:

- **Arenas / allocation:** the hint buffer is
  stack-local (`char hint[1024]`) with function-
  scoped lifetime, mirroring the setsid-help emit
  pattern. No new allocator surface.
- **Thread confinement:** `handle_step_exit` runs
  on the worker thread, which already owns the
  chain state and the `emit_build_log` path. No
  cross-thread data.
- **Module → library:** contained to
  `src/session/session.c` and `README.md`; no
  public-header changes.
- **C/C++ seam:** pure C11.
- **Error handling:** the hint is additive; the
  resulting `BdStatus` and `RunPhase` transitions
  are unchanged.
- **Testing:** black-box, through the existing
  session public API and the `ssh_stub_run.c`
  configurable stub. No white-box reach-ins.

#### Acceptance criteria

- [ ] `handle_step_exit`'s `RCHAIN_STEP_BUILD` case
      contains an H2-hint branch after the existing
      setsid-help branch and before the existing
      reason-mapping branch, gated on
      `exit_code != 0 && !target_is_sim &&
      kc_pass[0] == '\0'`.
- [ ] On that condition, `bd_codesign_hint_block`
      is called with `ctx->cfg.user`,
      `ctx->cfg.host`, `ctx->cfg.port`, and a
      stack-local 1024-byte buffer; on `BD_OK` the
      rendered bytes are emitted via
      `emit_build_log` before the existing reason-
      mapping branch runs.
- [ ] The H2-hint emission does **not** change the
      resulting `BdStatus`: a regular BUILD failure
      still produces `BD_ERR_BUILD` (or
      `BD_ERR_XCODE_MISSING` for exit 127); a
      setsid-missing BUILD failure still produces
      `BD_ERR_SETSID_MISSING`.
- [ ] `tests/session_run_test.c` covers all four
      H2-related cases enumerated in *Technical
      Details* above.
- [ ] `README.md` "Remote Mac (SSH target)"
      section names the keychain-unlock modal, the
      in-session cache, and the opt-in
      `REMEMBER KEYCHAIN`.
- [ ] `README.md` "Known issues" list contains an
      entry on the keychain prereq, tightened in
      the setsid-entry style (failure self-
      documents in-app; entry exists to document
      the remote-Mac fact).
- [ ] `make` builds cleanly; `make test` is all-
      green.
- [ ] Manual smoke: on a Mac with a locked
      `login.keychain`, pressing EXECUTE against a
      device target without entering a passkey
      (SKIP the modal) results in the Build Log
      showing the H2 hint footer
      (`── HINT ──` rules, `errSecInternalComponent`
      phrase, "Press EXECUTE; the keychain passkey
      modal will appear") before the EXPLOIT
      FAILED banner. The same smoke against a
      simulator target (failing the BUILD some
      other way) does **not** show the hint.
