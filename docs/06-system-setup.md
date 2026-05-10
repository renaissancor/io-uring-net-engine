# 06 — System setup

How a contributor gets a working dev box from a fresh distro install.
For CI / reproducibility envelope artifacts (Dockerfile, devcontainer,
`setup.sh`, `version-snapshot.txt`), see `07-ci-and-reproducibility.md`.
For CMake-side concerns, see `05-cmake.md`.

---

## Local developer workflow

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

`scripts/kernel-probe.sh` prints `IORING_FEAT_*` flags + opcode
availability + per-flag trial-submit results so contributors know what
their dev kernel supports without reading kernel docs. See
`02-build-and-toolchain.md` § "Three-layer feature detection".

---

## One-shot install (Ubuntu 24.04 / WSL2 — verified 2026-05-10)

Verified working on Ubuntu 24.04.4 LTS (Noble) under WSL2 kernel
6.6.114 on 2026-05-10. Kernel reports `IORING_FEAT == 0x3fff` (bits
0–13), so every multishot/provided-buffer feature this project uses is
available.

```bash
sudo apt update
sudo apt install -y \
  g++-14 clang-18 clang-tidy-18 clang-format-18 lld-18 \
  cmake ninja-build pkg-config \
  liburing-dev catch2 libfmt-dev \
  valgrind cppcheck gdb
```

Verified set after install:

| Tool             | Version | Notes                                                                   |
|------------------|---------|-------------------------------------------------------------------------|
| `g++-14`         | 14.2.0  | C++20; well above the gcc-12 floor                                      |
| `clang++-18`     | 18.1.3  | C++20; well above the clang-14 floor                                    |
| `clang-tidy-18`  | 18.1.3  | invoke as `clang-tidy-18`                                               |
| `clang-format-18`| 18.1.3  | invoke as `clang-format-18`                                             |
| `lld-18`         | 18.1.3  | `-fuse-ld=lld-18` for the release preset                                |
| `cmake`          | 3.28.3  | meets the 3.25+ floor                                                   |
| `ninja`          | 1.11.1  |                                                                         |
| `liburing` / `-dev` | 2.5  | meets the 2.5 floor                                                     |
| `catch2`         | 3.4.0   | `find_package(Catch2 3 REQUIRED)`                                       |
| `libfmt-dev`     | 9.1.0   | `find_package(fmt 10 CONFIG)` — **below floor** (lacks `fmt::println`, added in fmt 10). FetchContent fallback always triggers on noble (see `05-cmake.md`). The apt install is optional; only useful for the `-lfmt` smoke test in this doc. |
| `valgrind`       | 3.22.0  | helgrind / drd for concurrency triage                                   |
| `cppcheck`       | 2.13.0  | secondary static analysis                                               |
| `gdb`            | 15.1    |                                                                         |

`tl::expected` is **not** in apt; vendored via `FetchContent` from the
project root `CMakeLists.txt` — see `05-cmake.md`.

Noble's default `g++` (no version suffix) resolves to **g++-13**, which
also passes the C++20 floor. To use g++-14 explicitly: invoke `g++-14`,
set `CXX=g++-14`, or pass `-DCMAKE_CXX_COMPILER=g++-14` to CMake. Same
applies to versioned clang binaries.

---

## Cross-distro install model: system core + vendored deps

Stock distro packages for `liburing`, `{fmt}`, `Catch2`, and `cmake`
are **below this project's floors on most non-noble distros**. The
project therefore splits its dependencies in two:

- **System packages** — compiler, build tools, debug tools. Always
  installed via the distro package manager.
- **Project dependencies with version floors** — `liburing` ≥ 2.5,
  `{fmt}` ≥ 10, `Catch2` ≥ 3.4, `tl::expected`. Sourced via
  `FetchContent` from the root `CMakeLists.txt`. Optionally satisfied
  by system packages when the distro version meets the floor;
  `find_package(... CONFIG)` is tried first, `FetchContent` is the
  fallback. See `05-cmake.md` for the resolution logic.

This means **the install command changes by distro, but the build
recipe doesn't** — `cmake --preset default && cmake --build --preset
default` works identically on all supported distros.

### System packages by distro

| Distro       | One-shot system install                                                                                            |
|--------------|--------------------------------------------------------------------------------------------------------------------|
| Ubuntu 24.04 | `apt install -y g++-14 clang-18 clang-tidy-18 clang-format-18 lld-18 cmake ninja-build pkg-config valgrind cppcheck gdb` |
| Ubuntu 22.04 | `apt install -y g++-12 clang-15 cmake ninja-build pkg-config valgrind cppcheck gdb` (cmake from Kitware repo — see `07-ci-and-reproducibility.md` § setup.sh) |
| Debian 12    | `apt install -y g++ clang cmake ninja-build pkg-config valgrind cppcheck gdb`                                        |
| Fedora 38+   | `dnf install -y gcc-c++ clang clang-tools-extra cmake ninja-build pkg-config valgrind cppcheck gdb`                  |
| RHEL/Alma 9  | `dnf install -y gcc-toolset-12 ninja-build pkg-config python3-pip` + `pip3 install 'cmake>=3.25'` (enable SCL with `scl enable gcc-toolset-12 -- bash`) |

For an automated runner that handles all of the above, see
`scripts/setup.sh` in `07-ci-and-reproducibility.md`.

### What the distro gives you for liburing / fmt / Catch2 / cmake

| Distro       | liburing | {fmt} | Catch2 | cmake |
|--------------|----------|-------|--------|-------|
| Ubuntu 24.04 | 2.5 ✓    | 9.1 ✗ | 3.4 ✓  | 3.28 ✓ |
| Ubuntu 22.04 | 2.1 ✗    | 8.1 ✗ | 2.13 ✗ | 3.22 ✗ |
| Debian 12    | 2.3 ✗    | 9.1 ✗ | 3.4 ✓  | 3.25 ✓ |
| Fedora 38    | 2.3 ✗    | 9.1 ✗ | 3.3 ✗  | 3.27 ✓ |
| Fedora 39+   | 2.5 ✓    | 10.x ✓ | 3.4 ✓ | 3.27 ✓ |
| RHEL/Alma 9  | 2.1 ✗    | 9.1 ✗ | — ✗    | 3.20 ✗ |

`{fmt}` is below floor on every distro except Fedora 39+, so the
`FetchContent` fallback is the canonical path. This is by design —
the project's floor is fmt 10 (for `fmt::println`, added in fmt
10.0). The apt `libfmt-dev` is still worth installing because
clang-tidy/cppcheck use the headers for source analysis even when
the build links a vendored fmt.

Cells marked ✗ trigger the `FetchContent` fallback automatically; no
manual action is needed beyond running the system install command.

---

## Kernel and io_uring sanity check

```bash
uname -r                                # expect 5.19+; WSL2 default is 6.6
cat /proc/sys/kernel/io_uring_disabled  # must be 0
```

`scripts/kernel-probe.sh` (TBD) prints the runtime probe output —
`IORING_FEAT_*` bits, `io_uring_get_probe()` opcode bitmap, and
per-flag trial-submit results. See `02-build-and-toolchain.md` §
"Three-layer feature detection" for the expected output shape.

---

## Optional / deferred

- **`libc++-18-dev`** — required only if a contributor explicitly
  builds with `-stdlib=libc++`. The default clang-18 build uses
  libstdc++ from system gcc. Not required for the matrix above.
- **clang-19+** — not in noble. Available via the LLVM apt repo
  (`https://apt.llvm.org/`). Useful but not required.
- **`nanobench`** — not in apt. Vendored via `FetchContent` once
  `benchmarks/` exists. Not blocking v1.

---

## Smoke tests

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
