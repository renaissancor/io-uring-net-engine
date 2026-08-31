# cstr_hash_map — chained hash map keyed by C-string pointers

> **Status:** landed
> **Source:** `src/sds/cstr_hash_map.h`
> **Namespace:** `sds`
> **Depends:** none (`<utility>` for `std::move`/`std::pair`)

## Purpose

A separate-chaining hash map with `const char*` keys, the no-STL substitute for
`std::unordered_map<const char*, V>` where keys are stable string literals (e.g.
profiler scope names in `.rodata`). djb2 hash, power-of-two bucket count,
0.75 load factor.

## API

```cpp
namespace sds {

template <typename V>
class cstr_hash_map {
public:
    explicit cstr_hash_map(std::size_t capacity = 64);   // rounded up to power of 2
    ~cstr_hash_map() noexcept;
    // non-copyable; MOVABLE (steal-and-null)

    std::size_t size() const noexcept;   bool empty() const noexcept;
    std::size_t capacity() const noexcept;   float load_factor() const noexcept;
    void reserve(std::size_t) noexcept;  // grow only

    iterator find(const char* key) noexcept;          // end() if absent (+ const)
    bool     contains(const char* key) const noexcept;
    void     insert(const char* key, V value) noexcept;  // upsert
    V&       operator[](const char* key) noexcept;       // insert-default if absent
    void     erase(const char* key) noexcept;
    void     clear() noexcept;

    iterator begin() noexcept;  iterator end() noexcept;  // forward iterators (+ const)
    // reference exposes .key()/.value() and {first, second}
};

}  // namespace sds
```

## Invariants

- **Keys are borrowed, not owned.** The map stores the `const char*` pointer
  verbatim (no copy/strdup). Every inserted key **must outlive the map** — the
  intended keys are `.rodata` string literals. Comparison is by string content
  (`cstr_cmp`), lookup is by djb2 hash then content compare.
- Bucket count is always a power of two; `bucket_index = hash & (capacity-1)`.
- Auto-rehash (2x) when `size+1 > capacity * 0.75`.
- **Non-copyable, movable** (steals `buckets_`, nulls the source).
- `insert` and `operator[]` **upsert**: an existing key's value is overwritten
  (`insert`) or returned by reference (`operator[]`).

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `find`/`contains` on absent key | `end()` / `false` |
| `operator[]` on absent key | inserts `V{}` and returns the reference |
| `insert` on existing key | value overwritten (move-assigned) |
| `erase` on absent key | no-op |
| key pointer freed while still mapped | **UB** — keys must outlive the map |

Node storage uses `new`/`delete` (unlike [[malloc_vector]]); allocation failure
throws `std::bad_alloc` (not caught here — treated as fatal by the process).

## Notes

- Iterator is forward-only; `reference` is a lightweight `{const char* first;
  V& second;}` with `.key()`/`.value()` accessors so both `it->second` and
  `it.value()` read.
- `next_power_of_two` rounds the requested capacity up at construction and on
  `reserve`.

## Test plan

`tests/sds/cstr_hash_map_test.cpp`: insert/find/contains; `operator[]`
insert-default + upsert; erase; collision chaining; auto-rehash across the load
factor; iteration visits every entry; move ctor/assign.

## Done when

- [x] Builds on `default` and `floor` presets
- [x] Tests pass under ASan+UBSan (no node leaks: dtor/clear delete chains)
- [x] This spec matches the built API

## Rationale

- Intended for profiler scope tables keyed by literal names — see [[profiler_scope]].
- Borrowed-key contract is the key design decision; documented so no caller
  passes a transient buffer.
