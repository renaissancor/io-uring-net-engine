# 02 — Build and toolchain

What this project builds *against*: language standard, kernel
requirements, dependency floors, and repository layout. Pure design
statements; no install commands or CMake configuration here.

For the *how* that pairs with this *what*:

- **`05-cmake.md`** — CMake target shape, warning set, sanitizer
  presets, `cmake/deps.cmake` resolution pattern.
- **`06-system-setup.md`** — distro-specific install runbook, smoke
  tests, kernel sanity check.
- **`07-ci-and-reproducibility.md`** — CI matrix, floor job,
  Dockerfile / devcontainer / `version-snapshot.txt`.
- **`08-test-strategy.md`** — test pyramid, sanitizer policy.

---

## Targets and language standard

- **Language: C++20. Locked.** No C++23 features in the public
  surface. CMake: `target_compile_features(iouring_net PUBLIC
  cxx_std_20)`.
- Used from C++20: coroutines (`<coroutine>`), `<concepts>`,
  `<ranges>`, `std::jthread`, `std::stop_token`, `<bit>`, designated
  initializers.
- **Formatting and `expected` are not used from libstdc++.** Both
  depend on stdlib versions newer than the gcc-12 floor:
  - `<format>` is incomplete in libstdc++-12 (compile-time string
    parsing missing); `std::expected` doesn't exist in libstdc++-12
    at all.
  - The project standardizes on `{fmt}` for all formatted output and
    `tl::expected` for the `expected<T, E>` API, regardless of which
    stdlib version a contributor's compiler ships with.
- **Polyfills (vendored or apt):**
  - `tl::expected` — [`https://github.com/TartanLlama/expected`](https://github.com/TartanLlama/expected)
    (Sy Brand, header-only). Vendored via `FetchContent`. Re-exported
    as the project `expected` alias used in the wiki specs and source.
  - `{fmt}` — [`https://github.com/fmtlib/fmt`](https://github.com/fmtlib/fmt)
    (Victor Zverovich; the library `std::format` was modeled after).
    Floor is **fmt 10+** (for `fmt::println`, added in 10.0). **No
    distro currently ships fmt 10 in stable apt** — Ubuntu 24.04 has
    9.1, Debian 12 has 9.1, RHEL 9 has 9.1, Fedora 39+ is the
    exception. `find_package(fmt 10 CONFIG)` falls through to
    `FetchContent` everywhere except Fedora 39+; this is by design
    (see `05-cmake.md`). Use `fmt::format`, `fmt::print`,
    `fmt::println` everywhere — never `std::format` or `std::print`.
- **Why C++20 over C++23:** broader compiler/distro reach (Ubuntu
  22.04, Debian 12, RHEL 9, Fedora 36+ all build), no dependence on
  bleeding-edge stdlib features, and matches industry-baseline
  production C++ in 2026. The two C++23 APIs the design names are
  trivially polyfilled and the call sites don't change.
- Build: CMake **3.25+** (for `FILE_SET HEADERS`, `--workflow`).
- **Compiler floor:**
  - `g++ 12+` — sets the floor. Earlier libstdc++ versions lack a
    stable `std::jthread` / `std::stop_token`.
  - `clang++ 14+` paired with **libstdc++-12+** *or* **libc++-18+**.
    libc++ earlier than 18 lacks `std::jthread`.
- **Compiler ceiling: none.** g++-14 and clang-18 (current noble
  defaults) are tested. Newer toolchains are expected to work.

---

## Kernel requirements

| Feature                                  | Kernel | Detect via                                  | Used by                          |
|------------------------------------------|--------|---------------------------------------------|----------------------------------|
| Basic `io_uring` correctness             | 5.10+  | `io_uring_queue_init` returns ≥ 0           | everything                       |
| `io_uring_register_buffers` (fixed bufs) | 5.7+   | `io_uring_register_buffers()` returns ≥ 0   | reactor recv/send fast path      |
| `IOSQE_ASYNC`, `IOSQE_LINK`              | 5.7+   | implied by 5.10+ baseline                   | linked SQE chains                |
| `IORING_OP_ACCEPT`                       | 5.5+   | `io_uring_get_probe()` opcode bit           | listener                         |
| Multishot accept flag                    | 5.19+  | trial-submit with `IORING_ACCEPT_MULTISHOT`, expect not `-EINVAL` | listener |
| `IOSQE_BUFFER_SELECT` / classic provided buffers | 5.7+ | trial-submit; check CQE not `-EOPNOTSUPP` | recv ring buffers (legacy path) |
| Provided-buffer ring (`io_uring_register_buf_ring`) | 5.19+ | `io_uring_get_probe()` for `IORING_REGISTER_PBUF_RING` | recv ring buffers (preferred path) |
| `IORING_RECV_MULTISHOT`                  | 6.0+   | trial-submit with the flag                  | recv hot path                    |
| `IORING_OP_MSG_RING`                     | 5.18+  | `io_uring_get_probe()` opcode bit           | cross-thread wakeup              |
| `io_uring_prep_cancel_all`               | 6.0+   | `io_uring_get_probe()` opcode bit           | shutdown fast path (optional)    |

**Recommended baseline:** kernel **5.19+** (covers everything except
`IORING_RECV_MULTISHOT`, which is opt-in). Refuse to run below this.

### Three-layer feature detection

`IORING_FEAT_*` bits alone are **not sufficient** to verify the
features this project uses. They cover io_uring infrastructure
(fast-poll, native-workers, single-mmap), not opcode availability or
per-op flag support. The startup probe must run all three of:

1. **`io_uring_params.features`** (set by `io_uring_queue_init_params`)
   — for `IORING_FEAT_FAST_POLL`, `_NATIVE_WORKERS`, `_RSRC_TAGS`,
   `_CQE_SKIP`, `_LINKED_FILE`.
2. **`io_uring_get_probe()`** — returns a bitmap of supported
   `IORING_OP_*` values. Required for opcode availability
   (`OP_ACCEPT`, `OP_RECV`, `OP_MSG_RING`, `REGISTER_PBUF_RING`, etc.).
3. **Trial-submit** — for per-op flags that aren't reflected in
   either surface above (`IORING_ACCEPT_MULTISHOT`,
   `IORING_RECV_MULTISHOT`, `IOSQE_BUFFER_SELECT`). Submit one SQE
   with the flag against a non-blocking fixture and check whether
   the CQE returns `-EINVAL` / `-EOPNOTSUPP`.

The runtime cache is built once at reactor startup and consulted on
the hot path with no further syscalls.

### `scripts/kernel-probe.sh` output (target shape)

```
$ scripts/kernel-probe.sh
kernel:                6.6.114
io_uring_disabled:     0
liburing-dev:          2.5

io_uring_queue_init:   ok
io_uring_params.features:
  IORING_FEAT_FAST_POLL                ✓
  IORING_FEAT_NATIVE_WORKERS           ✓
  IORING_FEAT_RSRC_TAGS                ✓
  IORING_FEAT_CQE_SKIP                 ✓
  IORING_FEAT_LINKED_FILE              ✓

io_uring_get_probe (opcodes):
  IORING_OP_ACCEPT                     ✓
  IORING_OP_RECV                       ✓
  IORING_OP_SEND                       ✓
  IORING_OP_MSG_RING                   ✓
  IORING_REGISTER_PBUF_RING            ✓
  IORING_OP_ASYNC_CANCEL               ✓
  IORING_OP_ASYNC_CANCEL_ALL           ✓ (kernel 6.0+ optional)

trial-submit (per-op flags):
  IORING_ACCEPT_MULTISHOT              ✓
  IORING_RECV_MULTISHOT                ✓ (kernel 6.0+ optional)
  IOSQE_BUFFER_SELECT                  ✓

Project requirements (kernel 5.19+ baseline): SATISFIED
Optional (multishot recv 6.0+):                SATISFIED
```

**WSL2:** WSL2 kernel version varies by Windows release and
`wsl --update` cadence — historical defaults run from 5.10 to 6.6+.
Always run `uname -r` and `scripts/kernel-probe.sh` on each
contributor machine; do not assume a fixed WSL2 baseline. Suitable
for development and correctness testing. Performance benchmarking
belongs on bare metal or a cloud VM (EC2 `c7i`, GCP `c3`).

---

## Dependencies

Hard:

| Dep        | Min version | Source                                                       |
|------------|-------------|--------------------------------------------------------------|
| `liburing` | 2.5         | distro package, or `FetchContent` (see `05-cmake.md`)        |
| `pthreads` | system      | linked via `find_package(Threads REQUIRED)` + `Threads::Threads` (this guarantees `-pthread` on both compile and link) |

Soft (testing / dev):

| Dep        | Why                                                 |
|------------|-----------------------------------------------------|
| `Catch2 v3`| unit tests                                          |
| `nanobench`| micro-benchmarks (memory pool, ring buffer)         |
| `clang-tidy` | static analysis in CI                             |
| `clang-format` | formatting                                       |
| `cppcheck` | secondary static analysis                            |

No header-only deps from upstream. No `vcpkg`, no `conan` —
`find_package` or `FetchContent` only. The point of the project is
to demonstrate Linux systems programming, not a build orchestrator.

---

## Repository layout

```
iouring-net-lib/
├── CMakeLists.txt
├── cmake/
│   ├── compiler-warnings.cmake
│   ├── deps.cmake                  ← single canonical dep list
│   └── deps-pins.env.in            ← configure_file → build/deps-pins.env
├── src/                   headers and sources together; no separate include/
│   ├── data_structure/    ring_buffer, serial_buffer, cstr_hash_map,
│   │                      indexed_heap, malloc_vector
│   ├── memory/            memory_pool, object_pool, leak_tracker,
│   │                      guard_overflow
│   ├── sync/              atomic, mutex, shared_mutex, lock_free_stack
│   ├── diagnostic/        logger, deadlock_profiler, profiler
│   ├── runtime/           task, reactor, job_queue, thread_context
│   ├── network/           listener, service, session, packet_framing,
│   │                      packet_handler
│   └── error/             expected (alias header re-exporting tl::expected)
├── tests/                 mirrors src/ category layout
│   ├── data_structure/  memory/  sync/  diagnostic/  runtime/  network/
│   └── integration/       cross-subsystem (multi-process loopback,
│                          wire-format parity)
├── examples/
│   └── echo_server/
├── benchmarks/            nanobench micro-benchmarks; run on demand
├── docs/                  library-wide documentation
│   ├── 00-overview.md
│   ├── 01-windows-to-linux-mapping.md
│   ├── 02-build-and-toolchain.md   (this file — design-only)
│   ├── 04-coding-style.md
│   ├── 05-cmake.md
│   ├── 06-system-setup.md
│   ├── 07-ci-and-reproducibility.md
│   └── 08-test-strategy.md
├── wiki/                  per-source-file design docs (mirror of src/)
│   └── data_structure/  memory/  sync/  diagnostic/  runtime/  network/
├── .devcontainer/
│   ├── Dockerfile          ← see 07-ci-and-reproducibility.md
│   └── devcontainer.json
├── .github/workflows/
│   └── floor.yml           ← Ubuntu 22.04 + g++-12 floor job
└── scripts/
    ├── setup.sh            ← distro-aware one-shot install
    ├── kernel-probe.sh     ← prints IORING_FEAT_* + opcode + trial probe
    ├── emit-snapshot.sh    ← emits build/version-snapshot.txt
    └── lint.sh
```

Headers and sources sit together under `src/<category>/`. There is no
separate `include/` directory. Inside the project, `#include` paths
are relative to `src/`:

```cpp
#include "data_structure/ring_buffer.h"
#include "sync/mutex.h"
```

**Install layout.** CMake copies `src/**/*.h` to
`<prefix>/include/iouring_net/`, preserving relative paths. External
consumers write `#include <iouring_net/data_structure/ring_buffer.h>`.

**Folder vs. namespace.** Folders organize source files by *function*
(`data_structure/`, `sync/`, etc.), not by namespace. Most code lives
in the global namespace; `lnx::` is reserved for raw POSIX/Linux API
wrappers; per-subsystem namespaces are introduced only where a
subsystem groups multiple related types around a `manager` singleton.
See `04-coding-style.md` for the full rules.

---

## Open build questions

1. **Single TU vs. per-class TU.** The reference repos use header-only
   with `#pragma once` and few `.cpp` files. For this project, lean
   toward `.cpp` per non-template class to keep build times sane and
   ABI surface visible.
2. **Static vs. shared default.** Default to static. Add a CMake
   option `IOURING_NET_BUILD_SHARED=OFF` for users who need it.
3. **Module support (C++20 modules).** Skipped for v1. Toolchain
   support is uneven (clang's `import std` lands in different versions
   per distro). Revisit when GCC 14 and Clang 17 are baseline.
