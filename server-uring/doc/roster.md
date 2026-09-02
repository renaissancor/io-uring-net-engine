# roster — compile-time manifest of every thread in the process

> **Status:** landed
> **Source:** `src/roster.h`
> **Namespace:** `app::roster`
> **Depends:** `types`

## Purpose

Fixes the thread set before runtime: one supervisor, one acceptor, and
`k_worker_count` workers, where the worker count is a build-time constant taken
from the `IOURING_NET_WORKER_COUNT` preprocessor definition. Everything sized by
the roster (mesh edge arrays, the worker table) is exact-sized at compile time.

## API

```cpp
namespace app::roster {

#ifndef IOURING_NET_WORKER_COUNT
#define IOURING_NET_WORKER_COUNT 1
#endif

// Worker pool size. Set at configure time via -DIOURING_NET_WORKER_COUNT.
inline constexpr i32 k_worker_count = IOURING_NET_WORKER_COUNT;

static_assert(k_worker_count >= 1,
              "the roster needs at least one worker — nothing owns session fds otherwise");
static_assert(k_worker_count <= 8,
              "worker thread names are worker_0..worker_7 (8 chars, matching "
              "\"acceptor\" / \"database\"); beyond 8, scale horizontally by process");

// Mesh edges implied by the roster: one admission pipe + one close-notify pipe
// per worker.
inline constexpr i32 k_mesh_edge_count = 2 * k_worker_count;

// Every thread in the process: supervisor + acceptor + the worker pool.
inline constexpr i32 k_thread_count = 1 + 1 + k_worker_count;

}  // namespace app::roster
```

Roster as built:

| Role | Count | Named in |
|---|---|---|
| supervisor | 1 | `main()` itself; never runs an engine |
| acceptor | 1 | `acceptor_ctl`, thread name `acceptor` |
| worker | `k_worker_count` (1..8) | `worker_ctl`, thread names `worker_0`..`worker_7` |
| db, logger | 0 | deferred; not built |

## Invariants

- **Fixed, not bounded.** The worker count is a `constexpr`; no code path decides
  at runtime how many threads or edges exist.
- **Range 1..8** enforced by `static_assert`. The upper bound exists so every
  worker name is exactly 8 characters and a single decimal digit indexes it.
- **Worker ids are roster indices.** `worker_ctl`'s constructor traps on
  `id < 0 || id >= k_worker_count`; an id outside the roster names a thread that
  does not exist in this binary.
- **One definition per build.** CMake sets `IOURING_NET_WORKER_COUNT` as a
  `PUBLIC` compile definition on `uring_server_core`, so the server binary and
  the test suite compile against the same roster.
- **Scale-out is by process.** A different worker count is a different binary,
  configured with `-DIOURING_NET_WORKER_COUNT=N`.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `IOURING_NET_WORKER_COUNT` undefined | header defaults it to `1` |
| `IOURING_NET_WORKER_COUNT` < 1 or > 8 | compile error (`static_assert`) |
| `worker_ctl{id, cfg}` with `id >= k_worker_count` | `LNX_CHECK` trap in the ctl constructor |
| Roster large enough that main's pipe arrays exceed 1 MiB | compile error from the `static_assert` in `main.cpp` |

## Notes

- Consumers of `k_worker_count` today: `main.cpp` (pipe arrays, worker table,
  loop bounds, stack-budget `static_assert`), `acceptor_ctl` (edge pointer arrays
  and `install_pipes` array-reference parameters), `worker_ctl.cpp` (id guard).
- `k_mesh_edge_count` and `k_thread_count` are defined but not referenced by any
  source or test. The header's comment that the edge count is asserted against
  `main.cpp` describes an intent; no such assert exists in the code.
- The CMake cache variable `IOURING_NET_WORKER_COUNT` defaults to `"1"` and is
  the only supported way to change the roster.

## Test plan

No dedicated test. `tests/worker_ctl_skeleton_test.cpp` constructs every
`worker_ctl` with id 0, which relies on `k_worker_count >= 1` to pass the ctl's
roster-index guard; nothing tests the guard's failure side or the other constants.
