# Atomic wrappers (`lnx::atomic32`, `atomic64`, `atomic_ptr`)

## Purpose

Linux-side replacement for Windows `Interlocked*` wrappers from
`WindowsLibrary/Library/Include/WinAtomic.h`. This file is intentionally
not a general clone of `std::atomic<T>`; it is a small low-level vocabulary
for the integer and pointer widths used by the Windows reference code.

The immediate goal is educational and practical: keep the same mental model
as the WinAPI version while making the Linux backend explicit through
GCC/Clang `__atomic_*` builtins.

## Reference origin

- `WindowsLibrary/Library/Include/WinAtomic.h:6` — `Win::Atomic32`
- `WindowsLibrary/Library/Include/WinAtomic.h:51` — `Win::Atomic64`
- `WindowsLibrary/Library/Include/WinAtomic.h:95` — `Win::AtomicPtr`
- `docs/01-windows-to-linux-mapping.md` — Interlocked to Linux mapping

The Linux port keeps the structure first:

- `Win::Atomic32` -> `lnx::atomic32`
- `Win::Atomic64` -> `lnx::atomic64`
- `Win::AtomicPtr` -> `lnx::atomic_ptr`
- `LONG` / `LONGLONG` -> `std::int32_t` / `std::int64_t`
- `Interlocked*` -> `__atomic_*`

## Public surface

```cpp
namespace lnx {

class atomic32 {
public:
    explicit atomic32(std::int32_t value = 0) noexcept;

    std::int32_t load() const noexcept;
    std::int32_t load_relaxed() const noexcept;
    std::int32_t load_acquire() const noexcept;

    void store(std::int32_t value) noexcept;
    void store_relaxed(std::int32_t value) noexcept;
    void store_release(std::int32_t value) noexcept;

    std::int32_t fetch_add(std::int32_t value) noexcept;
    std::int32_t fetch_sub(std::int32_t value) noexcept;
    std::int32_t fetch_and(std::int32_t value) noexcept;
    std::int32_t fetch_or(std::int32_t value) noexcept;
    std::int32_t fetch_xor(std::int32_t value) noexcept;

    std::int32_t increment() noexcept;
    std::int32_t decrement() noexcept;
    std::int32_t exchange(std::int32_t value) noexcept;
    std::int32_t compare_exchange(std::int32_t desired,
                                  std::int32_t expected) noexcept;
};

class atomic64;
class atomic_ptr;

void memory_barrier() noexcept;

}  // namespace lnx
```

`atomic64` mirrors `atomic32` with `std::int64_t`. `atomic_ptr` mirrors the
load/store/exchange/compare-exchange subset for raw pointer values.

## Linux design

**Fixed-width classes over a template.** The public API uses
`atomic32`, `atomic64`, and `atomic_ptr` because this subsystem is mapping
Windows `Interlocked*`, not rebuilding `std::atomic<T>`. The WinAPI model is
width-specific (`InterlockedIncrement`, `InterlockedIncrement64`,
`InterlockedExchangePointer`), so fixed-width names are more honest at this
layer. If duplication becomes a problem, use an internal `detail::` base
template without changing the public names.

**Backend.** All synchronization goes through GCC/Clang `__atomic_*`
builtins:

| Operation | Linux backend |
|---|---|
| `increment` | `__atomic_add_fetch(..., __ATOMIC_SEQ_CST)` |
| `decrement` | `__atomic_sub_fetch(..., __ATOMIC_SEQ_CST)` |
| postfix increment/decrement | `__atomic_fetch_add` / `__atomic_fetch_sub` |
| `exchange` | `__atomic_exchange_n(..., __ATOMIC_SEQ_CST)` |
| `compare_exchange` | `__atomic_compare_exchange_n` |
| `memory_barrier` | `__atomic_thread_fence(__ATOMIC_SEQ_CST)` |

**Compare-exchange return value.** Windows `InterlockedCompareExchange`
returns the initial value observed at the destination. C++ `std::atomic` and
`__atomic_compare_exchange_n` usually expose success as `bool`. The wrapper
preserves the Windows-style return shape:

```cpp
auto old = value.compare_exchange(desired, expected);
if (old == expected) {
    // exchange happened
}
```

**No volatile storage.** The Windows source used `volatile` because the
WinAPI `Interlocked*` signatures historically take `volatile LONG*`.
Linux correctness comes from `__atomic_*` and the selected memory order,
not from `volatile`, so the backing fields are plain `std::int32_t`,
`std::int64_t`, and `void*`.

**Natural size by default.** `atomic32` is 4 bytes, `atomic64` is 8 bytes,
and `atomic_ptr` is pointer-sized. This keeps embedded refcounts and arrays
reasonable. Hot standalone counters that need false-sharing protection use
`lnx::cache_aligned<T>` explicitly. The alignment is the project ABI
constant `lnx::CACHE_LINE_SIZE` (`64` bytes), not
`std::hardware_destructive_interference_size`, because the standard-library
value may vary by compiler or tuning flags.

```cpp
lnx::atomic32 ref_count;
lnx::cache_aligned<lnx::atomic64> global_counter;
```

## Concurrency & ownership

- All public operations are thread-safe for concurrent access to the same
  wrapper object.
- Wrapper objects are non-copyable and non-movable. Copying atomic state is
  almost always a design smell, and moving it would make ownership of
  shared state ambiguous.
- Default `load()` and `store()` are sequentially consistent. Cheaper
  operations are explicit through `load_relaxed()`, `load_acquire()`,
  `store_relaxed()`, and `store_release()`.
- Arithmetic, bitwise operations, exchange, and compare-exchange use
  sequential consistency as the conservative Interlocked-like baseline.
- Implicit conversion and comparison operators are intentionally omitted so
  atomic reads stay visible at call sites.
- `compare_exchange` uses the strong form of `__atomic_compare_exchange_n`
  and preserves Windows-style return semantics: it returns the observed old
  value, not a success boolean.

## Test plan

Implemented in `tests/sync/atomic_test.cpp`:

- `atomic32` load/store/arithmetic/operator behavior.
- `atomic32` fetch-add/sub and bitwise operations.
- `atomic32::compare_exchange` returns the observed old value.
- `atomic64` increments correctly under multi-threaded contention.
- `atomic_ptr` exchange and compare-exchange operate on pointer identity.
- Atomic wrappers keep natural size; `lnx::cache_aligned<T>` provides
  explicit cache-line alignment.

Future tests:

- TSan run for contended arithmetic and compare-exchange loops.
- Ref-count pattern test once `shared_ptr` / object lifetime code uses this
  wrapper.
- Lock-free stack integration test once `lock_free_stack` lands.

## Open questions

1. **Add explicit memory-order overloads?** Current answer: not yet.
   Adding a project enum like `lnx::memory_order` would make the wrapper
   more general, but also moves it closer to reimplementing `std::atomic<T>`.
2. **Internal template base?** Current answer: wait. The duplicated
   `atomic32` / `atomic64` code is acceptable while the semantics are still
   being studied.
3. **Pointer template?** Current answer: keep `atomic_ptr` as `void*` for
   now. A typed `atomic_ptr<T>` is ergonomic, but the low-level WinAPI
   mapping is pointer-width, not type-aware.
