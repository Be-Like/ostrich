# ARD — Keychain unlock for codesign

## PRD

There is no standalone PRD for this work. The goal is scoped from
a live build-failure session (`metalmind` iOS build, codesign step
exiting with `errSecInternalComponent: Command CodeSign failed
with a nonzero exit code`) and is one in-app remediation for the
common locked-`login.keychain` failure mode that strands users at
the very last step of an otherwise-successful build.

Goal, one line:

  When a build would fail because the remote Mac's
  `login.keychain` is locked, let the user supply a keychain
  passkey **once per session** (or once-and-saved per connection)
  through a lazy modal, and silently unlock the keychain at the
  front of every build chain — without ever modifying the Mac's
  keychain auto-lock policy.

Motivation:

- An iOS device build on a freshly-rebooted Mac, or a Mac whose
  login keychain has auto-locked, surfaces a `codesign` failure
  at the very end of an otherwise-clean build. The raw output is
  opaque (`errSecInternalComponent`) and ostrich has no in-app
  affordance to fix it. The user has to leave ostrich, open a
  terminal, ssh in, run `security unlock-keychain`, and re-press
  EXECUTE.
- The remediation is one shell command per build
  (`security unlock-keychain -p '<pass>' <login.keychain>`),
  cheap and idempotent. The pain is purely UX: the user should
  type the passkey once and never see it again for the rest of
  the session (or never again at all if they opt to remember it).
- This work intentionally does **not** modify the Mac's keychain
  auto-lock policy (`security set-keychain-settings`). ostrich
  stays additive: every unlock is reissued from a cached passkey
  for the duration of the session, leaving no durable trace on
  the Mac. This matches the `setsid-install-help` precedent
  ("ostrich does not modify the remote Mac").

## Explanation of Architectural Components

### Where the failure surfaces today

For a target whose project requires codesigning (any iOS device
build, and `COMPILE` against the generic destination — which
still emits codesign work when `CODE_SIGNING_REQUIRED=YES`):

1. `bd_build_cmd` constructs the standard `setsid sh -c '…'`
   wrapper around `xcodebuild`.
2. The worker opens an SSH channel, runs the command, and streams
   bytes through `REV_BUILD_LOG`.
3. `xcodebuild` compiles successfully and reaches the `CodeSign`
   build phase.
4. `codesign` cannot access the signing identity's private key
   because the Mac's `login.keychain` is locked
   (no GUI session, or post-auto-lock).
5. `codesign` fails with `errSecInternalComponent`; the build
   step exits non-zero.
6. The chain transitions to `RUN_BUILD_FAILED`; the UI shows
   `EXPLOIT FAILED` with the raw codesign chunk in the Build Log.
   The user has no in-app affordance to fix the underlying
   "keychain is locked" condition.

Steps 1–6 (and the `setsid` wrapper, the chunk streaming, the
build-state machine) are correct behavior and remain unchanged.

### What changes

This is an additive feature. No libraries are reorganized; one
new chain step is grafted onto `RunChain`'s front, one new modal
is added to `libui`, two `Conn` fields persist the opt-in saved
passkey, and three pure helpers are added to `libbuilddeploy`.

**1. A new `unlock` step at the front of `RunChain`** (modifies
`libsession`). The chain becomes:

    unlock → settings → build → (prime) → install → launch

The `unlock` step is **gated on `WorkerCtx.kc_pass != ""`**. If
empty, the step is skipped silently and the chain begins at
`settings` exactly as today — there is no behavior change for
sessions where no passkey has been provided.

When `kc_pass` is non-empty, the step opens a fresh channel,
runs `bd_unlock_cmd`, accumulates output to EOF, and reads the
exit code:

- Exit 0 → emit a one-line confirmation chunk into the Build
  Log (`> KEYCHAIN UNLOCKED`); proceed to `settings`.
- Exit non-zero → emit the `bd_unlock_help_block` into the Build
  Log, fire `RUN_EV_UNLOCK_FAIL` (which transitions to
  `RUN_BUILD_FAILED` with `BD_ERR_UNLOCK_FAILED`), and abort the
  chain before any expensive step runs.

The step runs inside `RUN_BUILDING` — no new `RunPhase` is
introduced. The unlock command is wrapped in the same `setsid`
+ PID-marker shell envelope as `bd_build_cmd` so the existing
two-pronged ABORT path kills it correctly if the user aborts
during the (brief) unlock window.

**2. A new session-level setter for the worker's `kc_pass`**
(modifies `libsession`, `session.h`). Per the user choice
(Option Y from the design grilling), the keychain passkey is
**not** carried per-build on `SessionRunCmd`. Instead, a new
command kind injects it as worker-confined session state:

    SshStatus session_set_kc_pass(Session *s, const char *pass);

This enqueues `RCMD_SET_KC_PASS` on the existing `run_cmd_ring`,
carrying a fixed `char kc_pass[256]` POD field. The worker
processes it and writes the bytes into `WorkerCtx.kc_pass`
(worker-thread-confined). Passing `""` clears it. Ring ordering
guarantees that a `SET_KC_PASS` issued immediately before
`session_run_submit(EXECUTE, …)` is observed first — the worker
sees the new passkey before the chain starts.

Separating `SET_KC_PASS` from `EXECUTE`/`COMPILE` keeps:

- the cross-thread record `SessionRunCmd` from growing a
  256-byte passkey field that is meaningful only sometimes;
- the worker's run-chain code from coupling credential plumbing
  to the per-build payload;
- the unlock step testable in isolation against `WorkerCtx`
  state alone, independent of which run was submitted.

**3. Three new pure helpers in `libbuilddeploy`.**

    BdStatus bd_unlock_cmd(const char *kc_pass,
                           char *buf, size_t cap);

    BdStatus bd_unlock_help_block(
        const char *user, const char *host, int port,
        char *buf, size_t cap);

    BdStatus bd_codesign_hint_block(
        const char *user, const char *host, int port,
        char *buf, size_t cap);

`bd_unlock_cmd` constructs the shell-safe form
`security unlock-keychain -p '<pass>' \
  "$HOME/Library/Keychains/login.keychain-db"`, wrapped in
`setsid sh -c '…'` with the same PID-marker envelope
as `bd_build_cmd`. The passkey is single-quote escaped via the
existing `bd_quote` helper, then passed via an env-var
assignment (`__BD_KC_PASS=…`) so the inner shell expands it with
double-quoting — matching how `bd_build_cmd` handles
`__BD_PROJ`/`__BD_SCHEME`/etc.

`bd_unlock_help_block` is the F1-case rendering for "unlock
itself failed" (wrong passkey, missing/corrupt keychain). It
follows the exact pattern of `bd_setsid_help_block`: caller-
supplied buffer, `> ── REMEDIATION ──` / `> ── END REMEDIATION ──`
rules, `user@host` interpolated, optional `-p <port>` flag for
non-default port. Content:

```
> ── REMEDIATION ──
KEYCHAIN UNLOCK REJECTED.

The keychain passkey was rejected by the Mac. Likely causes:
    - wrong passkey (try again)
    - login.keychain is corrupt or missing

Verify manually from this host:
    ssh jake@mac.local 'security unlock-keychain \
        ~/Library/Keychains/login.keychain-db'

Press EXECUTE again; the keychain passkey modal will appear.
> ── END REMEDIATION ──
```

`bd_codesign_hint_block` is the H2-case footer emitted after a
non-simulator `RUN_BUILD_FAILED` where `kc_pass == ""` for that
run. Shorter — a hint, not a verdict:

```
> ── HINT ──
If this was a codesign / errSecInternalComponent failure, the
Mac's keychain may be locked. Press EXECUTE; the keychain
passkey modal will appear.
> ── END HINT ──
```

Why all three live in `libbuilddeploy`: same precedent as
`bd_setsid_help_block` (the in-line ARD notes:
*"the remediation is about a build-tooling prereq … the same
kind of text `bd_status_str` returns"*). No lexicon dependency;
the text is a string literal owned by
`src/builddeploy/builddeploy.c`.

A new enum value `BD_ERR_UNLOCK_FAILED` is added to `BdStatus`,
with matching `bd_status_str` and `bd_reason_lex` coverage and a
matching `runstate_reason_lex` mapping in `librunstate`.

**4. Two new RunEvents in `librunstate`.**

    RUN_EV_UNLOCK_OK,
    RUN_EV_UNLOCK_FAIL,

`RUN_EV_UNLOCK_OK` is a transient event that keeps `RunState`
in `RUN_BUILDING` and advances internal chain progress (no
observable phase change). `RUN_EV_UNLOCK_FAIL` transitions to
`RUN_BUILD_FAILED` exactly like `RUN_EV_BUILD_FAIL` does — same
phase, same reason-lex pipeline, but the reason value is
`BD_ERR_UNLOCK_FAILED` (a new `LexKey`).

No new `RunPhase` value is added — the unlock step lives inside
`RUN_BUILDING`, just as the existing `settings` substep does
(per the xcode-project-build-and-deploy ARD: *"the settings step
is the only request/response step … runstate→building"*).

**5. A new modal in `libui`** — the keychain passkey prompt.

The modal is a small ImGui popup with:

- One text input field (masked, like the BREACH `PASSKEY`
  field), bound to a `KcForm.passkey[256]` owned by the app.
- One checkbox `REMEMBER KEYCHAIN`, bound to
  `KcForm.remember` (mirrors the existing
  `REMEMBER PASSKEY` opt-in for SSH).
- Two buttons: `ENTER` (submit) and `SKIP` (skip-this-build,
  per C1).

It mirrors the BREACH modal's idiom and palette discipline.
`UiRunView` grows a `bool show_kc_prompt`; `UiRunIntents` grows
`bool kc_submit` and `bool kc_skip`. The app sets
`show_kc_prompt = true` exactly when its gating cascade fires
(below) and clears it on either intent.

`libui` stays sealed behind the existing pure-C `ui.h`. The
modal is C++/ImGui inside `src/ui/`; nothing else changes.

**6. `Conn` record extension in `libstore`.**

    typedef struct {
        char    label[64];
        char    host[256];
        int     port;
        char    user[128];
        SshAuth auth;
        bool    remember;       /* existing: SSH passkey       */
        char    passkey[256];   /* existing: SSH passkey       */
        bool    kc_remember;    /* NEW: keychain passkey opt-in */
        char    kc_passkey[256]; /* NEW: empty unless kc_remember */
    } Conn;

The on-disk format extends the existing line-based record by
two fields (no rename, no migration logic — older files
deserialize with `kc_remember = false` and `kc_passkey = ""`).
File path, atomic write, and `0600` permissions are unchanged.
ssh-agent connections may set `kc_remember` independently of
SSH auth method — the keychain passkey is its own secret.

**7. App-layer gating cascade** (modifies `src/app/`). On
`EXECUTE`/`COMPILE` intent:

    if target.is_simulator:
        # G2: no unlock for simulator builds
        session_set_kc_pass(s, "")
        session_run_submit(EXECUTE/COMPILE, …)
    elif app.kc_pass_cache != "":
        # in-session cache hit
        session_set_kc_pass(s, app.kc_pass_cache)
        session_run_submit(EXECUTE/COMPILE, …)
    elif active_conn.kc_remember and
         active_conn.kc_passkey != "":
        # persisted hit — promote to cache, then submit
        app.kc_pass_cache = active_conn.kc_passkey
        session_set_kc_pass(s, app.kc_pass_cache)
        session_run_submit(EXECUTE/COMPILE, …)
    else:
        # cold path — pop modal
        view.show_kc_prompt = true
        # (EXECUTE deferred until modal resolves)

On modal `ENTER` intent:

    app.kc_pass_cache = form.kc.passkey
    if form.kc.remember:
        active_conn.kc_remember = true
        active_conn.kc_passkey = form.kc.passkey
        store_save(connlist)
    session_set_kc_pass(s, app.kc_pass_cache)
    session_run_submit(EXECUTE/COMPILE, …)
    view.show_kc_prompt = false

On modal `SKIP` intent (C1):

    session_set_kc_pass(s, "")        # ensure worker has no pass
    session_run_submit(EXECUTE/COMPILE, …)
    view.show_kc_prompt = false
    # cache stays empty; next non-sim EXECUTE re-pops the modal

On `REV_PHASE(RUN_BUILD_FAILED, BD_ERR_UNLOCK_FAILED)`:

    app.kc_pass_cache = ""            # bad passkey, drop it
    session_set_kc_pass(s, "")
    # next EXECUTE re-pops the modal naturally

On disconnect (`CONN_SEVERED` or explicit `CMD_CLOSE`):

    memset(app.kc_pass_cache, 0, sizeof app.kc_pass_cache)
    # worker memory dies with the session; nothing to do there

The persisted passkey survives across sessions; the in-memory
cache and the worker copy do not.

### Control + data flow for an unlock-required EXECUTE

```
UI: user clicks EXECUTE on a device target.
app: gating cascade → no kc_pass cached, no persist; set
     view.show_kc_prompt = true; defer the EXECUTE submit.
UI: modal opens with the KEYCHAIN PASSKEY field and the
    REMEMBER KEYCHAIN checkbox.
UI: user types passkey, ticks REMEMBER, presses ENTER.
app: store_save() persists kc_remember/kc_passkey on the active
     Conn (since REMEMBER was ticked); app.kc_pass_cache filled;
     session_set_kc_pass(s, cache) enqueues RCMD_SET_KC_PASS;
     session_run_submit(EXECUTE, cfg, target) enqueues the chain.
worker: drains the ring — first SET_KC_PASS (writes
     WorkerCtx.kc_pass), then EXECUTE (starts RunChain).
worker: RunChain step 'unlock' — kc_pass non-empty, open channel,
     exec(bd_unlock_cmd), stream chunks → REV_BUILD_LOG, read
     exit code.
        exit 0 → REV_BUILD_LOG('> KEYCHAIN UNLOCKED\n'),
                 fire RUN_EV_UNLOCK_OK, proceed to settings.
        exit !0 → emit bd_unlock_help_block via REV_BUILD_LOG,
                  fire RUN_EV_UNLOCK_FAIL → RUN_BUILD_FAILED
                  (BD_ERR_UNLOCK_FAILED) via REV_PHASE, abort.
worker: settings → build → install → launch (unchanged).
```

On the H2 path (user pressed SKIP, build failed for a codesign
reason at the actual `build` step):

```
worker: RunChain step 'unlock' — kc_pass empty, skip step.
worker: settings → build (codesign error in build output) →
        build exit non-zero → emit bd_codesign_hint_block via
        REV_BUILD_LOG, then REV_PHASE(RUN_BUILD_FAILED,
        BD_ERR_BUILD).
```

The H2 hint is conditioned on `kc_pass == ""` and
`!target.is_simulator` and `RUN_BUILD_FAILED` from the actual
`build` step (not from unlock or settings). This avoids the
hint firing on simulator builds, on builds where the user
provided a passkey (unlock-rejected has its own help block), or
on settings-step failures.

## Interfaces

### `include/builddeploy.h` — additions

```c
typedef enum {
    BD_OK = 0,
    BD_ERR_XCODE_MISSING,
    BD_ERR_BUILD,
    BD_ERR_BOOT,
    BD_ERR_INSTALL,
    BD_ERR_LAUNCH,
    BD_ERR_PARSE,
    BD_ERR_OOM,
    BD_ERR_SETSID_MISSING,
    BD_ERR_UNLOCK_FAILED      /* NEW */
} BdStatus;

/* Build the keychain-unlock SSH command. The passkey is
   single-quote-escaped and passed via env-var assignment so
   the inner shell expands it under double quotes. Wrapped in
   the same setsid/PID-marker envelope as bd_build_cmd. */
BdStatus bd_unlock_cmd(const char *kc_pass,
                       char *buf, size_t cap);

/* F1 help block — emitted on unlock-step non-zero exit. */
BdStatus bd_unlock_help_block(const char *user,
                              const char *host,
                              int port,
                              char *buf, size_t cap);

/* H2 hint block — emitted after a non-simulator build failure
   where the unlock step was skipped (kc_pass empty). */
BdStatus bd_codesign_hint_block(const char *user,
                                const char *host,
                                int port,
                                char *buf, size_t cap);
```

`bd_status_str(BD_ERR_UNLOCK_FAILED)` returns
`"keychain unlock failed"`. `bd_reason_lex(BD_ERR_UNLOCK_FAILED)`
returns the new `LEX_REC_ERR_KC_UNLOCK` lexicon key. All other
existing functions in the header are unchanged.

### `include/runstate.h` — additions

```c
typedef enum {
    /* … existing events … */
    RUN_EV_UNLOCK_OK,        /* NEW: unlock step succeeded     */
    RUN_EV_UNLOCK_FAIL       /* NEW: unlock step exit non-zero */
} RunEvent;
```

`runstate_step(rs, RUN_EV_UNLOCK_OK)` keeps `rs->phase ==
RUN_BUILDING` and returns `RUN_ACT_NONE` (the chain step
advances inside the worker, not in the public state machine).
`runstate_step(rs, RUN_EV_UNLOCK_FAIL)` transitions
`rs->phase = RUN_BUILD_FAILED` and returns `RUN_ACT_DONE` — the
same shape as `RUN_EV_BUILD_FAIL`. `runstate_reason_lex(
RUN_BUILD_FAILED, BD_ERR_UNLOCK_FAILED)` returns
`LEX_REC_ERR_KC_UNLOCK`. No new `RunPhase` value; no new
`RunAction` value.

### `include/session.h` — additions

```c
typedef enum {
    RCMD_EXECUTE,
    RCMD_COMPILE,
    RCMD_ABORT,
    RCMD_SET_KC_PASS     /* NEW: writes WorkerCtx.kc_pass */
} SessionRunCmdKind;

typedef struct {
    SessionRunCmdKind kind;
    RunConfig         cfg;
    Target            target;
    bool              has_target;
    char              kc_pass[256];   /* valid for SET_KC_PASS */
} SessionRunCmd;

/* Set (or clear, when pass == "") the worker's keychain
   passkey for the current session. Enqueues RCMD_SET_KC_PASS
   on the existing run command ring; ordering with a subsequent
   session_run_submit(EXECUTE/COMPILE, …) is the ring's natural
   FIFO. Returns true if the command was enqueued. */
bool session_set_kc_pass(Session *s, const char *kc_pass);
```

`SessionRunEvent` is **unchanged**. The unlock step's bytes and
the F1 help block flow through the existing `REV_BUILD_LOG`
chunk events; the unlock step's success/failure transitions
flow through the existing `REV_PHASE` event with the new
`BD_ERR_UNLOCK_FAILED` reason value.

### `include/ui.h` — additions

```c
typedef struct {
    char passkey[256];
    bool remember;
} KcForm;

typedef struct {
    /* … existing fields … */
    bool show_kc_prompt;   /* NEW: render the keychain modal */
} UiRunView;

typedef struct {
    /* … existing intents … */
    bool kc_submit;        /* NEW: ENTER pressed in modal     */
    bool kc_skip;          /* NEW: SKIP pressed in modal      */
} UiRunIntents;
```

`KcForm` is owned by the app (parallel to `ConnForm`), passed
into `ui_frame` as a mutable pointer; the UI writes user keys
into `passkey`/`remember` while the modal is open; the app
reads on `kc_submit`. `ui_frame`'s signature gains a `KcForm *`
parameter (or grows it inside an existing forms struct,
depending on `libui`'s composite-form pattern at impl time).

### `include/store.h` — additions

```c
typedef struct {
    char    label[64];
    char    host[256];
    int     port;
    char    user[128];
    SshAuth auth;
    bool    remember;
    char    passkey[256];
    bool    kc_remember;        /* NEW */
    char    kc_passkey[256];    /* NEW: empty unless kc_remember */
} Conn;
```

On-disk format: the line-based record gains two fields at the
end of each connection record. Files written by earlier
versions are read with `kc_remember = false`,
`kc_passkey = ""` — no migration logic required because the
new fields are positional-additive and default-safe. The
`store_save` path continues to write `0600` atomically.

### Help-block content (illustrative)

For a connection with `user = "jake"`, `host = "mac.local"`,
`port = 22`, the F1 unlock-help block is:

```
> ── REMEDIATION ──
KEYCHAIN UNLOCK REJECTED.

The keychain passkey was rejected by the Mac. Likely causes:
    - wrong passkey (try again)
    - login.keychain is corrupt or missing

Verify manually from this host:
    ssh jake@mac.local 'security unlock-keychain \
        ~/Library/Keychains/login.keychain-db'

Press EXECUTE again; the keychain passkey modal will appear.
> ── END REMEDIATION ──
```

The H2 hint block is:

```
> ── HINT ──
If this was a codesign / errSecInternalComponent failure, the
Mac's keychain may be locked. Press EXECUTE; the keychain
passkey modal will appear.
> ── END HINT ──
```

For non-default port (e.g. `port = 2222`), the ssh line
becomes `ssh -p 2222 jake@mac.local …`. The exact text is
fixed in the implementation and is the contract the tests
assert against.

## Out of Scope

- **Modifying Mac keychain auto-lock policy.** Option II from
  the design grilling — `security set-keychain-settings` to
  disable auto-lock — was rejected on architectural grounds:
  it durably modifies user-owned state that survives ostrich
  exit, violating the setsid-help precedent
  (*"ostrich does not modify the remote Mac"*). The
  per-build re-unlock cost is negligible against a
  multi-minute `xcodebuild`.
- **Auto-detecting codesign failures via xcodebuild output
  string-matching.** Option H3 from the grilling
  (`errSecInternalComponent`, `Code Signing Error`,
  `codesign exited` regex) was rejected for fragility across
  Xcode versions, locales, and structured-output modes. The
  H2 hint is structural (`kc_pass == ""`,
  `!target.is_simulator`, build-step exit non-zero) — the same
  pattern as setsid-help's `build_pgid == 0` trigger.
- **OS keychain backing for the persisted passkey.** Stays
  with the existing connection-PRD-deferred hardening
  (libsecret on Linux, macOS Keychain on the host). The
  in-scope persistence is plaintext-with-`0600`, matching the
  existing `REMEMBER PASSKEY` posture exactly. This is
  recorded in workflow.md's persistence-deferred list.
- **Other codesign failure modes.** This ARD addresses
  exactly one cause: a locked `login.keychain`. Private-key
  ACL problems (codesign not in the key's access list),
  expired or missing provisioning profiles, signing-identity
  mismatch, `set-key-partition-list` issues on CI keychains —
  all are out of scope. If future demand emerges, generalize
  then; premature generalization is explicitly avoided, per
  setsid-help.
- **Keychains other than `~/Library/Keychains/login.keychain-db`.**
  `bd_unlock_cmd` hardcodes the path. Users with non-standard
  keychains can leave the modal blank and unlock by hand.
- **Proactive unlock at BREACH / Connect time.** Option P1
  from the grilling. The modal is lazy (P2): no field in
  the BREACH overlay, no SSH cost at connect time, no
  failure-mode coupling between connect and unlock.
- **A separate `unlock_test.c` binary.** All new test
  assertions land in existing test binaries
  (`builddeploy_test.c`, `runstate_test.c`, `store_test.c`,
  `session_run_test.c`) — same scoping discipline as
  setsid-help.
- **Theme/lexicon final wording.** This ARD fixes only the
  `LexKey` seam (`LEX_REC_ERR_KC_UNLOCK` and any modal-label
  keys); the canonical strings and `theme.md` reconciliation
  are that doc's call.
- **UI rendering changes beyond the new modal.** The Build
  Log, Live Feed, run-control cluster, target picker, preset
  selector, and connection bar are unchanged. The modal is
  the only new visible surface.

## Further Notes

### Non-arena allocations

None introduced. The kc_pass crosses the UI→worker boundary as
a 256-byte POD field inside `SessionRunCmd` (a record already
allocated from the existing `run_cmd_ring` storage, which is
the previously-flagged cross-thread `malloc` exception per the
xcode-project-build-and-deploy ARD). `WorkerCtx.kc_pass[256]`
lives inside the existing `WorkerCtx` struct on the worker
stack/heap (worker-thread-confined; lifetime is the worker's).
`app.kc_pass_cache[256]` lives inside the existing app-state
struct (UI-thread-confined; app-arena-lifetime). The two
help-block emission sites in `libsession` use stack-local
buffers, sized generously for the rendered length, matching
the setsid-help pattern.

### Failure-detection semantics (mirroring setsid-help)

The unlock-step-failed detection is structural:
**`unlock_exit_status != 0`**. No output string matching. The
help block fires on any non-zero exit from `security
unlock-keychain`, regardless of the remote shell's
error-message format.

The H2 hint-emit detection is structural:
**`!target.is_simulator && kc_pass == "" && RUN_BUILD_FAILED
from the build step (BD_ERR_BUILD)`**. The hint does **not**
fire on `BD_ERR_UNLOCK_FAILED` (its own help block already
fired), on `BD_ERR_SETSID_MISSING` (setsid-help already
fired), or on settings-step parse failures (`BD_ERR_PARSE`).

The two detections compose cleanly: the user gets at most one
remediation block per failed run (unlock-rejected OR codesign-
hint, never both), and they never get one when irrelevant
(simulator builds, builds that succeeded, or builds where they
already provided a passkey).

### Worker-side `kc_pass` lifetime

`WorkerCtx.kc_pass` is set by `RCMD_SET_KC_PASS`, read by every
`RunChain` start, and zeroed on `session_close` (alongside the
existing per-session arena teardown). It is **not** zeroed at
the end of each run — the user's stated intent is "one prompt
per session," so the passkey persists across `EXECUTE`s within
the same connected session. A subsequent `RCMD_SET_KC_PASS(""")`
from the app — issued on `BD_ERR_UNLOCK_FAILED`, on modal SKIP,
or on disconnect — explicitly clears it.

The worker copy and the app-side cache (`app.kc_pass_cache`)
are kept in sync by the app's discipline: every time the app
mutates the cache, it sends the matching `SET_KC_PASS`. The
worker is never the source of truth and never reports the
current `kc_pass` back to the app — the cache is one-way
UI→worker. This matches the existing
`UI-owns-form-and-state`/`worker-owns-link-and-channels` split.

### App-cache zeroing on disconnect

On any transition into `CONN_DISCONNECTED` or `CONN_SEVERED`
(per the existing connstate machine), the app composition root
calls `memset(app.kc_pass_cache, 0, sizeof app.kc_pass_cache)`
in the same frame it observes the transition. The persisted
`Conn.kc_passkey` is untouched (its lifetime is the on-disk
store, not the live session). On reconnect to the same Conn,
the gating cascade's "persisted hit" branch repopulates the
cache from the Conn record.

### Why no `RunPhase` change

The xcode-project-build-and-deploy ARD already established the
precedent: the chain's pre-build substeps (specifically
`settings`) live inside `RUN_BUILDING` rather than getting
their own phase. The unlock step is shorter and even less
user-visible than `settings`; promoting it to a `RUN_UNLOCKING`
phase would clutter the run-state machine for no UX win. The
Build Log already shows `> KEYCHAIN UNLOCKED` (on success) or
the F1 help block (on failure), which is the visible signal
the user needs.

### Why `RCMD_SET_KC_PASS` rather than a field on `SessionRunCmd`

Per Option Y from the design grilling:

1. **Separation of concerns.** Credential plumbing is session
   state, not per-build payload. A user who configures their
   passkey once and then runs ten builds should not have the
   passkey re-serialized into ten `SessionRunCmd` records.
2. **Testability.** The unlock step can be exercised against
   `WorkerCtx.kc_pass` set to known values, decoupled from the
   build command's RunConfig/Target plumbing. The stub-SSH
   `session_run_test.c` harness already isolates worker-side
   state from cross-thread payload mechanics — this fits.
3. **Smaller cross-thread record.** `SessionRunCmd` stays at
   its current size for `EXECUTE`/`COMPILE`/`ABORT`; only the
   new `SET_KC_PASS` kind carries the 256-byte field. The
   union-like nature of the existing struct (some fields
   meaningful only per kind) is preserved.

The ring-ordering guarantee (FIFO single-producer
single-consumer) is sufficient to make `SET_KC_PASS` →
`EXECUTE` behave atomically from the app's perspective: by the
time the worker drains `EXECUTE`, the preceding `SET_KC_PASS`
has already been applied.

### Doc reconciliation (top-authority flag)

- **`design.md`** — no change. The Mac is reachable, ostrich
  authenticates and orchestrates `xcodebuild`; whether the
  keychain is locked is a runtime detail beneath the design
  goals. The non-goal "No signing/provisioning management" is
  honored: ostrich does not configure or rotate signing
  identities, only unlocks the keychain so an
  already-configured codesign can find them.
- **`workflow.md`** — minimal addition. The persisted state
  list (currently "Connections", "Named run-config presets per
  connection", "Last-used target per connection") gains a
  fourth bullet: "**Per-connection opt-in remembered keychain
  passkey** — paired with the existing SSH `REMEMBER PASSKEY`
  opt-in; off by default; plaintext with `0600`." The deferred
  list's entry on "Keychain-backed password storage (macOS
  Keychain / libsecret) as the hardened replacement for
  opt-in plaintext" is broadened to cover the keychain
  passkey in addition to the SSH passkey.
- **`theme.md`** — gains the new lexicon keys: a modal title
  (e.g. `KEYCHAIN VAULT // PASSKEY` or whichever phrasing the
  theme owner lands on), `KEYCHAIN PASSKEY`, `REMEMBER
  KEYCHAIN`, `ENTER` (likely reuses an existing camp verb),
  `SKIP`, `KEYCHAIN REJECTED // INVALID PASSKEY`, and the
  hint/remediation block voice. This ARD fixes only the
  `LexKey` seams; the canonical strings are the theme owner's
  call.
- **`README.md`** — the "Remote Mac (SSH target)" section
  gains a brief paragraph about keychain unlock alongside the
  existing `setsid` paragraph, naming the lazy modal, the
  in-session cache, and the opt-in `REMEMBER KEYCHAIN`. The
  "Known issues" list gains an entry on the keychain
  prereq — tightened in the same style as the setsid entry
  (the failure mode self-documents in-app; the entry itself
  stays as documentation of the underlying remote-Mac fact).
- **`xcode-project-build-and-deploy/ard.md`** — a brief
  "See also" pointer is added under that ARD's Further Notes
  ("Doc reconciliation"), naming this project as the in-app
  remediation for the locked-keychain failure mode. The
  `RunChain` description (`settings → build → … → launch`) is
  updated to `unlock → settings → build → … → launch`, with a
  note that the new `unlock` step is gated on
  `WorkerCtx.kc_pass != ""`.
- **`connection/ard.md`** — no change. The connection layer
  does not learn about the keychain passkey; it remains
  scoped to SSH auth + host-key trust + reconnect.
- **`setsid-install-help/ard.md`** — no change. The two
  features are independent; the H2 hint composition rule
  ensures they never both fire on the same failure.

### Testing approach

All four named test binaries are existing. No new test
binary; no new smoke tool. Per the user's confirmation in the
grilling: full coverage on each.

**`tests/builddeploy_test.c`** (black-box additions):

- `bd_unlock_cmd`: shell escaping of the passkey (single
  quotes inside the passkey, backslashes, spaces, empty
  string returning a non-empty command that the inner shell
  will interpret as an empty passkey, OOM when `cap` is too
  small). The constructed command must wrap `security
  unlock-keychain` in the `setsid sh -c '…'` envelope with
  the same PID-marker `printf "__OSTRICH_PGID__%d\n" $$`
  prelude as `bd_build_cmd`.
- `bd_unlock_help_block`: presence of the literal
  `KEYCHAIN UNLOCK REJECTED.` header, presence of
  `ssh <user>@<host>` for the default-port case and
  `ssh -p <port> <user>@<host>` for non-default, presence of
  the `security unlock-keychain ~/Library/Keychains/login.\
  keychain-db` verify command, presence of the
  `> ── REMEDIATION ──`/`> ── END REMEDIATION ──` rules,
  NUL-termination, `BD_ERR_OOM` when `cap` is undersized.
- `bd_codesign_hint_block`: presence of the literal
  `errSecInternalComponent` and `keychain may be locked`
  phrases, presence of the `> ── HINT ──`/`> ── END HINT ──`
  rules, NUL-termination, `BD_ERR_OOM` undersize behavior.
- `BD_ERR_UNLOCK_FAILED`: `bd_status_str` returns a non-empty,
  non-`unknown` string; `bd_reason_lex` returns the new
  `LEX_REC_ERR_KC_UNLOCK` key.

**`tests/runstate_test.c`** (black-box additions):

- `RUN_EV_UNLOCK_OK` from `RUN_BUILDING` keeps `RUN_BUILDING`
  and returns `RUN_ACT_NONE`.
- `RUN_EV_UNLOCK_FAIL` from `RUN_BUILDING` transitions to
  `RUN_BUILD_FAILED` and returns `RUN_ACT_DONE`.
- `runstate_reason_lex(RUN_BUILD_FAILED, BD_ERR_UNLOCK_FAILED)`
  returns `LEX_REC_ERR_KC_UNLOCK`.
- All existing transitions are unchanged (the new events do
  not interfere with the existing event/phase matrix).

**`tests/store_test.c`** (black-box additions):

- Serialize/deserialize round-trip for a `Conn` with
  `kc_remember = true` and a non-empty `kc_passkey`: bytes
  survive verbatim across save/load.
- Serialize/deserialize for `kc_remember = false`,
  `kc_passkey = ""`: empty-by-default invariant survives.
- An ssh-agent `Conn` (i.e. `auth == SSH_AUTH_AGENT`) with
  `kc_remember = true` and a non-empty `kc_passkey`: round-
  trips correctly. The keychain passkey is independent of
  SSH auth method.
- Backward-compat: a legacy file written without the
  `kc_remember`/`kc_passkey` columns deserializes with
  `kc_remember = false`, `kc_passkey = ""`. (The exact
  legacy-file fixture is a single hardcoded string written
  to a temp file; the test asserts the load behavior.)
- The `0600` permission assertion on the written file is
  unchanged but re-run with the extended record to confirm
  the format change did not regress the permission policy.

**`tests/session_run_test.c`** (stub-SSH worker test
additions, using the existing `ssh_stub_run.c` machinery):

- `RCMD_SET_KC_PASS` with non-empty passkey followed by
  `RCMD_EXECUTE`: the stub channel for the chain's first
  step receives a command that contains `security
  unlock-keychain` (assertion on the captured exec string).
- `RCMD_SET_KC_PASS` with empty passkey (or no
  `RCMD_SET_KC_PASS` at all) followed by `RCMD_EXECUTE`: the
  stub channel for the chain's first step receives the
  `xcodebuild -showBuildSettings -json` command (the unlock
  step was skipped).
- Stub channel for the unlock step exits with code 0 → the
  chain proceeds to the settings step; the worker emits no
  `REV_PHASE(RUN_BUILD_FAILED)`.
- Stub channel for the unlock step exits with code non-zero
  → the worker emits a Build Log chunk containing the F1
  help block text *before* emitting
  `REV_PHASE(RUN_BUILD_FAILED, BD_ERR_UNLOCK_FAILED)`. The
  chain does not advance to `settings`.
- Stub channel for the actual `build` step exits non-zero
  while `WorkerCtx.kc_pass` is empty and the target is **not**
  a simulator → the worker emits a Build Log chunk
  containing the H2 hint text before emitting
  `REV_PHASE(RUN_BUILD_FAILED, BD_ERR_BUILD)`.
- Same scenario but `target.is_simulator = true` → the H2
  hint is **not** emitted.
- Same scenario but with non-empty `WorkerCtx.kc_pass` → the
  H2 hint is **not** emitted (the user already chose unlock;
  the failure isn't about the keychain being locked).

**No host-gated smoke tool.** A real EXECUTE against a Mac
with a locked keychain — easy to produce by issuing
`security lock-keychain` in a terminal — exercises the full
chain end-to-end. The structural tests cover the contract;
no `tools/keychain_smoke.c` is added.

**No white-box reach-ins.** All assertions are on the public
return values and the worker→UI event chunks observable
through `session_run_poll`.

### Theme & security posture

- **The lazy modal is not a whimsy gate.** It appears only
  when a build is about to run that materially needs the
  passkey (G2 trigger) and there is no cached or persisted
  value. The user has SKIP available for the case where they
  know unlock is unnecessary (C1).
- **Palette discipline.** The modal reuses the BREACH
  overlay's neon-on-dark palette and the masked-input field
  style. Semantic green/red is reserved for the unlock
  outcome chunks in the Build Log (`> KEYCHAIN UNLOCKED` is
  the existing dwell-stamp green; `KEYCHAIN UNLOCK REJECTED`
  inside the F1 block is the existing failure red).
- **Trust.** The opt-in `REMEMBER KEYCHAIN` carries the same
  posture as `REMEMBER PASSKEY`: plaintext, `0600`, accepted
  as a single-user local-tool tradeoff, with OS-keychain
  hardening recorded as deferred. The risk surface is larger
  than the SSH passkey alone (the keychain passkey unlocks
  every credential, certificate, and saved password the
  macOS user has stored), so REMEMBER is **off by default**
  in the modal. The user has to opt in deliberately.
- **No silent retries.** Per F1, an unlock failure is a
  loudly-surfaced `RUN_BUILD_FAILED`. ostrich never tries a
  second passkey it inferred from somewhere else; never
  prompts a second modal during a failed run; never retries
  the build automatically.

### Traceability

Realizes the operational ergonomics gap surfaced by the
`metalmind` build session: a user can compile cleanly on the
remote Mac but cannot get past codesign when the Mac's
`login.keychain` is locked. Traces to `design.md` non-goal
"No signing/provisioning management" by staying narrowly
scoped to unlock (ostrich does not configure or rotate
identities). Traces to `workflow.md` for the persisted-state
shape (per-connection opt-in remembered keychain passkey,
paired with the existing `REMEMBER PASSKEY` pattern). Traces
to `setsid-install-help/ard.md` for the help-block voice,
structural failure-detection discipline, and module-locality
of the rendering code in `libbuilddeploy`. Traces to
`xcode-project-build-and-deploy/ard.md` for the `RunChain`
extension shape (a new substep inside `RUN_BUILDING` exactly
like `settings`).

### ARD / IMPL conformance checklist

- [x] **Arenas named + lifetimes stated** — no new arenas.
      The cross-thread `SessionRunCmd` records live in the
      existing `run_cmd_ring` storage (the flagged
      cross-thread `malloc` from the xcode-build-deploy
      ARD); `WorkerCtx.kc_pass[256]` lives inside the
      existing worker context (worker-thread-confined);
      `app.kc_pass_cache[256]` lives inside the existing app
      state (UI-thread-confined, app-arena lifetime);
      stack-local help-block buffers have function-scoped
      lifetime.
- [x] **Allocation is caller-controlled** —
      `bd_unlock_cmd`, `bd_unlock_help_block`, and
      `bd_codesign_hint_block` all take `char *buf, size_t
      cap`; no hidden allocators. `session_set_kc_pass`
      enqueues a POD record onto the existing ring; no
      allocation.
- [x] **Thread-confinement respected** — the keychain
      passkey crosses the UI→worker boundary by **copy**
      into a fixed-size POD field inside `SessionRunCmd`.
      No shared pointer or arena reference crosses the
      boundary. The app-side cache and the worker-side
      copy are independent memory.
- [x] **Non-arena allocations flagged + justified** — none
      introduced. The ring storage is the previously-flagged
      cross-thread exception; everything new is stack-local
      or sits inside existing struct fields.
- [x] **Module → library decisions made** — no new
      libraries. Changes are additive to `libbuilddeploy`
      (three pure functions + one enum value),
      `librunstate` (two events + one lexicon mapping),
      `libsession` (one new command kind, one new chain
      step, two emission hooks), `libui` (one new modal),
      `libstore` (two new `Conn` fields), and `liblexicon`
      (one new error key + modal label keys). The
      app-layer gating cascade stays in `src/app/` as
      composition-root logic (per coding standards: branchy
      glue is not promotable until it grows real
      complexity).
- [x] **Library layout specified** — unchanged.
      `include/<m>.h` public contract, `src/<m>/` private,
      `build/lib<m>.a` archive, linked into `ostrich` and
      `tests/<m>_test` as today.
- [x] **C/C++ seam identified** — unchanged. The new modal
      lives in `src/ui/*.cpp` (ImGui) behind the existing
      pure-C `ui.h`; everything else added is pure C11.
- [x] **Error handling shape confirmed** — `BdStatus` /
      `RunPhase` enums returned, results via out-params,
      `bd_status_str` and `runstate_reason_lex` companions
      cover the new values. No hidden/global error state.
- [x] **Test approach per library** — black-box additions
      to `builddeploy_test.c`, `runstate_test.c`,
      `store_test.c`, and the stub-SSH `session_run_test.c`
      harness (which already isolates worker chain
      sequencing from real network). No new test binary;
      no new smoke tool; no white-box reach-ins.
