# check — fatal-check macro (`LNX_CHECK`)

> **Status:** landed
> **Source:** `src/check.h`
> **Namespace:** — (preprocessor macros)
> **Depends:** none

## Purpose

The project-wide fatal-assertion macro. No `<cassert>`, no `std::abort`, no
exceptions. On a failed condition it executes a software-breakpoint trap —
gdb-resumable in development, fatal in production (the kernel writes a core dump
if allowed) — matching Folly/abseil/Chromium `CHECK` semantics.

## API

```cpp
// Arch-aware software breakpoint (SIGTRAP where resumable, else fatal).
#define LNX_TRAP()      // int3 (x86) / brk (aarch64) / __builtin_debugtrap
                        // (clang) / __builtin_trap (fallback)

// Always-on fatal check — traps in BOTH debug and release. `cond` is always
// evaluated, so wrapping side-effecting calls is safe:
//     LNX_CHECK(pthread_mutex_lock(&m) == 0);
#define LNX_CHECK(cond)
```

There is **only** `LNX_CHECK` — always-on. **No `LNX_DCHECK`** (debug-only
variant) exists in the codebase; hot-path code that "wants a DCHECK" currently
just uses `LNX_CHECK` (see [[mutex]], whose lock/unlock paths trap
unconditionally). If a debug-only tier is ever wanted, it must be added here
first.

## Invariants

- On failure: `LNX_TRAP()` then `__builtin_unreachable()`. The unreachable hint
  lets the optimizer propagate the postcondition (`cond` holds) into later flow
  analysis, silencing `-Wnull-dereference` / `-Wuninitialized` on code that
  relies on the check for a non-null / initialized guarantee.
- Resuming past the trap in gdb is defined as "developer has explicitly broken
  the invariant" — UB follow-on is consistent with the fatal-in-production half
  of the contract.

## Errors & edge cases

- `cond` false → trap (fatal). `cond` true → the `do{}while(0)` body is a no-op.
- The macro is a statement (`do { … } while (0)`), so it composes in `if`/`else`
  without brace surprises.

## Notes

- Trap instruction selection: clang → `__builtin_debugtrap()`; GCC x86 → `int3`;
  GCC aarch64 → `brk #0xf000`; anything else → `__builtin_trap()` (SIGILL, not
  resumable but always fatal).

## Test plan

Exercised indirectly across the suite (every `LNX_CHECK` guard). A death-test of
the trap itself is impractical under Catch2 without a subprocess harness; the
postcondition-propagation behavior is covered by the fact that warning-clean
builds pass on both presets.

## Done when

- [x] Builds on `default` and `floor` presets (warning-clean)
- [x] This spec matches the built macros (no `LNX_DCHECK`)

## Rationale

- `../design-notes/` — no-`std::` primitive ethos (no `<cassert>`/`abort`/exceptions).
- Trap-not-terminate rationale mirrored in [[thread]] (join/detach traps).
