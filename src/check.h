#pragma once
// check.h
//
// Project-wide fatal-check macros. No <cassert>, no std::abort, no
// exceptions. Both expand to a software-breakpoint trap on failure —
// `int 3` on x86 (SIGTRAP) and `brk` on aarch64. The trap is gdb-resumable
// in development and fatal in production (kernel writes a core dump if
// the OS allows it), exactly matching Folly's CHECK / abseil's CHECK /
// Chromium's CHECK semantics.
//
//   LNX_CHECK(cond)  — always-on. Traps in debug AND release. Use for
//     invariants that must hold in production. Right for cold-path
//     correctness checks where one extra branch is negligible
//     (thread create/join/detach, resource-acquire failure, etc.).
//
//   LNX_DCHECK(cond) — debug-only. Traps under !NDEBUG; compiles to
//     `((void)0)` under NDEBUG. Use for hot-path correctness checks
//     where a branch per op would matter (mutex lock/unlock, atomic op
//     fast paths).

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

#define LNX_CHECK(cond)  do { if (!(cond)) LNX_TRAP(); } while (0)

#ifndef NDEBUG
    #define LNX_DCHECK(cond) LNX_CHECK(cond)
#else
    #define LNX_DCHECK(cond) ((void)0)
#endif
