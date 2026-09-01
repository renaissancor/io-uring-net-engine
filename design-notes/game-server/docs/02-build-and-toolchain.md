# 02 — Build and toolchain

The product inherits every toolchain decision from the library
verbatim. This document records **only the additions** specific to
the product. For the language-standard / kernel / dependency floor,
read
[`iouring-net-lib/docs/02-build-and-toolchain.md`](../../../engine-uring/doc/02-build-and-toolchain.md)
first — what follows assumes that as the baseline.

---

## Inherited from the library (do not duplicate decisions)

- **C++20 locked.** No C++23 in the public or private surface.
- **Compiler floor:** g++ 12+, clang 14+ (with libstdc++-12+ or libc++-18+).
- **Kernel:** 5.19+ recommended for multishot accept / fixed buffers /
  `IOSQE_BUFFER_SELECT`. 5.10+ minimum.
- **Distros tested:** Ubuntu 22.04 / 24.04, Debian 12, RHEL 9.
- **WSL2:** fine for development, not for performance numbers.
- **Polyfills:** `tl::expected`, `{fmt}` (consumed transitively
  through the library's PUBLIC link).
- **CMake floor:** 3.25.

The product's compiler-warnings, sanitizer presets, and CMake module
path conventions mirror the library's; see [`03-cmake.md`](03-cmake.md).

---

## Additions for the product

### 1. Python 3.10+ (build-time only)

The codegen pipeline (`codegen/rpc_gen.py`,
`codegen/stub_gen.py`, `codegen/proxy_gen.py`) is Python. Required
features:

| Feature                                 | Min version | Used for                                         |
|-----------------------------------------|-------------|--------------------------------------------------|
| `match` statement                       | 3.10        | Type-dispatch in stub/proxy emitters             |
| `tomllib` (no longer needed; we use JSON) | —          | Schema stays JSON for parity with the reference  |
| f-strings with `=` debug                | 3.8         | Diagnostic prints                                |

**Install (apt):** `sudo apt install python3 python3-pip`. The
scripts have no PyPI dependencies in v1; if that changes,
`codegen/requirements.txt` will pin them and CI will install via
`pip install --user -r codegen/requirements.txt`.

**Runtime:** the produced server has **no Python dependency**. The
scripts run once per build to emit `.h` / `.cpp` files that are
compiled in.

### 2. Library install prefix

The product must know where `iouring-net-lib` was installed. Three
ways to tell it:

```bash
# Method 1: CMAKE_PREFIX_PATH
cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/.local

# Method 2: iouring_net_DIR (points at the directory containing
# iouring_netConfig.cmake)
cmake -S . -B build -Diouring_net_DIR=$HOME/.local/lib/cmake/iouring_net

# Method 3: installed to a system prefix CMake searches by default
sudo cmake --install build-lib  # /usr/local
cmake -S . -B build              # picks it up automatically
```

CI uses Method 1 with a workflow-local prefix so the library install
is sandboxed and reproducible.

### 3. Devcontainer / Docker

The product's devcontainer extends the library's. Same base image
(Ubuntu 24.04, pinned by tag in v1, by digest later), plus:

```dockerfile
RUN apt-get update && apt-get install -y --no-install-recommends \
      python3 python3-pip \
 && rm -rf /var/lib/apt/lists/*

# Install the library to /usr/local in the image build, so
# find_package works out of the box.
ARG IOURING_NET_LIB_TAG=v0.0.1
RUN git clone --depth 1 --branch ${IOURING_NET_LIB_TAG} \
      https://example.invalid/iouring-net-lib.git /tmp/lib \
 && cmake -S /tmp/lib -B /tmp/lib/build \
 && cmake --build /tmp/lib/build --target install \
 && rm -rf /tmp/lib
```

The library tag is pinned, not floated — same logic as the
reproducibility envelope in the library's
[`07-ci-and-reproducibility.md`](../../../engine-uring/doc/07-ci-and-reproducibility.md).

### 4. liburing — transitive only

`liburing` is a `PUBLIC` dependency of `iouring_net::iouring_net`. The
product does **not** call `find_package(liburing)` itself, does not
`#include <liburing.h>`, does not link against `liburing::uring`
directly. The library is the only legitimate user of the io_uring API
in this two-repo system.

If a product handler wants something `liburing` exposes that the
library does not, the fix is to expose it from the library.

---

## What the product does NOT introduce

- **No new third-party C++ deps.** If a handler needs JSON parsing,
  use the polyfilled `{fmt}` for output and hand-roll a tiny parser
  for now; do not pull in `nlohmann/json`. (Reassess if more than
  one handler needs structured config.)
- **No new sanitizer beyond library's set.** The product's CI uses
  the same `default` (ASan+UBSan) and `tsan` presets the library
  uses.
- **No new compiler floor.** If the library compiles on g++-12 floor,
  this repo must too. Codegen output must not require anything newer
  than C++20.

---

## Smoke check (will exist once codegen + CMake land)

```bash
# 1. Library installed
cmake --build build-lib --target install
ls $HOME/.local/lib/cmake/iouring_net/iouring_netConfig.cmake   # exists

# 2. Codegen runs
python3 codegen/rpc_gen.py
ls generated/server_stub.h generated/client_proxy.h             # exists

# 3. Product configures
cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/.local
# Expected output: "-- Found iouring_net: ... (found version ...)"

# 4. Product builds + tests
cmake --build build
ctest --test-dir build --output-on-failure
```

Until then this section is the contract the CMake work needs to
satisfy.

---

## Cross-references

- [`iouring-net-lib/docs/02-build-and-toolchain.md`](../../../engine-uring/doc/02-build-and-toolchain.md)
  — authoritative toolchain matrix.
- [`03-cmake.md`](03-cmake.md) — how `find_package` and the codegen
  step are wired into CMake.
- [`05-codegen.md`](05-codegen.md) — what the Python scripts emit.
- [`07-ci.md`](07-ci.md) — CI matrix, including the library-tag pinning.
