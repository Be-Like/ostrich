# ostrich

A lightweight, fast, native desktop tool — running on **Linux and macOS** — that lets a single developer drive the entire iOS **build → install → launch → observe** loop on a remote Mac without ever opening the macOS GUI or Xcode.

See [`context/design.md`](context/design.md) for the full statement of intent and [`context/workflow.md`](context/workflow.md) for the UI layout and interaction model.

## Prerequisites

- A C toolchain (`cc`, defaulting to your system compiler)
- GNU Make

## Submodules

Third-party dependencies (Dear ImGui, GLFW, libssh2) are vendored under `third_party/` as git submodules. Initialize them after cloning:

```sh
git submodule update --init --recursive
```

> **Note:** the GUI (Dear ImGui + GLFW) and SSH (libssh2) layers are not yet wired into the build. Submodule initialization is only needed once those layers are integrated.

## Build

```sh
make
```

Produces `build/ostrich`.

## Run

```sh
./build/ostrich
```

## Test

```sh
make test
```

Compiles and runs the smoke-test suite under `tests/`. Exits non-zero on any failure.

## Clean

```sh
make clean
```

Removes the `build/` directory.

## Repository layout

```
src/           C source files
tests/         Test binaries
third_party/   Vendored git submodules (imgui, glfw, libssh2)
context/       Design documents and per-project implementation plans
build/         Build artifacts (git-ignored)
```
