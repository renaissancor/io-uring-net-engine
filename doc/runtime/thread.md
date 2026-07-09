# thread — pthread-backed thread wrapper

## Purpose

Provide the Linux/POSIX counterpart to the Windows `WinThread` wrapper:
a small RAII owner for native threads that uses the C runtime's supported
thread creation API, exposes the kernel TID for diagnostics, and composes
cleanly with project TLS users such as `profiler::manager` and
`thread_context`.

This is intentionally lower-level than `std::thread` / `std::jthread`.
The project is a Linux system-programming library; the public primitive
should make `pthread_create`, `pthread_join`, `pthread_detach`, and
`gettid()` behavior explicit.

## Reference Origin

- `WindowsLibrary/Library/Include/WinThread.h:8` — RAII wrapper around
  `_beginthreadex`, with join/detach, native handle, thread id, deleted
  copy, and move ownership.
- `WindowsLibrary/MainApp/Sources/TestWinThread.cpp` — exercises lambda
  launch, vector storage, move semantics, detach, and IOCP worker usage.
- `WindowsLibrary/MainApp/Sources/TestProfiler.cpp:62` — uses
  `_beginthreadex` directly for profiler worker threads.
- `IOCP_Rookiss/Engine/ThreadManager.h:15` and `.cpp:8` — TLS creation
  stubs only; useful as intent, not as implementation.

## Windows to Linux Mapping

The important mapping is runtime-aware thread creation:

| Windows | Linux |
|---|---|
| `CreateThread` | raw `clone(2)` or `clone3(2)` level; avoid for normal C/C++ threads |
| `_beginthreadex` | `pthread_create` |
| `WaitForSingleObject(thread)` | `pthread_join` |
| `CloseHandle(thread)` after detach | `pthread_detach` |
| `GetCurrentThreadId()` | `gettid()` / `syscall(SYS_gettid)` |
| Win32 `TlsAlloc` slots | `pthread_key_create` or C++ `thread_local` |
| C++ `thread_local` manager singleton | C++ `thread_local` manager singleton, initialized per pthread |

`pthread_create` is not the Linux equivalent of raw Win32
`CreateThread` for C/C++ code. It is the libc/POSIX thread creation API:
glibc/pthreads sets up user-space thread runtime state, TLS machinery,
per-thread `errno`, cancellation state, cleanup handlers, and normal
thread exit behavior. Raw `clone` is the API that bypasses most of that
and should be reserved for code intentionally building a thread runtime.

## Public API Sketch

```cpp
namespace lnx {

class thread {
public:
    thread() noexcept;
    thread(void* (*fn)(void*), void* arg) noexcept;
    ~thread() noexcept;

    thread(const thread&)            = delete;
    thread& operator=(const thread&) = delete;
    thread(thread&&) noexcept;
    thread& operator=(thread&&) noexcept;

    void      join() noexcept;
    void      detach() noexcept;
    bool      joinable() const noexcept;
    pthread_t native_handle() const noexcept;

private:
    pthread_t _tid;
    bool      _joinable;
};

namespace this_thread {

void      yield() noexcept;
void      sleep_for_ns(int64_t ns) noexcept;
pthread_t id() noexcept;
int       kernel_tid() noexcept;

} // namespace this_thread

} // namespace lnx
```

Initial implementation should prefer a C-style entry point:

```cpp
void* reactor_main(void* arg) noexcept;

lnx::thread worker{reactor_main, &reactor_context};
worker.join();
```

Lambda/function-object support can come later through a tiny heap or
malloc-backed start context and a static trampoline, but the first
version should keep ownership and error behavior visible.

## Linux Design

**Creation.** `thread(fn, arg)` calls `pthread_create` with default
attributes. The return is checked via `LNX_CHECK` from `src/check.h`
(see `doc/check.md`) — traps via `int 3` → `SIGTRAP` in **both debug
and release**, with no exception runtime involvement. `pthread_create`
failure is OOM/thread-limit territory and unrecoverable in practice;
trapping immediately at the misuse site produces a clean core dump for
the on-call engineer (gdb-resumable in dev, fatal in prod). No
`std::system_error`, no `std::abort`, no exception machinery.

**Join/detach policy.** Destroying a still-joinable thread is a programming
error. The destructor traps via `LNX_CHECK(!_joinable)`. Same trap fires
in both debug and release because thread ops are cold-path — adding a
branch costs nothing. This diverges from `std::thread` and `Win::Thread`
(both call `std::terminate()`) because the project deliberately keeps the
primitive layer free of `std::abort` / `std::terminate`. The trap is also
strictly better for debugging: `std::terminate` unwinds the stack, while
`int 3` halts at the misuse site with the stack intact, so post-mortem
core dumps point directly at the offending operation.

**Why `LNX_CHECK` here, not `LNX_DCHECK`?** Thread creation, join, and
detach are cold-path operations — they take microseconds and happen rarely.
A per-call branch cost is invisible. Choosing the always-on variant closes
the "release builds silently leak thread handles" failure mode that a
debug-only check would leave open. Folly/abseil/Chromium make the same
split (`CHECK_*` for cold-path invariants, `DCHECK_*` for hot paths).
`lnx::mutex` uses `LNX_DCHECK` instead because mutex lock/unlock is
hot-path and a branch per op would actually matter.

**Thread id (v1).** `pthread_t` is the pthread handle, exposed via
`native_handle()`. The kernel TID for the *current* thread is available
through `lnx::this_thread::kernel_tid()` (which calls `syscall(SYS_gettid)`).
v1 does NOT cache a per-wrapper kernel TID — there is no `lnx::thread::tid()`
accessor. Adding it requires a trampoline to capture `gettid()` from inside
the new thread before the user `fn` runs, because the wrapper-creating
thread cannot observe another thread's `gettid()` directly. Deferred until
a real use case (log/perf correlation, `top -H` integration) demands it.

**TLS correctness.** Threads created with `pthread_create` correctly
support C++ `thread_local`. Therefore `profiler::manager::instance()`
remains one manager per POSIX thread:

```cpp
thread_local profiler::manager instance;
```

The caution from Windows still applies conceptually: do not bypass the
runtime's thread creation API. On Windows that means prefer
`_beginthreadex` over `CreateThread`; on Linux that means prefer
`pthread_create` over raw `clone`.

**Naming.** Thread role/name belongs in `thread_context`; the thread
wrapper should not take a name in v1. `thread_context::set_thread_role`
can call `prctl(PR_SET_NAME, ...)` after the thread starts.

## Cooperative stop + blocking waits

A stop signal from the supervisor is a **cooperative** atomic flag
(`handle_thread::request_stop()` CAS-es `running → draining`). The engine loop
observes it and drains. This works only while the loop actually reaches the flag
check between iterations.

The current skeleton loops (`engine_worker::run_loop`, `engine_acceptor::run_loop`)
spin on `lnx::this_thread::yield()` with non-blocking `io_uring_peek_cqe`, so the
flag is observed every tick — no wake needed. The echo smoke
(`tests/net/echo_smoke_test.cpp`) does the same: busy-poll + a `stop` atomic, and
a throwaway connect to satisfy the parked accept SQE on the way out.

**The caveat lands the moment a loop blocks** in `io_uring_wait_cqe` or
`io_uring_submit_and_wait`. If no completion arrives, the thread sleeps in the
kernel and never observes `request_stop()` — the atomic flag alone cannot wake a
thread parked in a syscall. Pair `request_stop()` with a wake strategy:

- **eventfd wake** — the supervisor writes an eventfd that the loop has a
  registered `POLL`/`READ` SQE on; cleanest end state.
- **bounded-timeout wait** — `io_uring_wait_cqe_timeout` / a timeout SQE so the
  loop re-checks the flag at a bounded cadence. Acceptable for the first pass if
  the latency/CPU tradeoff is documented.
- **message-ring wake** — a mesh push doubles as the wake event.

For the first blocking implementation a bounded timeout is fine; migrate to
eventfd wake when the CPU/latency budget is tightened. See
`doc/10-realtime-server-architecture.md` §9. This is also tracked in Open
Questions #3 (cancellation vs. cooperative stop).

## Interaction With profiler::manager

TLS isolation is the profiler's multithreaded safety boundary. A
pthread-backed `lnx::thread` must preserve this invariant:

- main thread `profiler::manager::instance()` and worker thread
  `profiler::manager::instance()` are different objects.
- each worker appends to its own `cstr_hash_map<malloc_vector<record>>`.
- reports are per-thread in v1; cross-thread merge is future work.

Add a profiler test that uses `pthread_create` directly before or while
landing `lnx::thread`. That test proves the important runtime property
without depending on the wrapper itself.

## Test Plan

Implemented in `tests/runtime/thread_test.cpp`:

- `thread: default constructed is not joinable`
- `thread: created thread runs the function and is joinable`
- `thread: detached thread runs and the wrapper is no longer joinable`
- `thread: move ctor transfers ownership`
- `thread: move assign transfers ownership`
- `thread: native_handle returns pthread_t` (uses `pthread_equal` per POSIX,
  since `pthread_t` is opaque)
- `this_thread::id and kernel_tid return current thread identities`
- `this_thread::yield is callable and returns`
- `this_thread::sleep_for_ns sleeps for approximately the requested duration`

Future tests:

- **Profiler TLS interaction.** A `lnx::thread` worker records profiler
  scopes and the main thread records profiler scopes; each manager sees
  exactly its own record. Lands when the profiler subsystem grows
  multi-thread coverage.
- **`thread_context` integration.** Worker calls `set_thread_role`, then
  `this_thread()` reports the worker role while main remains `main`. Lands
  alongside `doc/runtime/thread_context.md` implementation.
- **TSan run.** Catch races in the join/detach state machine under
  contention. Ratchet TSan-clean as a CI gate when the tsan preset lands.

## Open Questions

1. **Cached kernel TID (deferred).** Add `lnx::thread::tid()` that returns
   the wrapped thread's `gettid()`. Requires a trampoline that captures
   `gettid()` before calling `fn`, plus a sync mechanism so the wrapper's
   `_kernel_tid` member is published before the constructor returns. Ship
   when log/perf correlation needs it.
2. **Callable support (deferred).** Lambda/function-object construction
   like `Win::Thread`'s `template<class F> Thread(F&&)` requires a heap-
   allocated closure (Win::Thread uses `new std::function<void()>`).
   Conflicts with the no-`std::` primitive ethos; would need a hand-rolled
   closure type. C-style `(void* (*)(void*), void*)` is plenty for v1 and
   is what production reactor code uses anyway.
3. **Cancellation.** POSIX cancellation is sharp and should be disabled
   or ignored initially. Use cooperative stop flags in owning subsystems.
4. **`pthread_attr_t` exposure (deferred).** CPU affinity
   (`pthread_setaffinity_np`), thread name (`pthread_setname_np`),
   scheduling policy, stack size — all useful for game-server worker pools.
   v1 omits them; add a `thread_attr` builder type and an
   `thread(thread_attr, fn, arg)` overload when the I/O reactor / worker
   pool subsystems need them.
