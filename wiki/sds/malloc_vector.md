# malloc_vector — malloc-backed vector for trivially copyable values

## Purpose

`sds::malloc_vector<T>` is a small `std::vector`-shaped dynamic array
that uses `malloc` / `free` instead of `operator new` / `operator delete`.
It exists for diagnostic and memory-tracking code paths where using
standard containers would either be too high-level for the subsystem or
could recurse through allocation tracking.

Primary current consumer: `profiler::manager`, which stores timing
records as `cstr_hash_map<malloc_vector<record>>`.

## Reference origin

- `WindowsLibrary/Library/Include/malloc_vector.h:5` — base template.

The Linux port keeps the source recognizable but adds:

- `sds::` namespace.
- explicit move constructor / move assignment so it can be stored inside
  `cstr_hash_map`.
- a `static_assert` limiting `T` to trivially copyable values, matching
  the raw `malloc` + assignment + `memcpy` implementation model.
- `nullptr`-safe `end()` for moved-from zero-capacity vectors.

## Public API sketch

```cpp
namespace sds {

template <typename T>
class malloc_vector {
public:
    explicit malloc_vector(size_t capacity = 4) noexcept;
    ~malloc_vector() noexcept;

    malloc_vector(const malloc_vector&)            = delete;
    malloc_vector& operator=(const malloc_vector&) = delete;
    malloc_vector(malloc_vector&&) noexcept;
    malloc_vector& operator=(malloc_vector&&) noexcept;

    iterator begin() noexcept;
    iterator end() noexcept;
    const_iterator begin() const noexcept;
    const_iterator end() const noexcept;

    bool   empty() const noexcept;
    size_t size() const noexcept;
    size_t capacity() const noexcept;

    void reserve(size_t capacity) noexcept;
    void resize(size_t size) noexcept;
    void push_back(const T& value) noexcept;
    void pop_back() noexcept;
    void clear() noexcept;

    iterator find(const T& value) noexcept;
    iterator erase(iterator pos) noexcept;
    iterator insert(iterator pos, const T& value) noexcept;

    T& operator[](size_t index) noexcept;
    const T& operator[](size_t index) const noexcept;
    T* data() noexcept;
    const T* data() const noexcept;
};

} // namespace sds
```

## Design

**Storage.** `_data` is allocated with `std::malloc(sizeof(T) * capacity)`
and released with `std::free`. Growth allocates a new buffer, copies
existing bytes with `memcpy`, then frees the old buffer.

**Type contract.** `T` must be trivially copyable. This is intentional:
the container does not run constructors or destructors for elements, and
it moves storage with `memcpy`. Use `std::vector` for general C++ object
storage.

**Failure policy.** Allocation failure is non-throwing. `reserve()` leaves
the vector unchanged if `malloc` returns null; `push_back()` drops the
append if capacity still cannot grow. This mirrors the Windows source's
`noexcept` behavior.

**Move support.** The Windows source deletes copy and has no move
operations. The Linux port adds move ownership because `cstr_hash_map`
stores values by moving them into nodes.

## Test Plan

- **Landed — construction.** Empty vector reports requested capacity.
- **Landed — growth.** `push_back()` grows and preserves values.
- **Landed — reserve/resize.** Capacity and size update as expected.
- **Landed — insert/erase/find.** Iterator-position mutation works.
- **Landed — pop/clear.** Size changes without freeing capacity.
- **Landed — const iteration.** Const iterators read all values.
- **Landed — move.** Move construction and assignment transfer storage.
- **Landed — aggregate records.** Trivial aggregate values store and
  compare correctly.

## Open Questions

1. **Should allocation failure be observable?** The current Windows-style
   contract silently preserves the old vector or drops a push. If callers
   need stronger behavior, add `bool reserve(...)` / `bool push_back(...)`
   variants or use project `expected`.
2. **Should this become a full object container?** Supporting non-trivial
   `T` requires placement construction, destruction, and exception
   policy decisions. For now, keep it as a raw storage tool for trivial
   diagnostics/memory records.
