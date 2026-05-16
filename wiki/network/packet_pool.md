# packet_pool — pre-allocated cs_packet / sc_packet per content thread

## Purpose

The packet pool holds **transient packet objects** — `cs_packet` (client→
server, incoming) and `sc_packet` (server→client, outgoing). It is **Tier 3
of three memory tiers** (see [[threading_model]]).

Like the [[session]] pool, packet pool entries are pre-allocated at server
startup and reused. Unlike the session pool, packet pool free lists are
**per-content-thread** because packets are content-thread-local in
lifetime — no cross-thread allocation or freeing happens on the hot path.

The design choice mirrors the session pool's rationale:

> Predictable latency over memory efficiency. Pre-allocate at startup so
> allocation never grows with load. A server that's fast at 1000 clients
> and slow at 8000 clients is broken for a real-time workload.

## Why a dedicated pool (not TLS Memory)

`cs_packet` and `sc_packet` could technically be allocated through the TLS
Memory pool ([[memory_pool]]) since they're content-thread-local. We use
a dedicated pool instead because:

1. **Predictable O(1) allocation under bursts.** TLS Memory bucket
   exhaustion is possible under packet bursts (recv 1000 chat messages
   in 16 ms); pre-allocated buckets in the packet pool eliminate that
   failure mode entirely.
2. **Fixed-size buckets per packet size class** — same bucket geometry
   as TLS Memory, but pre-populated. No bump-cursor advance, no first-
   touch faulting.
3. **Cleaner ownership story.** Packet pool is for *packets only*; TLS
   Memory is for *game state only*. Each pool has one job.
4. **Easier debugging.** Packet allocation rate, bucket exhaustion
   alerts, slow-packet detection — all per-bucket-counter-friendly
   when the pool is dedicated.

## Packet object layout

Both `cs_packet` and `sc_packet` are thin headers over an mmap'd byte
region. The header captures bucket metadata so the allocator can return
the right slot to the right free list.

```cpp
struct alignas(16) cs_packet {
    uint16_t       bucket_index;   // size class — used by free path
    uint16_t       size;           // actual packet size (≤ bucket size)
    uint32_t       _padding;       // align body to 8 B for the wire bytes that follow
    std::byte      body[];         // flexible array — body bytes
};

struct alignas(16) sc_packet {
    uint16_t       bucket_index;
    uint16_t       size;
    uint32_t       _padding;
    std::byte      body[];
};

static_assert(sizeof(cs_packet) == 16);
static_assert(sizeof(sc_packet) == 16);
```

The 8 B header bytes (the project's [[packet_header]]) live in `body[0..7]`,
followed by the payload — exactly as they appear on the wire. The
allocator-side `bucket_index` + `size` fields are pool bookkeeping and
do **not** go on the wire.

Optional: in v1, `cs_packet` and `sc_packet` may share an identical
underlying struct (`packet`) with the type distinction enforced by
typed pointer aliases. Whether to actually keep them as separate types
is open — see [Open questions](#open-questions).

## Pool structure

```cpp
namespace iouring_net::net {

constexpr size_t PACKET_BUCKET_COUNT = 8;  // tuned to packet-size histogram
constexpr size_t packet_bucket_size(size_t i);   // e.g., 64, 128, 256, 512, 1024, 2048, 4096, 16384

struct packet_bucket {
    packet*  free_head;          // intrusive free list
    uint32_t available;          // count for accounting / starvation alarm
};

struct packet_pool_per_thread {
    packet_bucket buckets[PACKET_BUCKET_COUNT];
    std::byte*    region_base;   // mmap'd backing store for THIS content thread
    size_t        region_size;
};

// Per-content-thread instance, accessed via thread_local pointer
thread_local packet_pool_per_thread* tls_packet_pool;

// Allocation API
cs_packet* alloc_cs_packet(size_t bytes_needed);
sc_packet* alloc_sc_packet(size_t bytes_needed);
void       free_cs_packet(cs_packet* p);
void       free_sc_packet(sc_packet* p);

} // namespace iouring_net::net
```

### Bucket sizing

A reasonable starting bucket geometry (subject to measurement):

| Bucket index | Size class | Typical use                                |
|--------------|------------|--------------------------------------------|
| 0            | 64 B       | tiny control packets, pings, acks          |
| 1            | 128 B      | small chat, simple commands                |
| 2            | 256 B      | medium chat, position updates              |
| 3            | 512 B      | larger chat, small state updates           |
| 4            | 1 KiB      | inventory snippets, small lists            |
| 5            | 2 KiB      | medium state updates                       |
| 6            | 4 KiB      | recv buffer-sized                          |
| 7            | 16 KiB     | large state dumps, near-max-size packets   |

Above the largest bucket, packet allocation **fails** (returns null) and
the connection is dropped. The server should never have to allocate >16 KiB
single packets; if it tries, that's an application bug.

Bucket count and exact sizes are an [Open question](#open-questions).

### Per-thread pre-allocation

At server startup, each content thread receives its own pre-built packet
buckets:

```
For each content_thread T in [0, M):
    For each bucket B in [0, PACKET_BUCKET_COUNT):
        N_B packets pre-allocated in T's pool
        threaded into T's bucket B's free list
```

The per-bucket count `N_B` is sized for the expected concurrent packet
in-flight at that bucket — measurement-driven. A reasonable initial
guess: enough packets per bucket to handle the worst-case burst (e.g.,
2× MAX_SESSIONS_PER_CONTENT_THREAD per bucket for the most common buckets).

### Lifecycle

```
[Server startup]
  packet_pool::init(num_content_threads)
    For each content thread:
      mmap one region (sum of all buckets' total bytes)
      Carve into packets, thread into per-bucket free lists
      Store region pointer in that thread's TLS pool slot

[During gameplay]
  Content thread T allocates:
    p = alloc_cs_packet(size)
      bucket = bucket_for_size(size)
      pop p from tls_packet_pool->buckets[bucket].free_head
      set p->bucket_index = bucket, p->size = size
      return p
  ... handler uses p ...
  Content thread T frees:
    free_cs_packet(p)
      bucket = p->bucket_index
      push p onto tls_packet_pool->buckets[bucket].free_head

[Server shutdown]
  packet_pool::shutdown()
    For each content thread: munmap region
```

All hot-path operations are O(1) and synchronization-free — content
thread T touches only `tls_packet_pool` for thread T.

## Concurrency discipline

Packets are **content-thread-local in lifetime**. They're allocated when
the content thread drains bytes from `session.recv_ring_buffer` (creating
a `cs_packet`) or builds a reply (creating an `sc_packet`), and freed
inside the same tick after the handler runs or the bytes are copied to
`session.send_ring_buffer`.

Therefore:

- **Allocator state is per-content-thread.** Each content thread has its
  own `packet_pool_per_thread` accessed via `thread_local`.
- **No cross-thread coordination on the hot path.** No atomics, no locks
  on `alloc_*` / `free_*`.
- **Network thread never touches packet objects.** It writes raw bytes to
  `session.recv_ring_buffer` (no packet object yet) and reads raw bytes
  from `session.send_ring_buffer` (no packet object anymore). The typed
  `cs_packet` / `sc_packet` exist only inside the content thread.

This is exactly the same invariant as TLS Memory ([[memory_pool]]):
alloc-thread == free-thread for content-thread allocations. The packet
pool just enforces it via a dedicated bucket pool instead of the general
TLS Memory pool.

## Sizing

For target portfolio scope (16 content threads, MAX_SESSIONS_PER_CONTENT
= 625, conservative per-bucket counts):

| Bucket | Size  | Packets per content thread | Bytes per thread |
|--------|-------|----------------------------|-------------------|
| 0      | 64 B  | 4096                       | 256 KiB           |
| 1      | 128 B | 4096                       | 512 KiB           |
| 2      | 256 B | 2048                       | 512 KiB           |
| 3      | 512 B | 2048                       | 1 MiB             |
| 4      | 1 KiB | 1024                       | 1 MiB             |
| 5      | 2 KiB | 512                        | 1 MiB             |
| 6      | 4 KiB | 256                        | 1 MiB             |
| 7      | 16 KiB| 64                         | 1 MiB             |
|        |       | **Per content thread**     | **~6 MiB**        |

Across 16 content threads: **~100 MiB total packet pool**, allocated
once at startup, distributed as 16 separate mmap regions (one per content
thread for NUMA / cache locality).

These counts are starting points. Real sizing depends on observed packet
rates and burst patterns — see [Open questions](#open-questions).

## Allocation path

```cpp
cs_packet* alloc_cs_packet(size_t bytes_needed) {
    auto* pool = tls_packet_pool;
    LNX_CHECK(pool != nullptr);  // pool must be initialized for this thread

    uint16_t bucket = bucket_for_size(bytes_needed);
    if (bucket >= PACKET_BUCKET_COUNT) {
        return nullptr;          // packet too large; caller drops connection
    }

    auto& b = pool->buckets[bucket];
    if (b.free_head == nullptr) {
        return nullptr;          // bucket exhausted; alarm + drop
    }

    cs_packet* p = b.free_head;
    b.free_head = p->next_free;   // intrusive list
    b.available--;

    p->bucket_index = bucket;
    p->size = static_cast<uint16_t>(bytes_needed);
    return p;
}
```

The `next_free` field overlays `body[0..7]` while the packet sits in the
free list — same trick as TLS Memory's `MemoryHeader`. Reusing the body
bytes for free-list linkage keeps the packet object exactly 16 B + body.

## Test plan

- Unit: `init` + per-bucket count of `alloc_cs_packet(b_size)` returns N
  distinct non-null packets; one more returns null.
- Unit: `alloc` then `free` returns to the right bucket; consecutive
  `alloc` returns the freshly-freed packet (LIFO).
- Unit: `alloc_cs_packet(size > 16 KiB)` returns null without affecting
  any bucket state.
- Concurrency: 16 content threads each running 100 k alloc/free cycles
  against their own packet pool; assert no cross-thread interference and
  no torn state under TSan.
- Stress: under sustained bucket pressure (alloc at rate near bucket
  saturation), verify graceful behavior — null return at exhaustion, no
  crash, no leak.

## Open questions

1. **Bucket count and sizes.** 8 buckets at the geometry above is a
   starting point. The right answer depends on the actual packet-size
   histogram — measure once we have a real workload.
2. **Per-bucket counts.** Same — needs measurement. The starting
   per-bucket counts may be too generous for some buckets and too lean
   for others.
3. **Separate types vs single type.** Should `cs_packet` and `sc_packet`
   be distinct C++ types (with separate allocators), or one underlying
   struct with typed aliases? Distinct types give compile-time safety
   ("can't accidentally write to a cs_packet") but cost a bit more
   allocator code. Lean toward distinct types.
4. **Bucket exhaustion policy.** When a bucket is fully empty:
   - (a) Return null, caller drops connection. Simple, harsh.
   - (b) Fall through to the next-larger bucket. Graceful, wastes space.
   - (c) Steal from another content thread's pool. Cross-thread cost.
   For v1: (a). Revisit if measurement shows real bucket starvation.
5. **NUMA pinning.** On NUMA machines, each content thread's packet
   region should be allocated from the same NUMA node as the thread.
   Use `numa_alloc_onnode` (libnuma) or `mbind` after mmap. Deferred
   until profiling demands it.
6. **Pool-bucket parity with TLS Memory.** Should packet bucket sizes
   match TLS Memory's 48 buckets, or be a deliberately different,
   coarser geometry? Coarser is fine here because packets are uniform
   (only ~3 distinct types) while TLS Memory serves arbitrary game-state
   types.

## See also

- [[memory_pool]] — TLS Memory tier (game state); explicitly NOT used for packets
- [[object_pool]] — typed wrapper over TLS Memory for game-state types
- [[session]] — Session pool tier; sister design with same pre-allocation principle
- [[threading_model]] — three-tier memory model + content-thread-local packet lifetime
- [[packet_header]] — 8 B wire header that lives at the start of `body[]`
