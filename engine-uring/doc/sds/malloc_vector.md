# malloc_vector — malloc-backed vector for trivially-copyable values

> **Status:** landed
> **Source:** `src/sds/malloc_vector.h`
> **Namespace:** `sds`
> **Depends:** none (`<cstdlib>` malloc/free, `<cstring>`)

## Purpose

A `std::vector`-shaped dynamic array backed by `malloc`/`free` instead of
`new`/`delete`. Its reason to exist is **allocator independence**: subsystems
that must not re-enter the global `new` (e.g. a leak tracker recording
allocations) need a growable container that never calls `new`. Elements are
restricted to trivially-copyable types so growth is a flat `memcpy`.

## API

```cpp
namespace sds {

template <typename T>   // static_assert: T trivially copyable
class malloc_vector {
public:
    explicit malloc_vector(std::size_t capacity = 4) noexcept;
    ~malloc_vector() noexcept;                 // free()s storage
    // non-copyable; MOVABLE (move ctor + move assign, steal-and-null)

    bool        empty() const noexcept;
    std::size_t size() const noexcept;
    std::size_t capacity() const noexcept;

    void reserve(std::size_t) noexcept;        // grow only; memcpy old -> new
    void resize(std::size_t) noexcept;
    void push_back(const T&) noexcept;         // amortized 2x growth
    void pop_back() noexcept;
    void clear() noexcept;                     // size -> 0 (keeps capacity)

    iterator find(const T&) noexcept;          // linear scan; end() if absent
    iterator erase(iterator) noexcept;         // shift-down
    iterator insert(iterator, const T&) noexcept;

    T& at(std::size_t) noexcept;               // NOT bounds-checked (see errors)
    T& operator[](std::size_t) noexcept;
    T& front() noexcept;  T& back() noexcept;
    T* data() noexcept;
    iterator begin() noexcept;  iterator end() noexcept;   // + const overloads
    // random-access iterator / const_iterator nested types
};

}  // namespace sds
```

## Invariants

- Element type is **trivially copyable** (`static_assert`) — growth relocates
  with `memcpy`, and there are no per-element ctor/dtor calls.
- **Non-copyable, movable**: move steals the buffer and nulls the source. (This
  is the opposite of [[static_vector]], which is fully pinned/non-movable.)
- Storage via `malloc`/`free` only — never `new`/`delete`.
- Growth is amortized 2x (`push_back`/`insert` double capacity from a floor of 4).

## Errors & edge cases

- **No bounds checking.** `at`, `operator[]`, `front`, `back` index `_data`
  directly — out-of-range or empty access is **UB** (unlike `std::vector::at`).
  Callers own the bounds contract; there is no `LNX_CHECK`.
- **Soft OOM.** `malloc` failure is not fatal and not thrown: the ctor leaves
  `capacity()==0`; `reserve`/`push_back`/`insert` silently no-op (a `push_back`
  that cannot grow simply does not append). Callers that must not lose data
  should check `size()`/`capacity()` after inserts under memory pressure.
- `find`/`erase`/`insert` on an absent/out-of-range iterator return `end()`.

## Notes

- Uses `==` on `T` for `find`; provide a comparable `T`.
- `resize` grows capacity if needed but does not construct — it only moves the
  logical `size` (values in `[old_size, new_size)` are whatever `malloc` left).

## Test plan

`tests/sds/malloc_vector_test.cpp`: push_back growth + capacity doubling;
reserve/resize; find/erase/insert; move ctor/assign steal semantics;
iteration; clear keeps capacity.

## Done when

- [x] Builds on `default` and `floor` presets
- [x] Tests pass under ASan+UBSan (no leaks: dtor frees)
- [x] This spec matches the built API

## Rationale

- Ported to back allocation-tracking / diagnostic subsystems that cannot re-enter
  `new` — see `doc/00-overview.md` (subsystem inventory, `leak_tracker` note).
- Contrast with [[static_vector]] (inline, pinned) and [[cstr_hash_map]]
  (new/delete nodes).
