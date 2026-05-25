# Implementation Plan - Logging & Debug Observability

## Purpose & Altitude

A developer-facing internal observability facility so the *developer
of ostrich* can see what the worker thread and the remote commands
are actually doing — the truth channel beneath the themed UI line.

This is a cross-cutting **infrastructure** concern, not a product
feature: it writes to a file, adds no end-user surface, alters no
happy path in `workflow.md`, and does not touch the diagnostics
footer. There is therefore **no PRD** and **no separate ARD** — the
architecture is small and self-contained, so the rationale and the
`coding_standards.md` conformance checklist live in the
"Architecture & Standards Conformance" section below (the agreed
impl-only altitude). It traces back to `design.md` ("lightweight,
fast, native") and `coding_standards.md`.

Today ostrich's only "what's going on" channel is the themed UI:
errors collapse to an enum → `status_str()` → a campy lexicon line
(e.g. `"HOST UNREACHABLE // NO ROUTE"`). The real detail — the
libssh2 error, the exact `xcodebuild`/`xcrun` command, its exit code
and raw output — is computed on the worker thread and discarded. The
only way to see the underbelly today is to quit and run a
`tools/*_smoke.c` binary by hand against a live Mac. This plan closes
that gap.

## Architecture & Standards Conformance

The design was settled by interview. Key decisions:

1. **Unified spine, operations-first.** One logging facility.
   Priority is what ostrich *does to the Mac* (command strings, raw
   output, exit codes, timing); internal events (phase transitions,
   etc.) ride the same spine via a subsystem tag.
2. **Sink: file primary + opt-in stderr mirror.** Durable append to
   a log file; optional stderr echo for live `tail`-style viewing.
   An in-app ImGui panel is **deferred** (it would be "the visuals"
   we want to get past, would force a high-volume worker→UI ring,
   and would show nothing on crash/hang).
3. **Module shape: plain source, the arena pattern.** The logger is
   `include/log.h` (pure-C public header everyone may include) +
   `src/log.c` (plain source, *not* a `.a` archive); symbols resolve
   at final link into the app/test/tool binaries. This mirrors
   `arena`, `spsc_ring`, and `framestats`. It must be callable from
   *inside* the sealed libraries (`libssh`, `libdiscovery`,
   `libsession`, `libstore`), which rules out making it a library.
4. **Global ambient logger (one sanctioned global).** `log_init()`
   runs once at startup *before any thread spawns*; thereafter a
   single file-static holds the fd + config and `LOG_*()` works
   anywhere with no handle, including deep inside a libssh2 callback.
   This is a deliberate, called-out departure from the "no globals"
   habit — justified because the logger allocates **no arena
   memory** (so it does not violate arena ownership). Threading a
   `Logger*` into every signature would discourage logging and
   defeat the purpose.
5. **Compile-time master switch `OSTRICH_DEBUG`, OFF by default.**
   Default `make` ships clean: every `LOG_*`/`LOG_BLOB` expands to
   `((void)0)` and `log_init`/`log_shutdown` are no-ops — no file
   created, literally zero cost. `make debug` adds
   `-DOSTRICH_DEBUG -g -O0` and lights up the full facility. This
   makes every instrumentation task trivially **releasable** (a
   no-op in the shipped build).
6. **Runtime control within a debug build (env vars, immutable).**
   `OSTRICH_LOG` sets the level (default `INFO`); `OSTRICH_LOG_FILE`
   overrides the path; `OSTRICH_LOG_STDERR=1` enables the stderr
   mirror. All read **once** at `log_init`; the level is a plain
   `int` both threads read that never changes after init, so **no
   atomic** is needed. Per-subsystem thresholds and runtime toggling
   are deferred.
7. **Levels:** `ERROR / WARN / INFO / DEBUG / TRACE`, default
   `INFO`. Lifecycle = INFO, capped body = DEBUG, parse-failure
   slice = WARN, fatal init = ERROR.
8. **Write model: stack buffer + one `write()` per record under
   `O_APPEND`.** Each record is `vsnprintf`'d into a fixed stack
   buffer (line ≈2 KB; blob ≈64 KB), then a single `write()` to the
   fd. The file is opened `O_APPEND`, so the kernel makes each
   append atomic at end-of-file on a local filesystem — worker and
   UI lines never tear, with **no mutex and no shared memory**.
   Writes are unbuffered (`write(2)`, not `fwrite`), so the log is
   crash-consistent. The stderr mirror is a second `write(2)`.
9. **Capture depth: lifecycle always, body capped, full on
   failure.** Always log command string / exit code / duration /
   bytes (INFO); a bounded raw-output slice at DEBUG; the relevant
   slice unconditionally at WARN on parse failure.
10. **File: XDG state dir, rotate-on-start.**
    `$XDG_STATE_HOME/ostrich/ostrich.log` (fallback
    `$HOME/.local/state/ostrich/ostrich.log`). Logs live in the
    *state* tree, **not** the config tree, so nuking logs never
    endangers `connections.ini` (saved passwords) or presets. On
    `log_init`: `mkdir -p` the dir, rename any existing
    `ostrich.log` → `ostrich.log.1`, open a fresh file. Keeps the
    current + previous session; survives one reflex relaunch after a
    crash; no hot-path size checks.
11. **Record format: plain-text columnar** (no JSON — there is no
    aggregator; the workflow is `grep` + eyeball):
    `<ISO8601 ms> +<delta>s <LEVEL> [<subsys>] [<thr>] <message>`
    e.g. `2026-05-25T14:03:22.481 +1.234s INFO  [disc] [wkr] `
    `exec ch=2 cmd="xcrun simctl list -json devices"`.
    The monotonic `+delta` gives free timing; the `[wkr]`/`[ui]`
    thread tag is essential for reading interleaved lines.
12. **libssh truth via the ambient logger.** For the real libssh2
    error, `libssh` logs it *itself* (via
    `libssh2_session_last_error`) at the point it computes the
    `SshStatus`, returning the coarse enum unchanged — so `ssh.h`
    needs **no** public-header change. (A clean demonstration of why
    the ambient logger was the right call.)

### `coding_standards.md` conformance checklist

- [x] **Arenas named + lifetimes** — N/A by design: the logger
      introduces no arenas and allocates **no** arena memory (stack
      buffers + one fd only). Deliberate departure called out: this
      foundational utility takes **no `Arena *`**, justified because
      it performs zero ostrich-owned allocation.
- [x] **Allocation caller-controlled** — the logger does not
      allocate, so no hidden allocator exists. The only owned OS
      resource is the log fd (opened at init, closed at shutdown).
- [x] **Thread-confinement respected** — no arena pointer crosses
      threads. The only shared state is one fd + immutable `int`s,
      set before the worker spawns and read-only after; line
      coherence comes from `O_APPEND` kernel atomicity, not shared
      memory. Each record formats into its own stack buffer
      (inherently thread-local).
- [x] **Non-arena allocations flagged** — the log fd is the one
      non-arena OS resource; called out. No `malloc`/`free`.
- [x] **Module → library decision** — `log` stays **plain source**
      (a shallow utility), not a `.a`, per "skip thin wrappers" and
      consistent with `arena`/`spsc_ring`/`framestats`.
- [x] **Library layout** — public `include/log.h`; private
      implementation `src/log.c` (single TU, no private header).
- [x] **C/C++ seam** — pure C11; `log.h` is C-includable with an
      `extern "C"` guard so `src/ui/ui.cpp` can use it too.
- [x] **Error handling shape** — `log_init` returns a `LogStatus`
      enum with a companion `log_status_str`; out-params not needed.
      Emit calls are fire-and-forget (logging never gates program
      logic; failures are swallowed).
- [x] **Test approach** — `tests/log_test.c`, black-box via
      `log.h`, compiled with `-DOSTRICH_DEBUG`, writes to a temp
      `OSTRICH_LOG_FILE` and reads it back to assert behavior.
      White-box not required.

## Summary of Tasks

1. **Core logging facility** — `log.h`/`log.c` + `log_test.c`: the
   full spine (levels, macros, `log_blob`, env config, state-dir
   path + rotate, stack-format + `O_APPEND` write, stderr mirror,
   timestamp + delta + thread tag, compile gate). Only task with an
   automated test.
2. **Stand up + mirror startup errors** — `log_init` before the
   worker spawns, `log_shutdown` on exit, main-thread tag, startup
   banner; mirror the two fatal `fprintf(stderr,…)` into the log
   (keeping stderr unconditional).
3. **Connection lifecycle** — worker thread tag; connstate phase
   transitions (INFO); the real libssh2 error logged inside `libssh`
   at status-computation points (WARN/ERROR).
4. **Command exec lifecycle** (ops-first core) — per remote command:
   command string + job/channel id at start (INFO); exit code +
   duration + byte count at completion (INFO); capped raw output
   body via `LOG_BLOB` (DEBUG).
5. **Discovery parsing + parse-failure capture** — parse-result
   counts (INFO/DEBUG); on parse failure, the disc status plus the
   raw output slice via `LOG_BLOB` (WARN).

## Task Dependency Relationships

```
        +-------------------------------+
        | Task 1: core log facility     |
        | (log.h/log.c + log_test.c)    |
        +---------------+---------------+
                        |
                        v
        +-------------------------------+
        | Task 2: stand up + mirror     |
        | startup errors (init/shutdown)|
        +---------------+---------------+
                        |
                        v
        +-------------------------------+
        | Task 3: connection lifecycle  |
        | (libssh truth + phases)       |
        +---------------+---------------+
                        |
                        v
        +-------------------------------+
        | Task 4: command exec lifecycle|
        | (ops-first core)              |
        +---------------+---------------+
                        |
                        v
        +-------------------------------+
        | Task 5: discovery parsing +   |
        | parse-failure capture         |
        +-------------------------------+
```

Tasks 3 and 4 both only *hard*-depend on Task 2; 3 is ordered first
because the connection comes up before any command runs, which reads
naturally in the log.

## Detailed Tasks

### Task 1 - Core logging facility

- **Status**: complete
- **Blocked by**: none
- **User stories covered**: n/a (infrastructure concern; no PRD)

#### What to build

The complete `log` spine as plain source plus its unit test. Public
API in `include/log.h` (C-includable, `extern "C"`):

```c
typedef enum { LOG_ERROR, LOG_WARN, LOG_INFO,
               LOG_DEBUG, LOG_TRACE } LogLevel;
typedef enum { LG_APP, LG_SSH, LG_CONN, LG_SESS,
               LG_DISC, LG_STORE, LG_UI } LogSubsys;
typedef enum { LOG_OK = 0, LOG_ERR_FILE } LogStatus;

LogStatus   log_init(void);        /* no-op if !OSTRICH_DEBUG */
void        log_shutdown(void);
void        log_set_thread_tag(const char *tag); /* _Thread_local */
void        log_emit(LogLevel, LogSubsys, const char *fmt, ...);
void        log_blob(LogLevel, LogSubsys, const char *label,
                     const char *data, size_t len);
const char *log_status_str(LogStatus);
```

Convenience macros gate on the compile switch:

```c
#ifdef OSTRICH_DEBUG
#  define LOG_INFO(sub, ...) log_emit(LOG_INFO, (sub), __VA_ARGS__)
   /* ...ERROR/WARN/DEBUG/TRACE likewise... */
#  define LOG_BLOB(lvl, sub, label, data, len) \
          log_blob((lvl), (sub), (label), (data), (len))
#else
#  define LOG_INFO(sub, ...) ((void)0)
   /* ...all macros expand to ((void)0)... */
#endif
```

End-to-end behavior: `log_init` reads `OSTRICH_LOG`,
`OSTRICH_LOG_FILE`, `OSTRICH_LOG_STDERR`; resolves the state-dir
path (decision 10); `mkdir -p`; rotates `ostrich.log` →
`ostrich.log.1`; opens the fd `O_APPEND|O_CREAT|O_WRONLY`; records
the monotonic start time; sets the immutable level; tags the calling
(main) thread. `log_emit` gates on level, formats one record
(decision 11) into a stack buffer, and does one `write()` (plus one
to stderr if mirrored). `log_blob` caps to the blob buffer with an
elision marker and writes the whole bounded block in one `write()`.

#### Technical Details

Plain-source module (decision 3); single file-static state struct
(decision 4) holding fd, level, stderr-mirror flag, and the start
`timespec`. Thread tag is a `_Thread_local char[8]` (default
`"main"`). Wall-clock from `CLOCK_REALTIME`, delta from
`CLOCK_MONOTONIC`. No arena, no `malloc`. Note the no-op macro
caveat: with `OSTRICH_DEBUG` off, arguments are unevaluated, so
debug-only locals must be `(void)`-marked or `#ifdef`-guarded to
avoid unused-variable warnings. Makefile gains a `debug` target
(`CFLAGS += -DOSTRICH_DEBUG -g -O0`) and `log_test` is compiled
**with** `-DOSTRICH_DEBUG`. `src/log.c` links into the app, every
test binary, and every smoke tool.

#### Acceptance criteria

- [x] `include/log.h` + `src/log.c` compile; symbols link into a
      test binary; `log.h` is includable from C and C++.
- [x] With `-DOSTRICH_DEBUG` and `OSTRICH_LOG_FILE` set, `log_init`
      creates the file; a second `log_init` renames the prior file
      to `ostrich.log.1`.
- [x] Emitted records match the columnar format (timestamp, delta,
      level, subsystem, thread tag, message), asserted by reading
      the file back.
- [x] Level gating: at default `INFO`, `LOG_DEBUG` writes nothing;
      `OSTRICH_LOG=debug` makes it appear.
- [x] `log_blob` truncates beyond the cap, emits one record with an
      elision marker, and writes the block in a single `write()`.
- [x] Without `-DOSTRICH_DEBUG`, macros are no-ops: no file is
      created and the default `make test` passes (the log test is
      built `-DOSTRICH_DEBUG`).
- [x] `make` builds with no logging compiled in; `make debug`
      builds the app with `-DOSTRICH_DEBUG -g -O0`.

### Task 2 - Stand up + mirror startup errors

- **Status**: complete
- **Blocked by**: Task 1
- **User stories covered**: n/a

#### What to build

Make the facility live in the running app. Call `log_init()` in app
init **before `session_open()` spawns the worker** (so the global is
fully configured before any second thread exists), tag the main
thread, write a startup banner record (build, pid, version), and
call `log_shutdown()` at clean exit. Mirror the two fatal startup
failures in `src/app/app.c` (`app.c:92` UI init,
`app.c:99` session open) into `LOG_ERROR(LG_APP, …)` **in addition
to** the existing `fprintf(stderr, …)`, which stays unconditional so
release users still see fatal errors.

#### Technical Details

The init-before-spawn ordering is the crux of the thread-confinement
guarantee (decision 4): after this point the file-static is
read-only across threads. In a release build `log_init`,
`log_shutdown`, and the banner are no-ops, so the only observable
behavior is the unchanged stderr messages.

#### Acceptance criteria

- [x] `log_init()` runs before `session_open()`; `log_shutdown()`
      runs at exit.
- [x] In a debug build, a successful launch writes a startup banner
      and rotates the prior run's file.
- [x] The two fatal startup errors appear in the debug log
      (`LOG_ERROR`) while still printing to stderr in all builds.
- [x] Default (release) build behavior is unchanged: no file, no
      records, only the existing stderr fatal messages.

### Task 3 - Connection lifecycle instrumentation

- **Status**: complete
- **Blocked by**: Task 2
- **User stories covered**: n/a

#### What to build

Make the connection path legible in the log. The worker tags its
thread `[wkr]` on entry. Each connstate phase transition is logged
(INFO) in the session worker with phase names (e.g.
`CONNECTING → AWAITING_HOSTKEY → ONLINE`, and failures → `SEVERED`).
At every point `libssh` maps an outcome to an `SshStatus`, it also
logs the underlying libssh2 detail (code + string) at WARN/ERROR —
the truthful counterpart to the campy reason line.

#### Technical Details

Phase logging sits where the worker emits `SessionEvent`s
(`src/session/session.c`, `emit_ev`) and across the connstate
machine (`src/connstate/connstate.c`). The libssh detail is logged
inside `src/ssh/ssh.c` at the status-computation sites via
`libssh2_session_last_error` / `libssh2_session_last_errno`; the
`ssh.h` public header is **unchanged** (decision 12). Pure additive
logging; no behavior change.

#### Acceptance criteria

- [x] Worker thread records carry the `[wkr]` tag.
- [x] Each connstate phase transition is logged with phase names
      (INFO).
- [x] On an SSH failure, the log carries the underlying libssh2
      code + string (auth / handshake / DNS / etc.), not just the
      `SshStatus` enum.
- [x] `ssh.h` is unchanged.
- [ ] Manual: a deliberate bad-host / bad-auth connect shows the
      real cause in the log while the UI still shows the campy line.

### Task 4 - Command exec lifecycle (ops-first core)

- **Status**: complete
- **Blocked by**: Task 2 (Task 3 recommended first)
- **User stories covered**: n/a

#### What to build

The payload that justifies the whole plan: make every remote command
visible. On the worker's exec path (and the discovery job driver),
log per command — the **exact command string** plus a job/channel id
at exec start (INFO); on completion the exit code, wall-clock
duration, and total byte count (INFO); and a **capped raw
stdout/stderr slice** via `LOG_BLOB` at DEBUG.

#### Technical Details

Instrument the channel exec/read/exit cycle the worker drives
(`src/session/session.c` `drive_disc_job` and the
`ssh_channel_exec` / `ssh_channel_read` / `ssh_channel_exit` calls
from `ssh.h`). Start and completion records share a job/channel id
so they correlate in an interleaved log. The DEBUG body reuses the
`log_blob` cap from Task 1. Additive only.

`djob_kind_str()` and the timing locals are gated by `#ifdef
OSTRICH_DEBUG` to suppress unused-symbol warnings in release builds
(per the no-op macro caveat in `coding_standards.md`).

A stub SSH library (`tests/ssh_stub.c`) simulates a full connect →
exec → read → exit cycle so `session_exec_test` can assert on log
output without a real Mac. The stub provides every symbol from
`ssh.h`; the test binary links `session.c` + stub, skipping
`libssh.a`/`liblibssh2.a`.

#### Acceptance criteria

- [x] Each remote command logs an exec-start record with the full
      command string and a job/channel id.
- [x] Each completion logs exit code, duration (ms), and total byte
      count.
- [x] At DEBUG, the captured output body is logged via `LOG_BLOB`,
      capped with an elision marker for large output.
- [x] Start and completion records share the job/channel id.
- [ ] Manual on a live Mac: a SCAN / READ / SWEEP run produces a
      readable command timeline in the log.

### Task 5 - Discovery parsing + parse-failure capture

- **Status**: complete
- **Blocked by**: Task 4
- **User stories covered**: n/a

#### What to build

Close the original failure case: when `xcodebuild`/`xcrun` returns
output the jsmn parser chokes on, make the offending bytes visible.
Log the parse-result counts on success (e.g. "parsed N schemes, M
configs"; "K targets") at INFO/DEBUG, and on a parse failure log the
`DiscStatus` plus the **relevant raw output slice** via `LOG_BLOB`
at WARN — unconditionally, because that is the moment fidelity
matters.

#### Technical Details

Instrument the jsmn parsers in `src/discovery/discovery.c` and the
`DEV_*_FAILED` emission points in `src/session/session.c`. The
failure slice reuses the `log_blob` cap (large enough to diagnose
typical discovery JSON). Additive only.

#### Acceptance criteria

- [x] Successful parses log a result-count summary (INFO/DEBUG).
- [x] A parse failure logs the `DiscStatus` and a raw output slice
      (WARN) via `LOG_BLOB`.
- [x] The logged slice is bounded but large enough to diagnose
      typical discovery JSON.
- [ ] Manual: feeding malformed / unexpected output (or an
      Xcode-version mismatch) surfaces the raw bytes in the log.
