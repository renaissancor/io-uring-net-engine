# static_vector — inline fixed-capacity vector for pinned objects

> **Status:** landed
> **Source:** `src/sds/static_vector.h`
> **Namespace:** `sds`
> **Depends:** `check`, `types`

## Purpose

Inline fixed-**capacity**, variable-**size** vector: `N` slots live inside the
object (no heap), and `size()` grows `0..N` as `emplace_back` placement-news a
`T` into the next free slot. It exists to hold **non-default-constructible AND
non-movable, address-pinned** types — e.g. `app::handle_worker` — which
`std::array`, `std::vector`, and `sds::malloc_vector` cannot.

## API

```cpp
namespace sds {

template <typename T, usize N>   // N >= 1 (static_assert)
class static_vector {
public:
    static_vector() noexcept;    // leaves slots uninitialized
    ~static_vector() noexcept;   // destroys live elements
    // non-copyable, non-movable

    static constexpr usize capacity() noexcept;   // == N
    usize size()  const noexcept;
    bool  empty() const noexcept;
    bool  full()  const noexcept;

    template <typename... Args>
    T& emplace_back(Args&&... args) noexcept;   // constructs in place; traps on overflow
    void pop_back() noexcept;                    // destroys last; traps on empty
    void clear()    noexcept;                    // destroys all in reverse order

    T&       operator[](usize i) noexcept;       // traps on i >= size()
    const T& operator[](usize i) const noexcept;
    T&       front() noexcept;  T& back() noexcept;    // trap on empty
    T*       data()  noexcept;  const T* data() const noexcept;
    T*       begin() noexcept;  T* end() noexcept;      // live prefix [0, size())
};

}  // namespace sds
```

## Invariants

- Storage is `N` inline slots (a `union { T _slot[N]; }`) — **no heap**.
- **Non-copyable and non-movable**: elements may be address-pinned, so the
  container cannot relocate them.
- Element lifetimes are managed by hand: `emplace_back` constructs; `pop_back` /
  `clear` / the destructor destroy, in reverse construction order.
- **Not thread-safe.** Single-owner by contract.
- Capacity is a hard contract, not a negotiation — see errors.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `emplace_back` when `size() == N` | `LNX_CHECK` trap |
| `pop_back` / `front` / `back` when empty | `LNX_CHECK` trap |
| `operator[]` with `i >= size()` | `LNX_CHECK` trap |

All are always-on traps (debug and release): overflow / out-of-range is a bug,
not a recoverable condition.

## Notes

- The `union` wrapper gives `T`'s alignment for free and leaves slots
  uninitialized without `std::launder` or manual `alignas`.
- `data()`/`begin()` return `_slot`; iteration covers only the live prefix
  `[0, size())`, not the full `N` capacity.

## Test plan

`tests/sds/static_vector_test.cpp`: construct empty; emplace grows size;
`full()` boundary + overflow trap; `pop_back`/`clear` destroy in reverse;
indexed access + OOB trap; iteration over the live prefix; carries a
non-movable element type.

## Done when

- [x] Builds on `default` and `floor` presets
- [x] Tests pass under ASan+UBSan
- [x] This spec matches the built API

## Rationale

- Built to hold `app::handle_worker` (non-movable, address-pinned) in the
  supervisor's LANDLORD worker table — see `design/2026-05-25-handle-engine-split.md`.
- Pairs with the non-movable [[ring_buffer]] storage rule.
