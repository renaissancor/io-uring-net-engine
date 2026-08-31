#!/usr/bin/env bash
# scripts/kernel-probe.sh — runtime io_uring environment probe
#
# Prints kernel version, io_uring sysctl state, RLIMIT_MEMLOCK / NOFILE,
# liburing version, and a SATISFIED / DEGRADED / NOT SATISFIED verdict
# against the project's 5.19+ baseline.
#
# By default, policy/runtime blocks that return EPERM/EACCES/ENOSYS are
# reported as DEGRADED (exit 0) so constrained dev environments can still
# compile and run non-io_uring tests. Use --strict to make those conditions
# fatal (exit 1), as CI should.
#
# This is the bash-only first-pass probe. The full three-layer probe
# (io_uring_params.features + io_uring_get_probe + per-flag trial-submit)
# lands when reactor code arrives — see docs/02-build-and-toolchain.md
# § "Three-layer feature detection".

set -euo pipefail

MIN_KERNEL_MAJOR=5
MIN_KERNEL_MINOR=19
MIN_LIBURING="2.5"
STRICT_MODE=0

for arg in "$@"; do
  case "$arg" in
    --strict)
      STRICT_MODE=1
      ;;
    --help|-h)
      echo "Usage: $0 [--strict]"
      echo "  --strict: treat policy/runtime io_uring blocks as fatal"
      exit 0
      ;;
    *)
      echo "kernel-probe.sh: unknown argument '$arg'" >&2
      echo "Usage: $0 [--strict]" >&2
      exit 2
      ;;
  esac
done

version_at_least() {
  # True when $1 >= $2 under dotted semantic-ish version ordering.
  [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n1)" = "$2" ]
}

# Builds and runs a tiny runtime probe against the host kernel.
# Output format: "<rc>|<errno>|<message>"
run_queue_init_probe() {
  local src bin cc_bin cflags libs
  src="$(mktemp /tmp/iouring-net-probe-XXXXXX.c)"
  bin="$(mktemp /tmp/iouring-net-probe-XXXXXX)"
  cc_bin="${CC:-cc}"

  cat >"$src" <<'EOF'
#include <errno.h>
#include <liburing.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    struct io_uring ring;
    const int rc = io_uring_queue_init(8, &ring, 0);
    if (rc < 0) {
        const int err = -rc;
        printf("%d|%d|%s\n", rc, err, strerror(err));
        return 1;
    }
    io_uring_queue_exit(&ring);
    puts("0|0|ok");
    return 0;
}
EOF

  cflags="$(pkg-config --cflags liburing)"
  libs="$(pkg-config --libs liburing)"

  if ! "$cc_bin" "$src" $cflags $libs -o "$bin" >/dev/null 2>&1; then
    rm -f "$src" "$bin"
    echo "compile_error|0|failed to build io_uring probe"
    return 0
  fi

  "$bin" 2>/dev/null || true
  rm -f "$src" "$bin"
}

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

queue_probe="(probe not run)"
queue_rc=""
queue_errno=""
queue_msg=""
if command -v pkg-config >/dev/null && pkg-config --exists liburing && command -v "${CC:-cc}" >/dev/null; then
  queue_probe="$(run_queue_init_probe)"
  queue_rc="$(printf '%s' "$queue_probe" | awk -F'|' '{print $1}')"
  queue_errno="$(printf '%s' "$queue_probe" | awk -F'|' '{print $2}')"
  queue_msg="$(printf '%s' "$queue_probe" | cut -d'|' -f3-)"
  if [ "$queue_rc" = "compile_error" ]; then
    echo "io_uring_queue_init:   compile error (${queue_msg})"
  elif [ "$queue_rc" = "0" ]; then
    echo "io_uring_queue_init:   ok"
  else
    echo "io_uring_queue_init:   rc=${queue_rc} errno=${queue_errno} (${queue_msg})"
  fi
else
  echo "io_uring_queue_init:   (probe skipped: compiler or liburing metadata unavailable)"
fi

echo
echo "Project requirements (kernel ${MIN_KERNEL_MAJOR}.${MIN_KERNEL_MINOR}+ baseline):"
ok=yes
degraded=no
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

if [ "$liburing_v" = "(pkg-config: liburing not found)" ]; then
  echo "  ✗ liburing not found via pkg-config"
  ok=no
elif version_at_least "$liburing_v" "$MIN_LIBURING"; then
  echo "  ✓ liburing ${liburing_v} ≥ ${MIN_LIBURING}"
else
  echo "  ✗ liburing ${liburing_v} < ${MIN_LIBURING}"
  ok=no
fi

if [ -n "$queue_rc" ] && [ "$queue_rc" != "0" ] && [ "$queue_rc" != "compile_error" ]; then
  case "$queue_errno" in
    1|13|38)
      echo "  ! io_uring_queue_init blocked by host policy/runtime (errno=${queue_errno}: ${queue_msg})"
      echo "    This is usually seccomp/container policy or a kernel/runtime that denies io_uring."
      if [ "$STRICT_MODE" -eq 1 ]; then
        ok=no
      else
        degraded=yes
      fi
      ;;
    *)
      echo "  ✗ io_uring_queue_init failed (errno=${queue_errno}: ${queue_msg})"
      ok=no
      ;;
  esac
elif [ "$queue_rc" = "compile_error" ]; then
  echo "  ✗ io_uring_queue_init probe could not be built"
  ok=no
elif [ -n "$queue_rc" ]; then
  echo "  ✓ io_uring_queue_init succeeded"
fi

# Note: 64 MiB is enough for development; stress tests with many fixed
# buffers may need `ulimit -l unlimited` or systemd LimitMEMLOCK=infinity.
if [ "$memlock" != "unlimited" ] && [ "$memlock" -lt 65536 ] 2>/dev/null; then
  echo "  ! RLIMIT_MEMLOCK = ${memlock} KB — low; raise before stress-testing fixed buffers"
fi

if [ "$ok" = "yes" ] && [ "$degraded" = "no" ]; then
  echo
  echo "Result: SATISFIED — environment is good to build and run."
  echo
  echo "Next: cmake --preset default && cmake --build --preset default && ctest --preset default"
  exit 0
elif [ "$ok" = "yes" ] && [ "$degraded" = "yes" ]; then
  echo
  echo "Result: DEGRADED — build/test are available, but io_uring runtime is blocked by host policy."
  echo "        Use '--strict' to treat this as a hard failure."
  exit 0
else
  echo
  echo "Result: NOT SATISFIED — see flagged items above."
  exit 1
fi
