# Ostrich — Coding Standards

These are the **design-level coding standards** for ostrich. They
exist to shape *architectural* decisions — how memory is owned, how
modules are sealed, how interfaces fail — so that ARDs and
implementation plans produce code that is encapsulated, testable,
and consistent.

This document is **loaded into context when authoring an ARD
(`write-a-ard`) or an implementation plan (`write-a-impl`)**. Every
ARD and IMPL is expected to conform to it; the checklist at the end
makes that conformance explicit.

Scope and altitude:

- This is about **design**, not line-level syntax. Pure formatting
  (bracing, spacing, column limit, include sorting) is owned by
  `.clang-format` and is **not** restated here.
- These standards sit beneath `context/design.md` ("Fixed technical
  constraints": C-first, plain Make, vendored submodules) and refine
  *how* code is written within those constraints. Where the two
  appear to conflict, `design.md` wins and the discrepancy should be
  surfaced.

## Memory management — arenas

ostrich-owned memory is managed with **arenas** (linear/bump
allocators reset as a group), not ad-hoc `malloc`/`free`.

Principles:

- **Group allocations by lifetime.** Allocations that live and die
  together share an arena. Free by **resetting the whole arena**, not
  by freeing objects one at a time. There is no per-object `free`
  within an arena's life.
- **The caller controls allocation.** Any function that allocates
  takes an explicit `Arena*` parameter; the caller owns the lifetime.
  A library **never** holds a hidden static/global arena or allocates
  behind the caller's back. Lifetimes are visible in the signature:

  ```c
  SshStatus ssh_connect(Arena *a, SshConfig cfg, Ssh **out);
  Schemes  *parse_schemes(Arena *a, Str json);
  ```

- **Arenas are thread-confined.** An arena is owned by exactly one
  thread; there is no locking and no shared concurrent access.
  Crossing a thread boundary (e.g. a worker handing log output to the
  UI thread) requires an **explicit copy/handoff into the consumer's
  own memory** — never a shared pointer into another thread's arena.
  The concrete handoff mechanism (queue, ring buffer, etc.) is an
  ARD-level decision. This keeps the non-blocking worker model free of
  allocator contention.
- **No fixed arena taxonomy.** Each ARD **names its own arenas and
  their lifetimes** (e.g. a per-run arena reset on each Play, a
  per-frame arena, a thread-local scratch arena). The standard fixes
  the principles; the specific arenas are designed per feature.

Escape hatch — non-arena allocation:

- Arenas are the **default** for ostrich-owned memory. `malloc`/`free`
  is permitted **only with a stated reason**, such as a genuinely
  dynamic/unbounded lifetime that does not fit any arena, or handing
  ownership to a C library that will free it.
- **Library-owned memory follows that library's model.** libssh2 and
  Dear ImGui allocate internally on their own terms; that is out of
  scope for these arena rules.
- Any non-arena allocation **must be called out in the ARD** with its
  justification.

## Modules as compiled libraries

The point of compiling a module into its own library is
**architectural**: the library boundary is *physically* enforced. The
single public header is the entire contract; the implementation
translation units are sealed and unreachable from outside. This forces
a disciplined interface and yields isolated testing and reuse for
free.

When to promote a module to a library:

- Promote when sealing a boundary **and** there is **real
  implementation to hide** — a deep module with a narrow, stable
  public interface over substantial internals (e.g. the SSH session
  layer, the config store, the run orchestrator, Xcode-output
  parsing).
- **Skip thin/shallow wrappers.** Code with little to encapsulate
  stays plain source; do not pay library ceremony for it.

Form:

- **Static archives (`.a`).** Each library compiles to
  `build/lib<module>.a` and links into both the app and its test
  binary, producing one self-contained executable. No shared objects,
  runtime load paths, or versioning.

Layout (public/private split):

```
include/<module>.h     <- the public contract (the ONLY thing
                          other modules may #include)
src/<module>/*.c        <- private implementation
src/<module>/*.h        <- private internal headers
build/lib<module>.a     <- the compiled archive
```

- **Everything in `include/` is public API by construction.**
  Everything under `src/<module>/` is private.
- Other modules may `#include` **only** a module's public header,
  never its private headers or `.c` files.

App / composition root (exempt from the library rule):

- The entry point and the glue that wires libraries and the UI
  together live as plain code: **`src/main.c`** plus **`src/app/*.c`**.
- This layer's job is **orchestration**, not hiding complexity, so it
  is **not** a library. It `#include`s public headers from `include/`
  and links the `.a` archives.
- If real logic accumulates in the app layer, **push it down into a
  library**.

C / C++ seam:

- ostrich is **C-first (C11)**. C++ exists **only** inside a library
  that must touch a C++ dependency (Dear ImGui).
- Such a library has **`.cpp` internals** under `src/<module>/`, but
  its **public header in `include/` is pure C** (`extern "C"`, no C++
  types). Example: a UI library with `src/ui/*.cpp` (ImGui) behind a
  C `include/ui.h`.
- **Everything else is pure C11** and sees only C headers — including
  `src/app/`, which consumes the UI library through its C header
  rather than touching ImGui directly.

## Interfaces (public headers)

Every public header is **C-includable** and follows two conventions:

- **Allocation:** allocating functions take a caller-supplied
  `Arena *` (see arenas above). No hidden allocators.
- **Error handling:** every fallible function reports failure through
  its **return value** — a status enum (`0`/`OK`, or a negative/enum
  error). Results come back via **out-parameters**. A companion call
  turns a status into a **human-readable reason** for the UI. There is
  **no** hidden `errno`-style or global error state, and **no**
  failure via exceptions (headers are C).

  ```c
  typedef enum { SSH_OK = 0, SSH_ERR_AUTH, SSH_ERR_IO } SshStatus;

  SshStatus   ssh_connect(Arena *a, SshConfig cfg, Ssh **out);
  const char *ssh_status_str(SshStatus st);
  /* caller: if (st != SSH_OK) show ssh_status_str(st); */
  ```

## Testing

Tests are **state-based** (assert observable behavior/state, not
internal calls), consistent with the existing `tests/` harness, and
`make test` must pass before any task is considered done.

- Each library has a test binary `tests/<module>_test.c` that links
  `build/lib<module>.a`.
- **Black-box by default:** a test `#include`s **only** the module's
  public header and exercises the public contract. If something cannot
  be tested through the public API, that is a **design signal — fix
  the interface**, do not peek.
- **White-box only with justification:** reaching into private
  internals (private headers / internal `.c`) is allowed only with a
  **stated reason in the ARD/IMPL** (e.g. a complex private algorithm
  worth unit-testing directly).

## Build integration

Plain Make, per `design.md`. A library is built and consumed roughly
as below (illustrative, not prescriptive); honor `CC`/`CFLAGS`
overrides so the build stays portable across Linux and macOS:

```make
build/libssh.a: $(wildcard src/ssh/*.c) | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -c $^ -o ...   # compile TUs
	ar rcs $@ ...                            # archive

build/ostrich: src/main.c $(APP_SRC) build/libssh.a | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -o $@ $^

build/ssh_test: tests/ssh_test.c build/libssh.a | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -o $@ $^
```

## ARD / IMPL conformance checklist

Every ARD and implementation plan should be able to check each box (or
state why it does not apply):

- [ ] **Arenas named + lifetimes stated** — each arena introduced is
      named with an explicit lifetime; freeing is by reset.
- [ ] **Allocation is caller-controlled** — allocating functions take
      an `Arena *`; no hidden/global allocators.
- [ ] **Thread-confinement respected** — cross-thread data uses an
      explicit copy/handoff, not shared arena pointers.
- [ ] **Non-arena allocations flagged + justified** — any
      `malloc`/`free` (or library-owned memory) is called out.
- [ ] **Module → library decisions made** — which modules become
      `.a` libraries (boundary + real complexity), and which are
      intentionally left as plain source.
- [ ] **Library layout specified** — `include/<m>.h` public contract,
      `src/<m>/` private, `build/lib<m>.a` archive.
- [ ] **C/C++ seam identified** — if C++ (ImGui) is touched, it is
      sealed inside a library behind a pure-C header; the rest stays
      C11.
- [ ] **Error handling shape confirmed** — status enum return +
      out-params + a status→string call; no hidden error state.
- [ ] **Test approach per library** — a `tests/<m>_test.c` linking the
      archive, black-box via the public header (white-box only if
      justified).
