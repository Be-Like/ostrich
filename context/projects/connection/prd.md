# PRD — Connection (Connect to the Mac over SSH)

## Problem Statement

ostrich today opens as a themed shell — a single window with the
resting ASCII wordmark and a diagnostics footer — but it cannot
reach a Mac. libssh2 is vendored yet unlinked; there is no
connection overlay, no authentication, no session. The `ONLINE`
in the footer means only "ostrich is running," not a live link.
So the application that exists to drive a remote Mac's
build → install → launch → observe loop currently connects to
nothing.

As the operator I live in Linux/Neovim and want the Mac to be a
headless appliance. Every downstream capability — discovering
schemes and devices, configuring a run, pressing Play, streaming
build and device logs — assumes a live, authenticated SSH session
to the Mac already exists. Until ostrich can connect, none of it
can be built or used. I am blocked at step one.

I also need connecting to be *trustworthy, fast, and forgiving*. I
should not retype host and user every day. The window must never
freeze while it reaches across the network, and an unreachable
host must not strand me. A Mac that sleeps or a Wi-Fi blip should
not end my session — it should heal. And I should be warned, in no
uncertain terms, if the machine answering on that address is not
the Mac I trust.

## Solution

ostrich gains its first real link to a Mac. On launch I am met
with the modal **BREACH** connection overlay: the static ASCII
wordmark over a still-legible form — **HOST**, **PORT** (default
22), **USER**, an **AUTH** choice of **SSH-AGENT** or **PASSKEY**
(password) with an opt-in **REMEMBER PASSKEY**, and a list of my
saved **KNOWN HOSTS**. My most-recently-used connection is
pre-selected, so the daily path is *launch → Enter*. ostrich never
auto-connects; it always waits for me to commit with **BREACH**.

When I commit, libssh2 — now linked into the build — performs the
real work off the UI thread: DNS, TCP, the SSH handshake, host-key
verification, and authentication via my running ssh-agent or the
password I typed. The window stays smooth throughout and shows
`BREACHING PERIMETER…`, dwelling for the handshake's *real*
duration with no fake delay; I can **ABORT** an attempt that hangs.
Host-key trust uses my existing OpenSSH world: ostrich checks the
Mac's key against `~/.ssh/known_hosts`, recognizes a host I have
already trusted from a terminal, appends a new host's fingerprint
on first trust, and **blocks with a loud warning if a known host's
key has changed** (possible interception).

On success, ostrich proves the session is actually usable by
running one trivial command over an exec channel; only then does it
declare `ACCESS GRANTED` — landing as a dwelling stamp while the
overlay dismisses instantly (no gate) — and reveal the working
area with a thin connection bar showing `user@host  * ONLINE`. On
failure I get a distinct, plain-language reason in ostrich's `>`
voice, right in the overlay where I sit to retry: unreachable,
refused / wrong port, timed out, `ACCESS DENIED` (auth), a
host-key mismatch, or "no usable shell" — each pointing at a
different fix, with my entered details preserved.

Once connected, the link is *kept*. Keepalives detect a dropped
Mac promptly; a drop shows `REACQUIRING SIGNAL…` and auto-retries
with backoff (off-thread, so the UI stays smooth), preserving my
local state, and resolves to `* ONLINE` on recovery or `LINK
SEVERED` when it cannot. From the connection bar I can **update**
the connection (re-opens the overlay pre-filled; an identity change
tears down and reconnects, a label/remember-only edit just
persists) or **close** it; selecting a different KNOWN HOST while
connected closes the current session first, honoring one Mac at a
time. My KNOWN HOSTS persist across launches, ssh-agent
connections store no secret, and an opted-in password is saved in
local config (plaintext, restrictive user-only permissions).

The session is deliberately built **multi-channel-ready**: the one
probe channel is the first of many, so later build-log and
device-log streams open concurrently over this same session
without a re-architecture or a second connection. This project
stops cleanly at "connected and kept": it builds no discovery, run
configuration, Play, or log panels.

## User Stories

1. As the operator, I want a connection overlay to appear on
   launch, so that connecting to a Mac is the first thing ostrich
   offers.

2. As the operator, I want the overlay to be modal over the
   resting shell, so that I focus on establishing the link before
   anything else.

3. As the operator, I want the overlay to paint instantly with the
   static ASCII wordmark and no boot animation, so that it is
   usable the moment the window appears (zero-cost whimsy).

4. As the operator, I want HOST, PORT (default 22), and USER
   fields, so that I can target a specific Mac and account.

5. As the operator, I want to choose an auth method — SSH-AGENT or
   PASSKEY (password) — so that I can use whichever my Mac accepts.

6. As the operator using ssh-agent, I want ostrich to authenticate
   through my running agent without entering any secret, so that
   key-based Macs need no password.

7. As the operator using password auth, I want a PASSKEY field, so
   that I can authenticate to a Mac that requires a password.

8. As the operator, I want an opt-in REMEMBER PASSKEY checkbox,
   off by default, so that storing my password is always my
   explicit choice.

9. As the operator, I want a KNOWN HOSTS list of my saved
   connections, so that I can load one with a single click instead
   of retyping.

10. As the operator, I want my most-recently-used connection
    pre-selected on launch, so that the everyday path is
    launch → Enter.

11. As the operator, I want ostrich to never auto-connect on
    launch and always wait for BREACH/Enter, so that it never
    silently reaches for a Mac that may be off.

12. As the operator, I want to save the current details
    (label, host, port, user, auth method) as a KNOWN HOST, so
    that the connection is there next time.

13. As the operator, I want a themed empty state
    (`// NO KNOWN HOSTS`) when nothing is saved, so that the first
    run reads intentionally rather than blank.

14. As the operator, I want to commit with BREACH (or Enter), so
    that there is one obvious action that starts the connection.

15. As the operator, I want the window to stay smooth and
    responsive during connect, never freezing, so that ostrich
    feels lightweight even while it reaches across the network.

16. As the operator, I want `BREACHING PERIMETER…` shown while the
    real handshake runs — dwelling for its actual duration with no
    artificial pause — so that I can see it is working.

17. As the operator, I want to ABORT an in-progress or hung
    connect, so that an unreachable host never strands me with no
    way out.

18. As the operator, I want ostrich to verify the Mac's SSH host
    key against my `~/.ssh/known_hosts`, so that I am connecting to
    the machine I trust, not an impostor.

19. As the operator connecting to a Mac I have already trusted
    from a terminal, I want ostrich to recognize it without
    re-prompting, so that ostrich shares my existing OpenSSH trust.

20. As the operator connecting to a brand-new host, I want its
    fingerprint recorded (appended to `~/.ssh/known_hosts`) on
    first trust, so that future connects are verified.

21. As the operator, I want a loud, blocking warning if a known
    host's key has changed, so that I am protected against a
    machine impersonating my Mac — especially on the password path.

22. As the operator, I want `ACCESS GRANTED` to land as a dwelling
    stamp and the overlay to dismiss instantly on success, so that
    I am working immediately with no success-gate to click through.

23. As the operator, I want "connected" to mean ostrich actually
    ran a trivial command over an exec channel, so that `* ONLINE`
    guarantees the session can do real work, not merely that auth
    passed.

24. As the operator with a locked-down or shell-less account, I
    want connect to fail clearly at connect time ("no usable
    shell") rather than mysteriously later at Play, so that I see
    the cause where I can fix it.

25. As the operator, I want distinct failure reasons —
    unreachable, refused / wrong port, timed out, ACCESS DENIED
    (auth), host-key mismatch, no usable shell — so that I know
    exactly what to change.

26. As the operator, I want failure messages in ostrich's `>`
    magenta voice in the overlay, where I am already sitting to
    retry, so that they are unmistakably ostrich and right where I
    act on them.

27. As the operator, after a failed connect I want my entered
    details preserved, so that I can correct one field and retry
    without retyping everything.

28. As the operator, once connected I want the overlay to collapse
    into a thin connection bar showing `user@host` and `* ONLINE`,
    so that the working area is revealed and status stays visible.

29. As the operator, I want the `* ONLINE` dot to pulse slowly, so
    that a live link reads at a glance.

30. As the operator, I want ostrich to detect a dropped link
    promptly via keepalives, so that a slept Mac or dropped Wi-Fi
    does not look like a hang.

31. As the operator, when the link drops I want
    `REACQUIRING SIGNAL…` and an automatic retry with backoff, so
    that a transient blip heals itself without my intervention.

32. As the operator, I want my local state (selected connection,
    layout) preserved across a drop and reconnect, so that I pick
    up where I left off.

33. As the operator, when reconnection ultimately fails I want a
    clear `LINK SEVERED` state, so that I know the link is gone and
    can decide what to do.

34. As the operator, I want reconnect attempts to also run off the
    UI thread, so that the window stays smooth while ostrich
    retries.

35. As the operator, I want an update control in the connection
    bar that re-opens the overlay pre-filled, so that I can change
    connection details.

36. As the operator, when I change connection identity (host,
    port, user, or auth), I want ostrich to tear down the live
    session and connect anew, so that the change actually takes
    effect.

37. As the operator, when I change only non-identity details
    (label, remember-password), I want ostrich to persist them
    without a needless reconnect, so that small edits are cheap.

38. As the operator, I want a close control to disconnect and
    return to the resting overlay, so that I can end a session
    deliberately.

39. As the operator, I want selecting a different KNOWN HOST while
    connected to close the current session first, so that ostrich
    honors one Mac at a time.

40. As the operator, I want my KNOWN HOSTS to persist across
    launches, so that my Macs are remembered.

41. As the operator who opted into REMEMBER PASSKEY, I want my
    password restored for that connection, so that connecting is a
    single Enter.

42. As the operator, I want ssh-agent connections to store no
    secret, so that key-based connections keep nothing sensitive on
    disk.

43. As the operator, I want any stored-password file created with
    restrictive, user-only permissions, so that the plaintext
    convenience does not expose it to other local users.

44. As the developer, I want the SSH session built so that
    multiple concurrent exec channels can be opened over it later,
    so that build-log and device-log streaming drop on without a
    re-architecture or a second connection.

45. As the developer, I want libssh2 finally vendored-in and
    linked into the Make build, so that real SSH is part of the
    ostrich binary.

46. As the developer, I want ostrich to build and connect on both
    Linux and macOS hosts, so that both supported platforms work.

47. As the developer, I want `make test` to stay meaningful and
    green for the connection layer, so that the project's automated
    gate stays trustworthy as SSH lands.

48. As the operator, I want all connection copy (BREACH, KNOWN
    HOSTS, BREACHING PERIMETER…, ACCESS GRANTED/DENIED, REACQUIRING
    SIGNAL…, LINK SEVERED, and the new failure / host-key / abort
    lines) sourced from the centralized lexicon, so that a future
    straight-mode stays a no-UI swap.

49. As the operator, I want connection palette discipline kept —
    decorative cyan/magenta for chrome, semantic green/red/amber
    only for granted/denied/busy meaning — so that the
    granted/denied signal stays unmistakable inside the neon skin.

50. As the operator, I want the overlay and connection bar to be
    ostrich's own voice surfaces (with the `>` signature) that
    never recolor or fabricate real SSH output, so that ostrich's
    narration stays distinct from the truth of the link.

51. As the operator, I want the overlay fully keyboard-drivable
    (tab between fields, Enter to BREACH, a sane Esc), so that I
    stay in the keyboard, consistent with my Neovim workflow.

52. As the operator, I want connection state always visible and
    recoverable — I can see at any moment whether I am online,
    reconnecting, or severed — so that the session's state, per the
    design's "visible, recoverable session state," is never a
    mystery.

## Out of Scope

Deferred to later projects or fixed as non-goals:

- **Discovery / auto-scan.** Querying the Mac for schemes, build
  configurations, devices, and simulators is a later project; it
  would *populate* a run-config form that does not exist yet. There
  are no fields to discover into here.

- **Run configuration and presets.** The run-config form and its
  six fields are a later project. (Note: per-connection run-config
  presets — and their persistence — belong with that project, even
  though *connection* persistence is fixed here.)

- **Play / Build orchestration and the run-state machine.** The
  build → install → launch chain, its abort, and the themed
  run-state labels (`COMPILING EXPLOIT…`, etc.) are later work.

- **Build Log and Device Log streaming.** No log panels are built.
  This project only makes the session *multi-channel-ready* and
  opens the single probe channel; the concurrent worker model is
  exercised in full by the log-streaming project.

- **The persistence mechanism.** *That* connections and an opt-in
  password persist is required here; the concrete on-disk format,
  file location, and store implementation are ARD decisions, not
  fixed by this PRD.

- **OS keychain / libsecret password storage.** The hardened
  replacement for opt-in plaintext stays the deferred hardening
  path; this project does plaintext-with-restrictive-perms.

- **Importing `~/.ssh/config`.** Saved connections are ostrich's
  own richer store (label + auth method + remember flag).
  Auto-importing hosts from `~/.ssh/config` is a possible future
  note, not MVP. (This is distinct from host-key trust, which
  *does* use the system `~/.ssh/known_hosts`.)

- **SSH port-forwarding and the debugger.** Future goals per
  `design.md`; the session layer should not preclude them, but
  neither is built.

- **NAT traversal / tunnelling / relay.** Non-goal; the Mac is
  assumed reachable on a LAN or a VPN the operator already runs.

- **Multi-Mac / multi-window / multiple simultaneous sessions.**
  Non-goal; one Mac, one active connection at a time.

- **Reattaching to an in-flight remote run across a reconnect.**
  Moot here (no runs exist) and deferred regardless.

- **Advanced SSH options** — ProxyJump / jump hosts, an
  identity-file picker, per-host key-algorithm selection,
  compression, custom ciphers — are not in MVP.

- **The threading / arena / module mechanism.** The PRD requires
  the *behaviors* (no freeze, cancelable, multi-channel-ready,
  cross-thread safety); the worker model, arenas, and library
  boundaries are designed in the ARD.

## Further Notes

- **Traceability.** This project realizes `design.md` core goal #1
  (connect over SSH via libssh2, authenticating by ssh-agent or by
  explicit user/host/port/password, with "visible, recoverable
  session state"). It traces to `workflow.md` for the overlay →
  connection-bar structure, happy paths 1 (cold start), 2
  (returning user), and 6 (recover from a drop), the persistence
  model, and the one-Mac/one-config mental model. It traces to
  `theme.md` for the BREACH overlay, KNOWN HOSTS, the asymmetric
  auth beats (`ACCESS GRANTED` dismisses instantly; `ACCESS DENIED`
  dwells where you retry), `REACQUIRING SIGNAL…` / `LINK SEVERED`,
  the palette discipline, and the `>` voice signature.

- **Decisions resolved while scoping this PRD.** (1) Saved
  connections, MRU pre-select, and opt-in remember-password are
  in-scope requirements; the storage mechanism is an ARD decision.
  (2) Host-key verification is trust-on-first-use with a loud,
  blocking mismatch warning. (3) The full connection lifecycle —
  keepalive detection, auto-reconnect with backoff, and state
  preservation — is in scope. (4) Connect and every reconnect run
  off the UI thread and are cancelable. (5) "Connected" requires
  auth *plus* a successful trivial command over one exec channel;
  Xcode/environment checks are deferred to Discovery. (6) Failures
  surface as distinct, actionable reasons in the overlay. (7) The
  session is built multi-channel-ready. (8) The remembered password
  is plaintext, opt-in, user-only file permissions, keychain
  deferred. (9) Host-key trust uses the system `~/.ssh/known_hosts`
  (read + append). (10) Update re-opens the overlay and reconnects
  on an identity change; switching hosts closes the current session
  first.

- **New camp copy (now in `theme.md`).** This project needed
  lexicon beats the theme did not yet name; they have been added to
  `theme.md`'s canonical lexicon as deliberate entries: the
  host-key mismatch alert `HOST KEY MISMATCH // POSSIBLE
  INTERCEPTION`, a **Connection failures** table naming each
  distinct cause (`HOST UNREACHABLE // NO ROUTE`, `PERIMETER
  SEALED // PORT CLOSED`, `NO RESPONSE // TIMEOUT`, `NO FOOTHOLD //
  SHELL DENIED`, alongside the existing `ACCESS DENIED`), and the
  `BREACH` action toggling to `■ ABORT` while connecting. The
  host-key mismatch is recorded there as a real **security stop**,
  not a whimsy gate. No structural change is implied; the
  centralized strings table simply gains these keys.

- **Two different "known hosts."** The theme's **KNOWN HOSTS** is
  ostrich's list of **saved connections** (its own store). The
  **SSH host-key store** is the system `~/.ssh/known_hosts`
  (server fingerprints). Both exist in this project and must not be
  conflated.

- **Architectural seams for the ARD (flagged, not designed).** An
  off-UI-thread connect/reconnect worker with an **explicit
  cross-thread handoff into the UI's own memory** (per
  `coding_standards.md` thread-confinement) — this is ostrich's
  first worker thread and sets the pattern for log streaming. The
  SSH session belongs in its own **deep library behind a pure-C
  header**; because libssh2 exposes a C API there is **no C/C++
  seam** here (unlike `ui`). A **config / known-hosts store**
  module is implied by persistence. The session must be structured
  so **multiple concurrent exec channels** can be opened later.
  Arenas (e.g. a per-session arena whose lifetime is the live
  session) are named in the ARD.

- **Keeping `make test` meaningful.** A real SSH connect cannot be
  unit-tested without a live server, so the ARD should keep the
  gate trustworthy by black-box testing the display-free,
  network-free parts — connection config validation, the
  connection state-machine transitions, the failure-code → reason
  mapping, saved-connections serialize/deserialize, and the backoff
  schedule — and gate any real-socket test behind host
  availability (printing a SKIP when absent), mirroring the
  app-shell `SKIP: no display` pattern. Keeping the gate green is a
  requirement (user story 47); the mechanism is the ARD's.

- **Whimsy stays zero-cost.** `BREACHING PERIMETER…` and
  `REACQUIRING SIGNAL…` dwell only for *real* network durations;
  there are no artificial pauses, and success dismisses the overlay
  instantly. The only connection motion is the slow `* ONLINE`
  pulse and the input cursor blink — both cut if they ever hinder
  the loop.

- **Security posture.** TOFU plus the shared system
  `~/.ssh/known_hosts` is genuine protection against a host
  impersonating the Mac, which matters most on the password path.
  The opt-in plaintext stored password is an accepted single-user,
  local-only tradeoff, mitigated by restrictive file permissions
  and with OS-keychain storage recorded as the deferred hardening.
