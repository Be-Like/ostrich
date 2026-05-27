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

Dear ImGui, GLFW, libssh2, and jsmn are vendored as submodules — Dear ImGui,
GLFW, and libssh2 are built from source; jsmn is used header-only — so you do
**not** need system packages for them. On Linux, GLFW is built with
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

### Remote Mac (SSH target)

The Mac that ostrich drives over SSH needs `setsid` available on the
non-interactive SSH PATH — the build/launch commands wrap `xcodebuild` in
`setsid sh -c '...'` so the worker can `kill -- -<pgid>` the whole process
tree on abort. macOS does **not** ship `setsid`, so install it from
util-linux and put it on the non-interactive PATH:

```sh
brew install util-linux
# util-linux is keg-only on macOS — expose setsid to non-interactive SSH:
echo 'export PATH="'"$(brew --prefix util-linux)"'/bin:$PATH"' >> ~/.zshenv
```

`~/.zshenv` (not `~/.zshrc`) is what zsh sources for non-interactive,
non-login sessions like `ssh host 'cmd'`. Confirm it took with
`ssh <user>@<host> 'command -v setsid'`. If this isn't set up you'll see
`Export failed` (the EXPLOIT FAILED header) in the Build Log along with
`command not found: setsid` when you click Execute.

Device builds also require the remote Mac's `login.keychain` to be unlocked —
`codesign` can't access the signing identity's private key through a locked
keychain. ostrich handles this with a lazy modal: the first time you press
EXECUTE against a device target in a session where the keychain is locked, a
**KEYCHAIN PASSKEY** prompt appears. Enter the passkey and press `ENTER` to
unlock the keychain for the rest of the session. Tick `REMEMBER KEYCHAIN` to
save the passkey on the connection record (plaintext, `0600`) so future
sessions skip the prompt automatically. Press `SKIP` to proceed without
unlocking — the build will fail at the codesign step, but the Build Log will
show a hint reminding you the modal is available on the next EXECUTE.

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

## Disclaimers

- **`setsid` dependency on the remote Mac.** `bd_build_cmd` and
  `bd_launch_cmd` (see `src/builddeploy/builddeploy.c`) wrap the remote
  command in `setsid sh -c '...'` so the worker can target the whole
  process group on abort. macOS doesn't ship `setsid`, so the remote Mac
  needs it installed from Homebrew's `util-linux` keg and put on the
  non-interactive SSH PATH. The Build Log surfaces the exact remediation
  on the first failed EXECUTE; the same steps are reproduced here for
  reference.

  Connect to the Mac (substitute your own user and host; add `-p <port>`
  if you SSH on a non-default port):

  ```sh
  ssh <user>@<host>
  ```

  Then on the Mac, run:

  ```sh
  brew install util-linux
  ```

  Then add ONE of the following to `~/.zshenv`, matching your Mac:

  ```sh
  # Apple Silicon:
  echo 'export PATH="/opt/homebrew/opt/util-linux/bin:$PATH"' >> ~/.zshenv
  # Intel:
  echo 'export PATH="/usr/local/opt/util-linux/bin:$PATH"' >> ~/.zshenv
  ```

  (Substitute your own path if `util-linux` is installed elsewhere.)

  Verify the fix from your local host:

  ```sh
  ssh <user>@<host> 'command -v setsid'
  ```

  See `context/projects/setsid-install-help/` for the in-app guidance
  design.

- **Remote Mac `login.keychain` must be unlocked for device builds.**
  Compiling and building to a physical device requires the remote Mac's
  `login.keychain` to be unlocked at sign time — `codesign` pulls the
  signing identity's private key from `login.keychain`, and on a headless
  Mac or after an auto-lock the keychain is locked, causing a `codesign`
  failure (`errSecInternalComponent`) at the very end of an otherwise-clean
  build.

  Ostrich surfaces an in-app **KEYCHAIN PASSKEY** modal on the first
  affected EXECUTE and asks for the keychain password to unlock it. The
  password is **not written to any drive** unless you explicitly tick
  `REMEMBER KEYCHAIN`, in which case it is saved on the connection record
  (plaintext, `0600`) so future sessions skip the prompt. By default the
  password lives only in memory for the life of the running Ostrich
  process and is destroyed when the application closes. Ostrich does not
  modify the Mac's keychain auto-lock policy; see
  `context/projects/keychain-unlock/` for the in-app design.

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

## Known Issues

Ostrich is currently in beta. It connects to a remote host Mac, finds Xcode
projects and devices (both physical and simulator) and provides an interface
for project compilation and build/deployment.

There are some known quirks with some of the buttons. Specifically with regard
to some of their boolean state. 

The logs are functional, and I identified several opportunities where I could
provide more explicit messaging to the user to help them resolve common issues.
There may be future opportunity to provide more clear guidance to the user, but
that will require some more trial and error and research in terms of what may
be common error cases.

### Known Bugs

* Device list contains physical devices that have been used previously but may not be
accessible at this moment.

    - This is actually how Xcode operates as well. While I don't think Xcode is
      one to emulate I suspect that they are approaching the problem similarly
      and it is just a reality of the CLI. It would require further research to
      determine how I might be able to make this feel like a better user
      experience.

* There is an issue with the content of the project dropdown where two options
  can contain the same naming. The result of this is that there are two options
  within the dropdown that share an ID where that ID is meant to be unique.

### Poor UX

* Inconsistent keyboard shortcuts.

* When building and deploying to a physical device, a user must have a valid
  code signing which requires that they provide a password to unlock their
  keychain. This is properly handled, but if the user exits out they will see
  an error as produced by the deploy logs that feels somewhat vague. There is
  an opportunity to capture that error and produce an error that is more user
  friendly; it also provides an opportunity to communicate with the user the
  reason we ask for the password to the keychain and how we handle that
  securely.

