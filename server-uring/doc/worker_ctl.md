# worker_ctl — worker-role control block and thread trampoline

> **Status:** landed
> **Source:** `src/worker_ctl.h`, `src/worker_ctl.cpp`
> **Namespace:** `app`
> **Depends:** `thread_ctl`, `config`, `mesh`, `roster`, `detail/thread_role`, `worker_engine`, `check`

## Purpose

The cross-thread surface of one worker: composes `thread_ctl` for lifecycle,
holds a copy of `config`, borrows the worker's two mesh edges from the
supervisor, and owns the pthread trampoline that installs the role token,
publishes identity, constructs the TLS `worker_engine`, and drives its loop.

## API

```cpp
namespace app {

struct worker_ctl {
    worker_ctl(i32 id, const config& cfg) noexcept;
    ~worker_ctl() noexcept = default;

    worker_ctl(const worker_ctl&)            = delete;
    worker_ctl& operator=(const worker_ctl&) = delete;
    worker_ctl(worker_ctl&&)                 = delete;
    worker_ctl& operator=(worker_ctl&&)      = delete;

    // Role-specific lifecycle — start() spawns with worker trampoline.
    void start() noexcept;

    // Install the mesh edges the supervisor (LANDLORD) owns. `in` is the
    // acceptor->worker admission pipe this worker reads; `out` is the
    // worker->acceptor close-notify pipe it writes. Must be called BEFORE
    // start() so the engine sees non-null edges on its first tick — checked
    // there, because skipping it is a silent race rather than a crash.
    void install_pipes(acceptor_to_worker_pipe* in,
                       worker_to_acceptor_pipe* out) noexcept;

    // Forward generic lifecycle to base.
    void request_stop() noexcept;
    void join()         noexcept;

    // Forward observers.
    i32         id()          const noexcept;
    const char* name()        const noexcept;
    state       get_state()   const noexcept;
    bool        is_starting() const noexcept;
    bool        is_running()  const noexcept;
    bool        is_draining() const noexcept;
    bool        is_stopped()  const noexcept;
    i32         kernel_tid()  const noexcept;

    // Data — public for composition.
    thread_ctl base;
    config     _cfg;

    // Mesh edges (supervisor-owned; this ctl only borrows). The engine
    // reads them via the ctl. Null until install_pipes().
    acceptor_to_worker_pipe* _from_acceptor = nullptr;
    worker_to_acceptor_pipe* _to_acceptor   = nullptr;

private:
    static void* entry(void* self) noexcept;   // per-class pthread trampoline
};

}  // namespace app
```

## Invariants

- **Id is a roster index:** the constructor traps unless
  `0 <= id < roster::k_worker_count`. The name is `worker_<id>`, always 8
  characters, matching `acceptor`.
- **Pipes before start:** `start()` traps if either edge is null. The edges
  reach the new thread only through `pthread_create`'s happens-before, so a
  late `install_pipes` would be a data race, which is why it is checked here.
- **Trampoline order is fixed:** (1) `detail::tls_role = worker`,
  (2) `pthread_setname_np`, (3) release-store kernel tid,
  (4) `mem::packet_pool::instance().prewarm()` then
  `worker_engine::instance().attach(this)`, (5) CAS `starting → running`,
  (6) `run_loop()` or the short-circuit. The role token must be first because
  `worker_engine::instance()` traps without it.
- **Stop is never lost:** if the CAS in step 5 observes `draining`, the
  trampoline skips `run_loop` and publishes `stopped` itself; any other losing
  value traps.
- **Engine publishes `stopped`:** on the normal path `run_loop` returns only
  after the engine has stored `stopped`; `join()` then asserts it.
- **No double start:** `start()` move-assigns into `base._thread`, and
  `lnx::thread`'s move-assign traps when the destination is already joinable.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `worker_ctl{id}` with `id < 0` or `id >= roster::k_worker_count` | `LNX_CHECK` trap in constructor |
| `start()` without `install_pipes()` (either pointer null) | `LNX_CHECK` trap before spawning |
| `start()` called twice | `LNX_CHECK` trap inside `lnx::thread` move-assign |
| `request_stop()` before `start()` | state `draining`; thread later short-circuits to `stopped` without running the engine |
| trampoline CAS observes a value other than `starting`/`draining` | `LNX_CHECK` trap |
| `kernel_tid()` right after `is_running()` first reads true | may still be 0 momentarily; callers retry |

## Notes

- `_cfg` is a by-value copy taken at construction; the engine reads it through
  the ctl pointer passed to `attach`.
- The header comment about future stats atomics and peer/db/supervisor inbox
  pointers describes work that is not built; the struct holds only what is
  listed above.
- The forwarding wrappers are one-line inline calls into `base`; the base
  member itself is public, so callers may also reach `base._heartbeat_seq`
  directly.

## Test plan

`tests/worker_ctl_skeleton_test.cpp` (each case wires real pipes, id 0):
- spawns, runs, drains, stops clean: `is_starting` before start, `is_running` and `kernel_tid() > 0` after, `is_stopped` after `request_stop`, `join` succeeds
- `request_stop` is idempotent: three calls on a running worker, one drain, clean join
- `request_stop` on a `starting` ctl: state is `draining` before `start()`; after `start()` the trampoline short-circuits to `stopped`, join succeeds
