# thread_context — per-thread role + identity

## Purpose

Centralize what each thread needs to know about itself: its **role** (main,
accept, network, content, test), its **role index** within that role (e.g.,
"network thread #3 out of 4"), its kernel **TID** for diagnostics, and a
short **name** for kernel-visible labeling. Used by logging (so log lines
say `[content-3]`), by profilers, and by anywhere code needs to confirm it's
running on the expected thread type.

The roles map directly onto the two-tier reactor model — see
[[threading_model]] for the architectural context.

## Reference origin

- `IOCP_Rookiss/Engine/ThreadManager.h:5`, `Engine/ThreadManager.cpp:8-14`
  — TLS stubs only; the design exists in intent but not in code. This
  doc is a fresh design tied to the project's two-tier topology.

## Public API sketch

```cpp
namespace iouring_net::rt {

enum class thread_role : uint8_t {
    main,           // process main thread (init, shutdown coordination)
    accept,         // accept thread (one per server, dispatches new connections)
    network,        // network worker (owns one io_uring ring, polls CQEs)
    content,        // content worker (owns sessions partition, runs tick loop)
    test            // tests / fixtures / harness threads
};

struct thread_context {
    thread_role role;
    uint32_t    role_index;     // 0..N-1 within the role
    pid_t       tid;            // gettid() cached at init
    char        name[16];       // kernel-visible name (prctl 15-char + null)
};

const thread_context& this_thread();
thread_context&       this_thread_mut();         // restricted; runtime use only

void set_thread_role(thread_role r, uint32_t index, const char* name);

} // namespace iouring_net::rt
```

`this_thread()` returns a const ref to the calling thread's context. Used
by logging (`[network-2]` line prefix), by the profiler (records keyed by
thread context), and by `LNX_DCHECK(this_thread().role == content)`-style
invariant guards on hot-path code that must run on a specific role.

## Linux design

**Storage.** A `thread_local thread_context` instance per thread. Default-
constructed values: `role = main`, `role_index = 0`, `tid = gettid()`,
`name = ""`. Threads call `set_thread_role` once at startup to finalize
their context.

**Kernel-visible naming.** `set_thread_role` invokes
`prctl(PR_SET_NAME, name)` so the thread name is visible in `top -H`,
`htop`, `ps -L`, `perf`, and `gdb`'s `info threads`. The kernel caps
thread names at 15 chars + null; the struct's `char name[16]` matches
this exactly.

**No `std::string`.** Foundational-layer discipline (see
[[threading_model]]) avoids `std::` STL containers. A 16-byte fixed
buffer captures all the name we need without dragging in `<string>`.

**TID caching.** `gettid()` is fast on Linux (single syscall) but doing
it once at thread init and caching the result avoids the syscall on the
log hot path. The cached value never changes for the lifetime of the
thread.

**No `current_job` field.** Earlier drafts of this struct included a
`job_handle* current_job` pointer that the (now-removed) `job_queue`
subsystem populated for crash-dump correlation. Under the two-tier
architecture, that role doesn't exist — content threads run a tick loop,
not a job-queue drainer — so the field is removed. If we ever need
"what is this content thread currently doing?" telemetry, it lives
elsewhere (e.g., a per-thread "current opcode" set by the dispatch
layer).

## Concurrency & ownership

- Per-thread storage; no synchronization needed for the struct itself.
- `set_thread_role` should be called exactly once per thread, before that
  thread starts its tick loop / io_uring poll. Calling it twice is a
  programming error caught by `LNX_DCHECK`.
- The main thread's context defaults to `role = main` even without
  explicit `set_thread_role` — the zero-initialized struct is already
  correct.
- `this_thread_mut()` is exposed for runtime initialization paths only;
  application code should treat the context as read-only.

## Test plan

- Unit: spawn a `lnx::thread`, call
  `set_thread_role(thread_role::network, 0, "net-0")`, read
  `this_thread().role == network` and `this_thread().name == "net-0"`.
  From the spawning (main) thread, `this_thread().role == main`.
- Unit: assert `this_thread().tid == gettid()` from the running thread.
- Unit: `prctl(PR_GET_NAME)` returns the name set via `set_thread_role`.
- Unit: `set_thread_role` called twice on the same thread fires
  `LNX_DCHECK` in debug builds.
- Integration: under a running server, log lines correctly identify
  thread role + index.

## Open questions

1. **Cross-thread context introspection.** Should the context be
   reachable from other threads (e.g., "what is content-3 currently
   doing")? Useful for diagnostics but adds synchronization. Defer;
   debug-only need.
2. **Role-index allocation.** Who decides that this is "network-2" vs
   "network-3"? Likely the main thread at server startup, passing the
   index to `set_thread_role` via the thread's start argument. Document
   precisely once the runtime startup sequence is written.
3. **`accept` role multiplicity.** v1 ships one accept thread; the schema
   allows multiple `accept` threads with distinct `role_index` for
   `SO_REUSEPORT` scaling in v2. No new design needed; just allocation
   policy.
4. **`test` role boundaries.** Test fixtures running under gtest may
   want to claim a content-thread-like context to exercise content-only
   code paths. Allow `set_thread_role(test, ..., ...)` to grant access
   to anything content-thread-restricted; document this carefully so
   nobody mistakes it for a back door in production code.
