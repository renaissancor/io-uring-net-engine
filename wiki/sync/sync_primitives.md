# Sync primitives — atomic, mutex, shared_mutex, scoped guards

## Purpose

The synchronization vocabulary used everywhere else in the project. Every
multi-threaded interaction goes through one of these types — there are no
ad-hoc `std::atomic_flag` spinlocks, no inline `compare_exchange` loops, no
custom `pthread_*` calls. Funneling the surface here makes correctness
arguments local and makes the deadlock profiler's job tractable.

## Reference origin

- Atomic: `IOCP_Rookiss/Engine/Atomic.h:5`,
  `WindowsLibrary/Library/Include/WinAtomic.h:6, 51, 95`
- Mutex: `IOCP_Rookiss/Engine/Mutex.h:5`,
  `WindowsLibrary/Library/Include/WinMutex.h:7`
- SharedMutex: `IOCP_Rookiss/Engine/SharedMutex.h:5`,
  `WindowsLibrary/Library/Include/WinMutex.h:28`
- LockGuard / UniqueLock / SharedLockGuard / ExclusiveLockGuard:
  `WindowsLibrary/Library/Include/WinMutex.h:49-143`

The reference implementation builds these as wrappers around
`InterlockedXxx` / `CRITICAL_SECTION` / `SRWLOCK`. On Linux, atomics use
GCC/Clang `__atomic_*` builtins behind `lnx::atomic32`, `lnx::atomic64`,
and `lnx::atomic_ptr`; see `wiki/sync/atomic.md`. Mutex and shared-mutex
wrappers remain planned as POSIX-backed primitives.

## Public API sketch

This subsystem keeps the raw Linux/POSIX-facing synchronization vocabulary
under `lnx::`. The public surface is split across focused headers such as
`sync/atomic.h`:

```cpp
namespace lnx {

// Atomics
class atomic32;
class atomic64;
class atomic_ptr;

// Locks
class mutex;
class shared_mutex;
class recursive_mutex;

// Scoped guards
class lock_guard;
class unique_lock;
class shared_lock_guard;
class exclusive_lock_guard;

} // namespace lnx
```

The wrappers exist so the project codebase imports `lnx::mutex` /
`lnx::atomic32` rather than raw `pthread_*` or `__atomic_*` calls, giving us
a single place to reason about platform behavior and future debug
instrumentation.

## Linux design

**Atomic.** `lnx::atomic32`, `lnx::atomic64`, and `lnx::atomic_ptr` are
direct ports of the WindowsLibrary `WinAtomic.h` structure. They wrap
`__atomic_*` builtins, stay naturally sized by default, and preserve
Windows-style `compare_exchange` return semantics: the method returns the
observed old value, not a success boolean. Use `lnx::cache_aligned<T>` when
false-sharing protection is needed. No `lnx::memory_barrier()` without a
written justification at the call site.

**`spin_mutex`.** Optional small lock for very short critical sections (no
allocation, no syscalls). Implementation:
```cpp
class spin_mutex {
    std::atomic<bool> flag_{false};
public:
    void lock() {
        while (flag_.exchange(true, std::memory_order_acquire)) {
            while (flag_.load(std::memory_order_relaxed))
                __builtin_ia32_pause();          // _mm_pause on x86-64
        }
    }
    bool try_lock() {
        return !flag_.exchange(true, std::memory_order_acquire);
    }
    void unlock() { flag_.store(false, std::memory_order_release); }
};
```
Use only in profiler-validated hot spots. Default to `std::mutex`.

**`mutex` policy.** `std::mutex` (libstdc++ implementation is futex-backed,
uncontended fast path is a single CAS). Adaptive `pthread_mutex_t` with
`PTHREAD_MUTEX_ADAPTIVE_NP` is a Linux-only fallback if profiling shows
contention; introduce only with measurement.

**`shared_mutex` policy.** `std::shared_mutex` is writer-preferring on
libstdc++. Acceptable for v1. If readers must be preferred, build a custom
rwlock — out of scope.

**Debug-build deadlock profiling.** In debug builds, `sync::mutex` and
`sync::shared_mutex` may be aliased to a wrapper that records lock-order
edges into the deadlock profiler. See
`wiki/diagnostic/deadlock_profiler.md`.

## Concurrency & ownership

- Locks own no resources beyond the kernel object underneath.
- RAII guards (`lock_guard`, `scoped_lock`, `shared_lock`,
  `exclusive_lock`) are the only way locks are taken in project code. No
  manual `lock()` / `unlock()` outside primitive-test code.
- `std::scoped_lock<M1, M2>` is the right tool for multi-lock acquisition;
  it implements deadlock-free ordered lock acquisition.

## Test plan

- Unit: build the same Catch2 test under `-fsanitize=thread` and against a
  release build; ratchet TSan-clean as a CI gate.
- Unit: `spin_mutex` correctness — 8 threads × 100k increments of a shared
  counter under spin_mutex; final value matches expected.
- Stress: lock-ordering test that intentionally creates a 2-cycle to verify
  the deadlock profiler catches it (debug build only).

## Open questions

1. **Ship `spin_mutex` at all?** Adds API surface for a primitive that
   should be used reluctantly. Alternative: keep it internal to the
   memory-pool / lock-free-stack code and don't expose. **Lean: keep
   internal for v1.**
2. **`recursive_mutex`.** Listed in the API for completeness. Project
   policy: never used in new code. Document in style guide.
3. **Wait/notify primitives.** `std::condition_variable` + `std::mutex` is
   standard; do we need a `condition_variable_any` alias? Defer until a use
   case shows up.
4. **Atomic shared_ptr.** C++20 has `std::atomic<std::shared_ptr<T>>`. We
   have one or two places that might use it (Service's session map). Don't
   alias yet — call out at the use site.
