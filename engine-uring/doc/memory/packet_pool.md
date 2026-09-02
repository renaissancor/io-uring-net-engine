# packet_pool — per-thread three-bucket byte pool over one `mmap` region

> **Status:** landed
> **Source:** `src/memory/packet_pool.h` + `src/memory/packet_pool.cpp`
> **Namespace:** `mem`
> **Depends:** `types`, `check`; `{fmt}` for the fatal diagnostics

## Purpose

Fixed-latency allocation for packet-sized objects on the thread that owns
them. Each thread has its own pool: one anonymous `mmap` region carved into
three buckets (64, 256, 1024 B) with intrusive free lists, no atomics, and
no allocation on the hot path once pre-warmed.

## API

```cpp
namespace mem {

class packet_pool {
public:
    static constexpr usize k_bucket_size_64   = 64;
    static constexpr usize k_bucket_size_256  = 256;
    static constexpr usize k_bucket_size_1024 = 1024;

    static constexpr usize k_prewarm_64   = 256;   // blocks per bucket
    static constexpr usize k_prewarm_256  = 256;
    static constexpr usize k_prewarm_1024 = 512;

    static constexpr usize k_bucket_count = 3;

    // Meyers thread_local singleton. First call on a thread mmaps the
    // region; thread exit munmaps it via the dtor.
    static packet_pool& instance() noexcept;

    // Populate intrusive free lists for all three buckets from the arena.
    // Idempotent — safe to call once per worker thread startup.
    void prewarm() noexcept;

    // Hot-path acquire. `size` selects the smallest bucket that fits
    // (<=64 → 64, <=256 → 256, <=1024 → 1024). > 1024 is fatal.
    // Empty free list is fatal.
    void* acquire(usize size) noexcept;

    // Hot-path release. Must be called on the acquiring thread, with the
    // same `size` class that was acquired.
    void release(void* p, usize size) noexcept;

    // Blocks currently outstanding in the bucket holding `bucket_size`-byte
    // allocations.
    usize in_use(usize bucket_size) const noexcept;

    packet_pool(const packet_pool&)            = delete;
    packet_pool& operator=(const packet_pool&) = delete;
    packet_pool(packet_pool&&)                 = delete;
    packet_pool& operator=(packet_pool&&)      = delete;
};

}  // namespace mem
```

Region size is `Σ align_up(bucket_size × prewarm, 16) + 4096` slack:
16 KiB + 64 KiB + 512 KiB + 4 KiB ≈ 596 KiB per thread.

## Invariants

- **Alloc-thread == free-thread.** A block acquired on thread A must be
  released on thread A. The free lists are plain pointers; a cross-thread
  release corrupts the list silently. This is what makes the pool
  atomic-free, and it is the caller's obligation, not a guarded one.
- **Pre-warm before first acquire.** `prewarm()` must run on each thread
  before that thread's first `acquire()`. Lazy fill is not a fallback; it is
  a trap (below).
- **Exhaustion is fatal.** An empty bucket means the pre-warm constant is
  wrong for the workload. The pool does not grow, spill to `malloc`, or
  return `nullptr`.
- Blocks are returned lowest-address-first after a fresh `prewarm()` (the
  free list is built in reverse), which keeps debug dumps predictable.
- One region per thread, mapped in the constructor, unmapped in the
  destructor at thread exit.

## Errors & edge cases

| condition | behaviour |
|---|---|
| `acquire(size)` with `size > 1024` | `LNX_CHECK` trap in bucket selection |
| `acquire()` before `prewarm()` | prints thread id and requested size to stderr, then `LNX_CHECK` trap |
| `acquire()` on an exhausted bucket | prints thread id, bucket size, pre-warm count, and in-use count to stderr, then `LNX_CHECK` trap |
| `release(p, size)` with a different `size` class than acquired | undefined; not guarded |
| `release()` on a different thread than `acquire()` | undefined; not guarded |
| `mmap` or `munmap` failure | `LNX_CHECK` trap |
| `prewarm()` called twice | second call is a no-op |

## Notes

- `instance()` is a function-local `thread_local`, so the region is mapped
  on first touch by each thread, not at process start. A thread that never
  calls `instance()` never maps a region.
- The three bucket sizes and pre-warm counts are compile-time constants.
  Retuning is a rebuild; the exhaustion diagnostic prints the numbers
  needed to pick the new value.
- No spec for a general-purpose or size-class allocator exists here. Earlier
  designs for a 48-class TLS allocator and a typed cs/sc packet pool were
  never built; they are kept as dated deliberation under
  [`../../../design-notes/unbuilt-specs-2026-05/`](../../../design-notes/unbuilt-specs-2026-05/).

## Test plan

`tests/memory/packet_pool_test.cpp`:

- prewarm fills the free list to capacity
- acquire/release round-trip returns the same block
- each size routes to a distinct bucket region
- exhausting a bucket fires `LNX_CHECK` (fatal trap)
- acquire before prewarm fires `LNX_CHECK` (fatal trap)
- thread exit munmaps the region cleanly
- prewarm + acquire works on a freshly spawned thread
