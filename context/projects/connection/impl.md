# Implementation Plan - Connection (Connect to the Mac over SSH)

## Summary of Tasks

Tackle in this order; each task is one buildable, testable, and
releasable commit/PR. Tasks 1-7 build the backend libraries bottom-up
while the app keeps shipping today's resting shell; task 8 widens the
UI seam with no behavior change; task 9 is the user-facing flip to a
working connection; tasks 10-15 layer the richer behaviors onto it.

1. **libssh2 build** — init the submodule and direct-compile
   `build/liblibssh2.a` (OpenSSL backend) into the plain Make build;
   a throwaway version smoke proves it links on Linux + macOS.
2. **spsc_ring primitive** — the lock-free SPSC ring
   (`include/spsc_ring.h` + `src/spsc_ring.c`) plus its black-box
   test; the cross-thread handoff `session` will use.
3. **lexicon connection keys** — add the connection copy `LexKey`s
   from `theme.md` and extend the lexicon test.
4. **ssh library** — `libssh.a`: `ssh.h` types + all libssh2
   primitives, with a dev-only `tools/ssh_smoke.c`.
5. **connstate library** — `libconnstate.a`: the pure lifecycle core
   (state machine, backoff, reason map, validation) + heavy test.
6. **store library** — `libstore.a`: saved-connections load/save
   (MRU, opt-in passkey, atomic `0600`) + black-box test.
7. **session library** — `libsession.a`: the worker thread, two
   rings, self-pipe, per-session arena, with `tools/session_smoke.c`.
8. **UI seam + app root** — widen `ui_frame` and extract
   `src/app/*.c`, with **no behavior change**.
9. **First light** — agent auth, known-hosts-only, BREACH/ABORT,
   distinct failures, probe → `* ONLINE` bar.
10. **Host-key TOFU** — unknown-host trust prompt + append, and the
    loud mismatch security stop.
11. **KNOWN HOSTS** — wire saved connections (no secrets) into the
    overlay: list, MRU pre-select, empty state, select, SAVE.
12. **Password auth** — the PASSKEY auth path + masked field.
13. **REMEMBER PASSKEY** — opt-in plaintext password persistence
    (`0600`) + restore into the form.
14. **Keepalive + auto-reconnect** — drop detection,
    `REACQUIRING SIGNAL…` + backoff, `LINK SEVERED`, state preserved.
15. **Update / close / switch** — connection-bar lifecycle controls.

## Task Dependency Relationships

```
Foundations (no blockers): 2 spsc_ring, 3 lexicon

  1 libssh2
     │
     ▼
  4 ssh ───────────────┐
     │                 │
     ├──► 5 connstate  │   (5 also needs 3 lexicon)
     ├──► 6 store      │
     │                 │
  2 ─┴──► 7 session ◄──┘   (7 needs 2, 4, 5)

  4 + 5 + 6 ──► 8 ui seam + app root ◄── 7
                     │
                     ▼
                 9 first light
                     │
   ┌────────┬────────┼─────────┬──────────┐
   ▼        ▼        ▼         ▼          ▼
  10       11       12        14         15
 host-    KNOWN    pass     keepalive   update/
 key      HOSTS    auth     +reconn     close/switch
 TOFU       │        │                    ▲
           │        │                     │
           └───┬────┘             (15 also needs 11)
               ▼
              13 REMEMBER PASSKEY  (needs 11 + 12)
```

## Detailed Tasks

### Task 1 - libssh2 build integration

- **Status**: pending
- **Blocked by**: none
- **User stories covered**: US-45 (libssh2 vendored-in and linked
  into the Make build), US-46 (builds on Linux and macOS).

#### What to build

Bring the vendored libssh2 submodule into the plain Make build as
`build/liblibssh2.a`, compiled against the OpenSSL backend, and prove
it links on both Linux and macOS. No ostrich code consumes it yet; a
throwaway version smoke (printing `libssh2_version(0)`) is the only
caller, so the shipping app is unchanged.

#### Technical Details

Per ARD "Build integration": init the `third_party/libssh2`
submodule and direct-compile the required `src/*.c` with
`-DLIBSSH2_OPENSSL` plus the necessary `HAVE_*` / config defines via a
checked-in minimal `libssh2_config.h` (glfw-style, not libssh2's
CMake/autotools). Locate `libssl` + `libcrypto` with `pkg-config`,
honoring the existing `~/.local` include/lib handling and adding a
Homebrew-prefix fallback on macOS. Keep `CC`/`CFLAGS` overrides
working. The version smoke is built by a dedicated Make target (not
part of `make test`, since it needs no server) and is superseded once
Task 4 consumes the archive.

#### Acceptance criteria

- [ ] `make` builds `build/liblibssh2.a` from the vendored submodule
      with the OpenSSL backend; the `libssh2` submodule is
      initialized.
- [ ] `libssl`/`libcrypto` are found via `pkg-config` with a Homebrew
      fallback on macOS; `~/.local` include/lib handling is honored.
- [ ] A throwaway smoke links `liblibssh2.a` and prints a real
      `libssh2_version(0)` string on Linux (the macOS path is wired).
- [ ] `make`/`make test` stay green and the shipping app is unchanged
      (no ostrich code references libssh2 yet).

### Task 2 - spsc_ring primitive

- **Status**: pending
- **Blocked by**: none
- **User stories covered**: US-44 (multi-channel-ready — this ring is
  the cross-thread handoff that lets later log streams drop on),
  US-47 (`make test` stays meaningful and green).

#### What to build

The lock-free single-producer/single-consumer ring the worker and UI
threads will hand POD records through. A standalone, fully unit-tested
primitive with no dependency on the rest of the project; the
log-streaming project reuses it unchanged.

#### Technical Details

Implements `include/spsc_ring.h` per ARD "Interfaces" →
`include/spsc_ring.h`: a typed, fixed-capacity ring over POD elements
of a caller-given size, power-of-two capacity,
push-fails-when-full / pop-fails-when-empty. Plain tested `.c`
(mirroring `arena`), not a library, per `coding_standards.md`
"Modules as compiled libraries" and ARD "The module map". Backing
storage is `malloc`-ed inside `spsc_create`/`spsc_destroy` — the
localized form of the flagged shared-ring allocation (ARD "Flagged
non-arena allocation"). Add `build/spsc_ring_test` to `make test`.

#### Acceptance criteria

- [ ] `include/spsc_ring.h` matches the ARD signature
      (`spsc_create`/`spsc_push`/`spsc_pop`/`spsc_destroy`).
- [ ] `tests/spsc_ring_test.c` black-box tests FIFO ordering,
      wraparound, full/empty edges, and capacity edges
      (single-threaded), per ARD "Testing".
- [ ] `make test` builds and runs `spsc_ring_test` alongside the
      existing pure-tier tests; all pass.
- [ ] Builds warning-clean under `-Wall -Wextra`; the shipping app is
      unchanged.

### Task 3 - lexicon connection keys

- **Status**: pending
- **Blocked by**: none
- **User stories covered**: US-48 (all connection copy sourced from
  the centralized lexicon so a future straight-mode is a no-UI swap);
  supports US-49/US-50 (palette/voice discipline) and US-47.

#### What to build

Extend the existing `lexicon` with every connection copy string the
later UI and `connstate` need, so no connection literal is ever
inlined in `src/ui/` or `src/app/`. Pure C; lookups stay infallible.

#### Technical Details

Add the `LexKey`s named in ARD "Interfaces" → `include/lexicon.h`
(additions), sourced verbatim from `theme.md`'s canonical lexicon:
`BREACH` / `■ ABORT`, `KNOWN HOSTS`, `BREACHING PERIMETER…`,
`ACCESS GRANTED` (+ `…WELCOME, OPERATOR.`), `ACCESS DENIED`,
`* ONLINE`, `REACQUIRING SIGNAL…`, `LINK SEVERED`, the
connection-failure lines (`HOST UNREACHABLE // NO ROUTE`,
`PERIMETER SEALED // PORT CLOSED`, `NO RESPONSE // TIMEOUT`,
`HOST KEY MISMATCH // POSSIBLE INTERCEPTION`,
`NO FOOTHOLD // SHELL DENIED`), the unknown-host prompt line, the
empty state `// NO KNOWN HOSTS`, the field/auth labels (`HOST`,
`PORT`, `USER`, `AUTH`, `SSH-AGENT`, `PASSKEY`, `REMEMBER PASSKEY`),
and the overlay identity `OSTRICH // UPLINK`. Keep the `LEX__COUNT`
terminator and the out-of-range placeholder behavior. Extend
`tests/lexicon_test.c`.

#### Acceptance criteria

- [ ] Every new connection `LexKey` resolves to the exact `theme.md`
      copy; `LEX__COUNT` still terminates the enum.
- [ ] An out-of-range key still returns a stable non-`NULL`
      placeholder (no crash).
- [ ] `tests/lexicon_test.c` asserts each new key is non-empty and
      the count is consistent; `make test` runs it and it passes.
- [ ] No behavioral change to the running app (keys unused until the
      UI tasks).

### Task 4 - ssh library + smoke

- **Status**: pending
- **Blocked by**: Task 1
- **User stories covered**: US-44 (multi-channel-ready —
  `ssh_channel_open` exists), US-45/US-46 (real SSH in the binary,
  Linux + macOS); foundation for US-6/7/18-25 (auth, host-key,
  probe, failure classification).

#### What to build

The deep, pure-C wrapper over libssh2 that performs the primitives of
a link — non-blocking TCP connect, the SSH handshake, host-key check
and trust against `~/.ssh/known_hosts`, ssh-agent / password auth, the
exec-channel liveness probe, additional channel opens, keepalive, and
disconnect. It spawns no threads and runs no policy. A dev-only
`tools/ssh_smoke.c` proves the wrapper against a real Mac before the
worker stacks on top.

#### Technical Details

Implements `include/ssh.h` exactly per ARD "Interfaces" →
`include/ssh.h` (the `SshStatus`/`SshAuth`/`SshConfig`/
`SshHostKeyVerdict` types and the resumable step functions returning
`SSH_AGAIN` until complete, with the socket fd exposed for `poll()`).
Builds `build/libssh.a` from `src/ssh/*.c`, linking `liblibssh2.a`
(Task 1). Pure C over libssh2's C API — no C/C++ seam (ARD "The
module map"; conformance "C/C++ seam identified"). Error handling is
the status-enum + out-param + `ssh_status_str` shape (ARD
conformance). Allocating calls take the caller's `Arena *`;
libssh2/OpenSSL memory is library-owned (ARD "Arenas and lifetimes").
Per ARD "Testing", `ssh` gets no automated test binary (a real socket
is required); instead `tools/ssh_smoke.c` is a host-gated dev harness
(agent-connect, run the `true` probe, print the verdict), built by a
dedicated Make target and excluded from `make test`.

#### Acceptance criteria

- [ ] `include/ssh.h` matches the ARD contract and is pure C; all
      libssh2 use is confined to `src/ssh/*.c`.
- [ ] `make` builds `build/libssh.a` and links it with
      `liblibssh2.a`, warning-clean, honoring `CC`/`CFLAGS`.
- [ ] `ssh_channel_open` exists and the probe path opens an exec
      channel (the multi-channel-ready seam), per ARD "The connect
      protocol".
- [ ] `tools/ssh_smoke.c` connects to a real Mac via ssh-agent, runs
      the `true` probe to a clean exit, and prints the result; it is
      not part of `make test`.
- [ ] `make test` stays green; the shipping app is unchanged (no app
      code calls `ssh` yet).

### Task 5 - connstate library

- **Status**: pending
- **Blocked by**: Task 3, Task 4
- **User stories covered**: US-47 (the meaningful weight of the
  `make test` gate), US-52 (visible, recoverable session state — the
  state model); backs US-25/US-31/US-33 (failure mapping, backoff,
  severed).

#### What to build

The pure lifecycle core: the connection state machine (including the
host-key pause and the trust/decline branches), the backoff schedule,
the `SshStatus → LexKey` failure-reason mapping, and connection-config
validation. Zero I/O, threads, or libssh2. This is the functional
core and the most test-critical module.

#### Technical Details

Implements `include/connstate.h` per ARD "Interfaces" →
`include/connstate.h` and the phase set in ARD "The connect protocol"
(`DISCONNECTED`, `CONNECTING`, `AWAITING_HOSTKEY`, `ONLINE`,
`REACQUIRING`, `SEVERED`) driven by `ConnEvent`s into `ConnAction`s.
The backoff is exponential + full jitter, capped, over a finite
attempt budget per ARD "Reconnect policy", exposed as the pure
`connstate_backoff_delay(attempt)` + `connstate_should_sever`.
`connstate_reason_lex` maps each failure `SshStatus` to its lexicon
key (Task 3). Builds `build/libconnstate.a`; includes `ssh.h` for the
shared types (Task 4) but links no libssh2. Fully black-box tested per
ARD "Testing".

#### Acceptance criteria

- [ ] `include/connstate.h` matches the ARD; the module is pure (no
      I/O, threads, or libssh2 linkage).
- [ ] `tests/connstate_test.c` covers every transition (incl. the
      host-key pause + trust/decline), the `backoff_delay` curve, the
      severed cap, the `SshStatus → LexKey` mapping, and config
      validation.
- [ ] Backoff is deterministic to test (the jitter seam is
      controllable) and `connstate_should_sever` fires at the budget
      per ARD "Reconnect policy".
- [ ] `make test` builds and runs `connstate_test`; all pass; the
      shipping app is unchanged.

### Task 6 - store library

- **Status**: pending
- **Blocked by**: Task 4
- **User stories covered**: US-40 (KNOWN HOSTS persist across
  launches), US-42 (ssh-agent stores no secret), US-43 (stored-
  password file is user-only), US-47 (gate stays meaningful).

#### What to build

The saved-connections store behind `store.h`: load, save, MRU
ordering, opt-in passkey persistence, path resolution, and
restrictive file permissions. The on-disk format and atomic write
live here; the app wires it into the UI in later tasks.

#### Technical Details

Implements `include/store.h` per ARD "Interfaces" →
`include/store.h`: the `Conn`/`ConnList` records and
`store_load`/`store_save`/`store_path`/`store_status_str`. On-disk is
one line-based text file at `$XDG_CONFIG_HOME/ostrich/connections`
(fallback `~/.config/ostrich/connections`), written temp-file +
atomic rename, `chmod 0600`; the `passkey` field is present only when
`remember` is set; ssh-agent connections carry no secret; MRU order
is encoded in the file. `store_load` takes the caller `Arena *`.
Includes `ssh.h` for `SshAuth` (Task 4). Builds `build/libstore.a`.
Black-box tested per ARD "Testing".

#### Acceptance criteria

- [ ] `include/store.h` matches the ARD; `store_load` takes an
      `Arena *`; errors use the status-enum + `store_status_str`
      shape.
- [ ] `tests/store_test.c` covers serialize/deserialize round-trip,
      MRU ordering, ssh-agent-stores-no-secret, opt-in passkey
      persisted, and atomic write, in a temp dir.
- [ ] The saved file is created `0600` and the test asserts the
      permission bits (US-43).
- [ ] `make test` builds and runs `store_test`; all pass; the
      shipping app is unchanged.

### Task 7 - session library + smoke

- **Status**: pending
- **Blocked by**: Task 2, Task 4, Task 5
- **User stories covered**: US-15 (window stays smooth during connect
  — work is off the UI thread), US-34 (reconnect also off the UI
  thread), US-44 (multi-channel-ready worker model).

#### What to build

The concurrency module and imperative shell: it owns the single
persistent SSH I/O worker thread, the two SPSC rings, the self-pipe,
and the per-session arena, and exposes a tiny interface (open, submit
a command, poll an event, close). Nothing above it sees threads,
rings, or libssh2. A dev-only `tools/session_smoke.c` drives a real
connection through the public interface before any UI exists.

#### Technical Details

Implements `include/session.h` per ARD "Interfaces" →
`include/session.h` and the concurrency design in ARD "Threading and
the cross-thread handoff" + "The connect protocol": the worker blocks
in `poll({ ssh socket, self-pipe })`, drives non-blocking libssh2
under the same loop, runs the connect protocol (incl. the
`AWAITING_HOSTKEY` pause awaiting TRUST/DECLINE), and exchanges
fixed-size POD records (inline buffers, no pointers crossing the
boundary) over the command/event rings (Task 2), using `ssh` (Task 4)
and `connstate` (Task 5) internally. `session_open` `malloc`s the
control block + the two rings and `session_close` joins + frees them
— the flagged non-arena allocation (ARD "Flagged non-arena
allocation"); the worker uses a thread-confined per-session arena (ARD
"Arenas and lifetimes"). Builds `build/libsession.a`. Per ARD
"Testing" it gets no automated test binary; `tools/session_smoke.c`
(open → submit BREACH → poll to `ONLINE` → close against a real Mac)
is the host-gated dev harness, excluded from `make test`.

#### Acceptance criteria

- [ ] `include/session.h` matches the ARD; callers see no threads,
      rings, or libssh2 through it.
- [ ] The worker is event-driven via `poll()` + self-pipe wakeups
      (no busy-wait); only the two SPSC rings + self-pipe + atomics
      cross the boundary (ARD conformance "Thread-confinement").
- [ ] The `session_open` malloc / `session_close` free of the control
      block + rings is the only new non-arena allocation; the worker
      uses a per-session arena.
- [ ] `tools/session_smoke.c` reaches `ONLINE` against a real Mac and
      closes cleanly (join, no leak); it is not part of `make test`.
- [ ] `make`/`make test` stay green; the shipping app is unchanged
      (the app does not yet create a session).

### Task 8 - UI seam + app composition root

- **Status**: pending
- **Blocked by**: Task 4, Task 5, Task 6
- **User stories covered**: none directly user-visible — the enabling
  refactor for US-1/US-28 (overlay + bar) and the app-glue data flow;
  explicitly no behavior change.

#### What to build

Widen the `ui_frame` seam and extract the composition root, with the
running app rendering exactly today's resting shell. `ui_frame` gains
the view-model-in / form-in-out / intents-out signature; `main.c`
shrinks to creating the app arena and running an `app_init` /
`app_tick` / `app_shutdown` loop in new `src/app/*.c`; the headless
`ui_test` is updated mechanically. Phase-branching (overlay vs bar) is
deliberately *not* added here.

#### Technical Details

Implements the new `ui_frame` and the
`UiConnView`/`ConnForm`/`UiIntents` types per ARD "Interfaces" →
`include/ui.h` (additions), keeping the pure-C `extern "C"` header and
the C++ confined to `src/ui/*.cpp` (ARD conformance "C/C++ seam").
`src/app/*.c` is plain source — the composition root, exempt from the
library rule (`coding_standards.md` "App / composition root") — and
consumes only public headers. This task wires the *shape*: the app
passes a zeroed/resting `UiConnView` + a `ConnForm` and ignores
intents; `ui_frame` still paints the existing wordmark/footer
regardless of phase. The app arena (existing 8 MB, UI-thread-confined)
gains room for the `ConnForm` and per-frame view-model (ARD "Arenas
and lifetimes"). No `session`/`store`/`ssh` calls yet.

#### Acceptance criteria

- [ ] `include/ui.h` carries the new `ui_frame` signature +
      `UiConnView`/`ConnForm`/`UiIntents`, pure C, no C++ types.
- [ ] `src/main.c` is reduced to the app arena +
      `app_init`/`app_tick`/`app_shutdown`; orchestration lives in
      `src/app/*.c`, which touches no ImGui directly.
- [ ] `tests/ui_test.c` compiles against the new signature (zeroed
      view/form, ignored intents) and still skips with
      `SKIP: no display`.
- [ ] Running `./build/ostrich` is visually identical to before (same
      resting shell + footer); `make`/`make test` stay green.

### Task 9 - First light (agent connect, end to end)

- **Status**: pending
- **Blocked by**: Task 7, Task 8
- **User stories covered**: US-1, US-2, US-3, US-4, US-5 (agent),
  US-6, US-11, US-14, US-15, US-16, US-17, US-19, US-22, US-23,
  US-24, US-25, US-26, US-27, US-28, US-29, US-44, US-51, US-52.

#### What to build

The first real link to a Mac. On launch ostrich shows the modal
BREACH overlay over the resting wordmark, with HOST / PORT (default
22) / USER and an SSH-AGENT auth path. Committing BREACH (button or
Enter) connects off the UI thread — `BREACHING PERIMETER…` for the
real handshake duration, with `■ ABORT` available — verifies the host
key against `~/.ssh/known_hosts` (known + matches only; unknown or
mismatch fail here with the host-key reason, full TOFU lands in Task
10), authenticates via ssh-agent, runs the `true` exec-channel probe,
and on success dismisses the overlay instantly to a thin connection
bar showing `user@host  * ONLINE` (with `ACCESS GRANTED` as a
dwelling stamp and a slow `* ONLINE` pulse). Failures surface in
ostrich's `>` voice in the overlay with entered details preserved.

#### Technical Details

This is the user-facing flip described in PRD "Solution" and the
first exercise of ARD "The connect protocol" end to end. The app
(`src/app/*.c`) creates the `session` (Task 7) at startup, drains
`session_poll` events each frame into a UI-thread-owned view struct,
builds the `UiConnView`, calls `ui_frame`, and translates returned
intents (BREACH / ABORT) into `SessionCmd`s (ARD "App / composition
root"). The overlay/bar rendering is added in `src/ui/*.cpp`, choosing
overlay vs bar from `view->phase` (now actually branched). All copy is
pulled from `lexicon` (Task 3); palette discipline and the `>` voice
signature are honored, and ostrich's voice never recolors or
fabricates SSH output (US-49/US-50). The distinct failure lines come
from `connstate_reason_lex` (Task 5) mapped from the worker's
`SshStatus`. Host-key handling this task is known-matches-or-fail (no
append, no prompt); `AWAITING_HOSTKEY` is not yet entered. Keyboard:
tab between fields, Enter = BREACH, a sane Esc (US-51). Multi-channel
readiness is proven by the probe opening an exec channel (US-44).
ostrich never auto-connects (US-11).

#### Acceptance criteria

- [ ] On launch the modal BREACH overlay paints instantly over the
      resting wordmark with HOST/PORT/USER + SSH-AGENT, fully
      keyboard-drivable; nothing connects until BREACH/Enter.
- [ ] A BREACH to a Mac already in `~/.ssh/known_hosts`, via ssh-
      agent, shows `BREACHING PERIMETER…` for the real duration,
      passes the `true` probe, and lands `* ONLINE` in the bar with
      the overlay dismissed instantly; the window stays smooth and
      `■ ABORT` cancels a hung attempt.
- [ ] `* ONLINE` only appears after a clean exec-channel probe
      (US-23) and the `* ONLINE` dot pulses slowly.
- [ ] Distinct failures (host unreachable / port closed / timeout /
      `ACCESS DENIED` / `NO FOOTHOLD`, plus a host-key reason for an
      unknown/mismatched key) render in the `>` voice in the overlay
      with entered details preserved.
- [ ] All connection copy is sourced from `lexicon`; palette/voice
      discipline holds; `make test` stays green (the headless
      `ui_test` still skips without a display).

### Task 10 - Host-key TOFU (trust prompt + mismatch stop)

- **Status**: pending
- **Blocked by**: Task 9
- **User stories covered**: US-18, US-20, US-21.

#### What to build

Complete trust-on-first-use. Connecting to a brand-new host now shows
the unknown-host fingerprint prompt; on TRUST ostrich appends the
fingerprint to `~/.ssh/known_hosts` and continues the handshake, on
DECLINE it aborts. Connecting to a known host whose key has changed
raises the loud, blocking `HOST KEY MISMATCH // POSSIBLE
INTERCEPTION` security stop — only DECLINE/ABORT is honored, never
proceed.

#### Technical Details

Implements the mid-handshake operator round-trip from ARD "The
connect protocol (and the host-key pause)" and "Decisions resolved
in this ARD" §5: the worker emits the host-key event with the
fingerprint, enters `AWAITING_HOSTKEY`, and waits for a TRUST or
DECLINE command; `ssh_hostkey_trust` appends to
`~/.ssh/known_hosts`. The UI renders the unknown-host prompt and the
mismatch alert from `view->show_hostkey_prompt` / `view->show_mismatch`
+ `fingerprint` (ARD "Interfaces" → `include/ui.h`). The mismatch is a
genuine security stop, not a whimsy gate (PRD "Further Notes";
`theme.md` Principle 1). The state-machine branches already exist in
`connstate` (Task 5); this task wires the worker emit + the UI prompt +
the TRUST/DECLINE intents.

#### Acceptance criteria

- [ ] A brand-new host shows the unknown-host prompt with its
      fingerprint; TRUST appends to `~/.ssh/known_hosts` and the
      handshake continues to `* ONLINE`; DECLINE aborts cleanly.
- [ ] After a TRUSTed connect, the host is recognized without
      re-prompting on the next connect (shares system known_hosts).
- [ ] A changed key for a known host raises the loud blocking
      `HOST KEY MISMATCH // POSSIBLE INTERCEPTION` stop; proceeding is
      impossible — only DECLINE/ABORT works.
- [ ] Copy comes from `lexicon`; `make test` stays green.

### Task 11 - KNOWN HOSTS (saved connections, no secrets)

- **Status**: pending
- **Blocked by**: Task 6, Task 9
- **User stories covered**: US-9, US-10, US-12, US-13, US-40, US-42.

#### What to build

Wire saved connections into the overlay. The overlay shows a KNOWN
HOSTS list of saved connections; selecting one fills the form; SAVE
persists the current details (label, host, port, user, auth) as a
KNOWN HOST; the most-recently-used connection is pre-selected on
launch so the everyday agent path is launch → Enter; an empty store
reads `// NO KNOWN HOSTS`. ssh-agent connections store no secret.

#### Technical Details

The app loads the `ConnList` via `store_load` (Task 6) into the app
arena at startup and owns it (ARD "App / composition root" + "Arenas
and lifetimes"), passes it into `UiConnView.known_hosts`, and handles
the `select_host` / `save` intents — `select_host` copies the chosen
`Conn` into the `ConnForm`, `save` writes via `store_save` with MRU
updated. MRU pre-select drives the launch → Enter daily path (PRD
"Solution"; workflow happy path 2). The empty state and list are
rendered from `lexicon` copy. This task persists only non-secret
fields; REMEMBER PASSKEY is Task 13. ostrich still never auto-connects
(US-11).

#### Acceptance criteria

- [ ] The overlay lists saved KNOWN HOSTS; selecting one fills the
      form; `// NO KNOWN HOSTS` shows when the store is empty.
- [ ] SAVE persists label/host/port/user/auth via `store`, and the
      list survives a relaunch (US-40).
- [ ] The MRU connection is pre-selected on launch so an agent
      connect is launch → Enter (US-10).
- [ ] An ssh-agent KNOWN HOST stores no secret on disk (US-42);
      `make test` stays green.

### Task 12 - Password auth (PASSKEY path)

- **Status**: pending
- **Blocked by**: Task 9
- **User stories covered**: US-5 (PASSKEY choice), US-7.

#### What to build

Add the password authentication path. The overlay's AUTH control now
offers SSH-AGENT or PASSKEY; choosing PASSKEY reveals a masked
password field, and committing BREACH authenticates to a Mac that
requires a password. The host-key, probe, failure, and bar behaviors
are unchanged from the agent path.

#### Technical Details

Wires the `SSH_AUTH_PASSWORD` path that `ssh` already implements (Task
4) through `session` (Task 7) — the `SshConfig.auth` / `passkey`
carried in the BREACH `SessionCmd`. The UI gains the AUTH toggle +
masked PASSKEY field on the `ConnForm` (ARD "Interfaces" →
`include/ui.h`). The host-key mismatch protection matters most on this
path (PRD "Security posture"); since Task 10 already landed it,
password connects are protected. No persistence of the password yet
(Task 13).

#### Acceptance criteria

- [ ] AUTH offers SSH-AGENT and PASSKEY; selecting PASSKEY reveals a
      masked password field.
- [ ] A BREACH with PASSKEY authenticates to a password-requiring Mac
      and reaches `* ONLINE` via the same probe path; a wrong password
      yields `ACCESS DENIED` in the `>` voice with details preserved.
- [ ] The password is held only in the in-memory `ConnForm` (not
      persisted this task).
- [ ] Copy from `lexicon`; `make test` stays green.

### Task 13 - REMEMBER PASSKEY (opt-in persistence)

- **Status**: pending
- **Blocked by**: Task 11, Task 12
- **User stories covered**: US-8, US-41, US-43.

#### What to build

The opt-in convenience of a remembered password. An off-by-default
REMEMBER PASSKEY checkbox lets the operator choose to store the
password for a KNOWN HOST; when set, the password is saved and
restored into the form on select, so a password connection becomes a
single Enter. Storing is always the operator's explicit choice, and
the file stays user-only.

#### Technical Details

Surfaces the `remember` + `passkey` fields the `store` (Task 6) and
`ConnForm` / `Conn` records already carry, behind a checkbox that is
off by default (PRD US-8). On SAVE with remember set, the password is
written; the file is created/kept `0600` (ARD "Decisions resolved in
this ARD" §6; PRD "Security posture") — the only place plaintext
secrets touch disk, isolated here for review. On select, a remembered
password is restored into the `ConnForm`. ssh-agent connections still
store nothing. OS-keychain storage stays the deferred hardening
(PRD/ARD "Out of Scope").

#### Acceptance criteria

- [ ] REMEMBER PASSKEY is off by default; enabling it persists the
      password for that KNOWN HOST, disabling it removes the stored
      password.
- [ ] A remembered password is restored into the form on select, so a
      password connect is launch → Enter (US-41).
- [ ] The connections file is `0600` whenever a password is stored
      (US-43); agent connections still store no secret.
- [ ] `make test` stays green (the `store` permission/round-trip
      coverage from Task 6 still passes).

### Task 14 - Keepalive + auto-reconnect

- **Status**: pending
- **Blocked by**: Task 9
- **User stories covered**: US-30, US-31, US-32, US-33, US-34, US-52.

#### What to build

Keep the link alive and heal it. Keepalives detect a dropped Mac
promptly; a drop shows `REACQUIRING SIGNAL…` and auto-retries with
backoff entirely off the UI thread, preserving local state, and
resolves to `* ONLINE` on recovery or the terminal `LINK SEVERED`
when the attempt budget is exhausted.

#### Technical Details

Implements ARD "Reconnect policy" and the keepalive/timeout behavior
in ARD "Threading and the cross-thread handoff": the worker drives
`ssh_keepalive` off the `poll()` timeout and turns a keepalive/I-O
error or missing reply into a drop, then reconnects on the `connstate`
schedule (exponential + full jitter, capped, finite budget — Task 5),
emitting `REACQUIRING` / `ONLINE` / `SEVERED` phase events. All
retries run on the worker thread (US-34); local UI state (selected
connection, layout) is untouched because it lives in app/UI memory
(US-32; ARD "Reconnect policy"). The bar reflects the live phase so
session state is always visible and recoverable (US-52). A `SEVERED`
link requires a manual re-breach (one Enter with MRU pre-select).

#### Acceptance criteria

- [ ] A dropped Mac (sleep / Wi-Fi blip) is detected promptly via
      keepalive and shows `REACQUIRING SIGNAL…` rather than a hang.
- [ ] Reconnect retries run off the UI thread with backoff; a
      transient drop heals to `* ONLINE` automatically and the window
      stays smooth.
- [ ] An exhausted attempt budget transitions to `LINK SEVERED`;
      local state (selected connection, layout) is preserved across
      the whole drop/reconnect cycle.
- [ ] Phase copy from `lexicon`; `make test` stays green.

### Task 15 - Update / close / switch

- **Status**: pending
- **Blocked by**: Task 9, Task 11
- **User stories covered**: US-35, US-36, US-37, US-38, US-39.

#### What to build

The connection-bar lifecycle controls. An update control re-opens the
overlay pre-filled; changing connection identity (host, port, user, or
auth) tears down the live session and connects anew, while changing
only a label or the remember flag just persists with no reconnect. A
close control disconnects and returns to the resting overlay, and
selecting a different KNOWN HOST while connected closes the current
session first — one Mac at a time.

#### Technical Details

Implements ARD "Update / close / switch semantics": the app compares
the new `ConnForm` identity against the live session's identity and
either CLOSEs + reopens (identity changed) or persists via `store`
with no reconnect (label / remember-only). CLOSE tears down to the
resting overlay; selecting a different KNOWN HOST while connected
issues a CLOSE first (one-Mac-at-a-time; workflow mental model). These
map to the `update` / `close` / `select_host` intents (ARD
"Interfaces" → `include/ui.h`) and `CMD_CLOSE` + a fresh `CMD_BREACH`
(ARD "Interfaces" → `include/session.h`).

#### Acceptance criteria

- [ ] The bar's update control re-opens the overlay pre-filled with
      the live connection's details.
- [ ] Changing host/port/user/auth tears down and reconnects;
      changing only label / remember persists with no reconnect.
- [ ] Close disconnects and returns to the resting overlay.
- [ ] Selecting a different KNOWN HOST while connected closes the
      current session first; `make test` stays green.
