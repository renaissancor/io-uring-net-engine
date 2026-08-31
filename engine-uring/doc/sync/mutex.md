# mutex — pthread-backed locks + RAII guards

> **Status:** landed
> **Source:** `src/sync/mutex.h`
> **Namespace:** `lnx`
> **Depends:** `check`

## Purpose

Thin `pthread`-backed mutual-exclusion primitives and their RAII guards, the
Linux counterpart to `std::mutex`/`std::shared_mutex` (banned by the no-STL
policy). Every `pthread_*` return code is checked with `LNX_CHECK`, so a
misused lock traps at the call site instead of failing silently.

## API

```cpp
namespace lnx {

class mutex {                         // non-copyable, non-movable
    void lock() noexcept;             // traps on pthread error
    bool try_lock() noexcept;         // true if acquired; EBUSY -> false; other rc traps
    void unlock() noexcept;
};

class shared_mutex {                  // pthread_rwlock; non-copyable, non-movable
    void lock_exclusive() noexcept;   bool try_lock_exclusive() noexcept;
    void unlock_exclusive() noexcept;
    void lock_shared() noexcept;      bool try_lock_shared() noexcept;
    void unlock_shared() noexcept;
};

class lock_guard {                    // RAII: locks a mutex for its scope; non-movable
    explicit lock_guard(mutex&) noexcept;
};

class unique_lock {                   // deferred/movable ownership over a mutex
    unique_lock() noexcept;           explicit unique_lock(mutex&) noexcept;  // locks
    unique_lock(unique_lock&&) noexcept;   unique_lock& operator=(unique_lock&&) noexcept;
    void lock();  void unlock() noexcept;  bool try_lock() noexcept;
    mutex* release() noexcept;        bool owns_lock() const noexcept;
};

class shared_lock_guard    { explicit shared_lock_guard(shared_mutex&) noexcept; };    // read lock
class exclusive_lock_guard { explicit exclusive_lock_guard(shared_mutex&) noexcept; }; // write lock

}  // namespace lnx
```

## Invariants

- `mutex` and `shared_mutex` are **non-copyable and non-movable** (they wrap an
  in-place `pthread_mutex_t` / `pthread_rwlock_t`).
- `lock_guard`, `shared_lock_guard`, `exclusive_lock_guard` are RAII, scope-bound,
  non-movable.
- `unique_lock` is **movable** (transfers ownership; the moved-from lock owns
  nothing) but non-copyable; its destructor unlocks only if it currently owns.
- **All lock/unlock paths use `LNX_CHECK` (always-on), not a debug-only check** —
  there is no `LNX_DCHECK` in the project (see [[check]]). Any prose elsewhere
  claiming mutex uses a debug-only check is stale.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| any `pthread_*` returns non-zero (except `try_*` → `EBUSY`) | `LNX_CHECK` trap |
| `try_lock*` contended | returns `false` (rc `EBUSY`), no trap |
| destroying a still-locked `mutex`, or unbalanced unlock | `pthread` UB surfaced via the `LNX_CHECK` on the failing rc |
| moved-from `unique_lock` used | owns nothing; `lock()`/`unlock()` are guarded no-ops on a null mutex |

## Notes

- `shared_mutex` is a `pthread_rwlock_t`; `lock_shared` = read lock,
  `lock_exclusive` = write lock.
- `unique_lock::lock()` is intentionally non-`noexcept` (mirrors the ownership
  state machine); `unlock()`/`try_lock()` are `noexcept`.

## Test plan

`tests/sync/mutex_test.cpp`: lock/unlock/try_lock correctness; `lock_guard`
scope release; `unique_lock` defer / move / release / `owns_lock`; shared vs
exclusive rwlock access; `try_*` returns `false` under contention.

## Done when

- [x] Builds on `default` and `floor` presets
- [x] Tests pass under ASan+UBSan (and TSan for contended paths)
- [x] This spec matches the built API

## Rationale

- `doc/04-coding-style.md` § no-STL — why `lnx::mutex` exists instead of `std::mutex`.
- Trap-on-misuse rationale shared with [[check]] and [[thread]].
