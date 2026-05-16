# session — pre-allocated network I/O state per connection

## Purpose

The `session` struct + its two ring buffers + sequence windows hold all
per-connection network I/O state. Sessions are pre-allocated at server
startup into the **session pool** (Tier 2 of three memory tiers — see
[[threading_model]]) and reused across the server's lifetime. Slots are
never freed back to the OS until process exit; instead, a closed session
returns its slot to the pool's free list for reuse.

This design choice is deliberate:

> Real-time servers prioritize **predictable latency** over memory
> efficiency. Pre-allocating MAX_SESSIONS slots at startup costs RAM the
> OS would have given us anyway. Avoiding runtime growth eliminates the
> failure mode where the server is fast at 1000 clients but laggy at 8000
> clients.

## Slot layout

Each session slot contains:

```cpp
struct alignas(64) session {
    // ─── Identity (read-only after init, no synchronization) ──────────
    uint32_t        session_id;          // slot index in pool
    int             socket_fd;           // assigned at accept
    uint32_t        network_thread_id;   // which network thread services I/O
    uint32_t        content_thread_id;   // which content thread runs handlers
    uint32_t        interaction_unit_id; // hashed at accept; determines content_thread

    // ─── Recv path (network writes, content reads — SPSC) ─────────────
    spsc_ring       recv_ring_buffer;    // 64 KiB raw bytes

    // ─── Send path (content writes, network reads — SPSC) ─────────────
    spsc_ring       send_ring_buffer;    // 16 KiB raw bytes

    // ─── Sequence tracking (content-thread-owned) ─────────────────────
    uint16_t        next_send_seq;       // increments per outgoing packet
    sequence_window recv_seq_window;     // last N sequence numbers seen for dedup

    // ─── State (content-thread-owned) ─────────────────────────────────
    session_state   state;               // CONNECTING / AUTH / ACTIVE / CLOSING
    uint64_t        user_id;             // 0 until auth complete
    // ... game-specific fields (current channel/zone/match pointer, etc.) ...

    // ─── Pool linkage (touched only by accept / close paths) ──────────
    session*        next_free;           // intrusive free-list link when in pool
};
```

`alignof(session) == 64` ensures each session lives on its own cache line
(or set of cache lines) — no false sharing between adjacent slots, which
matters because different threads access different slots concurrently.

## Session pool — process-wide, startup-allocated

```cpp
class session_pool {
public:
    static void init(size_t max_sessions);   // called once at server startup
    static void shutdown();                  // called once at process exit

    static session* claim();                 // pop from free list; returns null if exhausted
    static void     release(session* s);     // clear state, push to free list

    static session* by_id(uint32_t session_id);  // O(1) lookup by slot index

private:
    static std::byte* region_;               // mmap'd region, MAX_SESSIONS * sizeof(session)
    static size_t     capacity_;
    static session*   free_head_;            // intrusive free list
    static lnx::mutex free_list_lock_;       // accept thread + close path contention only
};
```

### Lifecycle

```
[Server startup]
  session_pool::init(MAX_SESSIONS)
    mmap one region for MAX_SESSIONS × sizeof(session)
    initialize every slot's recv_ring_buffer + send_ring_buffer
    thread every slot into the free list

[Connection accept]
  s = session_pool::claim()              // pop from free list
  s->socket_fd = accepted_fd
  s->session_id = (s - region_base) / sizeof(session)
  s->network_thread_id = ...             // accept-policy decision
  s->content_thread_id = hash(interaction_unit_id) % M
  s->state = CONNECTING
  ... register socket with network thread's io_uring ...

[During connection lifetime]
  network thread + content thread access s concurrently
  via SPSC ring buffers + ownership-discipline rules

[Connection close]
  network thread or content thread observes close
  drain any pending io_uring ops for this fd
  s->state = CLOSING
  reset ring buffer cursors
  reset sequence windows
  close socket_fd
  session_pool::release(s)               // push back to free list

[Server shutdown]
  session_pool::shutdown()
    iterate active sessions, force-close
    munmap region
```

## Concurrency discipline

Multiple threads can access the same session slot, but only via specific
disciplined patterns:

| Field                              | Writer                      | Reader            | Mechanism                            |
|------------------------------------|-----------------------------|--------------------|--------------------------------------|
| `recv_ring_buffer` bytes           | network thread              | content thread    | SPSC ring (lock-free)                |
| `send_ring_buffer` bytes           | content thread              | network thread    | SPSC ring (lock-free)                |
| `socket_fd`, `session_id`          | accept thread (init only)   | both              | Read-only after init, no sync needed |
| `state`, `user_id`, game fields    | content thread              | content thread    | Owned by content thread; network doesn't read |
| `next_send_seq`, `recv_seq_window` | content thread              | content thread    | Owned by content thread              |
| `next_free` (pool link)            | accept thread / close path  | accept thread     | Protected by `free_list_lock_`       |

Network thread accesses **only** the two ring buffers and `socket_fd` on
the hot path. It does not read `state`, `user_id`, or game fields. This
minimal-surface discipline prevents most cross-thread cache coherency
traffic.

## Sizing

Per-slot memory:

| Component               | Size       |
|-------------------------|------------|
| `session` struct fields | ~256 B     |
| `recv_ring_buffer`      | 64 KiB     |
| `send_ring_buffer`      | 16 KiB     |
| Padding (cache align)   | <64 B      |
| **Per slot total**      | **~80 KiB**|

For target portfolio scope (`MAX_SESSIONS = 10000`):

- Total pool size: 10000 × 80 KiB = ~800 MiB
- Allocated once via single `mmap()` at startup
- Distributed across no specific thread — pool region is process-wide
- Not part of any TLS Memory budget

For production-scale targets (`MAX_SESSIONS = 100000`):

- Total pool size: 100000 × 80 KiB = ~8 GiB
- Same single mmap, just bigger
- Server machine sized accordingly (32 GiB+ RAM)

## Why the slot, not separate allocations

Allocating `session` + `recv_buffer` + `send_buffer` as separate objects
would work but has drawbacks:

- Three allocations per session vs one
- Three regions of memory per session that may be on different pages
- Slot indexing by `session_id` becomes harder (needs an indirection map)

Embedding all three into one slot:

- One mmap'd region for everything
- One pointer (or one `session_id`) reaches the session + both buffers
- Cache locality between the struct fields and the buffer head/tail
- `session_id` is naturally a slot index → O(1) lookup

## Test plan

- Unit: `session_pool::init(N)` then `claim()` N times returns N distinct
  non-null sessions; `claim()` at N+1 returns null.
- Unit: `release()` followed by `claim()` returns a session in clean
  initial state — buffers empty, state == CONNECTING.
- Unit: `by_id(s.session_id) == &s` for every claimed session.
- Concurrency: 4 threads (simulating network threads) each write to their
  assigned session's `recv_ring_buffer`; 4 other threads (simulating
  content threads) each read from one session's `recv_ring_buffer`.
  Verify no data loss, no torn reads (run under TSan).
- Stress: cycle claim/release 1 M times in a tight loop; verify pool
  stays consistent.
- Memory: assert via `/proc/self/maps` that exactly one large anonymous
  mapping exists for the session pool.

## Open questions

1. **MAX_SESSIONS value.** Compile-time constant or runtime-configurable?
   Compile-time is simpler; runtime gives ops flexibility.
2. **Slot cleanup on release.** What exactly resets between sessions?
   At minimum: ring buffer cursors, sequence windows, state field. Game
   fields the application clears. Define a clean `session::reset()`
   sequence.
3. **Free-list locking.** A single mutex on the free list is fine for
   moderate accept rates but could bottleneck at very high connection
   churn. If churn becomes a hot path, consider lock-free pool free list
   or per-network-thread free-list segments.
4. **Slot zero-init at startup.** Should slots start zero-filled, or is
   field-by-field init sufficient? Zero-init is simpler; field init is
   slightly faster on startup.
5. **NUMA awareness.** On NUMA machines, should the session pool be
   allocated per-socket and slots assigned to network/content threads on
   the same socket? Deferred until profiling indicates NUMA pressure.

## See also

- [[memory_pool]] — TLS Memory tier (game state); explicitly NOT used for sessions
- [[packet_pool]] — Packet pool tier; sister design with same pre-allocation principle
- [[threading_model]] — the three-tier memory model + two-tier reactor
- [[packet_header]] — header read from `recv_ring_buffer` during framing
- `wiki/sync/spsc_ring.md` (deferred) — lock-free SPSC ring used for `recv_ring_buffer` and `send_ring_buffer`
