# MemoryPool — 48-class size-segregated allocator (per-thread)

## Purpose

Provide O(1) allocation/free for small objects (≤2048 bytes) by routing every
`xnew<T>()` call through one of 48 pre-bucketed free lists. The pool fragments
the address space at well-known boundaries, eliminates per-alloc free-list
traversal, and avoids any synchronization on the hot path because each thread
owns its own pool.

This is the single most important primitive in the project. Every other
subsystem (Session, RingBuffer, JobQueue, packet objects) allocates through it.

## Reference origin

| Component                    | File:line                                                            |
|------------------------------|----------------------------------------------------------------------|
| `Memory` (size-class router) | `IOCP_Rookiss/Engine/Memory.h:7`, `Engine/Memory.cpp:6`              |
| `MemoryPool` (one bucket)    | `IOCP_Rookiss/Engine/MemoryPool.h:14`                                |
| Allocation header            | `IOCP_Rookiss/Engine/MemoryPool.h` (`MemoryHeader` struct)           |

The size-class table holds **48** buckets — 32 entries at a 32-byte step
(32 → 1024 bytes), then 16 entries at a 64-byte step (1088 → 2048 bytes).
Hardcoded `constexpr` table at `IOCP_Rookiss/Engine/Memory.h:17-29`. The Win32
implementation uses a `SLIST_HEADER` free list per bucket (lock-free) and
`_aligned_malloc` for block sourcing. **Our Linux design diverges materially**:
each thread gets its own `Memory` instance via `thread_local`, so the bucket
free lists need no synchronization at all.

Reference layout (Win32):
```cpp
struct MemoryHeader {
    SLIST_ENTRY _entry;     // 8 B — must be first (SLIST contract)
    int32_t     allocSize;  // 4 B
    int32_t     _padding;   // 4 B → 16 B total
};
```

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

**Concurrency model: TLS singleton.** A `thread_local Memory` instance per
thread. Same pattern as `profiler_scope` and `logger` elsewhere in the project.
Constructor and destructor run automatically per thread (C++11 magic statics
guarantee thread-safe lazy init; reverse-construction destruction order at
thread exit).

```cpp
inline Memory& instance() {
    thread_local Memory inst;
    return inst;
}
```

This makes the alloc/release fast paths synchronization-free: every operation
touches only the current thread's own free lists and bump cursor. The cost is
the **alloc-thread == free-thread invariant** — see [Concurrency & ownership](#concurrency--ownership)
below.

**Block source: one `mmap` region per thread.** The constructor reserves the
thread's entire memory budget (`MEMORY_BUDGET_PER_THREAD`, default 64 MiB) in
a single `mmap` call. The destructor returns the whole region with `munmap`.
No chunk lists, no growth, no `madvise`.

```cpp
constexpr size_t MEMORY_BUDGET_PER_THREAD = 64ull * 1024 * 1024;

Memory::Memory() {
    void* p = ::mmap(nullptr, MEMORY_BUDGET_PER_THREAD,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    LNX_CHECK(p != MAP_FAILED);
    base_   = static_cast<std::byte*>(p);
    cursor_ = base_;
    end_    = base_ + MEMORY_BUDGET_PER_THREAD;
}

Memory::~Memory() {
    ::munmap(base_, MEMORY_BUDGET_PER_THREAD);
}
```

Rationale for picking `mmap` over `::malloc` / `std::aligned_alloc`:

- Real budget enforcement — going past 64 MiB is an error, not silent glibc growth.
- Real reclaim on thread exit — `munmap` returns pages to the OS immediately.
- Page-aligned by construction — no need for `aligned_alloc(4096, …)`.
- Zero glibc allocator interaction — no arena contention, no `MALLOC_TRIM` quirks.
- The stomp allocator (debug) falls out naturally with `mprotect(PROT_NONE)` on a guard page.

We accept the worst-case 64 MiB-per-thread RAM commitment as a deliberate
tradeoff. Per-thread budget waste is cheap on server-class hardware; allocator
determinism is not.

**Bucket table.** 48 entries, identical sizes to the reference repo for parity.
Lookup is a small `constexpr` array indexed by `(bytes - 1) >> 5` truncated to
a higher step above 1024 — same shape as `Engine/Memory.cpp`'s build-time
table.

**Bucket storage.** Each bucket is a plain intrusive free list:

```cpp
MemoryHeader* free_list_[48]{};   // 48 heads, all null at ctor
```

Alloc pops the head; release pushes onto the head. No atomics, no Treiber
stack, no tagged pointers — `thread_local` makes contention impossible.

**Alloc path.**

```cpp
void* Memory::alloc(size_t bytes) {
    const uint32_t idx = bucket_index(bytes + sizeof(MemoryHeader));
    if (idx == LARGE_ALLOC) {
        return large_alloc(bytes);              // separate mmap, see Fallback
    }
    const size_t block = bucket_size(idx);

    MemoryHeader* h = free_list_[idx];
    if (h != nullptr) {
        free_list_[idx] = h->next;              // pop
    } else {
        LNX_CHECK(cursor_ + block <= end_);     // budget exhausted == fatal
        h = reinterpret_cast<MemoryHeader*>(cursor_);
        cursor_ += block;                       // bump
    }
    h->allocSize = static_cast<uint32_t>(block);
    h->poolIndex = idx;
    return h + 1;
}
```

**Release path.**

```cpp
void Memory::release(void* p) {
    auto* h = static_cast<MemoryHeader*>(p) - 1;
    if (h->poolIndex == LARGE_ALLOC) {
        large_free(h);                          // munmap, see Fallback
        return;
    }
    h->next = free_list_[h->poolIndex];
    free_list_[h->poolIndex] = h;               // push
}
```

**Header layout.**

```cpp
struct alignas(16) MemoryHeader {
    MemoryHeader* next;        // 8 B — intrusive free-list link (undefined
                               //       while the block is in use by caller)
    uint32_t      allocSize;   // 4 B — total bytes including header
    uint32_t      poolIndex;   // 4 B — bucket # (0..47), or LARGE_ALLOC for >2048
};
static_assert(sizeof(MemoryHeader) == 16);
```

`poolIndex` identifies the bucket only — it does *not* identify the owning
thread. Under the TLS singleton model, the "owning thread" is implicit: it's
whichever thread runs `release`, and that thread MUST be the same as the one
that ran `alloc` (see invariant below).

**Fallback: bytes > 2048.** Routed to a separate `mmap`/`munmap` per
allocation, rounded up to a page boundary:

```cpp
void* Memory::large_alloc(size_t bytes) {
    const size_t total = round_up_to_page(sizeof(MemoryHeader) + bytes);
    void* p = ::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    LNX_CHECK(p != MAP_FAILED);
    auto* h = static_cast<MemoryHeader*>(p);
    h->next      = nullptr;
    h->allocSize = static_cast<uint32_t>(total);
    h->poolIndex = LARGE_ALLOC;                // sentinel value (e.g., uint32_t(-1))
    return h + 1;
}

void Memory::large_free(MemoryHeader* h) {
    ::munmap(h, h->allocSize);
}
```

Large allocations bypass the pool region entirely, so they don't fragment the
bump cursor's address space. The tradeoff is one syscall per large alloc/free,
which is acceptable because large allocations should be rare on hot paths
(typically: connection-scoped buffers, batch payloads).

## TLS construction-order anchor

`thread_local` objects are destroyed at thread exit in **reverse construction
order** within that thread. If other TLS singletons (`logger`, `profiler_scope`)
call `mem::alloc` or `mem::release` during their own destructors, then `Memory`
must be constructed *first* on every thread so it is destroyed *last*.

This is guaranteed by touching `mem::instance()` once at the very top of every
`lnx::thread` trampoline, before the user-supplied thread function runs:

```cpp
// inside lnx::thread's start-routine wrapper
extern "C" void* lnx_thread_trampoline(void* arg) {
    (void)iouring_net::mem::instance();        // anchor Memory as ctor[0]
    auto* ctx = static_cast<thread_ctx*>(arg);
    ctx->fn();
    return nullptr;
}
```

Same technique used by mimalloc and the libc++ TLS-destructor ordering shims.

## Concurrency & ownership

- **One `Memory` per thread**, lazy-initialized on first `mem::alloc` /
  `mem::release` call by that thread (or by the trampoline anchor above).
  Lifetime is bound to the thread, not to the process.
- **Alloc-thread == free-thread invariant.** A block allocated on thread A
  MUST be released on thread A. This is the central design constraint of the
  TLS singleton model. Violating it pushes the block onto a different thread's
  free list, which silently corrupts both threads' bookkeeping (the wrong pool
  thinks the bump cursor moved farther than it did; the right pool thinks it
  has fewer blocks in flight than it does).
- **No atomics.** Every free list and the bump cursor are plain `T*` fields.
- **No ABA, no tagged pointers, no lock-free stack.** All eliminated by the
  TLS model.
- **Thread exit = full reclaim.** `munmap` returns the entire 64 MiB region
  to the OS in one syscall.

The alloc-thread == free-thread invariant constrains the higher-level
threading model to "each worker thread runs its own io_uring ring end-to-end
(submit + completion + handler)" rather than a reactor with separate I/O and
worker thread pools. This matches IOCP_Rookiss's per-worker GQCS topology.

Objects do not cross thread boundaries directly. When data must move between
threads, it is **copied** via a per-thread MPSC inbox: the sender serializes
the bytes onto the destination's inbox, the destination thread pops the bytes
and allocates a fresh object in its own TLS pool. See [[threading_model]] for
the full rules. Packets / `serial_buffer` are an explicit carveout (their
size makes copy semantics wasteful) and have a separate design path.

## Test plan

- Unit: round-trip every bucket size (alloc / write / release) on a single
  thread. Assert free-list head pointer state after a known sequence.
- Unit: alloc 1000 objects of one bucket, release in reverse order, assert
  free-list count matches and bump cursor never moved past the first batch.
- Unit: large-alloc (>2048) round-trips — confirm `mmap`/`munmap` are called
  and the returned pointer is page-aligned.
- Unit: budget exhaustion — alloc until `LNX_CHECK` fires; confirm clean abort.
- Multi-thread isolation: 8 threads each allocate/release 1M blocks on their
  own pool, verify zero cross-thread interference (no shared state to break).
- Thread exit: spawn a thread, alloc blocks, exit thread, assert via
  `/proc/self/maps` (or `pmap`) that the 64 MiB region is unmapped.
- TLS construction-order anchor: spawn a thread whose user fn instantiates
  `logger` / `profiler_scope`, confirm `Memory` is still alive during their
  destructors at thread exit.
- Stomp allocator: regression test that an out-of-bounds write triggers
  `SIGSEGV` rather than corrupting heap.

## Open questions

1. **Budget value.** 64 MiB per thread is a starting point. May need
   per-thread-class tuning (e.g., accept threads 64 MiB, worker threads
   256 MiB). Decide once we have a real workload to measure.
2. **Bucket sizes.** 48 entries from 32 to 2048 is the reference layout. If
   measurement shows uneven distribution, retune. Don't pre-tune.
3. **Header size.** 16 bytes is wasteful for the smallest buckets (32 B
   payload pays 50% overhead). Reference accepts this. **Keep for v1.**
4. **Allocator-free large allocations.** Should >2048-byte allocations also
   record in a per-thread leak-tracker map for debug builds? Yes — see
   [[leak_tracker]].
5. **Cross-thread handoff** — **resolved by [[threading_model]].** Data
   moves between threads via byte-copy through per-thread MPSC inboxes,
   never by pointer sharing. Each thread allocates and frees only in its
   own TLS pool; the alloc-thread == free-thread invariant holds globally
   without escape hatches. Packets / `serial_buffer` are a carveout (their
   size makes copy semantics wasteful) and have a separate design path
   tracked in the threading-model doc.
