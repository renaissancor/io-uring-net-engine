#!/usr/bin/env bash
# scripts/kernel-probe.sh — runtime io_uring environment probe
#
# Prints kernel version, io_uring sysctl state, RLIMIT_MEMLOCK / NOFILE,
# liburing version, and a SATISFIED / NOT SATISFIED verdict against the
# project's 5.19+ baseline.
#
# This is the bash-only first-pass probe. The full three-layer probe
# (io_uring_params.features + io_uring_get_probe + per-flag trial-submit)
# lands when reactor code arrives — see docs/02-build-and-toolchain.md
# § "Three-layer feature detection".

set -euo pipefail

MIN_KERNEL_MAJOR=5
MIN_KERNEL_MINOR=19
MIN_LIBURING="2.5"

echo "=== iouring-net-lib kernel + runtime probe ==="
kernel=$(uname -r)
echo "kernel:                $kernel"

# Strip any -<distro> / + suffixes when comparing major.minor.
read -r kmaj kmin <<< "$(echo "$kernel" | awk -F'[.-]' '{print $1, $2}')"
if (( kmaj > MIN_KERNEL_MAJOR )) || \
   ( (( kmaj == MIN_KERNEL_MAJOR )) && (( kmin >= MIN_KERNEL_MINOR )) ); then
  kernel_ok=yes
else
  kernel_ok=no
fi

if [ -r /proc/sys/kernel/io_uring_disabled ]; then
  io_uring_disabled=$(cat /proc/sys/kernel/io_uring_disabled)
else
  io_uring_disabled="(sysctl missing)"
fi
echo "io_uring_disabled:     $io_uring_disabled"

memlock=$(ulimit -l)
echo "RLIMIT_MEMLOCK (KB):   $memlock"
nofile=$(ulimit -n)
echo "RLIMIT_NOFILE:         $nofile"

if command -v pkg-config >/dev/null && pkg-config --exists liburing; then
  liburing_v=$(pkg-config --modversion liburing)
else
  liburing_v="(pkg-config: liburing not found)"
fi
echo "liburing-dev:          $liburing_v"

echo
echo "Project requirements (kernel ${MIN_KERNEL_MAJOR}.${MIN_KERNEL_MINOR}+ baseline):"
ok=yes
if [ "$kernel_ok" != "yes" ]; then
  echo "  ✗ kernel $kernel < ${MIN_KERNEL_MAJOR}.${MIN_KERNEL_MINOR}"
  ok=no
else
  echo "  ✓ kernel $kernel ≥ ${MIN_KERNEL_MAJOR}.${MIN_KERNEL_MINOR}"
fi

if [ "$io_uring_disabled" = "1" ]; then
  echo "  ✗ /proc/sys/kernel/io_uring_disabled = 1 (host policy blocks io_uring)"
  ok=no
elif [ "$io_uring_disabled" = "0" ]; then
  echo "  ✓ io_uring not disabled by host policy"
fi

# Note: 64 MiB is enough for development; stress tests with many fixed
# buffers may need `ulimit -l unlimited` or systemd LimitMEMLOCK=infinity.
if [ "$memlock" != "unlimited" ] && [ "$memlock" -lt 65536 ] 2>/dev/null; then
  echo "  ! RLIMIT_MEMLOCK = ${memlock} KB — low; raise before stress-testing fixed buffers"
fi

if [ "$ok" = "yes" ]; then
  echo
  echo "Result: SATISFIED — environment is good to build and run."
  echo
  echo "Next: cmake --preset default && cmake --build --preset default && ctest --preset default"
  exit 0
else
  echo
  echo "Result: NOT SATISFIED — see flagged items above."
  exit 1
fi
