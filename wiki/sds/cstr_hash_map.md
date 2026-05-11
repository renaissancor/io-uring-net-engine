# cstr_hash_map — chained hash map keyed by `.rodata` C-string literals

## Purpose

A specialized hash map whose keys are raw `const char*` pointing into the
program's `.rodata` segment (string literals). Stores keys by pointer with
zero allocation and zero copy; hashes by content (djb2); compares with a
pointer-equality fast path falling back to `strcmp`.

Primary consumer: `Profiler` (see
`wiki/diagnostic/scope_profiler.md`). Profiler buckets timing samples by
section name, where each section name is a string literal supplied at the
call site (e.g. `Profiler::Enter scope(__func__);`). Since `__func__`,
`__FILE__`, and explicit `"section_xyz"` all live in `.rodata` with
program-lifetime duration, the map can store the key pointer raw — no
`std::string` allocation per insert, no key copy on lookup.

## Why not `std::unordered_map`

- `std::unordered_map<const char*, V>` with the default hasher hashes the
  **pointer value**, not the string content. Two identical literals from
  different translation units (or before linker constant-merging) would map
  to different buckets. Fixing that requires custom `Hash` and `KeyEqual`
  functors, which adds indirection on every lookup.
- `std::unordered_map<std::string, V>` allocates on every insert
  (`std::string` key copy) and, pre-C++20 heterogeneous lookup, also on
  every `find(const char*)`. For a profiler that instruments hot code, the
  allocation IS the measurement noise.
- `cstr_hash_map` inlines djb2, takes the pointer-equality fast path on
  merged literals, and pays zero allocations except for chain-node growth
  on first insert of a new key.

The Windows-side benchmark
(`WindowsLibrary/Library/SPSCMPSCSPMC.txt` and adjacent profiler runs)
showed `cstr_hash_map` materially faster than both `std::unordered_map`
forms for the literal-key workload. The Linux port preserves that
contract.

## Reference origin

- `WindowsLibrary/Library/Include/cstr_hash_map.h:5` — base template.
- `WindowsLibrary/Library/Include/Profiler.h:40` — primary consumer
  (`cstr_hash_map<std::vector<Record>> _records;`).

## Public API sketch

```cpp
namespace sds {

template <typename V>
class cstr_hash_map {
public:
    explicit cstr_hash_map(size_t capacity = 64);
    ~cstr_hash_map() noexcept;

    // Movable, non-copyable (avoid accidental deep-copy on a large table).
    cstr_hash_map(cstr_hash_map&&) noexcept;
    cstr_hash_map& operator=(cstr_hash_map&&) noexcept;
    cstr_hash_map(const cstr_hash_map&)            = delete;
    cstr_hash_map& operator=(const cstr_hash_map&) = delete;

    // Mutators
    V&   operator[](const char* key);          // insert-if-absent, returns ref
    void insert(const char* key, V value);
    void erase(const char* key) noexcept;
    void clear() noexcept;
    void reserve(size_t new_capacity);

    // Lookups
    iterator       find(const char* key) noexcept;
    const_iterator find(const char* key) const noexcept;
    bool           contains(const char* key) const noexcept;

    // Stats
    size_t size()         const noexcept;
    bool   empty()        const noexcept;
    float  load_factor()  const noexcept;

    // Iteration
    iterator       begin() noexcept;
    iterator       end()   noexcept;
    const_iterator begin() const noexcept;
    const_iterator end()   const noexcept;

private:
    struct Node {
        const char* key;   // raw .rodata pointer, NEVER copied/owned
        V           value;
        Node*       next;  // separate-chaining link
    };

    Node** buckets_;       // array of bucket-head pointers
    size_t capacity_;      // current bucket count
    size_t size_;          // node count
    static constexpr float kLoadFactor = 0.75f;
};

} // namespace sds
```

## Linux design

**Key contract — load-bearing.** Keys MUST be `const char*` pointing to
string-literal storage (`.rodata`) or otherwise outlive the map. The map
stores the raw pointer; it never copies or owns the key buffer. Passing
`std::string::c_str()` from a temporary, or any heap-allocated buffer
whose lifetime is not at least the map's, is undefined behavior.

A debug-only sanity check is optional: on insert, walk
`/proc/self/maps` once at startup to learn the `[r--p]` segment range and
assert each new key falls inside it. Disabled by default (off in release,
opt-in in debug via `IOURING_NET_DEBUG_CSTR_KEY_CHECK`).

**Hash function — djb2.** `h = (h * 33) + c` (`h` seeded at 5381). Walked
byte-by-byte until NUL. Empirically faster than FNV-1a for the short keys
(8–48 bytes) typical of function/section names, and avoids the multiply-by-
large-prime cost in FNV-1a's inner loop.

**Comparison — pointer-eq fast path.** First check `a == b` — when
`-fmerge-constants` (default at `-O1`+) has unified duplicate literals
across translation units, the same string at two call sites is the same
pointer, and we can skip `strcmp` entirely. Falls back to inline `strcmp`
on collision (most-likely path is "not equal," so the inline branch is
predictable).

**Collision handling.** Separate chaining via singly-linked list per
bucket. Each `Node` is heap-allocated with `new` — acceptable because:
- The profiler caches its section list at startup; new sections are not
  inserted on the hot path.
- A handful of allocations per `__func__` ever observed is amortized to
  effectively zero.
- A free-list / arena could be added later if section churn is ever
  observed in real use (it isn't expected to be).

**Rehash policy.** When `size_ > capacity_ * 0.75`, double `capacity_` and
reinsert every node. Node objects are reused — only the bucket array is
freed and reallocated. New bucket count is always a power of two so the
modulo can be `hash & (capacity_ - 1)` (current Windows source uses `%`;
change to mask on the Linux port — free win on the hot path).

**Storage alignment.** `Node** buckets_` allocated with `new Node*[cap]()`
(zero-init). For Linux, consider `std::aligned_alloc(64, …)` if profiling
shows bucket-pointer false-sharing in the rare cross-thread scenario.
Default: just use `new[]`; revisit only if measured.

**Exception policy.** All operations declared `noexcept`. `new` failure
(`std::bad_alloc`) is treated as fatal — the map is used by diagnostic
tooling, and the right behavior under OOM is to terminate, not to limp
along with a corrupted index. If the project policy evolves to surface
errors via `tl::expected`, this is the natural breakpoint.

## Concurrency & ownership

- v1: **single-threaded.** Profiler's `Manager` is `thread_local`, so each
  thread holds its own `cstr_hash_map`. No locks, no atomics.
- v1 reporting: cross-thread aggregation is done by the reporter thread
  reading each per-thread map after section work has quiesced (e.g., on
  shutdown or on an explicit `Profiler::Flush()` barrier).
- v2 (out of scope): if real-time cross-thread observation is ever needed,
  options are (a) a single flat-locked map, (b) a striped/sharded variant.
  Both are documented as future work; v1 intentionally takes the
  `thread_local` + offline-merge tradeoff.
- Lifetime: owned by `Profiler::Manager`. Constructed lazily on first use
  (the thread-local singleton), destroyed at thread exit.

## Test plan

- **Unit — basic.** Insert N literal keys, find each, assert values match.
  Erase half, assert remaining set is intact.
- **Unit — rehash.** Insert past `capacity * 0.75`; assert all prior keys
  still findable and `capacity_` doubled.
- **Unit — collision.** Construct two keys that collide under djb2-mod-cap;
  insert both; assert both retrievable and chain length is 2.
- **Unit — pointer-eq fast path.** Insert with literal `"X"`; look up
  with the same literal `"X"` (the compiler must merge them in the same
  TU). Instrument a debug-build counter for "pointer-eq hits vs strcmp
  hits"; assert ratio is 1:0 in this case.
- **Unit — pointer-eq miss.** Insert with `"X"`; look up with a `char[]`
  whose contents are `"X"` but whose pointer differs. Assert the `strcmp`
  fallback returns the value correctly.
- **Property test.** Random insert/find/erase mix of up to 10k literal-keyed
  entries; cross-check final state against `std::map<std::string_view, V>`
  as oracle.
- **Microbench.** Compare against `std::unordered_map<std::string, V>` and
  `std::unordered_map<const char*, V, custom_hash>` with 1k profiler-style
  inserts and 100k lookups; reproduce the Windows-side speedup.

## Open questions

1. **Pointer-eq fast path under separate compilation.** `-fmerge-constants`
   merges within a TU; cross-TU merging depends on identical-COMDAT
   handling at link time (gold/lld typically yes, BFD ld maybe). If two
   TUs each have `"my_section"`, do they share an address in our build?
   Answer empirically with `nm -C build/.../*.o` before relying on the
   fast path being the common case.
2. **Should we switch separate chaining to open addressing (Robin Hood /
   linear probing)?** Better cache locality, no per-node allocation. The
   downside is more complex erase. Given the profiler use case (small N,
   churn-free), the gain is probably not worth the complexity. **Decision:
   stay with chaining; revisit only if microbenchmarks show contention.**
3. **Heterogeneous lookup with `std::string_view`.** Currently impossible —
   the keys are required to be literals. Adding `string_view` lookup would
   weaken the lifetime contract and is intentionally not supported.
