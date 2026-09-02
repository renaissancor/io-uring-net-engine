# acceptor_ctl — acceptor-role control block and mesh hub

> **Status:** landed
> **Source:** `src/acceptor_ctl.h`, `src/acceptor_ctl.cpp`
> **Namespace:** `app`
> **Depends:** `thread_ctl`, `config`, `mesh`, `roster`, `detail/thread_role`, `acceptor_engine`, `check`

## Purpose

The cross-thread surface of the process's single acceptor thread: composes
`thread_ctl` for lifecycle, holds a copy of `config`, borrows one mesh edge
pair per worker from the supervisor, and owns the pthread trampoline that
installs the role token, publishes identity, constructs the TLS
`acceptor_engine`, and drives its loop.

## API

```cpp
namespace app {

struct acceptor_ctl {
    explicit acceptor_ctl(const config& cfg) noexcept;
    ~acceptor_ctl() noexcept = default;

    acceptor_ctl(const acceptor_ctl&)            = delete;
    acceptor_ctl& operator=(const acceptor_ctl&) = delete;
    acceptor_ctl(acceptor_ctl&&)                 = delete;
    acceptor_ctl& operator=(acceptor_ctl&&)      = delete;

    // Role-specific lifecycle — start() spawns with the acceptor trampoline.
    void start() noexcept;

    // Install the mesh edges the supervisor (LANDLORD) owns — the acceptor is
    // the hub, so it holds one edge pair PER WORKER: `out[i]` is the admission
    // pipe it writes toward worker i, `in[i]` the close-notify pipe it reads
    // back from worker i. Array-reference parameters so the roster size is
    // checked at the call site; must be called BEFORE start(), so the engine
    // sees non-null edges on its first tick. start() checks every edge — a
    // partially-wired hub is a silent race, not a crash.
    void install_pipes(acceptor_to_worker_pipe (&out)[roster::k_worker_count],
                       worker_to_acceptor_pipe (&in)[roster::k_worker_count]) noexcept;

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

    // Mesh edges (supervisor-owned; this ctl only borrows). Exact-sized by the
    // roster — no slack slots, and the fan-out loop has a constexpr bound.
    acceptor_to_worker_pipe* _to_worker[roster::k_worker_count]   = {};
    worker_to_acceptor_pipe* _from_worker[roster::k_worker_count] = {};

private:
    static void* entry(void* self) noexcept;   // per-class pthread trampoline
};

}  // namespace app
```

## Invariants

- **Singleton role:** exactly one acceptor per process. `base` is constructed
  with id 0 and name `acceptor`; the id is vestigial, not a roster index.
- **Fully wired before start:** `start()` traps unless all
  `2 * roster::k_worker_count` edge pointers are non-null. The array-reference
  signature of `install_pipes` makes passing a wrong-sized array a compile
  error, so the only way to be partially wired is to skip the call.
- **Trampoline order is fixed:** (1) `detail::tls_role = acceptor`,
  (2) `pthread_setname_np`, (3) release-store kernel tid,
  (4) `mem::packet_pool::instance().prewarm()` then
  `acceptor_engine::instance().attach(this)`, (5) CAS `starting → running`,
  (6) `run_loop()` or the short-circuit. Identical to `worker_ctl` apart from
  the role token and engine type.
- **Stop is never lost:** if the CAS in step 5 observes `draining`, the
  trampoline skips `run_loop` and publishes `stopped` itself; any other losing
  value traps.
- **Engine publishes `stopped`:** on the normal path `run_loop` returns only
  after the engine has stored `stopped`; `join()` then asserts it.
- **No double start:** `start()` move-assigns into `base._thread`, and
  `lnx::thread`'s move-assign traps when the destination is already joinable.
- **Edge indices are worker ids:** `_to_worker[i]` / `_from_worker[i]` pair
  with the `worker_ctl` constructed with id `i`; the supervisor is responsible
  for handing the same array elements to both sides.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `start()` without `install_pipes()` | `LNX_CHECK` trap on the first null edge, before spawning |
| `install_pipes` with arrays not sized `k_worker_count` | compile error (array-reference parameter) |
| `start()` called twice | `LNX_CHECK` trap inside `lnx::thread` move-assign |
| `request_stop()` before `start()` | state `draining`; thread later short-circuits to `stopped` without running the engine |
| trampoline CAS observes a value other than `starting`/`draining` | `LNX_CHECK` trap |
| `kernel_tid()` right after `is_running()` first reads true | may still be 0 momentarily; callers retry |

## Notes

- The acceptor prewarms its own thread-local `mem::packet_pool` in the
  trampoline, the same as each worker.
- `_cfg` is a by-value copy taken at construction; the engine reads it through
  the ctl pointer passed to `attach`.
- The header mentions a listen fd, accept loop, and handoff queue landing with
  the data path; none of that is held by this struct today (not built). The
  struct holds exactly the members listed above.
- Constructed by name in `src/main.cpp`, which also owns the pipe arrays and
  calls `install_pipes` on both the acceptor and every worker before any
  `start()`.

## Test plan

No dedicated test. The trampoline and edge-check logic mirror `worker_ctl`,
whose lifecycle is covered in `tests/worker_ctl_skeleton_test.cpp`; no test
constructs an `acceptor_ctl`.
