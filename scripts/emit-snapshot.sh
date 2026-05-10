#!/usr/bin/env bash
# scripts/emit-snapshot.sh — emit version-snapshot.txt for bisection trail
#
# Records every axis of the build environment (kernel, glibc, libstdc++
# ABI, liburing pkg vs .so, fmt, Catch2, gcc, cmake, ninja). CI uploads
# the result with 90-day retention.
#
# Usage:
#   scripts/emit-snapshot.sh [output_path]
# Defaults:
#   BUILD_DIR=build, output_path=$BUILD_DIR/version-snapshot.txt
#
# Sources $BUILD_DIR/deps-pins.env if present (written by cmake/deps.cmake)
# so the FetchContent tags are recoverable from the snapshot.
#
# See docs/07-ci-and-reproducibility.md § 3 for the field-by-field rationale.

set -euo pipefail

build="${BUILD_DIR:-build}"
out="${1:-${build}/version-snapshot.txt}"
test_bin="${build}/tests/iouring_net-test"

mkdir -p "$(dirname "$out")"

# shellcheck disable=SC1091
[ -f "${build}/deps-pins.env" ] && . "${build}/deps-pins.env"

# Highest GLIBCXX ABI symbol shipped by the active libstdc++ (not the first).
libstdcxx_so=$(g++ -print-file-name=libstdc++.so 2>/dev/null || true)
if [ -n "$libstdcxx_so" ] && [ -f "$libstdcxx_so" ]; then
  libstdcxx_abi=$(strings "$libstdcxx_so" \
                  | grep -oE 'GLIBCXX_[0-9.]+' | sort -V | tail -1)
else
  libstdcxx_abi=unknown
fi

# Runtime glibc floor encoded in the test binary, if it exists yet.
if [ -x "$test_bin" ]; then
  min_glibc=$(objdump -T "$test_bin" 2>/dev/null \
              | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1 || echo unknown)
else
  min_glibc=unknown
fi

# stdlib detection — only meaningful for clang builds with -stdlib=libc++.
stdlib=libstdc++
if g++ -x c++ -E -include cxxabi.h -dM /dev/null 2>/dev/null \
     | grep -q _LIBCPP_VERSION; then
  stdlib=libc++
fi

{
  echo "host_kernel=$(uname -r)"                      # io_uring ABI surface
  echo "build_glibc=$(ldd --version | head -1 | awk '{print $NF}')"
  echo "min_glibc=${min_glibc}"                       # forward-compat floor
  echo "libstdcxx=$(g++ -dumpversion) abi=${libstdcxx_abi}"
  if command -v clang++ >/dev/null; then
    echo "clang=$(clang++ --version | head -1)"
  fi
  echo "stdlib=${stdlib}"
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
  echo "snapshot_emitted=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$out"

echo "emit-snapshot: wrote $out"
