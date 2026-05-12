# ThreadContext — per-thread state and TLS conventions

## Purpose

Centralize what each thread needs to know about itself: its role
(reactor / worker / main), its name, the current job (if executing one),
and the small handful of `thread_local` values that other subsystems
expect (memory-pool stats, deadlock-profiler stack, leak-tracker counter).

## Reference origin

- `IOCP_Rookiss/Engine/ThreadManager.h:5`,
  `Engine/ThreadManager.cpp:8-14` — TLS stubs only; the design exists in
  intent but not in code. This doc is a fresh design.

## Public API sketch

```cpp
namespace iouring_net::rt {

enum class thread_role : uint8_t {
    main,
    reactor,
    worker,                                     // future: worker thread pool
    test                                        // tests / fixtures
};

struct thread_context {
    thread_role           role;
    uint32_t              role_index;           // 0..N-1 for reactor/worker
    std::string           name;                 // for prctl + tracing
    pid_t                 tid;                  // gettid()
    job_handle*           current_job{nullptr}; // set/cleared by job_queue
};

const thread_context& this_thread();
thread_context&       this_thread_mut();        // restricted; for runtime use

void set_thread_role(thread_role r, uint32_t index, std::string name);

} // namespace iouring_net::rt
```

`this_thread()` returns a const ref to the calling thread's context. Used
by logging (so log lines say `[reactor-0]`), the deadlock profiler (its
held-stack lives in `thread_context`), and the job queue (which job is
currently running).

## Linux design

**Storage.** A `thread_local thread_context` instance per thread. Default
constructed (role = `main`, index = 0, name = `""`); finalized via
`set_thread_role` when the thread joins its role.

**Naming.** `set_thread_role` calls `prctl(PR_SET_NAME, name.c_str())`
which sets the kernel-visible thread name, visible in `top`, `htop`,
`ps -L`, `perf`, and `gdb`. Name is truncated to 15 chars by the kernel.

**Tid.** Cached in the context struct (one `gettid()` per thread,
cached). Used in log lines and trace events.

**Current job pointer.** Set by `job_queue::dispatch` before invoking a
job, cleared after. Lets crash dumps show "this reactor was running job
X for session Y" without manual instrumentation.

## Concurrency & ownership

- Per-thread storage; no synchronization needed for the storage itself.
- `set_thread_role` should be called once per thread, before that thread
  starts taking work. Calling it twice is allowed but logged as a warning.
- The "main" thread context has role = `main` even if no one explicitly
  calls `set_thread_role` — the default-initialized struct is correct.

## Test plan

- Unit: spawn a pthread-backed `lnx::thread`, call
  `set_thread_role(reactor, 0, "rx-0")`, read
  `this_thread().name == "rx-0"`. From the spawning thread,
  `this_thread().role == main`.
- Unit: assert `this_thread().tid == gettid()` from the running thread.
- Integration: under a running reactor, log lines correctly identify
  thread role + index.

## Open questions

1. **Should the context be reachable cross-thread** (e.g., reactor 0
   queries reactor 1's current job)? Useful for diagnostics but adds
   synchronization. Defer; debug-only need.
2. **Multiple reactors per process.** v1 ships one reactor; the schema
   already supports `(role=reactor, index=N)`. No new design when v2
   adds a second reactor, only allocation policy.
3. **Worker thread pool.** Out of scope for v1. Reserved
   `thread_role::worker` for the v2 hand-off.
