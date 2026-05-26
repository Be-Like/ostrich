# Implementation Plan — setsid install help in the Build Log

## Summary of Tasks

1. **T1 — libbuilddeploy: `bd_setsid_help_block` + new
   `BD_ERR_SETSID_MISSING` classification.** Add the pure
   text-rendering helper, a new `BdStatus` enum value, a
   matching `LEX_REC_ERR_SETSID` lexicon key, and the
   `bd_status_str` / `bd_reason_lex` / `runstate_reason_lex`
   mappings. Ships as dead-but-safe code: no caller produces
   `BD_ERR_SETSID_MISSING` until T2.
2. **T2 — libsession emission hook + doc reconciliation.**
   Add the marker-absence check at the BUILD-step exit in
   `handle_step_exit`; on fire, emit the help block via the
   existing `emit_build_log` path and reroute the failure
   reason to `BD_ERR_SETSID_MISSING`. Extend
   `tests/session_run_test.c::test_build_failure` to assert
   the help-block bytes land in `REV_BUILD_LOG`. Tighten the
   README "Known issues" entry and add the cross-ARD "See
   also" pointer.
3. **T3 — surface SETTINGS step output in the Build Log so
   `EXECUTE` failures aren't silent.** Tee
   `RCHAIN_STEP_SETTINGS` stdout/stderr into `emit_build_log`
   while still buffering for `bd_parse_product_path`, and land
   the stashed regression test
   `tests/session_run_test.c::test_execute_settings_failure_shows_log`
   that proves the bug. Post-T2 follow-up: on `EXECUTE`
   (`has_target=true`) the chain typically fails at SETTINGS
   before reaching BUILD, so the T2 help block never fires
   and the user sees an empty Build Log with only the
   `EXPLOIT FAILED` banner.

## ARD amendment note

The parent ARD says reason mapping is unchanged and that the
lexicon table is not touched. During grilling, the team chose
to add `BD_ERR_SETSID_MISSING` (and the matching
`LEX_REC_ERR_SETSID` lexicon key) so the failure
classification matches the help-block text. This is a small,
intentional amendment to the ARD's "Out of Scope" /
"Failure-detection semantics" sections. The structural trigger
condition (`build_pgid == 0 && exit_status != 0`) is
unchanged; only the `BdStatus` it produces and its lexicon
companion are added.

The ARD body is left as-is; this IMPL is the authoritative
source for the enum/lexicon addition.

## Task Dependency Relationships

```
T1 (libbuilddeploy: helper + enum + lexicon plumbing)
        │
        ▼
T2 (libsession hook + session test + docs)
        │
        ▼
T3 (SETTINGS-step output → Build Log; EXECUTE reachability)
```

T2 is blocked by T1 because the session hook calls
`bd_setsid_help_block` and passes `BD_ERR_SETSID_MISSING`
into `fail_run_chain`; both must exist in `libbuilddeploy`
and the lexicon table before the hook compiles.

T3 is blocked by T2 only by sequencing — the bug it fixes is
a post-T2 discovery and its regression test (stashed at
`stash@{0}` on the implementer's checkout) was authored
against T2's emission path. T3 does not touch
`libbuilddeploy` and could in principle ship independently;
keeping it after T2 preserves the narrative "T2 made the
help block emittable; T3 made it reachable from EXECUTE."

## Detailed Tasks

### Task 1 — libbuilddeploy: setsid help helper + new error classification

- **Status**: complete
- **Blocked by**: none
- **User stories covered**: n/a (no PRD; goal sourced from
  ARD `## PRD` section's one-line goal)

#### What to build

A pure helper that renders the copy-paste remediation block
described in the ARD's "Help-block content (illustrative)"
section, plus the surrounding type/lexicon plumbing needed
for T2 to classify the failure correctly.

End-to-end behavior after T1: callers of `libbuilddeploy`
can render the help block into a caller-supplied buffer, and
`BdStatus` has a new `BD_ERR_SETSID_MISSING` value with
matching `bd_status_str`, `bd_reason_lex`, and
`runstate_reason_lex` coverage. Nothing in the running
application produces this status yet — it sits unused but
fully tested. The app remains buildable, all existing tests
pass, and shipping the binary in this state is safe (no
runtime behavior change).

#### Technical Details

See ARD `## Explanation of Architectural Components` →
"What changes" → item 1 (the helper) and the "Help-block
content (illustrative)" section under `## Interfaces` for
the exact text contract. See the ARD-amendment note above
for the rationale on adding `BD_ERR_SETSID_MISSING` and
`LEX_REC_ERR_SETSID` (which the ARD itself does not
introduce).

Files touched:

- `include/builddeploy.h` — add
  `BD_ERR_SETSID_MISSING` to the `BdStatus` enum and the
  `bd_setsid_help_block` prototype.
- `src/builddeploy/builddeploy.c` — implement the helper
  (single `snprintf`-style render, branching on
  `port == 22` for the two `ssh` lines); add the
  `BD_ERR_SETSID_MISSING` cases to `bd_status_str` and
  `bd_reason_lex`.
- `include/lexicon.h` — add `LEX_REC_ERR_SETSID` enum
  member next to `LEX_REC_ERR_XCODE`.
- `src/lexicon.c` — add the matching table entry (e.g.
  `"SETSID NOT FOUND"`); preserve the existing
  index-by-position layout.
- `src/runstate/runstate.c` — in `runstate_reason_lex`,
  add the `BD_ERR_SETSID_MISSING → LEX_REC_ERR_SETSID`
  branch alongside the existing `BD_ERR_XCODE_MISSING`
  branch.
- `tests/builddeploy_test.c` — new test cases per ARD
  `### Testing approach`, plus enum-coverage assertions
  for `bd_status_str` and `bd_reason_lex`.
- `tests/runstate_test.c` — add one assertion for
  `runstate_reason_lex(RUN_BUILD_FAILED,
  BD_ERR_SETSID_MISSING) == LEX_REC_ERR_SETSID`.

Conformance with `context/coding_standards.md`:

- **Arenas / allocation:** `bd_setsid_help_block` takes
  `char *buf, size_t cap` — caller-controlled, no hidden
  allocators. No new arenas. Matches existing
  `libbuilddeploy` precedent (`bd_build_cmd`, etc.).
- **Thread confinement:** pure function; no threading
  concerns introduced.
- **Module → library:** change is contained to existing
  libraries (`libbuilddeploy`, `liblexicon`-equivalent
  plain source, `librunstate`); no new libraries; no
  module reorganization.
- **C/C++ seam:** `libbuilddeploy` stays pure C11; no C++
  surface added.
- **Error handling:** new function returns `BdStatus`
  (`BD_OK` / `BD_ERR_OOM`) per the project's status-enum
  convention; `bd_status_str` companion covers the new
  enum value.
- **Testing:** black-box, through the public header,
  added to the existing `tests/builddeploy_test.c`
  binary and `tests/runstate_test.c` binary.

#### Acceptance criteria

- [x] `include/builddeploy.h` declares
      `bd_setsid_help_block(const char *user,
      const char *host, int port, char *buf,
      size_t cap)` returning `BdStatus`.
- [x] `include/builddeploy.h` `BdStatus` enum contains
      `BD_ERR_SETSID_MISSING`.
- [x] `include/lexicon.h` declares
      `LEX_REC_ERR_SETSID`; `src/lexicon.c` has the
      matching table entry; the lexicon table index
      ordering is preserved.
- [x] `bd_status_str(BD_ERR_SETSID_MISSING)` returns a
      non-empty string.
- [x] `bd_reason_lex(BD_ERR_SETSID_MISSING)` returns
      `LEX_REC_ERR_SETSID`.
- [x] `runstate_reason_lex(RUN_BUILD_FAILED,
      BD_ERR_SETSID_MISSING)` returns
      `LEX_REC_ERR_SETSID`.
- [x] `tests/builddeploy_test.c` asserts the rendered
      help block contains each of: `REMOTE MAC IS MISSING
      setsid.`, `ssh <user>@<host>` for the default-port
      case, `ssh -p <port> <user>@<host>` for the
      non-default-port case, `brew install util-linux`,
      `.zshenv`, `command -v setsid`, the
      `── REMEDIATION ──` opener rule, and the
      `── END REMEDIATION ──` closer rule.
- [x] `tests/builddeploy_test.c` asserts
      `bd_setsid_help_block` returns `BD_ERR_OOM` when
      `cap` is below the rendered length, and that on
      `BD_OK` the output is NUL-terminated.
- [x] `tests/builddeploy_test.c` asserts
      `bd_status_str(BD_ERR_SETSID_MISSING)` and
      `bd_reason_lex(BD_ERR_SETSID_MISSING)` per above.
- [x] `tests/runstate_test.c` asserts the new
      `runstate_reason_lex` mapping.
- [x] `make` builds cleanly; `make test` is all-green
      (existing assertions on `bd_build_cmd`,
      `bd_launch_cmd`, `bd_kill_cmd`,
      `bd_parse_pid_marker`, and
      `bd_parse_product_path` carry over unchanged).
- [x] The application binary, built at this commit, has
      no observable behavior change from the previous
      commit (no production caller produces
      `BD_ERR_SETSID_MISSING` yet).

### Task 2 — libsession emission hook + doc reconciliation

- **Status**: complete
- **Blocked by**: Task 1
- **User stories covered**: n/a (no PRD; goal sourced from
  ARD `## PRD` section's one-line goal)

#### What to build

The user-facing behavior described in the ARD's goal: on a
build failure where the wrapper itself failed before
reaching its `printf` marker line (the structural signature
of a missing `setsid`), the Build Log shows the raw shell
error followed by the rendered remediation block, then the
existing `RUN_BUILD_FAILED` phase transition. The phase
event carries `BD_ERR_SETSID_MISSING` instead of
`BD_ERR_XCODE_MISSING`, so downstream lexicon consumers can
label the failure correctly.

End-to-end behavior after T2: an `EXECUTE` against a Mac
without `setsid` renders, top-to-bottom in the Build Log:
the remote shell's `command not found: setsid` line,
followed by the `── REMEDIATION ──` block (with the
correct `ssh user@host` invocation and a `-p <port>` flag
when applicable), followed by the EXPLOIT FAILED banner.
The detection is single-shot per chain by construction.

#### Technical Details

See ARD `## Explanation of Architectural Components` →
"What changes" item 2 (the emission hook), item 3 (visual
demarcation), item 4 (what does not change), and the
`### Control + data flow` section for the post-change
sequence. See ARD `### Failure-detection semantics` for the
structural-signal rationale. See ARD `### Doc
reconciliation` for the README and cross-ARD edits.

Files touched:

- `src/session/session.c` — inside `handle_step_exit`'s
  `RCHAIN_STEP_BUILD` case, before the existing
  `exit_code != 0` reason mapping, insert:

  ```c
  if (exit_code != 0 && rc->build_pgid == 0) {
      char block[2048];
      if (bd_setsid_help_block(ctx->cfg.user,
                               ctx->cfg.host,
                               ctx->cfg.port,
                               block, sizeof(block))
          == BD_OK) {
          emit_build_log(ctx, block, strlen(block));
      }
      fail_run_chain(ctx, RUN_EV_BUILD_FAIL,
                     BD_ERR_SETSID_MISSING);
      return;
  }
  ```

  The existing `(exit_code == 127) ?
  BD_ERR_XCODE_MISSING : BD_ERR_BUILD` mapping is left
  intact for the fall-through path (wrapper succeeded
  past its `printf` marker, then the wrapped
  `xcodebuild` itself exited non-zero — same as today).
  Buffer is stack-local with function-scoped lifetime
  per ARD `### Non-arena allocations`.

- `tests/session_run_test.c` — extend
  `test_build_failure` to assert that, after the stubbed
  build exits 1 with no PID marker, the drained
  `REV_BUILD_LOG` event stream contains both
  `REMOTE MAC IS MISSING setsid.` and the
  `ssh ` substring. The existing assertions on phase
  reaching `RUN_BUILD_FAILED` and exec count remain.
  Helper for substring search may be added (small
  black-box, public-API-only utility on top of the
  existing `drain_until` pattern).

- `README.md` — tighten the "Known issues" `setsid`
  entry: keep the dependency documented, but lead with
  the in-app remediation ("when `setsid` is missing the
  Build Log surfaces the install command and the exact
  `ssh` invocation to fix it"). The "Remote Mac (SSH
  target)" section is left unchanged as the source of
  truth for the install steps.

- `context/projects/xcode-project-build-and-deploy/ard.md`
  — add a Further Notes "See also" pointer to this
  project for the in-app missing-`setsid` guidance. No
  substantive changes to the build/launch wrapper design
  or the two-pronged kill text.

- `context/projects/setsid-install-help/ard.md` — append
  a short "Amendment to ARD" stanza (or footnote)
  acknowledging that the IMPL added
  `BD_ERR_SETSID_MISSING` and `LEX_REC_ERR_SETSID`,
  pointing the reader at this IMPL for the full
  rationale. This keeps the ARD honest about what
  shipped without rewriting the body.

Conformance with `context/coding_standards.md`:

- **Arenas / allocation:** no new arenas. Help-block
  bytes go into a stack-local `char block[2048]`
  (function-scoped lifetime) and are copied into the
  existing `REV_BUILD_LOG` event records by
  `emit_build_log` — the same mechanism used for every
  real build chunk today.
- **Thread confinement:** the hook runs on the worker
  thread, which already owns the chain state and the
  Build Log emission path. No cross-thread data is
  introduced.
- **Module → library:** no library reorganization; the
  hook is a small block of code inside
  `src/session/session.c`. No `libsession` public-header
  change.
- **Testing:** the extended `test_build_failure` stays
  black-box (uses the public session API surface and the
  configurable SSH stub).

#### Acceptance criteria

- [x] `src/session/session.c` `handle_step_exit`
      `RCHAIN_STEP_BUILD` case checks
      `exit_code != 0 && rc->build_pgid == 0` before the
      existing reason-mapping branch.
- [x] On that condition, `bd_setsid_help_block` is
      called with `ctx->cfg.user`, `ctx->cfg.host`,
      `ctx->cfg.port`, a 2048-byte stack buffer, and on
      `BD_OK` the rendered bytes are emitted via
      `emit_build_log` before `fail_run_chain` is
      called.
- [x] `fail_run_chain` is called with
      `BD_ERR_SETSID_MISSING` on the fired path.
- [x] The existing `(exit_code == 127) ?
      BD_ERR_XCODE_MISSING : BD_ERR_BUILD` mapping is
      preserved for the marker-present path.
- [x] `tests/session_run_test.c::test_build_failure`
      drains the event stream and asserts the
      concatenated `REV_BUILD_LOG` payload contains both
      `REMOTE MAC IS MISSING setsid.` and `ssh `; the
      existing `RUN_BUILD_FAILED` phase assertion and
      exec-count assertion still pass.
- [x] `README.md` "Known issues" entry for `setsid` is
      retained but tightened to mention that the Build
      Log surfaces the fix in-app when `setsid` is
      missing.
- [x] `README.md` "Remote Mac (SSH target)" section is
      unchanged.
- [x] `context/projects/xcode-project-build-and-deploy/ard.md`
      gains a Further Notes "See also" pointer to this
      project.
- [x] `context/projects/setsid-install-help/ard.md`
      records a short amendment note about the
      `BD_ERR_SETSID_MISSING` + `LEX_REC_ERR_SETSID`
      additions.
- [x] `make` builds cleanly; `make test` is all-green
      (test binaries built and run individually; full
      `make test` blocked only by missing `libxkbcommon`
      system dep for the GLFW Wayland backend, unrelated
      to this change).
- [ ] Manual smoke: on a Mac whose non-interactive SSH
      `PATH` lacks `setsid`, an `EXECUTE` renders the
      Build Log top-to-bottom as: the remote shell's
      `command not found: setsid` line, the
      `── REMEDIATION ──` block (with the correct
      `ssh user@host` invocation; `-p <port>` present
      only when `cfg.port != 22`), the
      `── END REMEDIATION ──` closer, and the EXPLOIT
      FAILED banner.

### Task 3 — SETTINGS-step output → Build Log (EXECUTE reachability fix)

- **Status**: not started
- **Blocked by**: Task 2 (sequencing only — see T3 entry in
  "Task Dependency Relationships")
- **User stories covered**: n/a (no PRD; bug discovered in
  manual T2 smoke on a Mac missing `setsid`)

#### What to build

A one-line tee in `process_run_chunk`'s
`RCHAIN_STEP_SETTINGS` case so SETTINGS stdout/stderr also
lands in `REV_BUILD_LOG` events. The existing
`settings_buf` copy stays — `bd_parse_product_path` still
consumes the buffered JSON on success.

End-to-end behavior after T3: when `EXECUTE`
(`has_target=true`) submits a destination string that
`xcodebuild -showBuildSettings -destination id=<udid>`
can't resolve, the Build Log shows xcodebuild's actual
error text before the `EXPLOIT FAILED` banner — same
visibility the BUILD step already has. When SETTINGS
succeeds and BUILD then fails on missing `setsid`, the T2
help block fires as it does today.

#### Technical Details

Root cause: `tests/session_run_test.c::test_build_failure`
(added in T2) exercises the BUILD-step setsid path with
`has_target=true` but uses a stub that returns success at
SETTINGS. On a real Mac without `setsid`, COMPILE
(`has_target=false`, destination `generic/platform=iOS`)
still reaches BUILD and shows the help block — but EXECUTE
(`has_target=true`, destination `id=<udid>`) often fails at
SETTINGS first because the device isn't resolvable in that
xcodebuild invocation context. `process_run_chunk`'s
SETTINGS case at `src/session/session.c:588-592` copies
bytes into `rc->settings_buf` only, so when SETTINGS exits
non-zero the Build Log panel is empty and `fail_run_chain`
emits only the `RUN_BUILD_FAILED` phase event.

Trade-off accepted (per design Q&A 2026-05-26): with the
tee, the SETTINGS JSON blob also appears in the Build Log
on the success path. We're taking that visual noise to
keep the fix a one-line change and to make the data path
symmetric with BUILD/INSTALL output. If the JSON noise
proves annoying in practice, a follow-up can swap the tee
for a "flush settings_buf only on SETTINGS failure" guard
in `handle_step_exit`.

Files touched:

- `src/session/session.c` — inside `process_run_chunk`'s
  `RCHAIN_STEP_SETTINGS` case
  (`src/session/session.c:588-592`), add
  `emit_build_log(ctx, buf, n);` alongside the existing
  `settings_buf` memcpy. Position the call so SETTINGS
  bytes reach the Build Log even when `settings_buf` is
  full (don't gate the emit on the same `settings_len + n
  <= RUN_SETTINGS_BUF_CAP` check).

- `tests/session_run_test.c` — unstash and land
  `test_execute_settings_failure_shows_log` (currently at
  `stash@{0}`: "repro: EXECUTE settings-step failure
  leaves build log empty (setsid-install-help)"). The
  test scripts SETTINGS to exit 1 with the substring
  `"Unable to find a destination matching id=..."`,
  submits an `EXECUTE` (`has_target=true`), waits for
  `RUN_BUILD_FAILED`, and asserts the concatenated
  `REV_BUILD_LOG` payload contains `"Unable to find"`.
  Pre-fix it fails on that assertion; post-fix it passes.

Conformance with `context/coding_standards.md`:

- **Arenas / allocation:** no new allocators.
  `emit_build_log` copies bytes into existing SPSC event
  records, same as every BUILD chunk today.
- **Thread confinement:** `process_run_chunk` already runs
  on the worker thread that owns the Build Log emission
  path; no new cross-thread data.
- **Module → library:** change contained to
  `src/session/session.c`; no public-header change to
  `libsession`.
- **C/C++ seam:** untouched; pure C11 edit.
- **Testing:** black-box, public-API only, uses the
  existing `ssh_stub_run.c` configurable stub.

#### Acceptance criteria

- [ ] `src/session/session.c` `process_run_chunk`'s
      `RCHAIN_STEP_SETTINGS` case emits every chunk via
      `emit_build_log(ctx, buf, n)` in addition to the
      `settings_buf` memcpy; the emit is not gated on
      buffer-capacity availability.
- [ ] `tests/session_run_test.c::test_execute_settings_failure_shows_log`
      is unstashed, wired into `main()` next to
      `test_build_failure`, and passes; it asserts
      (a) `g_exec_next == 1` after `RUN_BUILD_FAILED`
      (only SETTINGS ran) and (b) the drained
      `REV_BUILD_LOG` payload contains
      `"Unable to find"`.
- [ ] `tests/session_run_test.c::test_execute_happy_path`
      still passes (sanity-check that teeing the
      settings-JSON success blob doesn't break the
      happy-path drain).
- [ ] `make` builds cleanly; `make test` is all-green
      (same `libxkbcommon` caveat as T2).
- [ ] Manual smoke: on the same Mac used for the T2
      smoke, an `EXECUTE` whose SETTINGS step fails
      renders the xcodebuild error text in the Build Log
      before the `EXPLOIT FAILED` banner; a COMPILE on
      the same Mac still shows the T2 help block as
      before.
