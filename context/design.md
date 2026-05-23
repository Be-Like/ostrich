# Ostrich — Design Goals

This document captures the **goals and general functionality** of
ostrich. It is the top-level statement of intent that product
requirement docs (PRDs), architecture review docs (ARDs), and
implementation plans should all trace back to. It deliberately does
**not** specify product flow/UX, work breakdown, or task scoping —
those live in per-project documents under `context/projects/`.

## One-line definition

A lightweight, fast, native desktop tool — running on **Linux and
macOS** — that lets a single developer drive the entire iOS
**build → install → launch → observe** loop on a remote Mac without
ever opening the macOS GUI or Xcode.

## Why it exists

- **Stay in my own workflow.** Keep iOS iteration inside the
  developer's Linux/Neovim environment. The Mac becomes a headless
  build/run appliance rather than something you sit in front of.
- **Lightweight and fast.** A focused native console for exactly the
  build/run/observe loop — not a laggy, heavy VNC/Screen Sharing
  session.
- **Editing is already solved.** Source editing over SSH + Neovim is
  acceptable today. The pain ostrich removes is the build/run/observe
  loop, which currently forces the developer into the macOS GUI and
  Xcode.

## Core functional goals

1. **Connect** to a Mac over SSH (libssh2), authenticating either via
   **ssh-agent** or with an explicit **user / host / port / password**,
   with visible, recoverable session state.
2. **Discover** Xcode build inputs by querying the Mac — schemes,
   build configurations, destinations, connected physical devices,
   and simulators — and present them as selectable inputs.
3. **Configure a run** through an interface that collects the
   parameters needed to build, install, and launch (e.g.
   project/workspace, scheme, device UDID, bundle ID).
4. **Play** — a single action orchestrates the full chain on the Mac:
   `xcodebuild …` (build) → `xcrun devicectl device install app …`
   (install) → launch the app on the chosen target.
5. **Stream build logs** live while the build runs.
6. **Stream device logs** live, in real time, while interacting with
   the running app. Physical devices are the primary target;
   simulators are also supported.
7. **Concurrency is a core goal.** ostrich shows multiple live
   streams at once — device logs stay live continuously, including
   across rebuilds, while build output streams in parallel. This is
   achieved with concurrent SSH channels and a non-blocking worker
   model, designed in from the start rather than bolted on later.

## Platform and audience

- The host application runs on **both Linux and macOS**.
- **Single user.** ostrich is a personal tool to assist one
  developer's process. It is **local-only** — never a team,
  collaborative, multi-user, or hosted product.

## Future goals

Stated ambitions, intentionally out of the initial scope, in rough
priority order:

1. **Debugger.** Use the Xcode/lldb debugger against the device from
   the host. Requires SSH port-forwarding.
2. **SSH port-forwarding.** General capability that enables the
   debugger, web inspectors, on-device debug servers, etc. The SSH
   layer should be built so this can be added without re-architecting.
3. **Local development with sync.** Develop locally on Linux and sync
   files/build to the Mac, removing the remote-editing step entirely.
   Lowest priority, since Neovim-over-SSH is acceptable today.

## Non-goals

- **Not a code editor / IDE.** ostrich never edits source; Neovim
  (local or over SSH) remains the editor.
- **No signing / provisioning management.** The project must already
  build and sign correctly via `xcodebuild` on the Mac; ostrich only
  invokes it.
- **No NAT traversal / tunnelling product.** The Mac is assumed
  reachable — same LAN, or a VPN the developer already runs (e.g.
  Tailscale/WireGuard). SSH plus optional port-forwarding is the
  ceiling; ostrich will not build relay or hole-punching
  infrastructure.
- **Not for teams.** No multi-user, collaboration, or hosting.

## Fixed technical constraints

These are already decided and constrain all downstream design:

- **C-first.** The codebase is C in general; C++ is used only where a
  library boundary requires it.
- **GUI:** Dear ImGui + GLFW (OpenGL 3, ImGui docking branch).
- **SSH:** libssh2, linked in-process.
- **Build system:** Make (plain Makefiles), with third-party
  dependencies vendored as git submodules under `third_party/`.

## Deferred (resolved in later sessions)

The following are explicitly **not** settled by this document and are
to be worked out in dedicated sessions / per-project docs:

- Detailed product flow and UX.
- Work scoping and task breakdown.
- Device-log scoping (full system firehose vs. filtered to the app's
  process), build-error parsing, and configuration persistence.
- Coupling risk and handling of Xcode CLI output formats across
  Xcode versions (discovery relies on the JSON-emitting subcommands
  such as `xcodebuild -list -json`, `xcrun simctl list -json`, and
  `xcrun devicectl … --json-output`).
