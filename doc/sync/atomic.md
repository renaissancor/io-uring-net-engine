# atomic — scalar atomics + cache-line alignment

> **Status:** landed
> **Source:** `src/sync/atomic.h`
> **Namespace:** `lnx`
> **Depends:** none (`<cstdint>`, GCC/Clang `__atomic_*` builtins)

## Purpose

Project-owned atomic scalar types (the no-STL substitute for `std::atomic`) plus
a cache-line alignment wrapper. Built on the compiler `__atomic_*` builtins so
the memory ordering is explicit at every call site. Load-bearing for the
lifecycle state machine ([[thread]] handle) and the SPSC ring cursors
([[ring_buffer]]).

## API

```cpp
namespace lnx {

inline constexpr std::size_t CACHE_LINE_SIZE = 64;

template <typename T>
struct cache_aligned {                 // aligns T to a cache line (no false sharing)
    alignas(CACHE_LINE_SIZE) T value;
    template <typename... Args> explicit cache_aligned(Args&&...) noexcept;
    T* operator->() noexcept;  T& get() noexcept;   // + const overloads
};

class atomic32 {                       // wraps int32_t; non-copyable, non-movable
    explicit atomic32(int32_t = 0) noexcept;
    int32_t load() const noexcept;              void store(int32_t) noexcept;      // seq_cst
    int32_t load_relaxed() const noexcept;      int32_t load_acquire() const noexcept;
    void    store_relaxed(int32_t) noexcept;    void    store_release(int32_t) noexcept;
    int32_t fetch_add/ fetch_sub/ fetch_and/ fetch_or/ fetch_xor(int32_t) noexcept; // seq_cst
    int32_t increment() noexcept;  int32_t decrement() noexcept;  int32_t exchange(int32_t) noexcept;
    // CAS: note arg order (desired, expected) and it RETURNS THE OBSERVED value (not bool).
    int32_t compare_exchange(int32_t desired, int32_t expected) noexcept;
    // operator= / ++ / -- / += / -= / &= / |= / ^=  (all seq_cst)
};

class atomic64 { /* identical surface over int64_t */ };
class atomic_ptr {                     // wraps void*; load/store (+relaxed/acq/rel),
    /* exchange, compare_exchange(desired, expected), operator= */
};

inline void memory_barrier() noexcept;  // __atomic_thread_fence(seq_cst)

}  // namespace lnx
```

## Invariants

- All three atomic classes are **non-copyable and non-movable** (an atomic has
  identity; copying one is meaningless).
- Default ordering for `load`/`store`/`fetch_*`/`exchange`/`compare_exchange` is
  **seq_cst**; the `*_relaxed` / `*_acquire` / `*_release` variants opt into
  weaker orderings explicitly.
- `compare_exchange(desired, expected)` takes **desired first**, uses a strong
  CAS, and **returns the observed value** — caller compares it to `expected` to
  learn success (this is the exact pattern in the worker/acceptor trampoline).
- `static_assert`s guarantee `sizeof(atomicNN) == sizeof(intNN)` and
  `alignof(cache_aligned<atomic32>) == CACHE_LINE_SIZE`.

## Errors & edge cases

- No error paths — every operation is total. Misuse is a data race (a
  correctness bug in the caller), not a reported error; catch it with TSan.
- `compare_exchange`'s non-standard "returns observed, not bool" shape is a
  frequent foot-gun for readers used to `std::atomic` — the return is the value
  seen, so success is `observed == expected`.

## Notes

- `cache_aligned<T>` is used to place the two SPSC ring cursors on separate
  cache lines; it forwards ctor args to `T` and proxies `->`.
- Ordinary (non-`cache_aligned`) `atomicNN` is exactly its scalar size, so it
  packs into structs without padding surprises.

## Test plan

`tests/sync/atomic_test.cpp`: single-thread op correctness for each method;
`compare_exchange` success/failure observed-value semantics; a TSan-clean
multi-threaded increment race; `is_always_lock_free`-style size/alignment
static assertions compile.

## Done when

- [x] Builds on `default` and `floor` presets
- [x] Tests pass under ASan+UBSan; increment race clean under TSan
- [x] This spec matches the built API

## Rationale

- `doc/04-coding-style.md` § no-STL — why `lnx::atomic*` exists instead of `std::atomic`.
- Consumed by [[thread]] (lifecycle CAS) and [[ring_buffer]] (spsc cursors).
