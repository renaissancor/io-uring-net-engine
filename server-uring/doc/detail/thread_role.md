# detail/thread_role — per-thread role token gating engine construction

> **Status:** landed
> **Source:** `src/detail/thread_role.h`, `src/detail/thread_role.cpp`
> **Namespace:** `app::detail`
> **Depends:** `types`

## Purpose

A `thread_local` enum that records which role the current thread was launched
as. Each role's entry trampoline writes it as its first statement; each
engine's `instance()` reads it before touching its Meyers `static thread_local`
body, so a call from the wrong thread traps instead of silently constructing a
second engine.

## API

```cpp
namespace app::detail {

enum class thread_role : i32 {
    none       = 0,   // zero-init on every thread that did not go through a trampoline
    worker     = 1,
    db         = 2,
    supervisor = 3,
    acceptor   = 4,
};

extern thread_local thread_role tls_role;

}  // namespace app::detail
```

Definition (`thread_role.cpp`):

```cpp
thread_local thread_role tls_role = thread_role::none;
```

## Invariants

- **Default is `none`:** any thread that did not run one of the trampolines
  below reads `tls_role == thread_role::none`. Test threads and the Catch2 main
  thread are in this set.
- **Installed first, before any TLS singleton:** the three writers are
  `worker_ctl::entry` (`worker`), `acceptor_ctl::entry` (`acceptor`), and
  `main()` (`supervisor`). In each, the assignment precedes
  `pthread_setname_np`, the kernel-tid publish, and every `instance()` call.
- **Read by the guards:** `worker_engine::instance()` executes
  `LNX_CHECK(tls_role == thread_role::worker)` and
  `acceptor_engine::instance()` executes
  `LNX_CHECK(tls_role == thread_role::acceptor)` before the
  `static thread_local` body is referenced.
- **Write-once in practice:** nothing in the tree writes `tls_role` a second
  time or resets it to `none`; a thread keeps its role for its lifetime.
- **Plain TLS, no atomics:** the variable is only ever read and written by the
  thread that owns it.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `worker_engine::instance()` called on a thread whose role is not `worker` | `LNX_CHECK` trap before construction — no engine body is allocated |
| `acceptor_engine::instance()` called on a thread whose role is not `acceptor` | `LNX_CHECK` trap, same |
| Either `instance()` called on the supervisor thread | trap — `main()` installs `supervisor`, which matches neither guard |
| Trampoline reordered so a singleton is touched before the assignment | trap on that first `instance()` call (the guard sees `none`) |

## Notes

- `thread_role::db` exists as an enumerator but no trampoline installs it and
  no guard checks for it — not built.
- The enum is `i32` rather than `u08` so the TLS slot is a natural word; there
  is no packing concern since it is one value per thread.
- The guard is what makes `static thread_local` engines safe: without it, a
  stray `instance()` on the supervisor would construct a full engine body on
  the root thread. The check runs on every `instance()` call, not only the
  first.

## Test plan

No dedicated test. `tests/worker_ctl_skeleton_test.cpp` spawns a `worker_ctl`
whose trampoline installs `thread_role::worker`, but no test reads or asserts
on `tls_role`.
