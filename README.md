# ostrich

A lightweight, fast, native desktop tool — running on **Linux and macOS** —
that lets a single developer drive the entire iOS **build → install → launch →
observe** loop on a remote Mac without ever opening the macOS GUI or Xcode.

## What it is

ostrich treats a Mac as a headless build appliance. You stay in your own editor
on Linux (or macOS), connect to the Mac over SSH, and drive Xcode from a native
GUI built with Dear ImGui — no screen sharing, no Xcode window, no leaving your
environment.

The tool is organized around the goals laid out in [`context/design.md`](context/design.md):

- **Connect** to a Mac over SSH with a clearly visible, persistent session
  state.
- **Discover** the available Xcode inputs — projects, schemes, build
  configurations, and target devices/simulators — by querying the Mac.
- **Configure** a run: pick a project, scheme, device, and bundle ID, with
  manual fallbacks for every field.
- **Play** — orchestrate `xcodebuild → install → launch` from a single action.
- **Observe** — stream build logs and device logs live, with a non-blocking
  worker model that keeps the UI responsive while multiple streams run
  concurrently.

It is a single-user personal tool. It is **not** an editor, does not handle
signing/provisioning, and is not built for teams. See
[`context/design.md`](context/design.md) for the full statement of intent,
[`context/workflow.md`](context/workflow.md) for the UI layout and interaction
model, and [`context/theme.md`](context/theme.md) for the visual identity.

## Prerequisites

- A C/C++ toolchain (`cc` and `c++`, defaulting to your system compiler; C11
  and C++17 are required)
- GNU Make
- `pkg-config`
- OpenSSL development headers (libssh2 is built against the OpenSSL backend)

Dear ImGui, GLFW, and libssh2 are vendored as submodules and built from source,
so you do **not** need system packages for them. On Linux, GLFW is built with
**both the X11 and Wayland backends** and selects one at runtime (based on
`XDG_SESSION_TYPE` / `WAYLAND_DISPLAY` / `DISPLAY`), so the build needs the
development headers for both display stacks:

- **Linux:** OpenSSL and OpenGL (Mesa) dev packages, plus:
  - **X11 backend** — `libX11`, `libXcursor`, `libXi`, `libXinerama`,
    `libXrandr`, `libXfixes` dev packages.
  - **Wayland backend** — `wayland` (provides the `wayland-client` headers and
    the `wayland-scanner` codegen tool) and `libxkbcommon` dev packages. The
    Wayland protocol XML is vendored inside GLFW, so `wayland-protocols` is
    **not** required.

  GLFW `dlopen()`s the X11/Wayland/GL client libraries at runtime rather than
  linking them, so they only need to be installed on the machine that *runs*
  ostrich. If you only have one display stack on a given machine, narrow the
  build with `make GLFW_BACKENDS=x11` or `make GLFW_BACKENDS=wayland`.
- **macOS:** the Cocoa/IOKit/OpenGL frameworks ship with the system. Install
  OpenSSL and pkg-config via Homebrew (`brew install openssl pkg-config`); the
  build falls back to the Homebrew prefix automatically if pkg-config can't
  find OpenSSL.

## Getting the source

The third-party dependencies live under `third_party/` as git submodules. Clone
with `--recurse-submodules` so they come down in one step:

```sh
git clone --recurse-submodules git@github.com:Be-Like/ostrich.git
cd ostrich
```

If you already cloned the repository **without** the submodules (or pulled
changes that touch them), initialize/update them in place:

```sh
git submodule update --init --recursive
```

Run that same command any time `git pull` reports changes under `third_party/`
to bring the vendored dependencies to the pinned revisions.

## Build

```sh
make
```

Produces `build/ostrich` along with the static libraries for each module
(`libui.a`, `libglfw.a`, `libssh.a`, `libsession.a`, `libdiscovery.a`, and so
on).

## Run

```sh
./build/ostrich
```

## Test

ostrich has two distinct kinds of checks: **unit tests** and **smoke tests**.
They serve different purposes and are run differently.

### Unit tests

```sh
make test
```

This is the suite you run routinely. It compiles and runs every test binary
under [`tests/`](tests/) in sequence and exits non-zero on the first failure.
These tests are fast, self-contained, and require **no network and no remote
Mac** — they exercise the pure, state-based cores of each module:

| Test | Covers |
| --- | --- |
| `arena_test` | Arena allocator: create, aligned alloc, reset, destroy |
| `spsc_ring_test` | Lock-free single-producer/single-consumer ring buffer |
| `lexicon_test` | UI string lookup table |
| `framestats_test` | Frame timing / FPS (EMA filter) |
| `connstate_test` | Connection state machine: phases, backoff, keepalive, liveness probes |
| `store_test` | Saved-connections persistence (XDG config dir) |
| `app_test` | Form ↔ SSH-config conversion, phase reason strings, connection list |
| `discovery_test` | Xcode discovery: JSON parsing, project/scheme/config/device enumeration |
| `ui_test` | ImGui window/layout rendering |

### Smoke tests

The smoke tests live under [`tools/`](tools/) and are **not** part of `make
test`. They are manual, developer-driven integration checks — most of them open
a real SSH connection to a live Mac and need connection arguments. Build (and,
where noted, run) each one individually:

| Target | Invocation | Needs a live Mac? |
| --- | --- | --- |
| `make ssh_version_smoke` | builds **and runs** it; prints the linked libssh2 version | No — offline sanity check that the SSH layer links |
| `make ssh_smoke` | builds `build/ssh_smoke`; run as `./build/ssh_smoke <host> <user> [port]` | Yes — exercises the full handshake/auth/probe |
| `make session_smoke` | builds `build/session_smoke`; run as `./build/session_smoke <host> <user> [port]` | Yes — exercises the worker thread + rings against a live session |
| `make discovery_smoke` | builds `build/discovery_smoke`; run as `./build/discovery_smoke <host> <user> <scan_root> [port] [depth] [--abort]` | Yes — exercises Xcode discovery against a real Mac |

In short: `make test` is the automated, offline regression suite; the smoke
tests are for hand-verifying real SSH and discovery behavior against an actual
Mac.

## Clean

```sh
make clean
```

Removes the `build/` directory.

## Repository layout

```
src/           C source — app shell, plus per-module dirs (ssh/, session/,
               connstate/, store/, discovery/, ui/) and shared cores
               (arena.c, spsc_ring.c, lexicon.c, framestats.c, main.c)
include/       Public headers for the modules in src/
tests/         Self-contained unit tests (run by `make test`)
tools/         Developer smoke tests (built/run individually)
third_party/   Vendored git submodules (imgui [docking], glfw, libssh2, jsmn)
context/       Design docs and per-project PRD/ARD/impl plans
assets/        Bundled assets (fonts)
scripts/       Helper scripts
build/         Build artifacts (git-ignored)
```
