# 02 — Build and toolchain

Build system, dependencies, kernel/distro requirements, and CI baseline.

---

## Targets and language standard

- **Language: C++20. Locked.** No C++23 features in the public surface.
  CMake: `target_compile_features(iouring_net PUBLIC cxx_std_20)`.
- Used from C++20: coroutines (`<coroutine>`), `<concepts>`, `<ranges>`,
  `std::jthread`, `std::stop_token`, `<bit>`, designated initializers.
- **Formatting and `expected` are not used from libstdc++.** Both depend
  on stdlib versions newer than the gcc-12 floor:
  - `<format>` is incomplete in libstdc++-12 (compile-time string parsing
    missing); `std::expected` doesn't exist in libstdc++-12 at all.
  - The project standardizes on `{fmt}` for all formatted output and
    `tl::expected` for the `expected<T, E>` API, regardless of which
    stdlib version a contributor's compiler ships with.
- **Polyfills (vendored or apt):**
  - `tl::expected` — [`https://github.com/TartanLlama/expected`](https://github.com/TartanLlama/expected)
    (Sy Brand, header-only). Vendored via `FetchContent`. Re-exported
    as the project `expected` alias used in the wiki specs and source.
  - `{fmt}` — [`https://github.com/fmtlib/fmt`](https://github.com/fmtlib/fmt)
    (Victor Zverovich; the library `std::format` was modeled after).
    Apt: `libfmt-dev` (10.x on noble); fallback via `FetchContent`.
    Use `fmt::format`, `fmt::print`, `fmt::println` everywhere — never
    `std::format` or `std::print`.
- **Why C++20 over C++23:** broader compiler/distro reach (Ubuntu 22.04,
  Debian 12, RHEL 9, Fedora 36+ all build), no dependence on bleeding-edge
  stdlib features, and matches industry-baseline production C++ in 2026.
  The two C++23 APIs the design names are trivially polyfilled and the
  call sites don't change.
- Build: CMake **3.25+** (for `FILE_SET HEADERS`, `--workflow`).
- **Compiler floor:**
  - `g++ 12+` — sets the floor. Earlier libstdc++ versions lack a stable
    `std::jthread` / `std::stop_token`.
  - `clang++ 14+` paired with **libstdc++-12+** *or* **libc++-18+**. libc++
    earlier than 18 lacks `std::jthread`.
- **Compiler ceiling: none.** g++-14 and clang-18 (current noble defaults)
  are tested. Newer toolchains are expected to work.

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

`IORING_FEAT_*` bits alone are **not sufficient** to verify the features
this project uses. They cover io_uring infrastructure (fast-poll,
native-workers, single-mmap), not opcode availability or per-op flag
support. The startup probe must run all three of:

1. **`io_uring_params.features`** (set by `io_uring_queue_init_params`) —
   for `IORING_FEAT_FAST_POLL`, `_NATIVE_WORKERS`, `_RSRC_TAGS`,
   `_CQE_SKIP`, `_LINKED_FILE`.
2. **`io_uring_get_probe()`** — returns a bitmap of supported `IORING_OP_*`
   values. Required for opcode availability (`OP_ACCEPT`, `OP_RECV`,
   `OP_MSG_RING`, `REGISTER_PBUF_RING`, etc.).
3. **Trial-submit** — for per-op flags that aren't reflected in either
   surface above (`IORING_ACCEPT_MULTISHOT`, `IORING_RECV_MULTISHOT`,
   `IOSQE_BUFFER_SELECT`). Submit one SQE with the flag against a
   non-blocking fixture and check whether the CQE returns `-EINVAL` /
   `-EOPNOTSUPP`.

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

**WSL2:** WSL2 kernel version varies by Windows release and `wsl
--update` cadence — historical defaults run from 5.10 to 6.6+. Always
run `uname -r` and `scripts/kernel-probe.sh` on each contributor
machine; do not assume a fixed WSL2 baseline. Suitable for development
and correctness testing. Performance benchmarking belongs on bare
metal or a cloud VM (EC2 `c7i`, GCP `c3`).

---

## Dependencies

Hard:

| Dep        | Min version | Source                                                       |
|------------|-------------|--------------------------------------------------------------|
| `liburing` | 2.5         | distro package, or `FetchContent` (see cross-distro section) |
| `pthreads` | system      | linked via `find_package(Threads REQUIRED)` + `Threads::Threads` (this guarantees `-pthread` on both compile and link) |

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
├── src/                   headers and sources together; no separate include/
│   ├── data_structure/    ring_buffer, serial_buffer, cstr_hash_map,
│   │                      indexed_heap, malloc_vector
│   ├── memory/            memory_pool, object_pool, leak_tracker,
│   │                      guard_overflow
│   ├── sync/              atomic, mutex, shared_mutex, lock_free_stack
│   ├── diagnostic/        logger, deadlock_profiler, profiler
│   ├── runtime/           task, reactor, job_queue, thread_context
│   └── network/           listener, service, session, packet_framing,
│                          packet_handler
├── tests/                 mirrors src/ category layout
│   ├── data_structure/  memory/  sync/  diagnostic/  runtime/  network/
│   └── integration/       cross-subsystem (multi-process loopback,
│                          wire-format parity)
├── examples/
│   └── echo_server/
├── benchmarks/            nanobench micro-benchmarks; run on demand
├── docs/                  library-wide documentation
│   ├── 00-overview.md ... 04-coding-style.md
│   └── testing/test-strategy.md
├── wiki/                  per-source-file design docs (mirror of src/)
│   └── data_structure/  memory/  sync/  diagnostic/  runtime/  network/
└── scripts/
    ├── lint.sh
    └── kernel-probe.sh    ← prints available IORING_FEAT_*
```

Headers and sources sit together under `src/<category>/`. There is no
separate `include/` directory. Inside the project, `#include` paths are
relative to `src/`:

```cpp
#include "data_structure/ring_buffer.h"
#include "sync/mutex.h"
```

**Install layout.** CMake copies `src/**/*.h` to
`<prefix>/include/iouring_net/`, preserving relative paths. External
consumers write `#include <iouring_net/data_structure/ring_buffer.h>`.

**Folder vs. namespace.** Folders organize source files by *function*
(`data_structure/`, `sync/`, etc.), not by namespace. Most code lives in
the global namespace; `lnx::` is reserved for raw POSIX/Linux API
wrappers; per-subsystem namespaces are introduced only where a subsystem
groups multiple related types around a `manager` singleton. See
`04-coding-style.md` for the full rules.

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

- `tests/<category>/` — Catch2 unit + component tests; the tree mirrors
  `src/<category>/` one file per source file.
- `tests/integration/` — multi-process loopback tests including the
  Linux-vs-Windows wire-format-parity test (sends pre-recorded packet
  fixtures captured from the Windows reference).
- `benchmarks/` — `nanobench`-driven micro-benchmarks; run on demand, not
  in CI by default.

See `docs/testing/test-strategy.md` for the test-pyramid breakdown.

---

## CI (target shape)

GitHub Actions, four-job matrix designed to enforce the C++20 floor and
catch portability regressions:

| Job                  | Distro       | Compiler  | Sanitizer  | Purpose                                      |
|----------------------|--------------|-----------|------------|----------------------------------------------|
| `linux-gcc-floor`    | Ubuntu 22.04 | g++-12    | asan+ubsan | enforces the gcc-12 minimum                  |
| `linux-gcc`          | Ubuntu 24.04 | g++-14    | asan+ubsan | primary; full unit + integration             |
| `linux-gcc-tsan`     | Ubuntu 24.04 | g++-14    | tsan       | concurrency tests only (asan/tsan clash)     |
| `linux-clang`        | Ubuntu 24.04 | clang++-18 | asan+ubsan | catches portability bugs gcc misses          |

The floor job is the load-bearing one: if a contributor accidentally
uses a C++23 feature, it fails on `linux-gcc-floor` even when noble
builds clean.

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

## System setup (Ubuntu 24.04 / WSL2)

Verified working on Ubuntu 24.04.4 LTS (Noble) under WSL2 kernel 6.6.114
on 2026-05-10. The kernel reports `IORING_FEAT == 0x3fff` (bits 0–13), so
every multishot/provided-buffer feature this project uses is available
without further kernel work.

### One-shot install (Ubuntu 24.04 / WSL2 — verified 2026-05-10)

```bash
sudo apt update
sudo apt install -y \
  g++-14 clang-18 clang-tidy-18 clang-format-18 lld-18 \
  cmake ninja-build pkg-config \
  liburing-dev catch2 libfmt-dev \
  valgrind cppcheck gdb
```

Verified set after install:

| Tool             | Version | Notes                                     |
|------------------|---------|-------------------------------------------|
| `g++-14`         | 14.2.0  | C++20; well above the gcc-12 floor        |
| `clang++-18`     | 18.1.3  | C++20; well above the clang-14 floor      |
| `clang-tidy-18`  | 18.1.3  | invoke as `clang-tidy-18`                 |
| `clang-format-18`| 18.1.3  | invoke as `clang-format-18`               |
| `lld-18`         | 18.1.3  | `-fuse-ld=lld-18` for the release preset  |
| `cmake`          | 3.28.3  | meets the 3.25+ floor                     |
| `ninja`          | 1.11.1  |                                           |
| `liburing` / `-dev` | 2.5  | meets the 2.5 floor                       |
| `catch2`         | 3.4.0   | `find_package(Catch2 3 REQUIRED)`         |
| `libfmt-dev`     | 10.x    | `find_package(fmt 10 CONFIG)` — formatting library; meets the 10.x floor |
| `valgrind`       | 3.22.0  | helgrind / drd for concurrency triage     |
| `cppcheck`       | 2.13.0  | secondary static analysis                 |
| `gdb`            | 15.1    |                                           |

`tl::expected` is **not** in apt; vendored via `FetchContent` from the
project root `CMakeLists.txt`. No system install needed.

Noble's default `g++` (no version suffix) resolves to **g++-13**, which
is also a supported compiler under the C++20 baseline. To use the more
recent g++-14 explicitly, invoke `g++-14`, set `CXX=g++-14`, or pass
`-DCMAKE_CXX_COMPILER=g++-14` to CMake. The same applies to versioned
clang binaries.

### Cross-distro install model: system core + vendored deps

Stock distro packages for `liburing`, `{fmt}`, `Catch2`, and `cmake` are
**below this project's floors on most non-noble distros**. The project
therefore splits its dependencies in two:

- **System packages** — compiler, build tools, debug tools.
  Always installed via the distro package manager.
- **Project dependencies with version floors** — `liburing` ≥ 2.5,
  `{fmt}` ≥ 10, `Catch2` ≥ 3.4, `tl::expected`. Sourced via
  `FetchContent` from the project's root `CMakeLists.txt`. Optionally
  satisfied by system packages when the distro version meets the floor;
  CMake's `find_package(... CONFIG)` is tried first, with `FetchContent`
  as the fallback.

This means **the install command changes by distro, but the build
recipe doesn't** — `cmake --preset default && cmake --build --preset
default` works identically on all supported distros.

#### System packages by distro

| Distro       | One-shot system install (verified)                                                                 |
|--------------|----------------------------------------------------------------------------------------------------|
| Ubuntu 24.04 | `apt install -y g++-14 clang-18 clang-tidy-18 clang-format-18 lld-18 cmake ninja-build pkg-config valgrind cppcheck gdb` |
| Ubuntu 22.04 | `apt install -y g++-12 clang-15 cmake ninja-build pkg-config valgrind cppcheck gdb`                |
| Debian 12    | `apt install -y g++ clang cmake ninja-build pkg-config valgrind cppcheck gdb`                       |
| Fedora 38+   | `dnf install -y gcc-c++ clang clang-tools-extra cmake ninja-build pkg-config valgrind cppcheck gdb` |
| RHEL/Alma 9  | `dnf install -y gcc-toolset-12 cmake ninja-build pkg-config valgrind cppcheck gdb` (enable SCL with `scl enable gcc-toolset-12 -- bash`) |

#### What the distro gives you for liburing / fmt / Catch2 / cmake

| Distro       | liburing | {fmt} | Catch2 | cmake |
|--------------|----------|-------|--------|-------|
| Ubuntu 24.04 | 2.5 ✓    | 10.1 ✓ | 3.4 ✓  | 3.28 ✓ |
| Ubuntu 22.04 | 2.1 ✗    | 8.1 ✗ | 2.13 ✗ | 3.22 ✗ |
| Debian 12    | 2.3 ✗    | 9.1 ✗ | 3.4 ✓  | 3.25 ✓ |
| Fedora 38    | 2.3 ✗    | 9.1 ✗ | 3.3 ✗  | 3.27 ✓ |
| Fedora 39+   | 2.5 ✓    | 10.x ✓ | 3.4 ✓ | 3.27 ✓ |
| RHEL/Alma 9  | 2.1 ✗    | 9.1 ✗ | — ✗    | 3.20 ✗ (use Kitware repo or `pip install cmake`) |

Cells marked ✗ trigger the `FetchContent` fallback automatically; no
manual action is needed beyond running the system install command.

#### FetchContent fallback (root `CMakeLists.txt` sketch)

```cmake
include(FetchContent)

# liburing — only fetched if pkg-config can't find ≥ 2.5
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING IMPORTED_TARGET liburing>=2.5)
if(NOT LIBURING_FOUND)
  FetchContent_Declare(liburing
    GIT_REPOSITORY https://github.com/axboe/liburing.git
    GIT_TAG        liburing-2.5)
  FetchContent_MakeAvailable(liburing)
  # liburing builds via configure+make, not CMake — wrap in a custom target
endif()

# {fmt}
find_package(fmt 10 CONFIG QUIET)
if(NOT fmt_FOUND)
  FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.2.1)
  FetchContent_MakeAvailable(fmt)
endif()

# Catch2
find_package(Catch2 3 CONFIG QUIET)
if(NOT Catch2_FOUND)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.4.0)
  FetchContent_MakeAvailable(Catch2)
endif()

# tl::expected — header-only, never available via apt; always fetched
FetchContent_Declare(tl_expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected.git
  GIT_TAG        v1.1.0)
FetchContent_MakeAvailable(tl_expected)
```

`liburing` is the awkward one — it's an autotools project, not CMake.
The fallback uses `ExternalProject_Add` with `configure && make` and
imports the resulting static library; details land in
`cmake/Findliburing.cmake` once code starts.

CI floor job: **Ubuntu 22.04 + g++-12 + FetchContent for everything
below floor.** That run validates the apt-core + FetchContent split
against the most-stripped supported distro.

### Kernel and io_uring sanity check

```bash
uname -r                                # expect 5.19+; WSL2 default is 6.6
cat /proc/sys/kernel/io_uring_disabled  # must be 0
```

`scripts/kernel-probe.sh` (TBD) prints the runtime `IORING_FEAT_*` bitmap
so contributors can confirm parity with the FEAT bits this project assumes.

### Optional / deferred

- **`libc++-18-dev`** — required only if a contributor explicitly wants
  to build with clang's stdlib (`-stdlib=libc++`). The default clang-18
  build uses libstdc++ from the system gcc. Not required for the matrix
  above.
- **clang-19+** — not in noble. Available via the LLVM apt repo
  (`https://apt.llvm.org/`). Useful but not required.
- **`nanobench`** — not in apt. Vendored via `FetchContent` once
  `benchmarks/` exists. Not blocking v1.

### Smoke test the toolchain

C++20 language + stdlib check (no `<format>` — that comes from `{fmt}`,
not libstdc++):

```bash
cat > /tmp/probe.cpp <<'EOF'
#include <coroutine>
#include <ranges>
#include <thread>
#include <numeric>
#include <vector>
int main(){
  std::vector v{1,2,3};
  auto sum = 0;
  for (int x : v | std::views::transform([](int n){ return n*n; })) sum += x;
  std::jthread t{[]{ /* stop_token-aware thread */ }};
  return sum == 14 ? 0 : 1;
}
EOF
g++-14 -std=c++20 /tmp/probe.cpp -o /tmp/probe && /tmp/probe \
  && echo "C++20 OK"
```

`{fmt}` link check (formatting + the `std::print` polyfill):

```bash
cat > /tmp/fmt_probe.cpp <<'EOF'
#include <fmt/core.h>
int main(){ fmt::print("ok {} -> {}\n", 6, fmt::format("{:#x}", 0x42)); }
EOF
g++-14 -std=c++20 /tmp/fmt_probe.cpp -lfmt -o /tmp/fmt_probe && /tmp/fmt_probe
```

`liburing` link check:

```bash
cat > /tmp/uring.cpp <<'EOF'
#include <liburing.h>
#include <cstdio>
int main(){
  io_uring_params p{}; io_uring r;
  io_uring_queue_init_params(8, &r, &p);
  std::printf("FEATURES=0x%x\n", p.features);
  io_uring_queue_exit(&r);
}
EOF
g++-14 -std=c++20 /tmp/uring.cpp $(pkg-config --cflags --libs liburing) \
  -o /tmp/uring && /tmp/uring   # expect: FEATURES=0x3fff (or higher)
```

---

## Reproducibility envelope

The io_uring stack has five independently-versioned axes: kernel,
glibc, libstdc++, liburing, and distro packages. Assumptions baked in
2026 rot silently by 2027. This section puts load-bearing artifacts in
place so the envelope stays *provable*, not remembered.

### 1. Pinned artifacts

Three files define the canonical tuple. The Dockerfile is the binding
contract; `setup.sh` is convenience. Both must produce an identical
`version-snapshot.txt`.

**`scripts/setup.sh`** — distro-aware one-shot install:

```bash
#!/usr/bin/env bash
set -euo pipefail
ID=$(grep -Po '(?<=^ID=).+' /etc/os-release | tr -d '"')
VER=$(grep -Po '(?<=^VERSION_ID=).+' /etc/os-release | tr -d '"')

# Kernel-headers package terminology — IMPORTANT:
#   linux-libc-dev (Debian/Ubuntu)  → kernel UAPI headers (linux/io_uring.h)
#   kernel-headers  (Fedora/RHEL)   → kernel UAPI headers (linux/io_uring.h)
#   linux-headers-generic            → kernel-MODULE headers (NOT what we need)
# liburing.h itself ships in liburing-dev (or the vendored FetchContent
# build); the project always needs both the UAPI headers AND a liburing
# (system or vendored) — they are separate dependencies.
case "$ID:$VER" in
  ubuntu:24.04) apt-get update && apt-get install -y \
      g++-14 clang-18 clang-tidy-18 clang-format-18 lld-18 \
      cmake ninja-build pkg-config linux-libc-dev \
      liburing-dev catch2 libfmt-dev valgrind cppcheck gdb ;;
  ubuntu:22.04)
      # Stock 22.04 cmake is 3.22, below the 3.25 floor. Pull from
      # Kitware's apt repo before any other install.
      apt-get update && apt-get install -y wget gnupg ca-certificates
      wget -qO - https://apt.kitware.com/keys/kitware-archive-latest.asc \
        | gpg --dearmor -o /usr/share/keyrings/kitware.gpg
      echo 'deb [signed-by=/usr/share/keyrings/kitware.gpg] https://apt.kitware.com/ubuntu/ jammy main' \
        > /etc/apt/sources.list.d/kitware.list
      apt-get update && apt-get install -y \
        g++-12 clang-15 cmake ninja-build pkg-config linux-libc-dev \
        valgrind cppcheck gdb ;;   # liburing/fmt/Catch2 → FetchContent
  debian:12)    apt-get update && apt-get install -y \
      g++ clang cmake ninja-build pkg-config linux-libc-dev \
      valgrind cppcheck gdb ;;
  fedora:*)     dnf install -y \
      gcc-c++ clang clang-tools-extra cmake ninja-build pkg-config \
      kernel-headers valgrind cppcheck gdb ;;
  rhel:9|almalinux:9)
      # RHEL 9 ships cmake 3.20; need Kitware repo or pip for 3.25+.
      dnf install -y python3-pip gcc-toolset-12 ninja-build pkg-config kernel-headers
      pip3 install --no-cache-dir 'cmake>=3.25' ;;
  *) echo "Unsupported distro: $ID:$VER" && exit 1 ;;
esac
```

**`.devcontainer/Dockerfile`** — pins the canonical tuple end-to-end.
**Pin the base image by digest, not by tag**, so the same Dockerfile
produces the same image six months later. Update the digest deliberately
when promoting a new envelope; tag-only pins are not reproducible.

```dockerfile
# Replace <DIGEST> with the current ubuntu:24.04 digest at envelope-bump time:
#   docker pull ubuntu:24.04 && docker inspect --format='{{index .RepoDigests 0}}' ubuntu:24.04
FROM ubuntu:24.04@sha256:<DIGEST>
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-14 clang-18 clang-tidy-18 clang-format-18 lld-18 \
    cmake ninja-build pkg-config linux-libc-dev \
    liburing-dev catch2 libfmt-dev \
    valgrind cppcheck gdb ca-certificates git \
 && rm -rf /var/lib/apt/lists/*
```

**`.devcontainer/devcontainer.json`** — VS Code / CLion Remote uses the
same image and env:

```json
{ "name": "iouring-net-lib", "build": { "dockerfile": "Dockerfile" },
  "runArgs": ["--cap-add=SYS_PTRACE", "--security-opt", "seccomp=unconfined"],
  "postCreateCommand": "cmake --preset default" }
```

**Envelope caveats — read these before relying on the artifacts above:**

- **Container kernel ≠ container distro.** A container does not ship
  its own kernel. io_uring's ABI surface is the *host's* kernel, not
  the distro in `FROM`. The Dockerfile pins glibc / libstdc++ /
  liburing-headers reproducibly; the kernel comes from whatever runs
  the container (host, GitHub Actions runner, WSL2).
  `version-snapshot.txt` records both, separately.
- **io_uring inside containers needs runtime checks, not extra caps.**
  io_uring itself does not require `SYS_ADMIN`. But it can be disabled
  by host policy (`/proc/sys/kernel/io_uring_disabled == 1`),
  rate-limited by `RLIMIT_MEMLOCK` for fixed buffers, or blocked by the
  container runtime's seccomp profile (some Docker / Kubernetes
  defaults block `io_uring_setup`). `scripts/kernel-probe.sh` must
  check all three at startup; the probe failing is a hard refuse-to-run.
- **`SYS_PTRACE` + `seccomp=unconfined`** in `devcontainer.json` are
  for `gdb attach` and ASan/TSan, not for io_uring itself. Production
  containers should not need either.
- **WSL2 kernel drift.** WSL2's kernel can change under you (Windows
  update, `wsl --update`). Historical defaults span 5.10 to 6.6+. The
  envelope guards against this by hard-pinning a minimum kernel
  version in `scripts/kernel-probe.sh` and refusing to start the
  reactor below it. Do not assume a fixed WSL2 baseline anywhere.
- **glibc forward-compat is one-way.** A binary built in a 24.04
  container links against glibc 2.39 and *will not run* on a 22.04
  host (glibc 2.35). The snapshot records both build-host glibc and
  the binary's actual minimum-required glibc symbol — see section 4.

### 2. Load-bearing CI floor job

Ubuntu 22.04 + g++-12 + FetchContent for everything below floor.
Branch protection requires it to pass. Without this job, "works on
noble" silently rots into "doesn't work on 22.04" because no one tests
the lower bound. The floor job is the difference between *guessing* the
floor still works and *proving* it on every PR.

```yaml
# .github/workflows/floor.yml
name: floor
on: [push, pull_request]
jobs:
  ubuntu-2204-gcc12:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - run: sudo bash scripts/setup.sh
      - run: cmake -S . -B build -GNinja -DCMAKE_CXX_COMPILER=g++-12
      - run: cmake --build build
      - run: ctest --test-dir build --output-on-failure
      - run: bash scripts/emit-snapshot.sh build/version-snapshot.txt
      - uses: actions/upload-artifact@v4
        with: { name: version-snapshot-floor, path: build/version-snapshot.txt, retention-days: 90 }
```

### 3. Vendor-everything-by-default (`cmake/deps.cmake`)

`find_package(... CONFIG QUIET)` is a fast-path optimization, not a
contract. A single `cmake/deps.cmake` owns floors and tag pins;
FetchContent blocks are always present and locked to git tags.
**`master` / `main` references are not permitted.**

`liburing` is autotools, not CMake — `FetchContent_MakeAvailable` will
not work. Use `ExternalProject_Add` to run `configure && make`, then
expose an `IMPORTED` target.

```cmake
# cmake/deps.cmake — single canonical dep list
include(FetchContent)
include(ExternalProject)
find_package(PkgConfig REQUIRED)

set(LIBURING_FLOOR "2.5")  set(LIBURING_TAG  "liburing-2.5")
set(FMT_FLOOR     "10")    set(FMT_TAG       "10.2.1")
set(CATCH2_FLOOR  "3.4.0") set(CATCH2_TAG    "v3.4.0")
set(TL_EXP_TAG    "v1.1.0")

# liburing — autotools fallback when the system version is below floor
pkg_check_modules(LIBURING IMPORTED_TARGET liburing>=${LIBURING_FLOOR})
if(NOT LIBURING_FOUND)
  set(LIBURING_INSTALL_DIR ${CMAKE_BINARY_DIR}/_deps/liburing-install)
  set(LIBURING_LIB         ${LIBURING_INSTALL_DIR}/lib/liburing.a)
  set(LIBURING_INC         ${LIBURING_INSTALL_DIR}/include)
  ExternalProject_Add(liburing_ep
    GIT_REPOSITORY https://github.com/axboe/liburing.git
    GIT_TAG ${LIBURING_TAG}
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=${LIBURING_INSTALL_DIR}
    BUILD_COMMAND     make -j
    INSTALL_COMMAND   make install
    BUILD_IN_SOURCE   1
    BUILD_BYPRODUCTS  ${LIBURING_LIB})         # required for Ninja
  file(MAKE_DIRECTORY ${LIBURING_INC})         # so INTERFACE dir exists at config time
  add_library(liburing::uring STATIC IMPORTED GLOBAL)
  add_dependencies(liburing::uring liburing_ep)
  set_target_properties(liburing::uring PROPERTIES
    IMPORTED_LOCATION             ${LIBURING_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${LIBURING_INC})
endif()

# fmt / Catch2 / tl::expected — CMake-native FetchContent
find_package(fmt ${FMT_FLOOR} CONFIG QUIET)
if(NOT fmt_FOUND)
  FetchContent_Declare(fmt GIT_REPOSITORY https://github.com/fmtlib/fmt.git GIT_TAG ${FMT_TAG})
  FetchContent_MakeAvailable(fmt)
endif()
find_package(Catch2 3 CONFIG QUIET)
if(NOT Catch2_FOUND)
  FetchContent_Declare(Catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG ${CATCH2_TAG})
  FetchContent_MakeAvailable(Catch2)
endif()
FetchContent_Declare(tl_expected GIT_REPOSITORY https://github.com/TartanLlama/expected.git GIT_TAG ${TL_EXP_TAG})
FetchContent_MakeAvailable(tl_expected)
```

### 4. `version-snapshot.txt`

Every successful build emits this file; CI uploads it as a 90-day
artifact. When "broke between version A and B" reports arrive in 2027+,
binary-search the snapshots rather than guess what changed.

CMake writes `build/deps-pins.env` at configure time so the snapshot
script has access to the same tag values defined in `cmake/deps.cmake`
(shell-level `$LIBURING_TAG` would otherwise be unset):

```cmake
# tail of cmake/deps.cmake
configure_file(
  ${CMAKE_SOURCE_DIR}/cmake/deps-pins.env.in
  ${CMAKE_BINARY_DIR}/deps-pins.env @ONLY)
# deps-pins.env.in contents:
#   LIBURING_TAG=@LIBURING_TAG@
#   FMT_TAG=@FMT_TAG@
#   CATCH2_TAG=@CATCH2_TAG@
#   TL_EXP_TAG=@TL_EXP_TAG@
```

```bash
# scripts/emit-snapshot.sh — invoked by setup.sh and CI
set -euo pipefail
build="${BUILD_DIR:-build}"
out="${1:-${build}/version-snapshot.txt}"
test_bin="${build}/tests/iouring_net-test"          # any project binary
# shellcheck disable=SC1091
[ -f "${build}/deps-pins.env" ] && . "${build}/deps-pins.env"

# Highest GLIBCXX ABI symbol shipped by the active libstdc++ (not the first).
libstdcxx_so=$(g++ -print-file-name=libstdc++.so)
libstdcxx_abi=$(strings "${libstdcxx_so}" \
                | grep -oE 'GLIBCXX_[0-9.]+' | sort -V | tail -1)

# Runtime glibc floor encoded in the test binary, if it exists yet.
if [ -x "${test_bin}" ]; then
  min_glibc=$(objdump -T "${test_bin}" \
              | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1)
else
  min_glibc=unknown
fi

{
  echo "host_kernel=$(uname -r)"                      # io_uring ABI surface
  echo "build_glibc=$(ldd --version | head -1 | awk '{print $NF}')"
  echo "min_glibc=${min_glibc}"                       # forward-compat floor
  echo "libstdcxx=$(g++ -dumpversion) abi=${libstdcxx_abi}"
  command -v clang++ >/dev/null && echo "clang=$(clang++ --version | head -1)"
  if g++ -x c++ -E -include cxxabi.h -dM /dev/null 2>/dev/null \
       | grep -q _LIBCPP_VERSION; then
    echo "stdlib=libc++"; else echo "stdlib=libstdc++"
  fi
  echo "liburing_pkg=$(pkg-config --modversion liburing 2>/dev/null \
                       || echo vendored:${LIBURING_TAG:-unset})"
  echo "liburing_so=$(ldconfig -p 2>/dev/null | grep -m1 liburing.so \
                       | awk '{print $NF}' | xargs -r readlink -f \
                       || echo vendored)"
  echo "fmt=$(pkg-config --modversion fmt 2>/dev/null \
              || echo vendored:${FMT_TAG:-unset})"
  echo "catch2=$(pkg-config --modversion catch2 2>/dev/null \
                 || echo vendored:${CATCH2_TAG:-unset})"
  echo "tl_expected=vendored:${TL_EXP_TAG:-unset}"
  echo "gcc=$(g++ --version | head -1)"
  echo "cmake=$(cmake --version | head -1)"
  echo "ninja=$(ninja --version)"
} > "${out}"
```

Why each axis matters:
- **`host_kernel`** — io_uring opcode surface; *not* implied by the
  Dockerfile's `FROM`.
- **`build_glibc` vs. `min_glibc`** — forward-compat is one-way; a
  24.04-container build picks up newer glibc symbols and won't run on
  22.04 hosts. `min_glibc` from `objdump -T` is the *actual* runtime
  floor.
- **`libstdcxx_abi`** — the highest `GLIBCXX_*` symbol the C++ stdlib
  *provides*, used to validate the build against runtime ABI.
- **`liburing_pkg` vs. `liburing_so`** — the headers (compile-time)
  and `.so` (runtime) versions are independent axes; record both.

See `scripts/kernel-probe.sh` (upcoming) for the runtime probe that
verifies `IORING_FEAT_*` bits, opcode availability via
`io_uring_get_probe`, and per-flag trial-submit results against the
live kernel.

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
