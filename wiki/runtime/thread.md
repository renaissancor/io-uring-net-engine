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
    using proc = void* (*)(void*);

    thread() noexcept = default;
    thread(proc start, void* arg);
    ~thread() noexcept;

    thread(const thread&)            = delete;
    thread& operator=(const thread&) = delete;
    thread(thread&&) noexcept;
    thread& operator=(thread&&) noexcept;

    void join();
    void detach();

    bool joinable() const noexcept;
    pthread_t native_handle() const noexcept;
    pid_t tid() const noexcept;

private:
    pthread_t handle_{};
    pid_t     tid_ = 0;
    bool      joinable_ = false;
};

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

**Creation.** `thread(proc, void*)` calls `pthread_create`. If creation
fails, return the `errno`-style error through the project's chosen error
surface. If this lands before a POSIX error wrapper exists, throwing
`std::system_error` is acceptable off the hot path, but `expected` is the
better project fit.

**Join/detach policy.** Match `std::thread` and the Windows `Thread`
wrapper: destroying a still-joinable thread is a programming error. The
Windows wrapper calls `std::terminate()`. Linux `lnx::thread` should do
the same unless the owning subsystem explicitly calls `join()` or
`detach()`.

**Thread id.** `pthread_t` is the pthread handle, not the kernel TID.
The wrapper should cache `gettid()` from inside the new thread's
trampoline before it calls the user's entry point. That makes `tid()`
useful for logs, profiler output, `perf`, `top -H`, and `htop`.

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

- **Unit — join.** Start a pthread-backed thread, mutate an atomic/counter
  in the entry point, join, and assert the mutation is visible.
- **Unit — detach.** Start a thread, detach, and assert the wrapper is no
  longer joinable. Use synchronization so the test does not race process
  exit.
- **Unit — move.** Move a joinable thread into another wrapper and join
  through the moved-to object.
- **Unit — native handles.** `native_handle()` is nonzero/valid while
  joinable; `tid()` is populated from inside the worker.
- **Unit — profiler TLS.** A pthread worker records scope `"X"` and the
  main thread records scope `"X"`; each manager sees exactly its own
  record.
- **Unit — thread_context.** Worker calls `set_thread_role`, then
  `this_thread()` reports the worker role while main remains `main`.

## Open Questions

1. **Error surface.** Should `thread(proc, void*)` throw on
   `pthread_create` failure, or return `expected<thread, posix_error>`?
   For system-code consistency, prefer `expected` once POSIX error
   wrappers are available.
2. **Callable support.** Do we want lambda/function-object construction
   like `WinThread`, or should runtime code use explicit C-style entry
   points? Start C-style; add callable support if examples/tests become
   too noisy.
3. **Cancellation.** POSIX cancellation is sharp and should be disabled
   or ignored initially. Use cooperative stop flags in owning subsystems.
