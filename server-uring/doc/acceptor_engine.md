# acceptor_engine — TLS-singleton body for the acceptor role

> **Status:** in-progress
> **Source:** `src/acceptor_engine.h`, `src/acceptor_engine.cpp`
> **Namespace:** `app`
> **Depends:** `acceptor_ctl`, `detail/thread_role`, `thread_ctl` (via `acceptor_ctl`), `check`, `runtime/thread`

## Purpose

The per-thread engine body behind `acceptor_ctl`, mirroring `worker_engine`. It
is constructed lazily on the acceptor thread, learns its control block via
`attach()`, and runs the tick loop that honors `running → draining → stopped`.
Today the loop is a skeleton: heartbeat and yield until stop is requested, then
publish `stopped`. No listen socket, accept path, io_uring, or handoff exists
(not built).

## API

```cpp
namespace app {

struct acceptor_ctl;  // fwd-decl: engine knows ctl, not the other way

class acceptor_engine {
public:
    // TLS-Meyers singleton with role-token guard. Calling from any
    // non-acceptor thread (role-token != acceptor) traps LNX_CHECK BEFORE
    // the static thread_local body is constructed — same protocol as
    // worker_engine.
    static acceptor_engine& instance() noexcept;

    // Post-construction wiring: engine learns its ctl.
    // LNX_CHECKs: no-double-attach + no-null-attach.
    void attach(acceptor_ctl* h) noexcept;

    // Tick loop. Honors the three-state lifecycle:
    //   running -> draining -> stopped.
    // The acceptor thread (and ONLY the acceptor thread) calls this; on
    // exit, it self-publishes state==stopped via release-store.
    // LNX_CHECKs: attach() must have happened first.
    void run_loop() noexcept;

    acceptor_engine(const acceptor_engine&)            = delete;
    acceptor_engine& operator=(const acceptor_engine&) = delete;
    acceptor_engine(acceptor_engine&&)                 = delete;
    acceptor_engine& operator=(acceptor_engine&&)      = delete;

private:
    acceptor_engine() noexcept;
    ~acceptor_engine() noexcept;

    acceptor_ctl* _ctl = nullptr;   // plain ptr — single-owner
};

}  // namespace app
```

`run_loop()` as built, per iteration:

1. Acquire-load `_ctl->base._state`; exit the loop if it is not `running`.
2. `fetch_add(1)` on `_ctl->base._heartbeat_seq`.
3. `lnx::this_thread::yield()`.

After the loop exits there is no drain work, and the engine release-stores
`state::stopped` into `_ctl->base._state`.

## Invariants

- **Role gate before construction:** `instance()` checks
  `detail::tls_role == thread_role::acceptor` before the `static thread_local`
  is first referenced, so a wrong-thread call traps without constructing a body.
- **One body per thread:** the singleton is `thread_local`. The process has one
  acceptor thread, so there is exactly one live instance.
- **Attach exactly once:** `_ctl` goes from null to non-null once. A second
  `attach()` or a null pointer traps.
- **Only the engine writes `stopped`** on the normal path. `acceptor_ctl::entry`
  writes it only on the "stop requested before running" short-circuit, where
  `run_loop()` is never entered.
- **Loop exit is stop-driven:** the loop leaves only when `_state` is no longer
  `running`, and the only such transition is `request_stop()`'s CAS to `draining`.
- **Constructor and destructor are empty.** Nothing is allocated or opened.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `instance()` from a thread whose role token is not `acceptor` | `LNX_CHECK` trap before construction |
| `attach()` when `_ctl` already set | `LNX_CHECK` trap |
| `attach(nullptr)` | `LNX_CHECK` trap |
| `run_loop()` before `attach()` | `LNX_CHECK` trap |
| `request_stop()` arrives before `run_loop()` starts | loop body never executes; falls straight through to the `stopped` store |
| State is never moved off `running` | loop spins forever (yielding); there is no timeout |

## Notes

- The engine never touches `acceptor_ctl::_to_worker[]` / `_from_worker[]`. The
  hub edges are wired by the supervisor and every slot is checked non-null in
  `acceptor_ctl::start()`, but nothing posts to or reads from them yet (not built).
- The heartbeat is incremented but nothing reads it today.
- The header and source are a near-verbatim copy of `worker_engine` with the role
  token and ctl type swapped. Any change to the lifecycle protocol must be made
  in both.

## Test plan

No dedicated test. `worker_ctl_skeleton_test.cpp` covers the identical protocol
for the worker role only; nothing spawns an `acceptor_ctl` under test.
