# ARD — Connection (Connect to the Mac over SSH)

## PRD

This ARD realizes `context/projects/connection/prd.md` — ostrich's
first real link to a Mac. In one line: stand up an authenticated,
multi-channel-ready SSH session to a Mac, off the UI thread, with a
BREACH overlay and a thin connection bar, TOFU host-key trust against
`~/.ssh/known_hosts`, distinct failure reporting, keepalive-driven
auto-reconnect, and saved KNOWN HOSTS with an opt-in remembered
passkey. It stops cleanly at "connected and kept" — no discovery,
run-config, Play, or log panels.

The PRD fixed the *behaviors* (no freeze, cancelable, multi-channel-
ready, cross-thread safety, recoverable session state) and explicitly
deferred the *mechanism* — the worker model, arenas, module
boundaries, on-disk format, and host-key trust flow — to this ARD.
Those are the decisions recorded below.

## Explanation of Architectural Components

### Where we start

The codebase today is a single themed window. `src/main.c` creates
one 8 MB app arena and passes it to `ui_init`, then loops
`while (ui_frame(ui)) {}` and tears down. Memory is arena-based
(`include/arena.h`); modules expose a `status enum` + out-param +
`*_status_str` C interface; the only library with a C/C++ seam is
`ui` (ImGui/GLFW behind a pure-C `include/ui.h`); `arena`, `lexicon`,
and `framestats` are plain tested `.c`. Tests are black-box and may
print `SKIP` when the environment can't support them (e.g.
`SKIP: no display`). libssh2 is vendored as a submodule but unlinked.

`ui_frame` currently takes nothing and returns only a bool — it has
no notion of application state. That is the seam this project widens.

### The module map

Six new or changed compilation units, plus the existing app/UI:

- **`ssh`** (`include/ssh.h`, `src/ssh/*.c` → `build/libssh.a`) — a
  deep, pure-C wrapper over libssh2. It performs the *primitives* of
  a link: non-blocking TCP connect, SSH handshake, host-key check and
  trust against `~/.ssh/known_hosts`, ssh-agent / password auth, an
  exec-channel liveness probe, additional channel opens (multi-
  channel-ready), keepalive, and disconnect. It owns the libssh2
  session handle. It **spawns no threads** and runs no policy — it is
  *called by* the worker. Because libssh2 is C, there is **no C/C++
  seam** here (unlike `ui`).

- **`connstate`** (`include/connstate.h`, `src/connstate/*.c` →
  `build/libconnstate.a`) — the **pure** lifecycle core: the
  connection state machine, the backoff schedule, the failure-code →
  reason (`LexKey`) mapping, and connection-config validation. It has
  **zero** I/O, threads, or libssh2. This is the functional core; it
  is the most test-critical module and is fully unit-tested with no
  server.

- **`session`** (`include/session.h`, `src/session/*.c` →
  `build/libsession.a`) — the deep concurrency module and the
  imperative shell. It owns the **persistent SSH I/O worker thread**,
  the **two SPSC rings**, the **self-pipe**, and the **per-session
  arena**. It uses `ssh` and `connstate` internally and exposes a
  tiny, stable interface to the rest of the app: open, submit a
  command, poll an event, close. Nothing above it sees threads,
  rings, or libssh2. The log-streaming project later extends *this*
  library rather than the app.

- **`store`** (`include/store.h`, `src/store/*.c` →
  `build/libstore.a`) — saved connections (KNOWN HOSTS): load, save,
  MRU ordering, opt-in passkey persistence, path resolution, and
  restrictive file permissions. Serialize/deserialize is black-box
  testable.

- **`spsc_ring`** (`include/spsc_ring.h`, `src/spsc_ring.c`) — the
  lock-free single-producer/single-consumer ring primitive over
  fixed-size POD elements. It is plain tested `.c` (mirroring how
  `arena` is a tested primitive, not a library) and is consumed by
  `session`. The log-streaming project reuses it unchanged.

- **`ui`** (extended) — the BREACH overlay and the thin connection
  bar, rendered in the existing C++ `ui` library behind its pure-C
  header. `ui_frame` grows a view-model-in / form-in-out / intents-
  out signature (below). `lexicon` (existing plain `.c`) gains the
  connection copy keys.

- **`app`** (`src/app/*.c`, plain source — the composition root,
  exempt from the library rule) — the thin glue. Each frame it drains
  `session` events into a UI-thread-owned view struct, builds the
  view-model, calls `ui_frame`, and translates the returned intents
  into `session` commands and `store` calls. It owns the `store`-
  loaded connection list and the `ConnForm`. `main.c` shrinks to
  creating the app arena and running `app_init` / `app_tick` loop /
  `app_shutdown`.

How they relate, top to bottom:

```
  main.c
    └─ app/  (glue: pump events → view-model → ui_frame → intents)
         ├─ store    (libstore.a)    persistence
         ├─ ui       (libui.a)       overlay + bar  [C++ seam]
         └─ session  (libsession.a)  worker + rings + per-sess arena
              ├─ connstate (libconnstate.a)  pure policy
              ├─ spsc_ring (plain .c)        cross-thread handoff
              └─ ssh       (libssh.a)        libssh2 primitives
                   └─ third_party/libssh2 (liblibssh2.a, OpenSSL)
```

### Threading and the cross-thread handoff

There is exactly one worker thread: the **SSH I/O thread**, owned by
`session`. It owns the libssh2 session and every channel for that
session's whole life — libssh2 sessions are not safe to drive
concurrently, so confining all libssh2 calls to this one thread
removes that hazard by construction. The UI thread never links
against or calls libssh2.

The two threads communicate only through **two lock-free SPSC rings**
of fixed-size POD records:

- **command ring** (UI → worker): BREACH (carrying an `SshConfig`),
  ABORT, CLOSE, TRUST, DECLINE.
- **event ring** (worker → UI): phase changes, the probe result, a
  host-key verdict + fingerprint, and a failure reason — each a self-
  contained record with **inline fixed-size buffers** for host, user,
  reason, and fingerprint. **No pointers cross the boundary.** The
  producer copies into the record; the consumer copies out into its
  own memory. This satisfies the thread-confinement rule (an explicit
  copy/handoff, never a shared arena pointer) and sets the exact
  pattern log streaming will use for line chunks.

The worker is event-driven, never busy-waiting. It blocks in `poll()`
over `{ ssh socket fd, self-pipe read fd }` with a timeout:

- The UI thread, after pushing a command, writes one byte to the
  **self-pipe** to wake the worker (the classic self-pipe trick —
  portable across Linux and macOS, unlike `eventfd`).
- The `poll()` timeout drives periodic `libssh2_keepalive_send`; a
  dead Mac surfaces as a keepalive / I-O error or a missing reply
  within timeout, which the worker turns into a drop.
- The connect handshake runs **non-blocking** under the same `poll()`
  loop with an overall connect timeout, so ABORT wakes the worker
  mid-handshake and a hang resolves to `NO RESPONSE // TIMEOUT`.

### The connect protocol (and the host-key pause)

Connect is **not** fire-and-forget. The host-key decision puts an
operator round-trip in the middle of the handshake:

1. UI submits BREACH with the form's `SshConfig`.
2. Worker: non-blocking DNS + TCP connect (timeout-bounded), then
   steps the SSH handshake.
3. Worker checks the host key against `~/.ssh/known_hosts`:
   - **known + matches** → continue to auth.
   - **unknown host** → emit a host-key event with the fingerprint;
     enter `AWAITING_HOSTKEY`; **wait** for a TRUST or DECLINE
     command. TRUST appends to `~/.ssh/known_hosts` and continues;
     DECLINE aborts.
   - **mismatch** → emit a mismatch event; this is a hard **security
     stop** — only DECLINE/ABORT is honored, never proceed.
4. Worker authenticates (ssh-agent or password per `SshConfig`).
5. Worker runs the **liveness probe**: open an exec channel, run a
   trivial command (`true`), require a clean channel + exit. Failure
   here is `NO FOOTHOLD // SHELL DENIED`. Success here is what makes
   `* ONLINE` mean "the session can do real work," and it exercises
   the multi-channel path once.
6. Worker emits `ONLINE`; the UI dismisses the overlay instantly and
   shows the bar with `ACCESS GRANTED` as a dwelling stamp.

`connstate` therefore models these phases: `DISCONNECTED`,
`CONNECTING`, `AWAITING_HOSTKEY`, `ONLINE`, `REACQUIRING`, `SEVERED`,
plus a transient failure surfaced in the overlay with its reason.
Drop → `REACQUIRING` with backoff; recovery → `ONLINE`; budget
exhausted → `SEVERED`.

### Reconnect policy

On a drop the worker auto-reconnects with **exponential backoff +
full jitter, capped max interval**, over a **finite attempt budget**
(tunable: ~6 attempts / ~1–2 min). Transient blips heal silently
under `REACQUIRING SIGNAL…`; an exhausted budget transitions to the
terminal `LINK SEVERED`, which requires a manual re-breach (one Enter
with MRU pre-select). The schedule lives entirely in `connstate` as
tunable constants plus a pure `connstate_backoff_delay(attempt)` and
a severed predicate, tested deterministically. Local UI state
(selected connection, layout) is preserved across a drop because it
lives in app/UI memory, untouched by the worker.

### Update / close / switch semantics

The app compares the new form identity (host, port, user, auth)
against the live session's identity:

- **identity changed** → CLOSE the session and open anew.
- **only label / remember-passkey changed** → persist via `store`
  with no reconnect.
- **CLOSE** → tear down and return to the resting overlay.
- **selecting a different KNOWN HOST while connected** → CLOSE first,
  honoring one Mac at a time.

### Arenas and lifetimes

- **App arena** (existing, 8 MB, **UI-thread-confined**): the `Ui`
  handle, fonts, the `store`-loaded KNOWN HOSTS list, the `ConnForm`,
  and the per-frame view-model. Unchanged in lifetime.
- **Per-session arena** (**worker-thread-confined**): created with
  `arena_create` *inside the worker* on each connect, holding the
  session's ostrich-side allocations; reset/destroyed on disconnect
  and recreated per (re)connect. Lifetime = one live session.
- **Per-attempt scratch** (worker-thread-confined): transient connect-
  attempt allocations (address resolution, fingerprint formatting),
  reset at the start of each attempt. May be a region of the per-
  session arena.
- **SPSC ring storage + the `Session` control block**: see the
  flagged non-arena allocation below.
- **libssh2 / OpenSSL memory**: library-owned, on their own terms —
  out of scope for the arena rules.

**Flagged non-arena allocation (justified).** `session_open()`
`malloc`s the `Session` control block and the two SPSC ring buffers;
`session_close()` frees them. Reasons: (a) a dynamic open / close /
reopen lifecycle that does not fit arena reset-group semantics, and
(b) the rings are **shared across the UI/worker boundary**, which
thread-confined arenas categorically cannot back. This is the
sanctioned handoff-mechanism exception, not arena allocation. No
other non-arena allocation is introduced by ostrich code.

### Build integration

libssh2 is **direct-compiled** into `build/liblibssh2.a` via plain
Make — the same approach the Makefile already uses for GLFW: compile
the required `third_party/libssh2/src/*.c` with `-DLIBSSH2_OPENSSL`
and the necessary `HAVE_*` / config defines (a checked-in minimal
`libssh2_config.h` or `-D` flags, rather than running libssh2's
CMake/autotools). The OpenSSL backend is libssh2's reference backend:
best-tested, with solid ssh-agent support. `libssl` + `libcrypto` are
located via `pkg-config`, honoring the existing `~/.local`
include/lib handling and adding a Homebrew-prefix fallback on macOS
(`brew install openssl`). The build stays plain Make and honors
`CC`/`CFLAGS` overrides for Linux/macOS portability. Each new module
compiles to `build/lib<m>.a` and links into both `build/ostrich` and
its test binary.

## Interfaces

All public headers are C-includable, take a caller `Arena *` where
they allocate (except the flagged `session` case), and report failure
through a status enum + out-params with a companion `*_status_str`.
Signatures below are illustrative of shape, not final to the byte.

### `include/ssh.h` (libssh.a)

Non-blocking, resumable primitives. Step functions return
`SSH_AGAIN` until the operation completes, so the worker can drive
them under `poll()`. The socket fd is exposed for `poll()`. Pure C
over libssh2.

```c
typedef enum {
    SSH_OK = 0, SSH_AGAIN,
    SSH_ERR_DNS, SSH_ERR_REFUSED, SSH_ERR_TIMEOUT,
    SSH_ERR_HANDSHAKE, SSH_ERR_HOSTKEY_MISMATCH,
    SSH_ERR_AUTH, SSH_ERR_NO_SHELL, SSH_ERR_IO, SSH_ERR_OOM
} SshStatus;

typedef enum { SSH_AUTH_AGENT, SSH_AUTH_PASSWORD } SshAuth;

typedef struct {
    char    host[256];
    int     port;            /* default 22 */
    char    user[128];
    SshAuth auth;
    char    passkey[256];    /* used only for SSH_AUTH_PASSWORD */
} SshConfig;

typedef enum {
    SSH_HOSTKEY_OK, SSH_HOSTKEY_UNKNOWN, SSH_HOSTKEY_MISMATCH
} SshHostKeyVerdict;

typedef struct Ssh        Ssh;        /* opaque session  */
typedef struct SshChannel SshChannel; /* opaque channel  */

SshStatus   ssh_connect_start(Arena *a, SshConfig cfg,
                              Ssh **out, int *out_fd);
SshStatus   ssh_handshake_step(Ssh *s);
SshStatus   ssh_hostkey_check(Ssh *s, SshHostKeyVerdict *verdict,
                              char *fp, size_t fp_cap);
SshStatus   ssh_hostkey_trust(Ssh *s);   /* append to known_hosts */
SshStatus   ssh_auth_step(Ssh *s);
SshStatus   ssh_probe_step(Ssh *s, int *exit_code); /* exec `true` */
SshStatus   ssh_channel_open(Ssh *s, SshChannel **out); /* multi   */
SshStatus   ssh_keepalive(Ssh *s, int *seconds_to_next);
void        ssh_disconnect(Ssh *s);
const char *ssh_status_str(SshStatus st);
```

### `include/connstate.h` (libconnstate.a)

Pure. No I/O, threads, or libssh2.

```c
typedef enum {
    CONN_DISCONNECTED, CONN_CONNECTING, CONN_AWAITING_HOSTKEY,
    CONN_ONLINE, CONN_REACQUIRING, CONN_SEVERED
} ConnPhase;

/* inputs to the machine (from the worker / timers / UI cmds) */
typedef enum {
    EV_BREACH, EV_TCP_UP, EV_HOSTKEY_UNKNOWN, EV_HOSTKEY_MISMATCH,
    EV_HOSTKEY_OK, EV_TRUST, EV_DECLINE, EV_AUTH_OK, EV_AUTH_FAIL,
    EV_PROBE_OK, EV_PROBE_FAIL, EV_FAIL, EV_ABORT, EV_CLOSE,
    EV_DROP, EV_RECONNECT_OK, EV_BACKOFF_EXPIRED
} ConnEvent;

/* actions the imperative shell must perform */
typedef enum {
    ACT_NONE, ACT_START_CONNECT, ACT_WAIT_HOSTKEY, ACT_DO_AUTH,
    ACT_DO_PROBE, ACT_GO_ONLINE, ACT_SHOW_FAILURE,
    ACT_SCHEDULE_BACKOFF, ACT_TEARDOWN, ACT_SEVERED
} ConnAction;

typedef struct {
    ConnPhase phase;
    int       attempt;       /* reconnect attempt count        */
    SshStatus last_reason;   /* for failure display            */
} ConnState;

void        connstate_init(ConnState *cs);
ConnAction  connstate_step(ConnState *cs, ConnEvent ev);
double      connstate_backoff_delay(int attempt); /* sec, jitter */
bool        connstate_should_sever(int attempt);
bool        connstate_validate(const SshConfig *cfg);
LexKey      connstate_reason_lex(SshStatus st);   /* → camp copy */
const char *connstate_phase_str(ConnPhase p);
```

### `include/session.h` (libsession.a)

The deep interface the app lives against. Allocation is the flagged
`malloc`/`free` exception (shared rings + dynamic lifecycle), so
`session_open` does not take an `Arena *`.

```c
typedef struct Session Session;

typedef enum {
    CMD_BREACH, CMD_ABORT, CMD_CLOSE, CMD_TRUST, CMD_DECLINE
} SessionCmdKind;

typedef struct {
    SessionCmdKind kind;
    SshConfig      cfg;      /* valid for CMD_BREACH */
} SessionCmd;

typedef struct {
    ConnPhase phase;
    SshStatus reason;        /* meaningful on failure        */
    bool      hostkey_unknown, hostkey_mismatch;
    char      user_host[384];
    char      fingerprint[128];
} SessionEvent;

SshStatus   session_open(Session **out);            /* spawns wkr  */
bool        session_submit(Session *s, const SessionCmd *cmd);
bool        session_poll(Session *s, SessionEvent *out);
void        session_close(Session *s);              /* join + free */
const char *session_status_str(SshStatus st);
```

### `include/store.h` (libstore.a)

```c
typedef struct {
    char    label[64];
    char    host[256];
    int     port;
    char    user[128];
    SshAuth auth;
    bool    remember;        /* opt-in passkey persistence  */
    char    passkey[256];    /* empty unless remember+pwd   */
} Conn;

typedef struct {
    Conn *items;
    int   count;
    int   mru_index;         /* pre-selected on launch      */
} ConnList;

typedef enum {
    STORE_OK = 0, STORE_ERR_IO, STORE_ERR_PARSE,
    STORE_ERR_PERMS, STORE_ERR_OOM
} StoreStatus;

StoreStatus store_load(Arena *a, ConnList *out);
StoreStatus store_save(const ConnList *list);  /* atomic + 0600 */
StoreStatus store_path(char *buf, size_t cap);
const char *store_status_str(StoreStatus st);
```

On-disk: one line-based text file at `$XDG_CONFIG_HOME/ostrich/
connections` (fallback `~/.config/ostrich/connections` on Linux and
macOS), written via temp-file + atomic rename, `chmod 0600`. One
record per connection; the `passkey` field is present only when
`remember` is set; ssh-agent connections carry no secret. MRU order
is encoded in the file.

### `include/ui.h` (additions to libui.a)

The app owns the mutable form; the UI is a pure renderer of the
read-only view + form, returning discrete intents. It chooses overlay
vs connection bar from `view->phase`.

```c
typedef struct {
    ConnPhase   phase;
    const char *user_host;   /* for the bar                 */
    const char *reason;      /* failure line, ostrich voice */
    const char *fingerprint; /* host-key prompt             */
    bool        show_hostkey_prompt; /* unknown host        */
    bool        show_mismatch;       /* security stop       */
    const Conn *known_hosts;
    int         known_count;
} UiConnView;

typedef struct {
    char    host[256];
    char    port[8];
    char    user[128];
    char    passkey[256];
    SshAuth auth;
    bool    remember;
    int     selected_known_host;
} ConnForm;

typedef struct {
    bool breach, abort, close, update, save, trust, decline;
    int  select_host;        /* -1 = none                   */
} UiIntents;

bool ui_frame(Ui *ui, const UiConnView *view,
              ConnForm *form, UiIntents *out);
```

`main.c` and `tests/ui_test.c` are updated mechanically to the new
signature (a zeroed view/form and ignored intents in the test).

### `include/spsc_ring.h`

A typed, fixed-capacity, lock-free SPSC ring over POD elements of a
caller-given size. Single producer, single consumer; capacity is a
power of two; push fails when full, pop fails when empty.

```c
typedef struct SpscRing SpscRing;

SpscRing *spsc_create(size_t elem_size, size_t capacity_pow2);
bool      spsc_push(SpscRing *r, const void *elem);
bool      spsc_pop(SpscRing *r, void *out);
void      spsc_destroy(SpscRing *r);
```

(Backing storage is `malloc`-ed here as part of `session`'s flagged
shared-ring allocation; `spsc_create`/`destroy` localize it.)

### `include/lexicon.h` (additions)

New `LexKey`s sourced from `theme.md`'s canonical lexicon, so a
future straight-mode stays a no-UI swap. The connection set:
`BREACH` / `■ ABORT`; `KNOWN HOSTS`; `BREACHING PERIMETER…`;
`ACCESS GRANTED` (+ `…WELCOME, OPERATOR.`); `ACCESS DENIED`;
`* ONLINE`; `REACQUIRING SIGNAL…`; `LINK SEVERED`;
`HOST UNREACHABLE // NO ROUTE`; `PERIMETER SEALED // PORT CLOSED`;
`NO RESPONSE // TIMEOUT`; `HOST KEY MISMATCH // POSSIBLE
INTERCEPTION`; `NO FOOTHOLD // SHELL DENIED`; the new unknown-host
prompt line (e.g. `UNKNOWN HOST // <fingerprint>`); the empty state
`// NO KNOWN HOSTS`; and the field/auth labels (`HOST`, `PORT`,
`USER`, `AUTH`, `SSH-AGENT`, `PASSKEY`, `REMEMBER PASSKEY`) plus the
overlay identity (`OSTRICH // UPLINK`). `connstate_reason_lex` maps
each `SshStatus` failure to its key.

## Out of Scope

Carried straight from the PRD, plus the boundaries this ARD draws:

- **Discovery / auto-scan, run configuration, Play / build
  orchestration, and Build/Device log panels.** None are built. The
  session is made multi-channel-ready (`ssh_channel_open`) and the
  single probe channel is opened, but no concurrent worker stream is
  wired — that is the log-streaming project, which will extend
  `session` and reuse `spsc_ring`.
- **OS keychain / libsecret password storage.** This project does
  opt-in plaintext with `0600`; keychain is the deferred hardening.
- **Importing `~/.ssh/config`.** KNOWN HOSTS is ostrich's own store.
  (Host-key trust *does* use the system `~/.ssh/known_hosts`.)
- **SSH port-forwarding, the debugger, NAT traversal, multi-Mac /
  multi-window / multiple simultaneous sessions, and reattaching to
  an in-flight remote run.** Non-goals here; the layering does not
  preclude port-forwarding later.
- **Advanced SSH options** (ProxyJump, identity-file picker, per-host
  algorithm selection, compression, custom ciphers).
- **Dedicated automated tests for `ssh`, `session`, and an expanded
  `ui`.** A real connect cannot be unit-tested without a live server,
  and the design deliberately keeps no env-gated tests this project
  (see Testing below). `ssh`/`session` correctness is verified by
  real manual connection; `connstate` carries the logic coverage.

## Further Notes

### Testing

`make test` stays fully green on any machine with **no special
setup**. Dedicated, always-run, black-box test binaries:

- **`connstate`** — every state transition (including the host-key
  pause and the trust/decline branches), the `backoff_delay` curve +
  the severed cap, the `SshStatus → LexKey` failure mapping, and
  config validation. This is the meaningful weight of the gate.
- **`store`** — serialize/deserialize round-trip, MRU ordering, ssh-
  agent-stores-no-secret, opt-in passkey persisted, atomic write, and
  the `0600` permission assertion (in a temp dir).
- **`spsc_ring`** — FIFO ordering, wraparound, full/empty, capacity
  edges, single-threaded.

`ssh` and `session` get **no new test binary** (a real socket/thread
is required; their pure logic already lives in `connstate`). The
existing headless `ui_test` remains a smoke test (`SKIP: no display`)
and is updated only to compile against the new `ui_frame` signature.
This is a deliberate scoping choice recorded here per user story 47:
the gate stays trustworthy by covering the display-free, network-free
parts in full.

### Conformance checklist (`context/coding_standards.md`)

- [x] **Arenas named + lifetimes stated** — app arena (UI thread);
      per-session arena (worker thread, reset per (re)connect);
      per-attempt scratch (worker thread). Freeing is by reset/
      destroy.
- [x] **Allocation is caller-controlled** — every allocating public
      function takes an `Arena *`, except the single flagged
      `session` case.
- [x] **Thread-confinement respected** — the UI and worker threads
      share *only* the two SPSC rings via POD records copied in/out;
      no arena pointer crosses the boundary; the self-pipe + atomics
      are the only shared state.
- [x] **Non-arena allocations flagged + justified** — `session_open`
      `malloc`s the control block + the two shared rings (dynamic
      open/close lifecycle + cross-thread sharing); libssh2/OpenSSL
      memory is library-owned. No other ostrich `malloc`.
- [x] **Module → library decisions made** — `ssh`, `connstate`,
      `session`, `store` become `.a` libraries (sealed boundary +
      real complexity); `spsc_ring` stays a plain tested primitive
      (like `arena`); the worker glue stays in `src/app/` because the
      real logic is pushed down into `connstate`/`ssh`/`session`.
- [x] **Library layout specified** — `include/<m>.h` public,
      `src/<m>/` private, `build/lib<m>.a` archive, linked into the
      app and the test binary.
- [x] **C/C++ seam identified** — the only seam stays in `ui` (ImGui
      behind pure-C `ui.h`). `ssh` is pure C over libssh2's C API —
      no seam. Everything else is C11.
- [x] **Error handling shape confirmed** — status enum return + out-
      params + `*_status_str` across `ssh`, `session`, `store`; no
      hidden/global error state; no exceptions.
- [x] **Test approach per library** — `tests/{connstate,store,
      spsc_ring}_test.c` link the archives and exercise the public
      headers black-box. `ssh`/`session` are verified by real
      connection (justified above), not white-box peeking.

### Decisions resolved in this ARD

1. **Threading** — one persistent SSH I/O worker owns the session +
   all channels; the UI thread never touches libssh2.
2. **Handoff** — two lock-free SPSC rings of fixed POD records,
   copy-on-handoff; built now to set the log-streaming pattern.
3. **Worker placement** — wrapped in a `session` library (the app
   stays thin); the pure policy is a separate `connstate` library
   (functional core / imperative shell).
4. **UI data flow** — `ui_frame(view, form, intents)`; the app owns
   `ConnForm`; the UI retains no connection state.
5. **Host-key** — TOFU with a fingerprint prompt on a brand-new host
   (append on trust), a hard blocking stop on mismatch; the worker
   pauses mid-handshake awaiting TRUST/DECLINE.
6. **Persistence** — a single `0600` line-based file under
   `$XDG_CONFIG_HOME/ostrich/`, atomic write, inline opt-in passkey.
7. **Reconnect** — exponential + jitter, capped interval, finite
   budget → `LINK SEVERED`; pure schedule in `connstate`.
8. **Worker I/O** — non-blocking libssh2 + `poll({socket,
   self-pipe})` + self-pipe wakeups + libssh2 keepalive + connect
   timeout (abortable, prompt drop detection).
9. **Allocation** — `session` mallocs its handle + shared rings
   (flagged); worker uses a thread-confined per-session arena.
10. **Build** — libssh2 direct-compiled to `liblibssh2.a` with the
    OpenSSL backend via `pkg-config` (+ Homebrew fallback), plain
    Make, glfw-style.

### Theme & security posture

- **Whimsy stays zero-cost.** `BREACHING PERIMETER…` and
  `REACQUIRING SIGNAL…` dwell only for real network durations; there
  are no artificial pauses; success dismisses the overlay instantly.
  The host-key prompt and mismatch alert are genuine **security
  confirms/stops**, not decorative gates (allowed by `theme.md`
  Principle 1). The only motion is the slow `* ONLINE` pulse and the
  input-cursor blink.
- **Palette discipline** — decorative cyan/magenta for chrome;
  semantic green/red/amber only for granted/denied/busy. ostrich's
  voice surfaces (overlay, bar) carry the `>`/magenta signature and
  never recolor or fabricate real SSH output.
- **Trust** — TOFU plus the shared system `~/.ssh/known_hosts` is
  real protection against a host impersonating the Mac, mattering
  most on the password path. The opt-in plaintext passkey is an
  accepted single-user, local-only tradeoff, mitigated by `0600` and
  with OS-keychain storage recorded as deferred hardening.

### Traceability

Realizes `design.md` core goal #1 (connect over SSH via libssh2,
ssh-agent or explicit user/host/port/password, with visible,
recoverable session state) and its concurrency intent (a non-blocking
worker model designed in from the start). Traces to `workflow.md` for
the overlay → connection-bar structure, happy paths 1/2/6, the
persistence model, and one-Mac-at-a-time. Traces to `theme.md` for
the BREACH overlay, KNOWN HOSTS, the asymmetric auth beats, the
failure/host-key/reacquire/severed lexicon, and the voice signature.
