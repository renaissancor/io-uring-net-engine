#pragma once
// check.h
//
// Project-wide fatal-check macro. No <cassert>, no std::abort, no
// exceptions. Expands to a software-breakpoint trap on failure —
// `int 3` on x86 (SIGTRAP) and `brk` on aarch64. The trap is gdb-resumable
// in development and fatal in production (kernel writes a core dump if
// the OS allows it), exactly matching Folly's CHECK / abseil's CHECK /
// Chromium's CHECK semantics.
//
//   LNX_CHECK(cond)  — always-on. Traps in debug AND release. The
//     expression is always evaluated, so wrapping side-effecting calls
//     (e.g. pthread_*) is safe:
//         LNX_CHECK(pthread_mutex_lock(&m) == 0);
//
// The trapping branch is hinted unreachable so the optimizer propagates
// the postcondition (`cond` is true) into subsequent flow analysis —
// this silences -Wnull-dereference / -Wuninitialized warnings on code
// that relies on the check for non-null / initialized guarantees.
// A developer who resumes past the trap in gdb has explicitly broken
// the invariant; UB follow-on is consistent with the "fatal in
// production" half of the contract.

// LNX_TRAP() — arch-aware software breakpoint.
// - Clang: __builtin_debugtrap() emits the right instruction per target.
// - GCC + x86-64 / i386: inline asm `int 3` raises SIGTRAP.
// - GCC + aarch64: `brk #0xf000` raises SIGTRAP.
// - Anything else: fall back to __builtin_trap() (SIGILL; not resumable
//   but always fatal, which is what we ultimately want).
#if defined(__clang__)
    #define LNX_TRAP() __builtin_debugtrap()
#elif defined(__i386__) || defined(__x86_64__)
    #define LNX_TRAP() __asm__ volatile("int3")
#elif defined(__aarch64__)
    #define LNX_TRAP() __asm__ volatile("brk #0xf000")
#else
    #define LNX_TRAP() __builtin_trap()
#endif

#define LNX_CHECK(cond)                                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            LNX_TRAP();                                                        \
            __builtin_unreachable();                                           \
        }                                                                      \
    } while (0)
