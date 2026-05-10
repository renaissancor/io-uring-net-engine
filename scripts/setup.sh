#!/usr/bin/env bash
# scripts/setup.sh — distro-aware one-shot install
#
# This is the convenience runner; the .devcontainer/Dockerfile is the
# binding artifact. Both must produce an identical version-snapshot.txt.
# See docs/07-ci-and-reproducibility.md for the design rationale.
#
# Usage: sudo ./scripts/setup.sh

set -euo pipefail

if [ ! -f /etc/os-release ]; then
  echo "setup.sh: /etc/os-release missing — unsupported platform" >&2
  exit 1
fi

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
  ubuntu:24.04)
    apt-get update
    apt-get install -y \
      g++-14 clang-18 clang-tidy-18 clang-format-18 lld-18 \
      cmake ninja-build pkg-config linux-libc-dev \
      liburing-dev catch2 libfmt-dev \
      valgrind cppcheck gdb
    ;;
  ubuntu:22.04)
    # Stock 22.04 cmake is 3.22, below the 3.25 floor. Pull from
    # Kitware's apt repo before any other install.
    apt-get update
    apt-get install -y wget gnupg ca-certificates
    wget -qO - https://apt.kitware.com/keys/kitware-archive-latest.asc \
      | gpg --dearmor -o /usr/share/keyrings/kitware.gpg
    echo 'deb [signed-by=/usr/share/keyrings/kitware.gpg] https://apt.kitware.com/ubuntu/ jammy main' \
      > /etc/apt/sources.list.d/kitware.list
    apt-get update
    apt-get install -y \
      g++-12 clang-15 cmake ninja-build pkg-config linux-libc-dev \
      valgrind cppcheck gdb
    # liburing/fmt/Catch2 → FetchContent (system versions below floor)
    ;;
  debian:12)
    apt-get update
    apt-get install -y \
      g++ clang cmake ninja-build pkg-config linux-libc-dev \
      valgrind cppcheck gdb
    ;;
  fedora:*)
    dnf install -y \
      gcc-c++ clang clang-tools-extra cmake ninja-build pkg-config \
      kernel-headers valgrind cppcheck gdb
    ;;
  rhel:9|almalinux:9|rocky:9)
    # RHEL 9 ships cmake 3.20; need pip for 3.25+.
    dnf install -y python3-pip gcc-toolset-12 ninja-build pkg-config kernel-headers
    pip3 install --no-cache-dir 'cmake>=3.25'
    ;;
  *)
    echo "setup.sh: unsupported distro: $ID:$VER" >&2
    echo "  Supported: ubuntu 22.04/24.04, debian 12, fedora 38+, rhel/alma/rocky 9" >&2
    exit 1
    ;;
esac

echo
echo "setup.sh: install complete on $ID:$VER"
echo "  next: cmake --preset default && cmake --build --preset default && ctest --preset default"
