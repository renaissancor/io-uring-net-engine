# Fatal check macros (`LNX_CHECK`, `LNX_DCHECK`)

## Purpose

Project-wide fatal-check macros for invariant violations. Used by every
primitive in `src/sync/` and `src/runtime/` to catch programming errors at
the misuse site rather than letting them propagate, throw, or silently
corrupt state. Deliberately replaces `<cassert>` / `std::abort` /
`std::terminate` / `std::system_error` so the primitive layer stays free
of the C++ exception runtime.

## Reference origin

Project-original. Not a port of Win::* code; Win::Mutex and Win::Thread
both rely on `std::terminate` and `std::runtime_error`, which the project
intentionally bypasses. The two-macro split and the `int 3` trap choice
follow industry convention:

- **Folly** (`folly/lang/CheckedMath.h`, `folly/lang/SafeAssert.h`):
  `CHECK_*` always-on, `DCHECK_*` debug-only.
- **abseil** (`absl/log/check.h`): `CHECK`, `DCHECK`, `CHECK_OK` family
  with the same split.
- **Chromium** (`base/check.h`): `CHECK`, `DCHECK`.
- **Linux kernel** (`include/asm-generic/bug.h`): `BUG_ON`, `WARN_ON` —
  similar shape, kernel-specific death path.

All four use `int 3` (SIGTRAP) over `ud2` (SIGILL) for the same reason
this project does: it is gdb-resumable in development and equally fatal
in production.

## Public surface

```cpp
// src/check.h
#define LNX_TRAP()        // arch-aware software breakpoint
#define LNX_CHECK(cond)   // always-on
#define LNX_DCHECK(cond)  // debug-only (compiles to ((void)0) under NDEBUG)
```

Both `LNX_CHECK` and `LNX_DCHECK` take a single condition expression.
On failure, control transfers to `LNX_TRAP()` which raises a software
breakpoint. The condition is evaluated even in release for `LNX_CHECK`;
it is NOT evaluated in release for `LNX_DCHECK` (the entire macro becomes
`((void)0)`).

## Linux design

**Trap instruction: `int 3` (SIGTRAP), not `ud2` (SIGILL).**

The two trap instructions both halt the process when no debugger is
attached, but they differ in development:

| | `int 3` / SIGTRAP | `ud2` / SIGILL (`__builtin_trap`) |
|---|---|---|
| Without debugger | Process dies, core dumped at fault site | Process dies, core dumped at fault site |
| With gdb attached | gdb stops at the trap; `continue` resumes | gdb cannot continue past it |
| Sanitizer compat (ASan/UBSan/TSan) | Clean termination, sanitizer report intact | Same |
| Post-mortem core dump | Stack intact at trap site | Stack intact at trap site |
| Spurious "patch and continue" | Possible but discouraged | Impossible |

`int 3` is the strictly more useful choice in development. Production
behavior is identical (SIGTRAP also dies without a handler). This matches
Folly / abseil / Chromium.

**Cross-compiler implementation.**

`__builtin_debugtrap()` exists only on Clang. GCC 12+ (the project's
floor) does not provide it. The portable approach is inline asm:

```cpp
#if defined(__clang__)
    #define LNX_TRAP() __builtin_debugtrap()
#elif defined(__i386__) || defined(__x86_64__)
    #define LNX_TRAP() __asm__ volatile("int3")
#elif defined(__aarch64__)
    #define LNX_TRAP() __asm__ volatile("brk #0xf000")
#else
    #define LNX_TRAP() __builtin_trap()
#endif
```

`int 3` on x86 is a single-byte instruction (`0xCC`); `brk #imm16` on
aarch64 takes a 16-bit immediate operand which Linux's signal handler
maps to `SIGTRAP`. The fallback `__builtin_trap()` produces a non-
resumable trap (SIGILL on most targets) — correct but less developer-
friendly. Project targets are Linux x86-64 and aarch64 in practice.

**Cold path vs hot path: `LNX_CHECK` vs `LNX_DCHECK`.**

| Macro | Release cost | Use for | Used by |
|---|---|---|---|
| `LNX_CHECK(cond)` | One branch | Cold-path invariants. The release-build branch cost is negligible because the operation itself is microsecond-scale or rarer. | `lnx::thread` (create/join/detach); future resource acquire/release paths |
| `LNX_DCHECK(cond)` | Zero | Hot-path correctness checks. The release-build branch would be measurable in tight loops. | `lnx::mutex` (lock/unlock); future `lnx::atomic` argument checks; future per-allocation memory-pool checks |

The split is identical to Folly / abseil / Chromium. The convention is:
default to `LNX_DCHECK` for anything called more than ~1k times per
second; default to `LNX_CHECK` for anything called less. When in doubt,
benchmark and decide.

**No exception runtime, no `std::` dependency.**

The trap goes through the kernel signal handler, not the C++ ABI. There
is no stack unwinding, no destructor cascade, no `std::terminate_handler`,
no `__cxa_throw`. This keeps the primitive layer compileable without
`-fexceptions` and keeps the binary footprint small. It is also strictly
better for debugging: `std::terminate` would unwind the stack, obscuring
the misuse site in core dumps. The software breakpoint halts at exactly
the offending operation with the stack intact.

## Concurrency & ownership

N/A — these are macros with no state. Thread-safe by definition.

## Core dump behavior

When the trap fires and the OS is configured to allow core dumps, the
kernel writes a dump containing the failing stack. On modern Linux
distributions:

- `ulimit -c unlimited` enables dumps for the current shell.
- `systemd-coredump` (default on Ubuntu/Fedora/Arch) collects dumps under
  `/var/lib/systemd/coredump/`.
- `coredumpctl list` shows recent dumps; `coredumpctl debug` opens gdb on
  the most recent.

Because the trap site is the top of the dumped stack — `lnx::thread::join`
or wherever the misuse happened — post-mortem analysis points directly
at the offending operation without any `std::terminate` /
`abort_message` frames in the way.

Richer crash reporting (custom signal handlers writing backtraces, remote
upload, Breakpad/Crashpad integration) is out of scope for `check.h`.
That belongs in a separate `src/runtime/crash_handler.h` when the server
needs it.

## Test plan

Trap paths cannot be exercised by ordinary unit tests — the process dies.
Verification is by inspection plus the fact that all callers compile
under both NDEBUG and !NDEBUG. A future Catch2 death-test harness (using
`fork` + `WTERMSIG` parsing) could trigger and assert specific trap
sites, but is not in scope for v1.

## Open questions

1. **Cross-platform.** If the project ever ships on Windows (unlikely
   given the Linux-only scope), the `LNX_TRAP()` fallback would extend
   with `__debugbreak()` for MSVC. The project memory commits to
   Linux-only; this is informational only.
2. **`LNX_CHECK_EQ` / `LNX_CHECK_NE` / etc.** Folly and abseil provide
   binary-comparison variants that print both operands on failure
   (`CHECK_EQ(a, b)` reports `a` and `b` when they differ). Useful for
   diagnostics; requires either compile-time formatting or a runtime
   message buffer. Defer until a callsite needs it; bare condition
   checks are sufficient for now.
3. **Crash handler integration.** When a real reactor process lands, a
   `src/runtime/crash_handler.h` would install a `SIGTRAP` / `SIGILL` /
   `SIGSEGV` handler that writes a one-line summary (thread name, kernel
   TID, timestamp, brief backtrace) before re-raising. Tracked here as a
   future cross-reference; do not add until the server needs it.
