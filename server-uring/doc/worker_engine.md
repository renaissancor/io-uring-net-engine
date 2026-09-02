# worker_engine — TLS-singleton body for the worker role

> **Status:** in-progress
> **Source:** `src/worker_engine.h`, `src/worker_engine.cpp`
> **Namespace:** `app`
> **Depends:** `worker_ctl`, `detail/thread_role`, `thread_ctl` (via `worker_ctl`), `check`, `runtime/thread`

## Purpose

The per-thread engine body behind `worker_ctl`. It is constructed lazily on the
owning worker thread, learns its control block via `attach()`, and runs the tick
loop that honors the `running → draining → stopped` lifecycle. Today the loop is a
skeleton: it bumps a heartbeat and yields until asked to stop, then publishes
`stopped`. No sockets, io_uring, sessions, or mesh traffic are wired (not built).

## API

```cpp
namespace app {

struct worker_ctl;  // fwd-decl: engine knows ctl, not the other way

class worker_engine {
public:
    // TLS-Meyers singleton with role-token guard. Calling from any
    // non-worker thread (role-token != worker) traps LNX_CHECK BEFORE
    // the static thread_local body is constructed.
    static worker_engine& instance() noexcept;

    // Post-construction wiring: engine learns its ctl.
    // LNX_CHECKs: no-double-attach + no-null-attach.
    void attach(worker_ctl* h) noexcept;

    // Tick loop. Honors the three-state lifecycle:
    //   running → draining → stopped.
    // The worker thread (and ONLY the worker thread) calls this; on
    // exit, it self-publishes state==stopped via release-store.
    // LNX_CHECKs: attach() must have happened first.
    void run_loop() noexcept;

    worker_engine(const worker_engine&)            = delete;
    worker_engine& operator=(const worker_engine&) = delete;
    worker_engine(worker_engine&&)                 = delete;
    worker_engine& operator=(worker_engine&&)      = delete;

private:
    worker_engine() noexcept;
    ~worker_engine() noexcept;

    worker_ctl* _ctl = nullptr;   // plain ptr — single-owner
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
  `detail::tls_role == thread_role::worker` before the `static thread_local`
  is first referenced, so a wrong-thread call traps without constructing a body.
- **One body per thread:** the singleton is `thread_local`; every worker thread
  has its own instance and no other thread can reach it.
- **Attach exactly once:** `_ctl` goes from null to non-null once. A second
  `attach()` or a null pointer traps.
- **Only the engine writes `stopped`:** `run_loop()` is the sole writer of
  `state::stopped` on the normal path. The trampoline in `worker_ctl::entry`
  writes it only on the "stop requested before running" short-circuit, where
  `run_loop()` is never entered.
- **Loop exit is stop-driven:** the loop leaves only when `_state` is no longer
  `running`. The only transition out of `running` is `request_stop()`'s CAS to
  `draining`, so exit implies a stop was requested.
- **Constructor and destructor are empty.** The ctor allocates nothing; the dtor
  runs at thread exit as part of TLS teardown.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `instance()` from a thread whose role token is not `worker` | `LNX_CHECK` trap before construction |
| `attach()` when `_ctl` already set | `LNX_CHECK` trap |
| `attach(nullptr)` | `LNX_CHECK` trap |
| `run_loop()` before `attach()` | `LNX_CHECK` trap |
| `request_stop()` arrives before `run_loop()` starts | loop body never executes; falls straight through to the `stopped` store |
| State is never moved off `running` | loop spins forever (yielding); there is no timeout |

## Notes

- The engine never touches `worker_ctl::_from_acceptor` / `_to_acceptor`; the
  mesh edges are wired by the supervisor and checked in `worker_ctl::start()`,
  but nothing reads them yet (not built).
- Because the singleton lives for the thread's lifetime and `attach()` refuses a
  second call, one OS thread cannot serve two `worker_ctl`s in sequence. Each
  `worker_ctl::start()` spawns a fresh thread, so this is not reached in practice.
- The heartbeat is incremented but nothing reads it today (no watchdog is built).
- Yielding rather than spinning keeps an idle skeleton from pinning a core; this
  is what makes the test suite cheap under TSan.

## Test plan

`tests/worker_ctl_skeleton_test.cpp` (exercises the engine through `worker_ctl::entry`):
- "worker_ctl spawns, runs, drains, stops clean" — constructs the singleton on the worker thread, attaches, runs the loop, exits on `request_stop`, publishes `stopped`
- "worker_ctl::request_stop is idempotent" — repeated stop requests; loop exits once and publishes `stopped` once
- "request_stop on starting ctl transitions to draining; worker short-circuits to stopped" — singleton constructed and attached, but `run_loop()` is skipped; the trampoline publishes `stopped`

No test calls `instance()`, `attach()`, or `run_loop()` directly, and none covers the wrong-thread trap.
