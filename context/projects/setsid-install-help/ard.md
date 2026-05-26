# ARD — setsid install help in the Build Log

## PRD

There is no standalone PRD for this work. The goal is scoped from
the README's "Known issues" entry ("`setsid` dependency on the
remote Mac") and the existing
xcode-project-build-and-deploy ARD's Further Notes
("Two-pronged kill") — the upstream design that introduced the
`setsid` wrapper.

Goal, one line:

  When a build fails because `setsid` is missing on the remote
  Mac, surface the exact remediation steps (including the `ssh`
  command to connect to that Mac) inside the Build Log so the
  user can fix the problem without leaving the app or hunting
  through documentation.

Motivation:

- The `setsid` dependency is small (one Homebrew install + one
  shell-rc edit, paid once per remote Mac). The harm is a poor
  *first-time* experience: an opaque
  `command not found: setsid` inside an EXPLOIT FAILED banner
  with no in-app guidance on how to fix it.
- The user is already technical enough to use ostrich; they do
  not need ostrich to install dependencies *for* them. They need
  ostrich to *tell* them what to do, with commands they can copy
  and paste — including the specific `ssh user@host` invocation
  for the very Mac that just failed.
- This work intentionally **does not remove** the `setsid`
  dependency. An alternative ARD (Perl wrapper) was considered
  and rejected on cost/value grounds: a permanent code change
  with subtle correctness considerations, to save ~5–10 minutes
  of one-time setup per Mac, was judged not worth it relative to
  improving the in-app guidance for the same failure.

## Explanation of Architectural Components

### Where the failure surfaces today

When a build is attempted against a Mac that lacks `setsid` on
the non-interactive SSH PATH:

1. `bd_build_cmd` constructs the standard wrapper
   `setsid sh -c 'printf "__OSTRICH_PGID__%d\n" $$; exec
   xcodebuild …'`.
2. The worker opens an SSH channel and runs the command.
3. The remote shell prints `<shell-name>: command not found:
   setsid` (zsh) or a similar variant (bash/dash) to stderr.
4. The channel exits with status 127.
5. The BUILD step's chunk handler (`process_chunk` in
   `src/session/session.c:584`) tries to parse the PGID marker
   from each chunk. **It is never found** because the wrapper
   failed before reaching its `printf` line. `RunChain.build_pgid`
   remains `0`.
6. The chain transitions to `RUN_BUILD_FAILED`; the UI shows
   EXPLOIT FAILED with the raw `command not found: setsid` chunk
   in the Build Log.

Everything in steps 1–6 is correct behavior and remains
unchanged by this work.

### What changes

**1. A new pure helper in `libbuilddeploy`.**

  BdStatus bd_setsid_help_block(
      const char *user, const char *host, int port,
      char *buf, size_t cap);

The function writes a multi-line, copy-paste-ready remediation
block into the caller's buffer, with `user@host` interpolated and
the optional `-p <port>` flag included only when `port != 22`.
The text — header, section labels, brew commands, verify
command, README pointer — is a string literal inside
`src/builddeploy/builddeploy.c`. There is no lexicon
involvement; this matches the existing precedent of
`bd_status_str`, `ssh_status_str`, and other module-local string
companions that live outside lexicon.

Why libbuilddeploy: the remediation is *about* a build-tooling
prereq. The text and the SSH command rendering have no UI
concerns — they are technical instructions, the same kind of
text `bd_status_str` returns.

Why caller-supplied buf (no allocation): consistent with every
other `bd_*` function. Caller chooses the buffer; helper writes
and returns `BD_OK` or `BD_ERR_OOM`.

**2. A new emission hook in `libsession`'s BUILD-failure path.**

At the point where the BUILD step's exit status is observed and
the chain is about to transition to `RUN_BUILD_FAILED`, the
worker checks the marker-absence signal:

  if (rc->build_pgid == 0 && exit_status != 0) {
      /* the setsid wrapper failed before its first action */
      char block[2048];
      if (bd_setsid_help_block(ctx->cfg.user, ctx->cfg.host,
                               ctx->cfg.port,
                               block, sizeof(block)) == BD_OK) {
          emit_build_log(ctx, block, strlen(block));
      }
  }
  /* then proceed to the existing RUN_BUILD_FAILED emission */

The help block is emitted through the **existing**
`emit_build_log` path, which packages bytes into `REV_BUILD_LOG`
events that the app already drains into the Build Log's
`logbuf`. The logbuf assembles lines incrementally across chunk
boundaries (the same mechanism used for real build output), so a
multi-chunk help block renders correctly without any UI changes.

Order matters: help-block chunks are emitted **before** the
`REV_PHASE(RUN_BUILD_FAILED)` event. The user reads the Build
Log top-to-bottom as: raw shell error → remediation help →
EXPLOIT FAILED banner.

The detection is **single-shot by construction**: the
condition (`build_pgid == 0`) is only evaluated at the BUILD →
FAILED transition, and the chain transitions there at most once
per EXECUTE/COMPILE.

**3. Visual demarcation reuses the existing precedent.**

The DevConsole already inserts `> ── NEW PAYLOAD ──` rules into
its log to mark ostrich-injected content (see
`logbuf_mark` in `liblogbuf`). The help block follows the same
voice: opens with `> ── REMEDIATION ──` and closes with
`> ── END REMEDIATION ──`, with the technical command lines in
between unprefixed and paste-ready. The wrappers make the
inserted block visually distinct from the surrounding raw tool
output without breaking the copyability of the commands.

**4. Nothing else changes.**

- `bd_build_cmd`, `bd_launch_cmd`, `bd_kill_cmd`,
  `bd_parse_pid_marker`, `bd_parse_product_path` — all
  unchanged.
- `setsid` remains a remote prereq. The README's "Remote Mac"
  section and the workaround it documents are unchanged.
- The README's "Known issues" entry is tightened (since the
  failure mode now self-documents in-app) but is **not
  removed** — the dependency itself still exists.
- `librunstate`, `liblogbuf`, `libssh`, `liblexicon`, and all
  UI / composition-root code are unchanged.
- No new SSH primitive, no new arenas, no new SPSC rings, no
  new event types in the run-event family.

### Control + data flow for a setsid-missing EXECUTE (after change)

UI intent → `session_run_submit(EXECUTE, …)`. The worker steps
the RunChain into BUILD as today. The wrapped `setsid sh -c …`
command runs on the remote; the remote shell emits
`command not found: setsid` and exits 127. The chunk handler
streams the shell-error bytes into the Build Log via
`REV_BUILD_LOG` as today. At the BUILD-exit transition, the
new check fires: marker was never seen (`build_pgid == 0`) and
exit was non-zero → the help block is generated by
`bd_setsid_help_block` and emitted as additional
`REV_BUILD_LOG` chunks. The existing
`REV_PHASE(RUN_BUILD_FAILED)` event follows. The UI's app frame
drains the events into the Build Log's logbuf in order.

## Interfaces

### `include/builddeploy.h` — additions only

  BdStatus bd_setsid_help_block(const char *user,
                                 const char *host,
                                 int port,
                                 char *buf, size_t cap);

Returns `BD_OK` on success, `BD_ERR_OOM` if `cap` is too small
for the full block. All existing functions in the header are
unchanged.

### Help-block content (illustrative)

For a connection where `cfg.user = "jake"`, `cfg.host =
"mac.local"`, `cfg.port = 22`:

```
> ── REMEDIATION ──
REMOTE MAC IS MISSING setsid.

To install, connect to the Mac:
    ssh jake@mac.local

Then on the Mac, run:
    brew install util-linux
    echo 'export PATH="'"$(brew --prefix util-linux)"'/bin:$PATH"' >> ~/.zshenv

Verify the fix with (from this host):
    ssh jake@mac.local 'command -v setsid'

(See README "Remote Mac (SSH target)" for context.)
> ── END REMEDIATION ──
```

For a non-default port (e.g. `cfg.port = 2222`), the two `ssh`
lines become:

    ssh -p 2222 jake@mac.local
    …
    ssh -p 2222 jake@mac.local 'command -v setsid'

The exact text is fixed in the implementation, but is captured
here as the contract the tests assert against.

### `libsession` — emission hook

No public-header change. A small block (~10 lines) is added
inside the BUILD-step exit handling in `src/session/session.c`,
guarded by the `build_pgid == 0 && exit_status != 0` condition.
The emission goes through the existing `emit_build_log` helper
and the existing `REV_BUILD_LOG` event family.

## Out of Scope

- **Removing the `setsid` dependency.** A Perl-wrapper ARD was
  considered (replacing the `setsid sh -c '…'` shell wrapper
  with `perl -MPOSIX -e '…' -- xcodebuild …`) and rejected on
  cost/value grounds. The dependency stays; this ARD only
  improves the failure-mode guidance.
- **Auto-installing the dependency.** ostrich does not modify
  the remote Mac. It runs commands the user authored. The
  remediation block tells the user exactly what to run; running
  it is the user's choice.
- **Proactive prereq probing at Connect or Discover.** A
  one-shot `command -v setsid` after the SSH handshake would
  catch the problem earlier but adds a new SSH path in the
  connect/discover flow. Deferred unless first-build friction
  remains a real complaint after this ARD ships. The reactive
  help-block in this ARD is the cheaper first step.
- **Generalizing the help system to other prereqs.** Only
  `setsid` is in scope. If future remote prereqs emerge (Perl,
  newer `xcrun` flags, devicectl-only operations on older
  macOS), generalize then. Premature generalization is
  explicitly avoided.
- **New event types in the run-event family.** Emission reuses
  `REV_BUILD_LOG`. No `REV_HELP` or similar is added.
- **Lexicon entries for the help text.** Per the chosen module
  ownership, `libbuilddeploy` owns the text directly (matching
  `bd_status_str` precedent). Lexicon stays a placeholder-free
  static table.
- **UI rendering changes.** The help block lands in the
  existing Build Log via the existing chunk path. No new panel,
  no new color, no new affordance.
- **A host-gated smoke tool.** No `tools/setsid_help_smoke.c`.
  The unit-level tests on `bd_setsid_help_block` plus a real
  build against a Mac without `setsid` (which any one of the
  existing user's reachable Macs can produce in seconds by
  removing the PATH entry) cover this end-to-end.

## Further Notes

### Doc reconciliation (top-authority flag)

This project leaves
`context/projects/xcode-project-build-and-deploy/ard.md`
substantively unchanged — the build/launch wrapper design and
the two-pronged kill remain as written. A brief
"See also" pointer is added under that ARD's Further Notes
("Doc reconciliation") referencing this project for the
in-app guidance on missing `setsid`.

`README.md` updates:

- The "Remote Mac (SSH target)" section is unchanged — it
  remains the source of truth for the install steps.
- The "Known issues" entry for `setsid` is tightened: the
  in-app guidance is mentioned ("when missing, the Build Log
  surfaces the fix"). The entry itself is **not removed** —
  the underlying dependency is real and still needs to be
  documented; the entry just no longer needs to lead with
  "this is opaque on first failure."

### Non-arena allocations

None. `bd_setsid_help_block` writes into a caller-supplied
buffer; the emission helper uses a stack-local buffer of fixed
size in the BUILD-failure code path (sized generously for the
expected help-block length).

### Failure-detection semantics

The detection is `rc->build_pgid == 0 && exit_status != 0` at
the BUILD-step exit. The two conditions together mean: the
wrapper itself failed before reaching its `printf` line, AND
the channel reported a non-zero exit. This is a structural
signal that does not depend on the remote shell's
error-message format (zsh, bash, dash all phrase
"command not found" differently). It will fire correctly for
**any** wrapper-failure-before-marker case, which today means
"`setsid` is missing" with very high confidence — but would
also fire correctly if some future wrapper failure had the
same shape. The text is `setsid`-specific because today's
wrapper is `setsid`-specific.

### Testing approach

- **`tests/builddeploy_test.c`** (black-box, the existing test
  binary) — new test cases for `bd_setsid_help_block`:
  - presence of the literal `REMOTE MAC IS MISSING setsid.`
    header;
  - presence of `ssh <user>@<host>` for the default-port case;
  - presence of `ssh -p <port> <user>@<host>` for the
    non-default-port case;
  - presence of the `brew install util-linux` line and the
    `.zshenv` line;
  - presence of the `command -v setsid` verify command;
  - presence of `> ── REMEDIATION ──` /
    `> ── END REMEDIATION ──` rules;
  - `BD_ERR_OOM` when `cap` is below the rendered length;
  - NUL-termination of the output.
  All existing assertions on `bd_build_cmd`, `bd_launch_cmd`,
  `bd_kill_cmd`, `bd_parse_pid_marker`, and
  `bd_parse_product_path` carry over unchanged (none of these
  are modified by this work).
- **No new smoke tool.** A real EXECUTE against a Mac that is
  missing `setsid` exercises the emission end-to-end (chunk
  capture, marker-absence detection, help-block emission,
  REV_BUILD_LOG → logbuf rendering, EXPLOIT FAILED banner).
- **No white-box reach-ins.** All assertions are on the public
  return values of `bd_setsid_help_block`.

### ARD / IMPL conformance checklist

- [x] **Arenas named + lifetimes stated** — no new arenas;
      `bd_setsid_help_block` is a caller-buffer-writing pure
      utility, and the emission site uses a stack-local buffer
      with function-scoped lifetime.
- [x] **Allocation is caller-controlled** —
      `bd_setsid_help_block` takes `char *buf, size_t cap`; no
      hidden allocators.
- [x] **Thread-confinement respected** — no cross-thread data
      added or moved. The help-block bytes are copied into
      `REV_BUILD_LOG` event records by the existing
      `emit_build_log` path, exactly as real build output is.
- [x] **Non-arena allocations flagged + justified** — none
      introduced.
- [x] **Module → library decisions made** — change is contained
      inside `libbuilddeploy` (the new helper) plus a small
      hook in `libsession` (no public-header change). No new
      libraries; no module reorganization.
- [x] **Library layout specified** — unchanged.
      `include/builddeploy.h` is the public contract;
      `src/builddeploy/*.c` is private;
      `build/libbuilddeploy.a` is the archive; linked into
      `ostrich` and `tests/builddeploy_test` as today.
- [x] **C/C++ seam identified** — unchanged.
      `libbuilddeploy` is pure C11; no new C++ surface.
- [x] **Error handling shape confirmed** — unchanged. The new
      function returns `BdStatus` (`BD_OK` / `BD_ERR_OOM`),
      results via out-params, companion strings via
      `bd_status_str` continue to apply.
- [x] **Test approach per library** — black-box additions to
      `tests/builddeploy_test.c`; no new test binary; no new
      smoke. Justification recorded above under Testing
      approach.
