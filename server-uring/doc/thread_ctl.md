# thread_ctl — per-thread identity and atomic lifecycle state

> **Status:** landed
> **Source:** `src/thread_ctl.h`, `src/thread_ctl.cpp`
> **Namespace:** `app`
> **Depends:** `runtime/thread`, `sync/atomic`, `check`, `types`

## Purpose

The role-independent part of a thread control block: numeric id, 16-byte
name, atomic four-state lifecycle, cached kernel tid, heartbeat counter, and
the OS thread handle. `worker_ctl` and `acceptor_ctl` compose it as a public
member named `base`; nothing inherits from it.

## API

```cpp
namespace app {

// Four-state lifecycle. `starting` is the ctor default and exists to
// prevent a race between the trampoline's "publish running" store and a
// concurrent request_stop(): if the supervisor called request_stop()
// before the trampoline got there, an unconditional store_release(running)
// would clobber the draining write. The trampoline therefore CAS-promotes
// starting→running; if the CAS loses, the worker exits via the rare
// "stopped during startup" path. The runtime three-state contract
// (running → draining → stopped) is unchanged; `starting` is purely a
// pre-running sentinel.
enum class state : i32 {
    starting = 0,
    running  = 1,
    draining = 2,
    stopped  = 3,
};

struct thread_ctl {
    thread_ctl(i32 id, const char* name) noexcept;
    ~thread_ctl() noexcept = default;

    thread_ctl(const thread_ctl&)            = delete;
    thread_ctl& operator=(const thread_ctl&) = delete;
    thread_ctl(thread_ctl&&)                 = delete;
    thread_ctl& operator=(thread_ctl&&)      = delete;

    // Observers — safe from any thread (atomic snapshots).
    i32         id()          const noexcept;
    const char* name()        const noexcept;
    state       get_state()   const noexcept;   // load_acquire on _state
    bool        is_starting() const noexcept;
    bool        is_running()  const noexcept;
    bool        is_draining() const noexcept;
    bool        is_stopped()  const noexcept;

    // kernel_tid() returns 0 until the worker thread has published its
    // gettid() AND we've observed state==running via acquire-load. The
    // release/acquire pair on _state is what makes the _kernel_tid read
    // synchronized. Calling kernel_tid() during the start()→trampoline
    // race window can legitimately see 0; callers should retry or treat
    // 0 as "not yet known."
    i32         kernel_tid()  const noexcept;

    // Mutators — generic across roles.
    void        request_stop() noexcept;   // CAS starting|running→draining; idempotent
    void        join()         noexcept;   // waits for thread exit + post-asserts stopped

    // Data — public for composition; the role-specific ctl's entry()
    // trampoline writes _name, _kernel_tid, _state directly.
    i32                                _id;
    char                               _name[16];        // pthread_setname_np cap
    lnx::cache_aligned<lnx::atomic32>  _state;           // default = starting (= 0)
    lnx::cache_aligned<lnx::atomic32>  _kernel_tid;      // default = 0
    lnx::cache_aligned<lnx::atomic64>  _heartbeat_seq;   // default = 0
    lnx::thread                        _thread;          // OS handle; supervisor joins
};

}  // namespace app
```

## Invariants

- **Legal transitions only:** `starting → running` (trampoline CAS),
  `starting → draining` and `running → draining` (`request_stop`),
  `draining → stopped` (the owning thread, on loop exit or short-circuit).
  No path goes backwards and nothing but the owning thread writes `stopped`.
- **`request_stop` is idempotent and race-free:** it CAS-loops while the
  observed state is `starting` or `running`; once `draining` or `stopped` is
  observed it returns without writing.
- **Kernel tid is published-before-running:** the trampoline stores
  `_kernel_tid` with release, then CAS-promotes `_state` with the same release
  ordering. `kernel_tid()` acquire-loads `_state` first and returns 0 unless it
  sees `running`, so a nonzero return is always the real tid.
- **Name is bounded:** the constructor copies at most 15 bytes plus NUL; a null
  `name` yields the empty string. Role ctls overwrite `_name` in their own
  constructors.
- **Each atomic sits on its own cache line** (`lnx::cache_aligned`), so
  supervisor polling of `_state` does not contend with the owner's
  `_heartbeat_seq` writes.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `request_stop` before `start()` (state `starting`) | state becomes `draining` synchronously; the trampoline later short-circuits to `stopped` |
| `request_stop` called repeatedly | first CAS wins; later calls observe `draining`/`stopped` and no-op |
| `kernel_tid()` while state is not `running` | returns 0 (including after `draining`/`stopped`) |
| `join()` with no thread ever started | skips the join, then `LNX_CHECK` traps unless state is already `stopped` |
| `join()` returns with state not `stopped` | `LNX_CHECK` trap: lifecycle protocol violated by the owning thread |
| `name` longer than 15 bytes | silently truncated |

## Notes

- `thread_ctl` never spawns anything. `start()` belongs to the composing role
  ctl, which move-assigns an `lnx::thread` into `_thread`; that move-assign
  traps if `_thread` is already joinable, which is what prevents double-start.
- `_heartbeat_seq` is declared and zero-initialised but no unit in `src/`
  currently increments or reads it.
- The header names `db_ctl` and `supervisor_ctl` as composers; only
  `worker_ctl` and `acceptor_ctl` exist (the roster defers db to v2).

## Test plan

No dedicated test. Exercised through `worker_ctl` in
`tests/worker_ctl_skeleton_test.cpp`:
- spawns, runs, drains, stops clean: `starting` before start, `running` + nonzero `kernel_tid()` after, `stopped` after `request_stop`, clean `join`
- `request_stop` is idempotent: three calls, one drain
- `request_stop` on a `starting` ctl transitions to `draining` synchronously; trampoline short-circuits to `stopped`
