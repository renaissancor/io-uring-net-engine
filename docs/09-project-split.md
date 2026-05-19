# 09 — Library / Product split

This document defines the two-project architecture that the portfolio
ships as: `iouring-net-lib` (this repo, the **library**) and
`iouring-net-server` (a sibling repo, the **product**, to be created).
It specifies the boundary, the seam that connects them, what belongs on
each side, and the dependency direction. Together with
[`00-overview.md`](00-overview.md) it is the architectural source of
truth.

The split mirrors the Windows reference repos:

| Windows reference                          | Linux equivalent       | Role     |
|--------------------------------------------|------------------------|----------|
| `WindowsLibrary/Library/`                  | `iouring-net-lib/`     | library  |
| `WindowsLibrary/MainApp/`                  | `iouring-net-server/`  | product  |
| `IOCP_Rookiss/Engine/`                     | `iouring-net-lib/`     | library  |
| `IOCP_Rookiss/Server/` + `Client/`         | `iouring-net-server/`  | product  |

---

## Why two projects, not one

1. **API hygiene is enforced by the build system, not by discipline.**
   When library and product live in the same tree, a product file can
   reach into `src/` internals through a relative include. With a
   project-level split, only what the library installs under
   `<prefix>/include/iouring_net/` is reachable from the product. The
   public surface becomes the actual surface.

2. **Two CI / sanitizer policies, no conditional logic.** The library
   wants TSan-clean, ASan-clean, property tests, microbenchmarks,
   `-Werror`. The product wants integration tests, latency profiling,
   traffic generation, end-to-end packet replay. Different lanes,
   different cadence, different definitions of "green".

3. **Portfolio narrative reads cleaner.** A reviewer sees a Linux
   systems library and a server built on it. One repo doing both blurs
   which work is the reusable contribution.

4. **The `find_package` round trip is the strongest possible test of
   the install/export config.** `examples/hello/` is a smoke; a real
   sibling project that builds from a clean `cmake --install` is the
   integration test.

---

## Repo topology

```
~/CLionProjects/
├── iouring-net-lib/                 # this repo — library only
│   ├── src/                         # implementation + private headers
│   ├── examples/hello/              # MINIMAL find_package smoke
│   ├── tests/                       # unit + property + stress
│   ├── docs/                        # cross-cutting design
│   ├── wiki/                        # per-source-file specs
│   └── (installs)
│       ├── <prefix>/lib/libiouring_net.a
│       ├── <prefix>/include/iouring_net/...
│       └── <prefix>/lib/cmake/iouring_net/iouring_netConfig.cmake
│
└── iouring-net-server/              # sibling repo — product (planned)
    ├── proto/                       # packets.json schema
    ├── codegen/                     # rpc_gen.py / stub_gen.py / proxy_gen.py
    ├── generated/                   # build-time outputs (gitignored)
    ├── server/                      # server main, handlers
    ├── client/                      # echo/test client
    ├── tests/                       # integration + E2E
    └── CMakeLists.txt               # find_package(iouring_net REQUIRED)
```

The product **never** lives under `iouring-net-lib/server/` or similar.
Two repositories, two `git` histories, two `CMakeLists.txt` trees.

---

## What lives where — boundary criteria

The rule of thumb: **if a component is generic enough that a different
product could reuse it, it belongs in the library; if it is specific to
one product's protocol, business logic, or operational shape, it
belongs in the product.**

### Library side (`iouring-net-lib`)

Layers 1–3 from `00-overview.md` — Primitive, Runtime, Network.

| Component                                      | Layer      | Notes                                         |
|------------------------------------------------|------------|-----------------------------------------------|
| `memory_pool`, `object_pool`, `stl_allocator`  | Primitive  | Generic allocators                            |
| `ring_buffer`, `serial_buffer`, `malloc_vector`, `cstr_hash_map`, `indexed_heap` | Primitive | Generic containers (under `sds::`) |
| `lnx::mutex` / `shared_mutex` / `atomic*`      | Primitive  | POSIX-wrapped sync                            |
| `lock_free_stack`                              | Primitive  | Treiber stack — generic                       |
| `profiler::scope` / `manager`                  | Primitive  | Diagnostic, generic                           |
| `deadlock_profiler::manager`                   | Primitive  | Diagnostic, generic                           |
| `leak_tracker::manager`                        | Primitive  | Diagnostic, generic                           |
| `log::logger`                                  | Primitive  | Generic async logger                          |
| `reactor` (io_uring)                           | Runtime    | The io_uring abstraction itself               |
| `job_queue`, `thread_context`                  | Runtime    | Generic scheduling primitive                  |
| `service`, `listener`, `session`               | Network    | Generic TCP server building blocks            |
| `packet_framing` (size/id header parser)       | Network    | The `[uint16 size][uint16 id]` codec — generic. **This is the entire Network-layer surface the library v1 ships for packets.** No dispatcher base class, no codec template, no unhandled-id policy. See [`../wiki/network/packet_handler.md`](../wiki/network/packet_handler.md) for why the dispatcher is product-side. |

### Product side (`iouring-net-server`)

Layer 4 (Application) from `00-overview.md`, plus everything specific
to one wire protocol.

| Component                                      | Notes                                              |
|------------------------------------------------|----------------------------------------------------|
| `proto/packets.json`                           | Wire schema for this product's RPCs                |
| `codegen/rpc_gen.py` / `stub_gen.py` / `proxy_gen.py` | Pre-build code generation; mirrors `SelectServer/TestSerialize/` pipeline |
| `generated/*.h`, `generated/*.cpp`             | Build artifacts; gitignored                        |
| `packet_dispatcher` + concrete handlers        | The product owns the dispatcher (`server/dispatch.{h,cpp}`) and the per-packet `handle_X` free-function handlers (`server/handlers/*.cpp`). See [`../../iouring-net-server/wiki/server/dispatch.md`](../../iouring-net-server/wiki/server/dispatch.md) and [`handlers.md`](../../iouring-net-server/wiki/server/handlers.md). |
| `server/main.cpp`                              | Server entry point                                 |
| `client/main.cpp`                              | Echo/test client                                   |
| Integration & E2E tests                        | Two-process tests, traffic replay, fuzz harnesses  |
| Latency profiling rig                          | Product-level perf measurement                     |

### Gray zone — `job_queue` and `thread_context`

These are arguable. `NextProject.md` describes them in product terms
("per-entity job queue" for game entities), but the mechanism is a
generic FIFO that serializes work without per-op locking. **Decision:
library-side.** The library exposes a generic `job_queue<T>` and a
`thread_context` TLS slot; the product instantiates them for its
entities and ties them to its handlers.

If a future product needs a different scheduling model, the library's
`job_queue` is one option, not a mandate — the product wires it up.

### Hard "no" list — never library-side

- `packets.json` and any generated stubs
- Any `#include` of a generated header
- Any business logic, room/player/session-game-state types
- Any `main()` for a server or client
- Any code that knows the product's `packet_id` numeric range

### Hard "no" list — never product-side

- Direct `liburing` calls (go through `reactor`)
- Direct POSIX socket calls (go through `session` / `listener`)
- Reimplementing primitives the library already provides
- `#include` paths into `iouring-net-lib/src/` — the install prefix is
  the only legal source of library headers

---

## The seam — how the two projects connect

### One-way dependency

```
iouring-net-server  →  iouring-net-lib  →  liburing, fmt, tl::expected
       (product)         (library)             (third-party)
```

The library does not know the product exists. It builds, tests, and
installs without `iouring-net-server` on disk. The product depends on
the library through one CMake call: `find_package(iouring_net)`.

### The contract surface

Three concrete artifacts cross the boundary:

1. **Installed headers** at `<prefix>/include/iouring_net/`.
   Configured via `FILE_SET HEADERS` (CMake ≥ 3.23; our floor is 3.25):
   ```cmake
   # iouring-net-lib/CMakeLists.txt
   target_sources(iouring_net
     PUBLIC
       FILE_SET iouring_net_public_headers
       TYPE HEADERS
       BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
       FILES
         src/types.h
         src/error/expected.h
         src/diagnostic/profiler_scope.h
         src/sds/cstr_hash_map.h
         src/sds/malloc_vector.h
         src/sds/ring_buffer.h
         src/sync/atomic.h)

   install(TARGETS iouring_net
     EXPORT iouring_netTargets
     ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
     FILE_SET iouring_net_public_headers
       DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iouring_net)
   ```
   **The contract is mechanical and explicit**: every public header is
   enumerated in the FILE_SET `FILES` list. Anything in `src/` that
   does NOT appear in that list is private and is not installed,
   regardless of filename or directory. Adding a new public header is
   a one-line FILE_SET edit; removing or restricting a header is a
   one-line delete. Reviewing a PR for boundary impact is a single
   grep of the `target_sources(... FILE_SET ...)` block.

2. **The static archive** `libiouring_net.a` at `<prefix>/lib/`.

3. **The CMake package config**
   `<prefix>/lib/cmake/iouring_net/iouring_netConfig.cmake` plus
   `iouring_netTargets.cmake` and `iouring_netConfigVersion.cmake`.
   Already wired up in `iouring-net-lib/CMakeLists.txt` and
   `cmake/iouring_netConfig.cmake.in`.

Everything else — `src/_placeholder.cpp`, `tests/`, `wiki/`, build
scripts — is private to the library repo.

### Consumer-side CMake (product)

The minimal `iouring-net-server/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(iouring_net_server
  VERSION   0.0.1
  LANGUAGES CXX
  DESCRIPTION "Reference product on top of iouring_net")

find_package(iouring_net 0.0.1 REQUIRED)

add_executable(server server/main.cpp)
target_link_libraries(server PRIVATE iouring_net::iouring_net)
target_compile_features(server PRIVATE cxx_std_20)
```

Pointing CMake at a local install:

```bash
# Build + install the library to a local prefix
cmake -S ~/CLionProjects/iouring-net-lib -B build-lib \
      -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build-lib
cmake --install build-lib

# Build the product against it
cmake -S ~/CLionProjects/iouring-net-server -B build-srv \
      -DCMAKE_PREFIX_PATH=$HOME/.local
cmake --build build-srv
```

`fmt::fmt`, `tl::expected`, `liburing::uring`, `Threads::Threads` are
PUBLIC dependencies of `iouring_net` and propagate to the product
through the imported `iouring_net::iouring_net` target — the product
does not `find_package` them itself.

---

## Versioning and ABI

- **No ABI guarantee in v1.** The product rebuilds when the library
  bumps. `write_basic_package_version_file(... COMPATIBILITY
  SameMajorVersion)` is already configured, so the product's
  `find_package(iouring_net X.Y REQUIRED)` will accept any `X.*`
  release.
- **Pin by git tag, not version range.** The product's CI clones a
  specific library tag, installs it, then builds. Avoids surprise
  breaks on library `main`.
- **Static linkage only in v1.** No `.so`, no `LD_LIBRARY_PATH`
  surprises. The product binary statically contains the library.

---

## When to split — sequencing

The **physical repo split** (code, CMake, tests) is a plan, not a
"do this today" item. The **design docs** for the sibling repo have
been written ahead of the code, following the same doc-first
methodology that bootstrapped this library — they live at
`~/CLionProjects/iouring-net-server/` as of 2026-05-14. See that
directory's [`README.md`](../../iouring-net-server/README.md) and
`docs/` tree; in particular
[`iouring-net-server/docs/00-overview.md`](../../iouring-net-server/docs/00-overview.md)
and [`01-architecture.md`](../../iouring-net-server/docs/01-architecture.md)
specify the product-side architecture, and
[`docs/05-codegen.md`](../../iouring-net-server/docs/05-codegen.md)
specifies the codegen pipeline.

Code lands per this sequence:

| Milestone                                    | Action                                                |
|----------------------------------------------|-------------------------------------------------------|
| Primitive layer complete (current)           | Stay single-repo for code. Library is not consumable yet. Server-side design docs are written. |
| Runtime layer (reactor + task) lands         | Still single-repo for code. Add a richer `examples/echo/` here. |
| Network layer (session + framing) lands      | **`git init` `iouring-net-server/`** as a sibling repo. Move the in-repo `examples/echo/` into it; keep `examples/hello/` here as the minimal smoke. Begin implementing per `iouring-net-server/wiki/`. |
| First end-to-end packet round-trip green     | Both repos exist independently; library has no `server/` directory under it.   |

Until the network layer lands, the seam is theoretical and
`examples/hello/` is the only buildable consumer. That is fine — the
seam being defined now (in this doc) and the product architecture
being specified now (in the sibling's `docs/`+`wiki/`) matters more
than the second repo being a live build target.

---

## Anti-patterns

- **Do not** add a `server/`, `app/`, or `product/` directory to this
  repo "for now". Once it exists, it absorbs ad-hoc code that should
  be product-side, and the boundary blurs.
- **Do not** let the product `#include` from `iouring-net-lib/src/` via
  a relative path. If a product needs something not in the installed
  headers, the fix is to expose it from the library, not to reach in.
- **Do not** let the library `#include` any product header, by any
  path. The dependency arrow is one-way.
- **Do not** vendor `iouring-net-lib` as a git submodule of the
  product. Use `find_package` against an installed prefix. Submodules
  bypass the install/export step — the part we most want exercised.
- **Do not** introduce a third "shared" repo for code "used by both".
  Either it belongs in the library (and the product consumes it
  through the install) or it is product-specific.

---

## Cross-references

- [`00-overview.md`](00-overview.md) — layered subsystem map and which
  layer each component sits in.
- [`02-build-and-toolchain.md`](02-build-and-toolchain.md) — toolchain
  floor that both repos must agree on.
- [`05-cmake.md`](05-cmake.md) — library-side CMake conventions; the
  product follows the same conventions for warnings, presets, and
  sanitizer matrices.
- [`07-ci-and-reproducibility.md`](07-ci-and-reproducibility.md) —
  reproducibility envelope; the product pins to a library tag that was
  built inside the envelope.
- [`08-test-strategy.md`](08-test-strategy.md) — library-side test
  pyramid. The product owns integration / E2E above that.
