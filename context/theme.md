# Ostrich — Theme & Voice

This document captures ostrich's **look, feel, and voice** — the
visual identity (a dark, cyberpunk multi-neon skin), the typography,
the motion/FX, and the full-camp **infiltration/heist** copy that
makes the tool *fun* to use.

It sits **beneath `context/design.md`** (it must obey the fixed
constraints there — Dear ImGui + GLFW, C-first, lightweight/fast,
stay-in-workflow) and is a **peer to `context/workflow.md`**:
`workflow.md` owns *what panels exist and how you move through them*;
`theme.md` owns *how they look and what they say*. Downstream
PRDs/ARDs/IMPLs under `context/projects/` trace to this doc for
look-and-feel the way they trace to `workflow.md` for flow.

It is **deliberately whimsical**. The decisions here may shape *what
is generated* (colors, copy, micro-animations) but must never steer
critical architectural or flow decisions. Where this doc and an
upstream doc conflict, see **Authority & governance** below.

---

## Governing principles

Two rules sit above every specific choice in this document. When a
detail below conflicts with one of these, the principle wins.

### 1. Whimsy is zero-cost decoration

Whimsy **never adds latency and never gates the workflow**. It only
decorates states that are *already true*: a real successful auth
renders `ACCESS GRANTED`; a real failure renders `ACCESS DENIED`.

- **No fake delays.** No artificial `decrypting…` / `breaching…`
  pauses, no progress theatre, nothing you must wait through.
- **No gates.** No splash you must click through, no success-beat you
  wait on before the app becomes usable. The result is usable the
  instant it is real.
- **Drama rides real durations.** A build genuinely takes seconds, so
  `COMPILING EXPLOIT…` dwells naturally; install takes a beat, so
  `DEPLOYING PAYLOAD…` dwells. Genuinely-instant events (auth) may
  flash for a frame and then resolve to a dwelling stamp — that is
  fine, and is *not* padded to be readable.

### 2. Fun is strictly subordinate to function

The theme exists **purely to be fun** and must **never hinder the
build → run → observe loop**. This is the trump card over the FX kit
and everything else: if any effect, animation, or piece of copy ever
distracts from or impedes the core loop or log readability in
practice, **it is cut, no debate.**

---

## Authority & governance

`theme.md` is **sovereign over wording and look**, and **has no say
over structure**.

**It MAY override upstream COPY** (and each override is recorded here
as deliberate):

- Action labels: `Play → EXECUTE`, `Build → COMPILE`, `Stop → ABORT`
  (overrides `workflow.md`/`design.md` wording).
- Run-state labels: `running → TARGET ACQUIRED // LIVE`, etc.
  (overrides the literal state names in `workflow.md`).
- Connection-overlay wording (`KNOWN HOSTS`, `BREACH`, …).
- Recon wording: scan → `⌖ SCAN HOST`, project results →
  `BLUEPRINTS RECOVERED`, sweep → `↻ SWEEP FOR TARGETS`, target
  results → `TARGETS IN RANGE` (themes `workflow.md`'s neutral
  discovery flow).
- Build/deploy wording: deploy failure →
  `DEPLOYMENT FAILED // PAYLOAD REJECTED`, simulator boot →
  `PRIMING TARGET…`, running-app-behind-build →
  `PAYLOAD STALE // NEW EXPLOIT READY`, and the Device Log run
  separator `> ── NEW PAYLOAD ──` (themes the build/deploy chain
  and its logs).
- All colors, typography, FX, and ostrich's voice.

**It MAY NOT touch STRUCTURE:**

- Which panels exist, the layout, or docking — owned by `workflow.md`.
- The product flow or the run-state machine's *transitions* — owned by
  `workflow.md`. (Theme renames the states; it does not add, remove, or
  re-route transitions.)
- Anything architectural — memory/arenas, module boundaries,
  threading — owned by `design.md` and `context/coding_standards.md`.

**Conflict order:** `design.md` > `workflow.md` > `theme.md`. If a
theme choice would force a structural or architectural change, the
**theme yields** and the conflict is surfaced upstream to be resolved
there — it is never resolved by bending architecture to the vibe.

---

## Visual identity

### Flavor

A **dark, cyberpunk multi-neon** skin. The organizing idea is
**brightness = attention**: the resting UI is *dark/dim neon*, and
*bright neon* is reserved as an accent spike for whatever matters right
now (focus, hover, the active phase, a granted/denied flash). Your eye
is always pulled to the single bright thing.

### Palette

Two neon families do different jobs, and they never trade roles.

- **Decorative neons (cyan + magenta)** — carry *no meaning*. Used for
  chrome: borders, panel headers, focus rings, selection, identity,
  brand pops.
- **Semantic colors (green / red / amber)** — carry *only* meaning,
  never decoration. `green` always = ok/granted, `red` always =
  fail/denied, `amber` always = in-progress/busy. Because they are
  never used decoratively, the granted/denied signal stays
  unmistakable inside the colorful skin.
- **Log body text is calm off-white** — never neon-tinted. Readability
  of dense, hours-long build/device logs is a core function and wins
  over vibe (Principle 2).

| Role | Token | Hex | Use |
| --- | --- | --- | --- |
| Base | `bg` | `#07090d` | near-black, cool window background |
| Surface | `panel` | `#0d1117` | raised panels |
| Dim cyan | `cyan-dim` | `#0e5a63` | resting borders / headers |
| Dim magenta | `magenta-dim` | `#6e1457` | resting secondary chrome |
| Bright cyan | `cyan` | `#00f0ff` | **accent spike**: focus / active |
| Bright magenta | `magenta` | `#ff2bd6` | **accent spike**: selection / brand pop / ostrich's voice |
| Green (semantic) | `ok` | `#19ff7a` | GRANTED / success — meaning only |
| Red (semantic) | `fail` | `#ff3b50` | DENIED / failure — meaning only |
| Amber (semantic) | `busy` | `#ffb000` | in-progress / warning — meaning only |
| Log body | `text` | `#c8d0d8` | off-white log + body text |

> Exact hex values are a starting point an implementation may tune for
> contrast; the **roles and the brightness/semantic discipline are the
> fixed part.**

### Typography

- **Monospace everywhere** — chrome and logs alike — for a unified
  terminal/console feel. **JetBrains Mono** (OFL, vendorable), baked
  into the ImGui font atlas.
- Two TTFs only: **regular + bold** (ImGui has no automatic
  bold/italic; each weight is a separate atlas font). Ligatures are
  irrelevant — ImGui does no ligature shaping.
- **Emphasis comes from ALL-CAPS + brightness + color**, not from
  extra typefaces. Big moments (`ACCESS GRANTED`/`DENIED`, the
  wordmark) reuse the same mono at large size with wide letter-spacing.

### Motion & FX

A bounded, curated kit — adopted under Principle 2 (the principle
trumps any item here). Local GPU effects are cheap and do **not**
violate "lightweight" (which is a contrast to heavy *remote-desktop
streaming*, not local rendering) nor zero-cost (which is about
workflow gating, not GPU).

**Allowed:**

- **Flash** on `ACCESS GRANTED` / `ACCESS DENIED`, decaying to the
  dwelling stamp.
- **Slow pulse** on the `* ONLINE` dot and the `LIVE` indicator.
- **Static glow / bloom** on bright-neon accents (the look, not an
  animation).
- **Faint, STATIC scanline + vignette** overlay behind chrome only,
  tuned so logs stay crisp.
- **Blinking block cursor** in input fields and at the log tail.

**Banned:**

- Any motion **on top of log text**.
- Screen flicker, strong chromatic aberration, matrix-rain.
- Anything that, in practice, distracts from the core loop → cut it.

---

## Voice & lexicon

### Tone

**Full Hollywood camp**, held together by a single coherent fiction so
it reads as a themed tool rather than scattered action-movie noise.

### The fiction (infiltration / heist spine)

You are the **OPERATOR**. Everything maps 1:1 onto the real domain:

| Fiction | Real thing |
| --- | --- |
| OPERATOR | you, the developer |
| HOST | the remote Mac |
| BREACH / uplink | the SSH connection |
| recon (SCAN / SWEEP) | querying the HOST for build inputs |
| BLUEPRINT | a buildable Xcode project / workspace |
| EXPLOIT | the app build artifact being compiled |
| PAYLOAD | the app being installed |
| TARGET | the iOS device / simulator |
| go LIVE / acquire | the app launched and running |

ostrich addresses you as **OPERATOR** in flavor text (e.g.
`ACCESS GRANTED. WELCOME, OPERATOR.`). Note the distinction from the
literal **USER** field in the connection form, which is the real SSH
username you type.

### Delivery model

- **Stamps dwell; transitions ride dwelling labels.** Persistent/
  terminal beats are stamps that stay until the state changes
  (`ACCESS GRANTED`, `TARGET ACQUIRED // LIVE`). Transition narration
  is the **run-state label** in the Control/Status strip, which dwells
  for each phase's *real* duration — no log injection, no fake delay.
- **Tool panels stay 100% raw.** The Build Log shows only raw
  `xcodebuild` output; the Device Log shows only the device's own
  output. ostrich's camp voice lives in its **own** surfaces (status
  strip, connection overlay/bar, the Device Log *header*), never in the
  tool output, and **never recolors real tool output.**
- **ostrich's voice has a fixed signature** wherever it appears: a
  `>` prefix in `magenta` (the brand pop), instantly distinct from raw
  output.
- **Auth is asymmetric.** On success the overlay dismisses *instantly*
  (no gate) and `ACCESS GRANTED` lands as a dwelling stamp in the
  connection bar. On failure `ACCESS DENIED` dwells in the overlay,
  where you are still sitting to retry.

### Canonical lexicon

This table is the **single source of truth** for camp copy. The
implementation keeps all of it in **one centralized strings table** (a
single lexicon source, not scattered string literals) so a future
"straight mode" is a trivial swap with no UI work.

**Connection / auth** (the live-link lifecycle)

| Real event/state | Copy |
| --- | --- |
| connecting (handshake) | `BREACHING PERIMETER…` |
| auth success | `ACCESS GRANTED` (then `…WELCOME, OPERATOR.` in narration) |
| connected | `* ONLINE` |
| reconnecting (with backoff) | `REACQUIRING SIGNAL…` |
| disconnected | `LINK SEVERED` |

**Connection failures** — each beat dwells in the overlay, where
the operator is sitting to retry, in the `>` voice and the semantic
`fail` red. `ACCESS DENIED` is the auth-specific case; the rest name
distinct causes so the fix is obvious (a sealed port is not a wrong
passkey).

| Real failure | Copy |
| --- | --- |
| auth rejected | `ACCESS DENIED` |
| host unreachable / no route / DNS | `HOST UNREACHABLE // NO ROUTE` |
| connection refused (port closed / sshd off) | `PERIMETER SEALED // PORT CLOSED` |
| connect timed out | `NO RESPONSE // TIMEOUT` |
| host-key mismatch (possible interception) | `HOST KEY MISMATCH // POSSIBLE INTERCEPTION` |
| authenticated, no usable shell / channel | `NO FOOTHOLD // SHELL DENIED` |

**Run-state label** (Control/Status strip)

| Real state | Copy |
| --- | --- |
| idle | `STANDBY` |
| building | `COMPILING EXPLOIT…` |
| simulator boot (if needed) | `PRIMING TARGET…` |
| installing | `DEPLOYING PAYLOAD…` |
| launching | `EXECUTING PAYLOAD…` |
| running | `TARGET ACQUIRED // LIVE` |
| failed (build error) | `EXPLOIT FAILED` (→ see Build Log) |
| failed (install / launch) | `DEPLOYMENT FAILED // PAYLOAD REJECTED` (→ see Build Log) |
| aborted (Stop / mid-run drop) | `OPERATION ABORTED` |

**Actions** (buttons)

| Real action | Label | Notes |
| --- | --- | --- |
| Connect (the breach) | `BREACH` | toggles to `■ ABORT` while connecting |
| Play (full chain) | `▶ EXECUTE` | toggles to `■ ABORT` while running |
| Build (build-only) | `COMPILE` | |
| Stop | `■ ABORT` | consistent with `OPERATION ABORTED` |

**Device Log header (while streaming):** `LIVE FEED // INTERCEPTING`

**Device Log run separator (each new launch):**
`> ── NEW PAYLOAD // <time> ──` — preserves history while marking
where a fresh instance's output begins (the Device Log is not
cleared on re-Play; it is demarcated).

**Stale-build indicator** (the running app is behind the latest
build, e.g. after a build-only COMPILE): `PAYLOAD STALE // NEW
EXPLOIT READY`, in the `>` voice, cleared by the next EXECUTE.

**Discovery / recon** (querying the HOST for build inputs). Recon
copy lives only on ostrich's own surfaces in the `>`/magenta voice
and never recolors or fabricates real Xcode output. A real recon
failure reads in the `>` voice **and** the semantic `fail` red so it
is unmistakable; an empty-but-successful result is a calm empty
state (see Conventions), not an error.

| Real action / state | Copy |
| --- | --- |
| scan a root for buildable projects | `⌖ SCAN HOST` (toggles to `■ ABORT SCAN` while scanning) |
| project results header | `BLUEPRINTS RECOVERED` |
| sweep for devices + simulators | `↻ SWEEP FOR TARGETS` |
| target results header | `TARGETS IN RANGE` |
| preset complete + a target locked | `READY // ARMED` |
| Xcode command-line tools absent | `XCODE NOT FOUND` |
| command failed / output unparseable | `COULD NOT READ INVENTORY` |

### Connection overlay (launch screen)

A **static ASCII wordmark/banner** (paints instantly — no boot
animation, per Principle 1) framing a still-legible form. Field
*values* are real SSH inputs, so field labels stay literal-ish.

```
+------------------------------------------+
|        .-.   OSTRICH  // UPLINK          |
|       (o.o)   infiltration console       |
|        |=|                               |
|  HOST [____________]  PORT [22]          |
|  USER [____________]                     |
|  AUTH ( ) SSH-AGENT  ( ) PASSKEY [____]  |
|       [ ] REMEMBER PASSKEY               |
|  -- KNOWN HOSTS --                       |
|   o studio-mac     o mac-mini            |
|              [ BREACH ]                   |
+------------------------------------------+
   submit -> BREACHING PERIMETER... -> ACCESS GRANTED
```

- Saved connections → **`KNOWN HOSTS`** (an SSH `known_hosts` pun).
  Note this is ostrich's *saved-connections* list, distinct from the
  real SSH host-key store (the system `~/.ssh/known_hosts`).
- Connect button → **`BREACH`**; while a breach is in flight it
  becomes **`■ ABORT`** to cancel the attempt (no fake delay — it
  dwells only for the real handshake duration).
- Auth methods → **`SSH-AGENT`** / **`PASSKEY`**, with
  **`REMEMBER PASSKEY`**.
- Field labels (`HOST` / `PORT` / `USER`) stay literal for usability.
- A changed host key raises a **blocking**
  **`HOST KEY MISMATCH // POSSIBLE INTERCEPTION`** alert in semantic
  `fail` red *before* any passkey is sent. This is a real **security
  stop**, not a whimsy gate (Principle 1 forbids only *decorative*
  gating): it blocks because proceeding could leak a credential to an
  impostor, exactly as real `ssh` refuses a changed host key.

---

## Conventions & minor defaults

These are the small, tweakable fills-in. They follow the rules above;
adjust freely as long as the principles hold.

**Glyphs / punctuation**

| Glyph | Meaning |
| --- | --- |
| `>` | prefix for ostrich's own voice (magenta) |
| `*` | online / live status dot (pulses slowly) |
| `//` | section / status separator |
| `■` | abort glyph (and the Stop button) |
| `▶` | execute / play glyph |
| `⌖` | scan / recon reticle (SCAN HOST) |
| `↻` | sweep / re-query (SWEEP FOR TARGETS) |
| `o` | known-host bullet |

**Footer:** `ostrich // 60 FPS // ONLINE` (ostrich's own diagnostics —
distinct from build/device output).

**Empty states** (themed placeholders, in `magenta` ostrich voice):

- Build Log, nothing built yet: `// NO PAYLOAD COMPILED`
- Device Log, nothing launched: `// NO SIGNAL — TARGET DARK`
- No saved connections: `// NO KNOWN HOSTS`
- No run-config preset yet: `// NO OPERATION CONFIGURED`
- No projects found by a scan: `// NO BLUEPRINTS`
- No targets available from a sweep: `// NO TARGETS IN RANGE`
- A target not yet selected: `// NO TARGET LOCKED`

**Configurability:** the theme is **fixed** — one identity, no runtime
toggle or theme picker (that would cut against "lightweight"). The
only structural requirement is the centralized strings table above,
which keeps a future straight-mode a no-UI swap.

---

## Out of scope

- **Audio.** No beeps, chimes, or sound FX. A sound layer is a *new
  subsystem*, which would cross from theme into architecture
  (forbidden by **Authority & governance**). If ever wanted, it is a
  `design.md`/ARD decision, not a theme one.
- **Image-asset icons.** Iconography is ASCII/Unicode glyphs only,
  consistent with the all-mono aesthetic; no bitmap/SVG icon assets in
  the atlas.
- **Structural change of any kind** — see **Authority & governance**.

---

## Conformance note

Any downstream PRD/ARD/IMPL that produces user-facing surfaces should:

- [ ] Use the **canonical lexicon** for all camp copy (and source it
      from the single strings table).
- [ ] Apply the **palette discipline** — decorative vs semantic neons
      kept separate; logs off-white.
- [ ] Honor **zero-cost** (no fake delays/gates) and **fun is
      subordinate to function** (cut anything that hinders the loop).
- [ ] Keep **tool panels raw**; render ostrich's voice only in its own
      surfaces with the `>`/magenta signature.
- [ ] Record any **upstream copy override** as deliberate, and make
      **no structural/architectural change** (raise such conflicts
      upstream instead).
