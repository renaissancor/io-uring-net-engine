# static_vector — inline fixed-capacity, variable-size vector

## Purpose

`sds::static_vector<T, N>` is a **fixed-CAPACITY, variable-SIZE** vector
whose `N` element slots live **inside the object** — no heap allocation.
`capacity()` is the compile-time constant `N`; `size()` grows `0..N` as
`emplace_back` placement-news a `T` into the next free slot.

It exists because the worker-boot path needs the supervisor to own `n`
`app::handle_worker` objects, and `handle_worker` is simultaneously:

- **non-default-constructible** — its ctor needs `(id, const config&)`,
- **non-movable** — copy and move are `= delete`d, and
- **address-pinned** — a `handle_worker`'s address is handed to its own
  TLS engine via `engine_worker::attach(h)`, so the object must never
  relocate after construction.

No off-the-shelf container satisfies all three. `static_vector` is the
minimal primitive that does. Closest standard analog: C++26
`std::inplace_vector` / `boost::container::static_vector`.

## Reference origin

**Net-new — no Windows source.** Unlike `ring_buffer`, `malloc_vector`,
and `cstr_hash_map` (ported from `WindowsLibrary/`), this type has no
upstream. The Windows codebase leaned on `std::vector` for owning storage;
the no-STL Linux policy (see `docs/04-coding-style.md`) plus the
address-pinning requirement made a bespoke inline container necessary.

## Why not the obvious alternatives

| Candidate | Why it fails for `handle_worker` |
|---|---|
| `std::array<T, N>` | Holds N *already-constructed* T's → requires a default ctor, and size is always exactly N (can't bring up a runtime-chosen `n < N`). |
| `std::vector<T>` | Banned by the no-STL policy; also reallocation **moves** elements → breaks address-pinning and requires movable `T`. |
| `sds::malloc_vector<T>` | `static_assert`s `is_trivially_copyable` and grows via `memcpy` → cannot carry a non-trivial type with a real destructor. |
| plain `T buf[N]` | Needs a default ctor; constructs all N whether used or not. |

`static_vector` constructs **on demand** (no default ctor needed), stores
**in place** (address-stable, never moved), manages lifetimes with
**placement-new + explicit `~T()`** (non-trivial types are correct), and
tracks a **live count** so `size() <= N` is observable.

## Public API (implemented)

```cpp
namespace sds {

template <typename T, usize N>
class static_vector {
public:
    static_vector() noexcept;     // leaves slots uninitialized
    ~static_vector() noexcept;    // destroys the live prefix [0, size)

    // Non-copyable AND non-movable: the elements may be address-pinned,
    // so relocating the container would defeat the purpose.
    static_vector(const static_vector&)            = delete;
    static_vector& operator=(const static_vector&) = delete;
    static_vector(static_vector&&)                 = delete;
    static_vector& operator=(static_vector&&)      = delete;

    static constexpr usize capacity() noexcept;   // == N
    usize size()  const noexcept;
    bool  empty() const noexcept;
    bool  full()  const noexcept;

    // Construct a T in-place at the next free slot, forwarding ctor args.
    // Traps (LNX_CHECK) on overflow. Returns a reference to the element.
    template <typename... Args>
    T& emplace_back(Args&&... args) noexcept;

    void pop_back() noexcept;   // destroys last element; traps on empty
    void clear()    noexcept;   // destroys all live elements (reverse order)

    T&       operator[](usize i)       noexcept;   // traps if i >= size
    const T& operator[](usize i) const noexcept;
    T&       front() noexcept;   const T& front() const noexcept;
    T&       back()  noexcept;   const T& back()  const noexcept;
    T*       data()  noexcept;   const T* data()  const noexcept;

    // Iteration over the live prefix [0, size).
    T*       begin() noexcept;   T*       end() noexcept;
    const T* begin() const noexcept;   const T* end() const noexcept;
};

}  // namespace sds
```

## Linux design

**Storage — union-of-array.** The slots are a single anonymous-union
member `union { T _slot[N]; };`. The union is what makes this both correct
and clean:

- It gives the storage `T`'s natural alignment **for free** — no manual
  `alignas`.
- Naming the array member after a placement-new is well-defined — **no
  `std::launder`, no `reinterpret_cast`** of a `std::byte[]` buffer.
- The empty-body default ctor `static_vector() noexcept {}` intentionally
  leaves the union with no active member, i.e. raw uninitialized slots.

This is deliberately a different storage model from `malloc_vector`
(heap `malloc` + `memcpy` growth) — `static_vector` never copies bytes and
never touches the heap.

**Manual lifetimes.** `emplace_back` does
`::new (&_slot[_size]) T(std::forward<Args>(args)...)` then increments
`_size`. `pop_back`, `clear`, and the destructor call `_slot[i].~T()` in
**reverse construction order**. The destructor delegates to `clear()`, so
leaving scope with live elements is safe (each gets exactly one `~T()`).

**Failure policy — trap, don't negotiate.** `emplace_back` past `N`,
`operator[]` out of range, and `pop_back`/`front`/`back` on an empty
vector all `LNX_CHECK` (fatal software-breakpoint trap). Capacity is a
compile-time contract; overflowing it is a programming error, not a
runtime condition to recover from. This matches `spsc_queue::push` and the
project-wide `LNX_CHECK` philosophy (see `wiki/check.md`). There is no
`try_emplace_back` returning `bool` yet — added only if a consumer needs
graceful-full behavior.

**Minimal API by design.** No `insert` / `erase` / `reserve` / `resize`.
The worker-boot consumer only emplaces during boot and iterates
thereafter; the other operations are added when a concrete consumer needs
them, not speculatively. (Contrast `malloc_vector`, which carries the full
`std::vector`-shaped surface because its diagnostic consumers used it.)

## Concurrency & ownership

- **Single-threaded by contract.** No internal synchronization. The
  intended owner is the supervisor thread, which emplaces all workers
  during boot and then only reads the container. Cross-thread access
  requires external synchronization — but note that *the elements*
  (`handle_worker`) carry their own atomics for cross-thread observation;
  the `static_vector` itself is touched only by its owner.
- **Address stability is the load-bearing guarantee.** Because storage is
  inline and the container never relocates elements, `&v[i]` is stable for
  the lifetime of the container. This is exactly what
  `engine_worker::attach(&worker)` relies on. See
  `wiki/runtime/threading_model.md` and the `.omc` handle/engine split
  design log.
- **Lifetime.** The container owns its elements. Destroying the container
  destroys every live element; there is no ownership transfer (the type is
  non-movable).

## Test plan

**8 Catch2 cases / 30 assertions** in `tests/sds/static_vector_test.cpp`,
green under the default ASan+UBSan preset.

The load-bearing element type, `tracked`, is **non-default-constructible,
non-copyable, non-movable, and self-counts live instances** — deliberately
the same shape as `handle_worker`. Cases:

- `starts empty` — flags/size/capacity on a fresh vector.
- `emplace_back grows size and returns the element` — `size()` increments,
  returned ref is usable, `operator[]`/`front()`/`back()` read back.
- `fills to capacity then reports full` — reaching N flips `full()`.
- `pop_back destroys only the last element` — live-count drops by exactly
  one; `back()` points at the prior element.
- `clear destroys every live element` — live-count returns to 0.
- `destructor destroys the live prefix exactly once` — leave scope with 10
  live elements; dtor runs each `~T()` once (ASan would catch a leak or
  double-free).
- `carries non-movable elements without relocating them` — capture
  `&v[0]`, emplace more, assert the address is unchanged. **This is the
  worker-boot invariant.**
- `iterates the live prefix in insertion order` — `begin()/end()` walk
  `[0, size)`.

**Not tested:** the overflow / out-of-range `LNX_CHECK` traps — they abort
the process by design, and there is no in-process death-test harness.

## Open questions

1. **`try_emplace_back` (graceful full).** Currently full is a trap. If a
   consumer ever wants to attempt-and-fall-back instead of asserting, add a
   `bool try_emplace_back(...)` sibling rather than changing the trapping
   contract of `emplace_back`.
2. **`insert` / `erase`.** Omitted until needed. Mid-vector erase on a
   non-movable type would have to be destroy-and-compact via re-construction
   (no element moves possible), which changes element addresses — so erase
   would *break* the address-stability guarantee for elements after the
   removed index. Only add it for consumers that don't depend on pinning.
3. **Shared use by SoA session/room storage.** Sessions and rooms also want
   an in-place, non-relocating, fixed-cap pattern (see
   `wiki/network/session.md`). Decide whether they reuse `static_vector<T,
   N>` directly or get a specialized SoA container — the column layout may
   want parallel arrays rather than an array of structs.
4. **`N == 0`.** Currently `static_assert(N >= 1)`. A zero-capacity
   specialization is meaningless for the worker use case; revisit only if a
   generic consumer needs an always-empty instance.
