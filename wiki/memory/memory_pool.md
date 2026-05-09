# MemoryPool — 48-class size-segregated allocator

## Purpose

Provide O(1) allocation/free for small objects (≤2048 bytes) by routing every
`xnew<T>()` call through one of 48 pre-bucketed free lists. The pool fragments
the address space at well-known boundaries, eliminates per-alloc free-list
traversal, and gives lock-free push/pop on the free list.

This is the single most important primitive in the project. Every other
subsystem (Session, RingBuffer, JobQueue, packet objects) allocates through it.

## Reference origin

| Component                | File:line                                                            |
|--------------------------|----------------------------------------------------------------------|
| `Memory` (size-class router) | `IOCP_Rookiss/Engine/Memory.h:7`, `Engine/Memory.cpp:6`          |
| `MemoryPool` (one bucket)    | `IOCP_Rookiss/Engine/MemoryPool.h:14`                            |
| Allocation header            | `IOCP_Rookiss/Engine/MemoryPool.h` (`MemoryHeader` struct)       |

The size-class table holds **48** buckets — 32 entries at a 32-byte step
(32 → 1024 bytes), then 16 entries at a 64-byte step (1088 → 2048 bytes).
Hardcoded `constexpr` table at `IOCP_Rookiss/Engine/Memory.h:17-29`. Falls
through to `_aligned_malloc` above 2048. The free list per bucket is a Win32
`SLIST_HEADER`; allocation prepends a 16-byte header so the deallocator can
recover the bucket.

Reference layout (Win32):
```cpp
struct MemoryHeader {
    SLIST_ENTRY _entry;     // 8 B — must be first (SLIST contract)
    int32_t     allocSize;  // 4 B
    int32_t     _padding;   // 4 B → 16 B total
};
```
`MEMORY_ALLOCATION_ALIGNMENT = 16` on x64 is mandated by Windows SLIST. Our
Linux replacement uses a Treiber stack instead and is no longer bound to
that constraint, but we keep 16-byte allocation alignment for cache-line
hygiene anyway.

## Public API sketch

```cpp
namespace iouring_net::mem {

void* alloc(size_t bytes);                    // dispatches to bucket or fallback
void  release(void* p);                       // reads header, returns to bucket

template <class T, class... Args>
T* xnew(Args&&... args);                      // alloc + placement-new

template <class T>
void xdelete(T* p);                           // ~T() + release

template <class T>
T* xnew_array(size_t n);                      // size-class-aware

template <class T>
void xdelete_array(T* p);

} // namespace iouring_net::mem
```

`xnew<T>` is the only allocation API used by other subsystems. Raw `alloc` is
for opaque-byte buffers (`SerialBuffer`, ring storage); `xnew` covers typed
allocations.

## Linux design

**Bucket table.** 48 entries, identical sizes to the reference repo for
parity. Lookup table indexed by `(bytes - 1) >> 5` truncated to log2 spacing
above 256 — same as `Engine/Memory.cpp` build-time table.

**Bucket storage.** Each `MemoryPool` owns one Treiber stack (see
`wiki/sync/lock_free_stack.md`) of `MemoryHeader*`. Push/pop are
lock-free via tagged-pointer CAS.

**Block source.** Each pool requests blocks from the kernel via
`std::aligned_alloc(64, size)` to keep cache lines clean. No `mmap` page
guards in the production allocator; the optional `StompAllocator` (debug
build) uses `mmap` per allocation with a `PROT_NONE` guard page after the
data, equivalent to the reference repo's `VirtualAlloc` + offset trick.

**Header layout (Linux).**

```cpp
struct alignas(16) MemoryHeader {
    MemoryHeader* next;       // 8 B — Treiber-stack intrusive link, kept
                              //       first for parity with the reference's
                              //       SLIST_ENTRY layout
    uint32_t      allocSize;  // 4 B — total bytes including header
    uint32_t      poolIndex;  // 4 B — uint32_t(-1) for >2048 fallback allocs
};
static_assert(sizeof(MemoryHeader) == 16);
```

Every `alloc` returns `header + 1`. Every `release` does
`static_cast<MemoryHeader*>(p) - 1` to recover the bucket index. The
`next` field is repurposed by the lock-free stack while the block sits on
the free list; it is undefined memory while the block is in use by the
caller.

**Fallback.** Bytes > 2048 → `std::aligned_alloc` directly; header records
`poolIndex = uint32_t(-1)`; release goes to `std::free`.

## Concurrency & ownership

- One `Memory` (router) per process. Static storage; constructed at first
  use via `Meyers singleton`. Constructor builds the bucket table and the
  48 `MemoryPool` instances.
- Each `MemoryPool` owns its own Treiber stack. Pools never communicate.
- Push/pop are lock-free; the only contention is per-bucket. Hot paths
  (Session recv buffer alloc, packet object alloc) are typically all in the
  same bucket, so contention is real but bounded.
- ABA: see `lock-free-stack.md` — tagged pointers, 16-bit generation counter
  in the low bits of the (16-byte-aligned) header pointer.
- The `Memory` instance must be a function-local static or a deliberately
  constructed-on-first-use global. Translation-unit static-init order
  fiascos around allocators are a real failure mode in C++.

## Test plan

- Unit: round-trip every bucket size (alloc / write / release) under TSan.
- Unit: alloc 1000 objects of one bucket, release in reverse order, assert
  the pool's free-list count matches.
- Stress: 8 threads × 1M alloc-release pairs across mixed sizes. Run under
  TSan and ASan separately.
- Bench: micro-benchmark vs. plain `malloc`, vs. `std::pmr::pool_resource`,
  on a representative workload (32B / 128B / 512B mix). Numbers are not a
  release criterion; parity with `pmr::pool_resource` within 2× is fine.
- Stomp allocator: regression test that an out-of-bounds write triggers
  `SIGSEGV` rather than corrupting heap.

## Open questions

1. **Per-thread cache layer.** Reference repo has none. Adding a small
   per-thread cache (tcmalloc-style) would reduce CAS traffic. **Defer to
   v2** — measure first.
2. **Bucket sizes.** 48 entries from 32 to 2048 is the reference layout. If
   measurement shows uneven distribution, retune. Don't pre-tune.
3. **Header size.** 16 bytes is wasteful for tiny allocations (32B bucket
   pays 50% overhead). Reference accepts this. **Keep for v1.**
4. **Allocator-free large allocations.** Should >2048-byte allocations also
   record in a per-thread leak-tracker map for debug builds? Yes — see
   `wiki/memory/leak_tracker.md`.
