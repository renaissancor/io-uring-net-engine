# Mutex wrappers (lnx::mutex, lnx::shared_mutex, RAII guards)

## Purpose

Linux replacement for `Win::Mutex` / `Win::SharedMutex` from
`WindowsLibrary/Library/Include/WinMutex.h`; bundles `lock_guard`,
`unique_lock`, `shared_lock_guard`, and `exclusive_lock_guard`; this is the only
way locks are taken in project code outside of primitive tests.

## Reference origin

- `WindowsLibrary/Library/Include/WinMutex.h:7` — `Win::Mutex`
- `WindowsLibrary/Library/Include/WinMutex.h:11` — spin-count constructor
- `WindowsLibrary/Library/Include/WinMutex.h:23-25` — exclusive lock operations
- `WindowsLibrary/Library/Include/WinMutex.h:28` — `Win::SharedMutex`
- `WindowsLibrary/Library/Include/WinMutex.h:40-46` — shared/exclusive SRWLOCK
  operations
- `WindowsLibrary/Library/Include/WinMutex.h:49-143` — RAII guards and
  `UniqueLock`
- `IOCP_Rookiss/Engine/Mutex.h:5` — secondary `Mutex`
- `IOCP_Rookiss/Engine/Mutex.h:8, 18-20` — SRWLOCK exclusive backend
- `IOCP_Rookiss/Engine/SharedMutex.h:5` — secondary `SharedMutex`
- `IOCP_Rookiss/Engine/SharedMutex.h:18-24` — shared/exclusive SRWLOCK
  operations
- `IOCP_Rookiss/Engine/SharedMutex.h:27-51` — shared/exclusive guards

`IOCP_Rookiss/Engine/Mutex.h` uses SRWLOCK exclusive, not `CRITICAL_SECTION`
like `Win::Mutex`; this project sides with `Win::Mutex` (futex-backed
`std::mutex`, single ownership).

## Public surface

```cpp
namespace lnx {

class mutex {
public:
    mutex() noexcept = default;
    ~mutex() = default;

    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    mutex(mutex&&) = delete;
    mutex& operator=(mutex&&) = delete;

    void lock() noexcept;
    bool try_lock() noexcept;
    void unlock() noexcept;
};

class shared_mutex {
public:
    shared_mutex() noexcept = default;
    ~shared_mutex() = default;

    shared_mutex(const shared_mutex&) = delete;
    shared_mutex& operator=(const shared_mutex&) = delete;
    shared_mutex(shared_mutex&&) = delete;
    shared_mutex& operator=(shared_mutex&&) = delete;

    void lock_exclusive() noexcept;
    bool try_lock_exclusive() noexcept;
    void unlock_exclusive() noexcept;

    void lock_shared() noexcept;
    bool try_lock_shared() noexcept;
    void unlock_shared() noexcept;
};

class lock_guard {
public:
    explicit lock_guard(mutex& m) noexcept;
    ~lock_guard();

    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;
    lock_guard(lock_guard&&) = delete;
    lock_guard& operator=(lock_guard&&) = delete;
};

class unique_lock {
public:
    unique_lock() noexcept;
    explicit unique_lock(mutex& m) noexcept;
    ~unique_lock();

    unique_lock(const unique_lock&) = delete;
    unique_lock& operator=(const unique_lock&) = delete;

    unique_lock(unique_lock&& other) noexcept;
    unique_lock& operator=(unique_lock&& other) noexcept;

    void lock();
    void unlock() noexcept;
    bool try_lock() noexcept;
    mutex* release() noexcept;
    bool owns_lock() const noexcept;
};

class shared_lock_guard {
public:
    explicit shared_lock_guard(shared_mutex& m) noexcept;
    ~shared_lock_guard();

    shared_lock_guard(const shared_lock_guard&) = delete;
    shared_lock_guard& operator=(const shared_lock_guard&) = delete;
    shared_lock_guard(shared_lock_guard&&) = delete;
    shared_lock_guard& operator=(shared_lock_guard&&) = delete;
};

class exclusive_lock_guard {
public:
    explicit exclusive_lock_guard(shared_mutex& m) noexcept;
    ~exclusive_lock_guard();

    exclusive_lock_guard(const exclusive_lock_guard&) = delete;
    exclusive_lock_guard& operator=(const exclusive_lock_guard&) = delete;
    exclusive_lock_guard(exclusive_lock_guard&&) = delete;
    exclusive_lock_guard& operator=(exclusive_lock_guard&&) = delete;
};

} // namespace lnx
```

## Linux design

- **Backend.**
  - `lnx::mutex` wraps `pthread_mutex_t` directly with default attributes
    (`PTHREAD_MUTEX_NORMAL`). glibc backs it with futexes — uncontended fast
    path is a single CAS, same as the libstdc++ `std::mutex` implementation
    underneath.
  - `lnx::shared_mutex` wraps `pthread_rwlock_t` directly with default
    attributes. glibc's rwlock is writer-preferring.
- **Why not `std::mutex` / `std::shared_mutex`?** Honest `noexcept`.
  `std::mutex::lock` is specified to throw `std::system_error` on failure; a
  noexcept wrapper around it would terminate the process if the throw ever
  fired, claiming a guarantee the inner type does not give. pthread is a C
  API — its functions return `int`, they cannot throw, and the noexcept claim
  is therefore real. For `PTHREAD_MUTEX_NORMAL` the non-zero return paths
  reduce to UB territory (uninitialized mutex) or system-catastrophe (OOM,
  thread limit), matching `Win::Mutex`'s "succeed or you're in UB" contract.
- **No `countSpinLock_` parameter.** `Win::Mutex`'s `CRITICAL_SECTION` spin
  count has no analog for `pthread_mutex_t` with default attributes.
  `PTHREAD_MUTEX_ADAPTIVE_NP` (Linux extension, passed via
  `pthread_mutexattr_t`) is the escape hatch if profiling demands it; not
  exposed yet.
- **Unified rwlock unlock.** POSIX `pthread_rwlock_unlock` releases whichever
  mode the calling thread holds. `lnx::shared_mutex::unlock_exclusive` and
  `unlock_shared` both forward to the same call but are kept as separate
  methods to mirror `Win::SharedMutex`'s `Release*Exclusive` /
  `Release*Shared` split.
- **Debug-only misuse trap.** Every pthread call's return value is checked
  through `LNX_DCHECK(cond)` from `src/check.h` (see `wiki/check.md`).
  Traps via `int 3` → `SIGTRAP` when `NDEBUG` is unset; compiles to
  `((void)0)` otherwise. Zero cost in release — appropriate for the
  hot-path lock/unlock surface. Catches double-unlock (`EPERM`), use of
  a destroyed mutex (`EINVAL`), init-time `EAGAIN`/`ENOMEM`, and any
  other surprise non-zero return. For try variants, the macro permits
  both `0` and `EBUSY` since `EBUSY` is the documented "lock held"
  return path, not an error. Contrast with the cold-path `lnx::thread`,
  which uses the always-on `LNX_CHECK` variant — see
  `wiki/runtime/thread.md`.
- **Init/destroy:** with `nullptr` attributes both are effectively infallible
  on Linux/glibc — the documented `EAGAIN` / `ENOMEM` paths are
  system-catastrophe states. Debug builds trap on the rare failure for
  development visibility; release builds proceed unchecked, matching
  `InitializeSRWLock`'s void-return contract on Windows.
- **Win-style `lock_exclusive` naming on `shared_mutex`.** Intentional: it
  mirrors `Win::SharedMutex`. Project code provides its own guards, so
  std-style `lock()` / `lock_shared()` would only enable `std::scoped_lock`
  interop, which is unused. Add std aliases as inline forwards if a use case
  appears.
- **No `recursive_mutex`.** Listed in the surface sketch for completeness but
  deliberately not implemented. Project policy: never used in new code. Add
  when a real callsite proves the need.
- **`unique_lock` semantics.** Constructor takes `mutex&` and locks
  immediately; there is no `defer_lock` tag. Default-constructed is empty.
  `release()` returns the pointer without unlocking. Match `WinMutex.h` pattern
  verbatim.
- **Guards are not movable.** `lock_guard`, `shared_lock_guard`, and
  `exclusive_lock_guard` are all non-movable, non-copyable: the same shape as
  `std::lock_guard`. Move would invite double-unlock or use-after-move bugs.

## Concurrency & ownership

- All public operations are thread-safe with respect to the underlying mutex.
- Wrapper objects are non-copyable; `mutex` and `shared_mutex` are non-movable;
  `unique_lock` is movable; the three guard classes are non-movable.
- RAII guards are the only acceptable way to acquire locks in project code
  outside of primitive tests. Bare `lock()` / `unlock()` is allowed only inside
  `unique_lock` / guards.
- No reentrant locking; `lnx::mutex` is non-recursive. A second `lock()` from
  the same thread deadlocks.
- `std::scoped_lock<lnx::mutex, lnx::mutex>` works because
  `lnx::mutex::{lock,try_lock,unlock}` satisfy Lockable, if multi-lock ordered
  acquisition is ever needed.

## Test plan

- `mutex: lock and unlock acquire and release`
- `mutex: multi-thread counter via lock_guard reaches expected total`
- `unique_lock: default constructed is empty`
- `unique_lock: construction locks and destruction unlocks`
- `unique_lock: try_lock returns false when contended`
- `unique_lock: move transfers ownership`
- `unique_lock: release relinquishes pointer without unlocking`
- `unique_lock: unlock then try_lock re-acquires`
- `shared_mutex: exclusive blocks shared via guard scopes`
- `shared_mutex: multiple shared holders coexist`
- `shared_mutex: read-heavy stress with single writer`
- Future tests:
  - TSan run for contended `unique_lock` move-assignment scenarios; ratchet
    TSan-clean as a CI gate once tsan preset lands.

## Open questions

- Add adaptive `pthread_mutex_t` only with measurement.
- Debug-build deadlock-profiler aliasing — see
  `wiki/diagnostic/deadlock_profiler.md`.
- `lnx::condition_variable` not yet wrapped; `std::condition_variable` +
  `std::unique_lock<std::mutex>` does not compose with `lnx::mutex` (different
  mutex type). Decision deferred until a use case appears.
