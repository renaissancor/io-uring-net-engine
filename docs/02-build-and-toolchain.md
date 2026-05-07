# 02 — Build and toolchain

Build system, dependencies, kernel/distro requirements, and CI baseline.

---

## Targets and language standard

- Language: C++20 mandatory. Coroutines (`<coroutine>`), `<concepts>`,
  `<ranges>`, `std::jthread`, `std::stop_token`, designated initializers.
- C++23 used opportunistically: `std::expected`, `std::print`. Behind a
  `__cpp_lib_expected` feature test.
- Build: CMake **3.25+** (for `FILE_SET HEADERS`, `--workflow`).
- Compiler matrix:
  - `clang++ 16+` (primary; best coroutine codegen on Linux)
  - `g++ 13+` (secondary; `std::expected` since 13.1)

---

## Kernel requirements

| Feature                                  | Kernel | Used by                               |
|------------------------------------------|--------|----------------------------------------|
| Basic `io_uring` correctness             | 5.10+  | everything                             |
| `io_uring_register_buffers` (fixed bufs) | 5.7+   | reactor recv/send fast path            |
| `IOSQE_ASYNC`, `IOSQE_LINK`              | 5.7+   | linked SQE chains                      |
| Multishot accept (`IORING_OP_ACCEPT` + `IORING_ACCEPT_MULTISHOT`) | 5.19+ | listener |
| `IOSQE_BUFFER_SELECT` / provided buffers | 5.19+  | recv ring buffers                      |
| Multishot recv (`IORING_RECV_MULTISHOT`) | 6.0+   | recv hot path                          |
| `IORING_OP_MSG_RING`                     | 5.18+  | cross-thread wakeup                    |

**Recommended baseline:** kernel **5.19+**. Document degradations for older
kernels but do not maintain a polyfill matrix in code; probe `IORING_FEAT_*`
flags at startup and refuse to run if the kernel is too old.

**WSL2:** WSL2 ships kernel 5.10+ by default and 5.15+ in current
distributions. Suitable for development and correctness testing. Performance
benchmarking belongs on bare metal or a cloud VM (EC2 `c7i`, GCP `c3`).

---

## Dependencies

Hard:

| Dep        | Min version | Source                                |
|------------|-------------|---------------------------------------|
| `liburing` | 2.5         | distro package or build from source   |
| `pthreads` | system      | linked transitively via libstdc++     |

Soft (testing / dev):

| Dep        | Why                                                 |
|------------|-----------------------------------------------------|
| `Catch2 v3`| unit tests                                          |
| `nanobench`| micro-benchmarks (memory pool, ring buffer)         |
| `clang-tidy` | static analysis in CI                             |
| `clang-format` | formatting                                       |
| `cppcheck` | secondary static analysis                            |

No header-only deps from upstream. No `vcpkg`, no `conan` — `find_package`
or `FetchContent` only. The point of the project is to demonstrate Linux
systems programming, not to demonstrate a build orchestrator.

---

## Repository layout

```
iouring-net-lib/
├── CMakeLists.txt
├── cmake/
│   └── compiler-warnings.cmake
├── src/
│   ├── primitives/
│   ├── runtime/
│   └── network/
├── include/iouring_net/
│   ├── primitives/
│   ├── runtime/
│   └── network/
├── tests/
│   ├── unit/
│   └── integration/
├── examples/
│   └── echo_server/
├── docs/                  ← this directory
├── benchmarks/
└── scripts/
    ├── lint.sh
    └── kernel-probe.sh    ← prints available IORING_FEAT_*
```

Public headers live under `include/iouring_net/`; the install prefix is
`<prefix>/include/iouring_net/...`. The directory mirrors the layered
subsystem map: `primitives/`, `runtime/`, `network/`.

---

## CMake conventions

- One `add_library(iouring_net STATIC ...)` per layer is rejected. Use a
  single static (and optional shared) library target plus per-test executables.
- Target compile features expressed via `target_compile_features(.. PUBLIC
  cxx_std_20)`.
- `target_compile_options` warning set lives in `cmake/compiler-warnings.cmake`:
  `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
  -Wcast-align -Woverloaded-virtual -Wconversion -Wsign-conversion
  -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
  -Werror`. (Ratcheted in; not all on at v0.)
- Sanitizer presets via CMake presets (`CMakePresets.json`):
  - `asan` — address + UB
  - `tsan` — thread sanitizer (mandatory for any concurrency PR)
  - `release` — `-O2 -DNDEBUG`

---

## Test layout

- `tests/unit/` — Catch2 unit tests; one source per primitive.
- `tests/integration/` — multi-process loopback tests, including the
  Linux-vs-Windows wire-format-parity test (sends pre-recorded packet
  fixtures captured from the Windows reference).
- `benchmarks/` — `nanobench`-driven micro-benchmarks; run on demand, not in
  CI by default.

See `docs/testing/test-strategy.md` for the test-pyramid breakdown.

---

## CI (target shape)

GitHub Actions, three job matrix:

| Job              | Compiler | Sanitizer | Notes                                    |
|------------------|----------|-----------|------------------------------------------|
| `linux-clang`    | clang 16 | asan+ubsan | full unit + integration                  |
| `linux-clang-tsan` | clang 16 | tsan      | concurrency tests only (avoid asan/tsan clash) |
| `linux-gcc`      | gcc 13   | none      | release build, runs unit tests          |

Kernel inside the GHA runner is sufficient for unit tests. Integration tests
that exercise multishot accept require a self-hosted runner or a
container with kernel passthrough — out of scope for v0 CI.

---

## Local developer workflow

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

`scripts/kernel-probe.sh` prints `IORING_FEAT_*` flags so contributors know
what their dev kernel supports without reading kernel docs.

---

## Open build questions

1. **Single TU vs. per-class TU.** The reference repos use header-only with
   `#pragma once` and few `.cpp` files. For this project, lean toward `.cpp`
   per non-template class to keep build times sane and ABI surface visible.
2. **Static vs. shared default.** Default to static. Add a CMake option
   `IOURING_NET_BUILD_SHARED=OFF` for users who need it.
3. **Module support (C++20 modules).** Skipped for v1. Toolchain support is
   uneven (clang's `import std` lands in different versions per distro).
   Revisit when GCC 14 and Clang 17 are baseline.
