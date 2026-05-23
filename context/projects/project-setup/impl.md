# Implementation Plan - Project Setup

## Summary of Tasks

This section contains one line summary description of the tasks in
the sequence they should be tackled.

1. Makefile build and hello-world C entry point.
2. `make test` smoke-test harness (state-based test binary).
3. Project README.md.

## Task Dependency Relationships

This section outlines the dependencies between tasks.

```
Task 1  -->  Task 2  -->  Task 3
(build)      (test)       (README)
```

Each task depends strictly on the one before it: the test harness
needs something to build (Task 1), and the README documents the
finished `make` / `make test` surface (Tasks 1 and 2).

## Detailed Tasks

This section has all of the tasks in the sequence they should be
tackled in.

### Task 1 - Makefile build and hello-world entry point

- **Status**: done
- **Blocked by**: none
- **User stories covered**: none (foundational scaffolding; no PRD
  exists for this bootstrap)

#### What to build

A plain `Makefile` at the repository root and a minimal C entry
point at `src/main.c`. Running `make` (the default target) compiles
`src/main.c` into the executable `build/ostrich`. The entry point
prints a short greeting to stdout and returns exit code 0. A
`make clean` target removes the `build/` directory. This proves the
C toolchain compiles and links a runnable executable end to end.

#### Technical Details

Per `context/design.md` "Fixed technical constraints": the codebase
is C-first and the build system is plain Make (updated this session
from CMake) with third-party dependencies vendored as git
submodules under `third_party/`. This task wires up only the
hello-world executable; no vendored libraries (Dear ImGui, GLFW,
libssh2) are compiled or linked yet -- that integration is deferred
to a later project.

Conventions:

- Source under `src/`, build artifacts under `build/` (already
  git-ignored).
- Default target builds the executable; provide a `clean` target.
- Compile with warnings enabled (e.g. `-Wall -Wextra`) and a fixed
  C standard (e.g. `-std=c11`).
- Honor `CC` / `CFLAGS` overrides so the toolchain stays portable
  across Linux and macOS (both are supported hosts per
  `design.md`).

#### Acceptance criteria

- [x] `make` from a clean checkout produces `build/ostrich`.
- [x] Running `build/ostrich` prints the greeting and exits 0.
- [x] `make clean` removes the `build/` directory.
- [x] The build is warning-clean with `-Wall -Wextra`.

### Task 2 - make test smoke-test harness

- **Status**: done
- **Blocked by**: Task 1
- **User stories covered**: none (foundational scaffolding)

#### What to build

A `make test` target and a minimal state-based test at
`tests/smoke_test.c`. `make test` compiles the test binary and runs
it; the test asserts the entry point's observable behavior (its
exit code and/or emitted greeting) and returns non-zero on failure
so Make reports the failure. This establishes the test foundation
the RALPH loop expects (`make test` must pass before a task is
considered done).

#### Technical Details

Plain Make is the build system, so CTest (a CMake feature) is not
available; the harness is a Make target plus a small C test binary,
kept state-based so behavior is asserted on observable
output/exit-state rather than on internal calls. Keep the entry
point factored so its behavior is testable (e.g. a small function
the test can drive, or assertions on the process's stdout/exit
code). No external test framework is introduced -- a tiny
assert-based runner is sufficient for the smoke check.

Conventions:

- Tests under `tests/`, test artifacts under `build/`.
- `make test` depends on the build and exits non-zero on any failed
  assertion.

#### Acceptance criteria

- [x] `make test` compiles and runs `tests/smoke_test.c`.
- [x] The test asserts the entry point's exit code and greeting.
- [x] `make test` exits 0 when behavior is correct and non-zero on
      a deliberately broken assertion.
- [x] The test build is warning-clean with `-Wall -Wextra`.

### Task 3 - Project README.md

- **Status**: pending
- **Blocked by**: Task 1, Task 2
- **User stories covered**: none (foundational scaffolding)

#### What to build

A top-level `README.md` that establishes the project for a new
reader: a one-paragraph description of ostrich tracing to
`context/design.md`, prerequisites (a C toolchain and `make`), how
to initialize the vendored submodules (noting their build
integration is deferred), the build/run/test commands (`make`,
`./build/ostrich`, `make test`), and the project layout (`src/`,
`tests/`, `third_party/`, `context/`).

#### Technical Details

The README documents only what exists after Tasks 1 and 2, so it is
the last task. It should point to `context/design.md` and
`context/workflow.md` as the sources of intent rather than
duplicating them, and flag that the GUI (Dear ImGui + GLFW) and SSH
(libssh2) layers are not yet wired into the build.

#### Acceptance criteria

- [ ] README describes ostrich and links to `context/design.md`.
- [ ] README lists prerequisites and the submodule-init steps.
- [ ] README documents `make`, running the binary, and `make test`.
- [ ] README describes the repository layout.
