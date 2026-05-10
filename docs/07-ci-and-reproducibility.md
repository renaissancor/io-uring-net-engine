# 07 — CI and reproducibility

How CI is shaped and how the build envelope stays *provable* over
time. For language standard / kernel requirements / dependency floors,
see `02-build-and-toolchain.md`. For local dev install, see
`06-system-setup.md`. For CMake-side conventions, see `05-cmake.md`.

---

## CI matrix (target shape)

GitHub Actions, four-job matrix designed to enforce the C++20 floor
and catch portability regressions:

| Job                  | Distro       | Compiler   | Sanitizer  | Purpose                                  |
|----------------------|--------------|------------|------------|------------------------------------------|
| `linux-gcc-floor`    | Ubuntu 22.04 | g++-12     | asan+ubsan | enforces the gcc-12 minimum              |
| `linux-gcc`          | Ubuntu 24.04 | g++-14     | asan+ubsan | primary; full unit + integration         |
| `linux-gcc-tsan`     | Ubuntu 24.04 | g++-14     | tsan       | concurrency tests only (asan/tsan clash) |
| `linux-clang`        | Ubuntu 24.04 | clang++-18 | asan+ubsan | catches portability bugs gcc misses      |

The floor job is the load-bearing one: if a contributor accidentally
uses a C++23 feature, it fails on `linux-gcc-floor` even when noble
builds clean.

Kernel inside the GHA runner is sufficient for unit tests. Integration
tests that exercise multishot accept require a self-hosted runner or a
container with kernel passthrough — out of scope for v0 CI.

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

**`.devcontainer/devcontainer.json`** — VS Code / CLion Remote uses
the same image and env:

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
  rate-limited by `RLIMIT_MEMLOCK` for fixed buffers, or blocked by
  the container runtime's seccomp profile (some Docker / Kubernetes
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
  the binary's actual minimum-required glibc symbol — see section 3.

### 2. Load-bearing CI floor job

Ubuntu 22.04 + g++-12 + FetchContent for everything below floor.
Branch protection requires it to pass. Without this job, "works on
noble" silently rots into "doesn't work on 22.04" because no one
tests the lower bound. The floor job is the difference between
*guessing* the floor still works and *proving* it on every PR.

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

### 3. `version-snapshot.txt`

Every successful build emits this file; CI uploads it as a 90-day
artifact. When "broke between version A and B" reports arrive in
2027+, binary-search the snapshots rather than guess what changed.

CMake writes `build/deps-pins.env` at configure time so the snapshot
script has access to the same tag values defined in `cmake/deps.cmake`
(shell-level `$LIBURING_TAG` would otherwise be unset). See
`05-cmake.md` § "Dependency resolution" for the `configure_file` block.

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

---

See `scripts/kernel-probe.sh` (upcoming) for the runtime probe that
verifies `IORING_FEAT_*` bits, opcode availability via
`io_uring_get_probe`, and per-flag trial-submit results against the
live kernel. The probe is the runtime complement to this section's
build-time envelope.
