# Server Architecture Discussion — 2026-05-19

Discussion record between Stephen Park and Claude (Opus 4.7) on the architectural
foundations of `engine-uring`. Captures decisions reached, the reasoning behind
them, and open questions still on the table.

---

## TL;DR

The architecture of `engine-uring` rests on seven load-bearing decisions:

1. **No multithreading in the content layer.** Real-time service requires single-threaded content execution. This is a hard constraint, not a v1 simplification.
2. **Channel = one content thread.** Each interaction zone (chat room, game match, MMO zone) runs on exactly one user-space thread, with its own io_uring instance and its own session ring buffers.
3. **Three-category object bifurcation.** Every type falls into exactly one of: channel-local (TLS pool), globally shared mutable (singleton + lock), cross-channel event (POD message).
4. **Kernel/content layer split.** io_uring's kernel side handles socket ↔ ring buffer byte transport; the content thread does everything else (framing, dispatch, handlers, mutation).
5. **Packets are channel-local.** Framing happens in the content thread. No separate user-managed "worker thread" for packet generation.
6. **Ring buffers are SPSC, designed with memory barriers — no atomic CAS / no LOCK-prefix instructions.** The kernel and the content thread are the producer-consumer pair; each writes one index, reads the other. Ordering is enforced by acquire/release barriers, not interlocked atomics.
7. **Content threads never block on other threads' locks.** Sibling principle to #1. Content threads are sealed off from all cross-thread coordination costs. All cross-thread reads use RCU snapshots or async request/reply; all cross-thread writes are lock-free queue pushes. Locks exist but live in non-content threads where milliseconds don't matter.

These decisions imply a much simpler architecture than the IOCP-era spec inherited from `IOCP_Rookiss`. The wiki specs are now ahead of the actual design intent on several axes and owe updates.

**Final calibration (added 2026-05-19 evening):** The architecture is now **clear and conventional.** The io_uring + SPSC ring buffer + single-thread-per-channel combination is a well-understood design pattern shared by Seastar, Tigerbeetle, and modern real-time multiplayer game servers. Once you commit to the principles, "where does this state live" and "which thread does this work" have unambiguous answers. The remaining work is engineering (io_uring API surface, lifecycle edge cases, observability), not architecture.

---

## Part 1: The root principle — no MT in content layer

**Stated 2026-05-19, verbatim:** *"multithreading in content layer interaction is not possible in real time service."*

This is the foundational constraint of the project. Not optimization, not v1-only simplification — the hard floor that real-time service correctness rests on.

### The four properties it preserves

1. **Determinism.** Replay, anti-cheat verification, server-authoritative validation, and lockstep simulation all require that the same input sequence produces the same state. Cross-thread mutation interleaving destroys this.
2. **Bounded tail latency.** Single-thread strands have predictable per-tick cost. Locks introduce unbounded tail latency under contention — fine for batch systems, fatal for "every player must hear about this event within 50 ms."
3. **Atomicity of chained mutations.** A single content event ("cast spell → reaction → leaderboard update → broadcast") is a dependent mutation chain that must observe a consistent intermediate state. Multi-thread access means every read needs a snapshot and every chain needs coordination — death by a thousand locks.
4. **Reasoning cost.** Content logic written against single-thread invariants is a fundamentally different (and smaller, more correct) program than the same logic written for multi-thread safety.

### Industry precedent

- **WoW / Blizzard** — single-threaded per zone, network I/O folded into zone thread.
- **EVE Online** — single-threaded per solar system; their "time dilation" feature is literally slowing the one thread under load. Could not work with multi-threaded simulation.
- **Riot Games (LoL, Valorant)** — single-threaded per match.
- **Roblox** — single-threaded per place.
- **Source 2 (Dota 2, CSGO)** — single-threaded per match.
- **ScyllaDB / Seastar** — explicit shard-per-core philosophy; one reactor per core does I/O, framing, application logic, and userspace TCP stack.
- **Tigerbeetle** — single-threaded io_uring reactor on purpose; benchmarks show unified beating multithreaded for their workload.
- **Erlang/BEAM** — per-process state, message-passing only.
- **LMAX Disruptor** — single-writer principle across pipeline stages.

### Implications across the architecture

All other architectural decisions in this project derive from this principle:

- Allocator → TLS-resident `ObjectPool<T>` per channel
- `job_queue` per-entity → unnecessary (thread is the strand)
- Reactor → per-channel io_uring, not shared
- Coroutines → ergonomic choice within a channel, not concurrency primitive
- Cross-channel coordination → message passing only, never shared state

---

## Part 1.5: Sibling principle — content threads never block

**Stated 2026-05-19:** *"entire server architecture to handle this kind of issue should be based on not decreasing the performance of all content threads existing. Other threads getting blocked can be handleable by UX."*

This is a **load-bearing performance principle** that sits alongside the no-MT-in-content principle from Part 1. Both protect the content thread's serial budget:

- **No MT in content** (Part 1): content thread itself is never multi-threaded — no within-thread locks needed
- **Content threads never block** (this part): content thread is never blocked by locks held by *other* threads

Together: **the content thread runs in complete isolation from all other threads' synchronization concerns.**

### The asymmetry

| Thread role | Hot path? | Allowed to hold locks? | Latency budget |
|---|---|---|---|
| Content thread (per channel) | Yes | No | Microseconds |
| Accept / gateway thread | No | Yes | Hundreds of ms (UX absorbs) |
| Session registry (data structure) | No (from content thread's perspective) | Yes (for writers) | 1–10 ms per lookup |
| Broadcaster | No | Yes | 1–50 ms per fan-out |
| Janitor / lease checker | No | Yes | Seconds, runs occasionally |

The content thread is hot path; everything else is slow path by design. The slow path absorbs latency that the UX can hide.

### Implementation patterns (from content thread's perspective)

| Operation | Required pattern |
|---|---|
| Content → other thread (writes) | Lock-free MPSC inbox push. Never wait. |
| Content reading cross-thread state | RCU-style snapshot (atomic pointer to immutable map), or async request/reply. **`shared_mutex` is NOT acceptable** — even reader-side lock acquisition can briefly contend. |
| Content publishing for other threads | Push POD message; don't wait for ack. |

### The trap to avoid

Many "shared-nothing" architectures violate this principle on the slow path. A lookup in some global table acquires a mutex, blocks briefly under contention, and the "real-time" thread is no longer real-time. Symptom: p99 latency is 10× p50 because rare contention events stall the hot path.

This principle eliminates that failure mode by **construction**, not by hoping contention is rare. Locks exist in the codebase — they just don't live anywhere the content thread can encounter them.

---

## Part 2: Channel-per-thread architecture

### Channel = one content thread

A **channel** is a "place where interaction between members happens" (game-server zone, chat room, match instance, etc.). Each channel pins to a single user-managed thread, called the **content thread**.

- **One content thread per channel.** No content multi-threading within a channel.
- **One io_uring instance per content thread.** Per-channel reactor; no cross-channel reactor sharing.
- **One session ring buffer per Session** (recv + send), all owned by the content thread.
- **Content layer caps entities per channel** — population is bounded and thread-local.

### Why raw `pthread_create` instead of `std::thread`

Resolved 2026-05-19. The choice is explicit lifecycle control over the channel context. `std::thread` is rejected because:

1. It forwards exceptions to `std::terminate` — incompatible with the project's no-exceptions LNX_CHECK + `tl::expected` policy.
2. It hides controls Stephen actually needs — CPU affinity (`pthread_setaffinity_np`), scheduling policy/priority, stack-size/guard-page attrs.
3. Its joinable-state machinery adds nothing the channel doesn't already manage.

`thread_local T` for the pools themselves is also rejected because destruction order across TUs is unspecified and there's no place to drain in-flight work before destruction.

### The `channel_context` pattern

Stack-scoped `channel_context` struct + `thread_local channel_context*` pointer for fast access:

```cpp
struct channel_context {
    io_uring                    ring;
    ObjectPool<Session>         session_pool;
    ObjectPool<SerialBuffer>    packet_pool;
    ObjectPool<entity_player>   player_pool;
    ObjectPool<entity_npc>      npc_pool;
    serial_buffer               scratch;
    recv_ring_buffer            recv_ring;
    send_ring_buffer            send_ring;
    mpsc_inbox                  inter_channel_inbox;
};

thread_local channel_context* g_channel = nullptr;

void* channel_thread_main(void* arg) {
    // 1. bare-metal init (name, affinity, signal mask) — must come first
    auto* params = static_cast<channel_params*>(arg);
    pthread_setname_np(pthread_self(), params->name);
    pin_to_core(params->core_id);
    setup_signal_mask();

    // 2. construct ctx on stack; assign g_channel; signal ready barrier
    channel_context ctx{ params->ring_depth, params->entity_caps };
    g_channel = &ctx;
    params->ready_barrier.signal();

    // 3. main loop
    run_channel_loop(ctx);

    // 4. CRITICAL: drain in-flight CQEs + inter-channel inbox BEFORE ctx destructs
    drain_in_flight_cqes(ctx);
    drain_inter_channel_inbox(ctx);

    // 5. clear g_channel before ctx goes out of scope
    g_channel = nullptr;

    // 6. ctx destructs here, in reverse field-decl order
    return nullptr;
}
```

### Critical ordering invariant

**Drain (step 4) must finish before `ctx` destructs** so no in-flight CQE callback or queued message can land on a freed pool. Declare `ring` last in `channel_context` so it tears down after the pools — defense in depth.

---

## Part 3: Three-category object bifurcation

Every type `T` in the project falls into exactly one of three categories, and each has its own primitive:

| Category | Access pattern | Primitive | Lock? |
|---|---|---|---|
| **Channel-local** (entities, sessions, packets/SerialBuffer, ring buffers) | One content thread, high churn | TLS `ObjectPool<T>` per channel (or stack-local for SerialBuffer / non-owning `frame_view` for inbound packets) | None — sole owner |
| **Globally shared mutable** | All threads, usually long-lived | Global instance (often singleton) | Yes — mutex by default |
| **Cross-channel event** | Producer thread → consumer thread, transfer-once | Value-typed POD message | None — copied, no shared state |

These are **three different primitives**, not one parameterized `ObjectPool<T>` with a "thread-safe?" template flag. Such a flag would be a leaky abstraction and a silent-race generator.

### Why ObjectPool<T> doesn't extend to cross-thread

Stated 2026-05-19: *"if it become inter thread, then object pool must be global, and should need lock mechanism. honestly Object Pool template architecture would be very unlikely."*

Three reasons:

1. `ObjectPool<T>::make_shared` is intrinsically single-owner — the pool is the lifetime authority for T. Cross-thread free would return to a foreign TLS pool, defeating the whole point.
2. Cross-thread `shared_ptr` ref-count atomics stop being free (uncontended under TLS, contended cross-thread).
3. "Same T in two channels" is ambiguous — shared identity (lock) or copy (data)?

### "Global pool" usually collapses to "global singleton + lock"

Pools optimize for high allocation/deallocation churn. Most cross-channel state is long-lived:

- **Server-wide leaderboard** → `std::unique_ptr<leaderboard> + std::mutex`. There's one.
- **Broadcast broker / subscriber list** → one instance, mutated on subscribe/unsubscribe.
- **Global config** → one instance, mostly read-only; `shared_mutex` if reads dominate.
- **Auth/session registry** → *this* is the unusual case where a pool helps because login churn is real. Different class from channel-local `ObjectPool<T>`.

### Lock choice priority

1. `lnx::mutex` (once release-mode bug is fixed) — default
2. `lnx::shared_mutex` — many readers, few writers
3. RCU / seqlock — extremely read-heavy, rare writes
4. Lock-free — only if profiling proves the mutex is the bottleneck

The TLS-local hot path is where the perf lives. The global path is by definition the slow path.

### Type-system enforcement (open decision)

Channel-local vs globally-shared is a category distinction the type should ideally advertise. Options:

1. **Convention only** — `entity_player` is local because we say so. Mistakes silent.
2. **Namespace separation** — `channel_local::entity_player` vs `global::leaderboard`. Visible at use site. (Most production codebases land here.)
3. **Tag types / wrapper templates** — `channel_local<T>` vs `global_shared<T, Lock>`. Compile-time enforced.

Worth picking before too many entity types exist.

---

## Part 4: Kernel/content layer boundary

The cleanest mental model is two layers separated by the io_uring boundary:

```
┌─────────────────────────────────────────────────────────────────┐
│ KERNEL SIDE (io_uring — "workers" in IOCP-era terminology)      │
│                                                                 │
│   socket  ◄─────── byte transport ────────►  ring_buffer        │
│                                                  (in user mem,  │
│                                                   owned by      │
│                                                   Session)      │
└─────────────────────────────────────────────────────────────────┘
                              ▲
                              │  (content thread submits SQEs,
                              │   drains CQEs)
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ CONTENT THREAD (one per channel/zone, user-managed)             │
│                                                                 │
│   ring_buffer  ◄──── framing ────►  SerialBuffer / frame_view   │
│   (recv/send)                       (a "packet")                │
│                                          │                      │
│                                          ▼                      │
│                                      dispatcher                 │
│                                          │                      │
│                                          ▼                      │
│                                      handler → mutate entities  │
│                                          │                      │
│                                          ▼                      │
│                                      build response             │
│                                          │                      │
│                                          ▼                      │
│                                      back to ring_buffer        │
└─────────────────────────────────────────────────────────────────┘
```

### What each layer does

**Below the line (kernel):**
- Moves bytes between TCP socket and ring buffer (user memory).
- Doesn't know what a packet is. Doesn't know what a Session is.
- Just writes to / reads from the address you gave it in the SQE.

**Above the line (content thread):**
- Everything semantic — framing, dispatcher, handlers, entity mutation, response building.
- Submits SQEs telling the kernel WHERE to put/get bytes.
- Drains CQEs to learn what happened.

### Terminology clarification

"Worker thread" in IOCP / io_uring context means the **kernel-side** completion or submission thread that moves bytes between TCP and the ring buffer transparently — **not a user-managed thread**. The user-side thread in this project is the **content thread** (one per channel), which submits SQEs, processes CQEs, frames packets, and runs content handlers. There is exactly one content thread per channel; there is no separate user-managed "worker" thread.

### Without SQPOLL

For typical case (no `IORING_SETUP_SQPOLL`):

```
content thread:                kernel:
  build SQE (recv into          
   ring_buffer[offset])    →    
  io_uring_submit()        →    socket → ring_buffer[offset]
                                (asynchronous, when packet arrives)
  io_uring_wait_cqe()      ←    CQE: recv completed, N bytes
  frame packet from
   ring_buffer
  ...
```

SQPOLL dedicates a kernel thread per ring to poll the SQ without needing `io_uring_enter`. Worth enabling only under very high QPS.

---

## Part 4.5: Ring buffer implementation — SPSC with memory barriers

The recv/send ring buffers between kernel and content thread are **SPSC (single-producer, single-consumer)**, designed with memory barriers only — **no atomic CAS, no LOCK-prefixed instructions, no interlocked operations required**.

### Why SPSC by construction

For the **recv ring**:
- Producer: io_uring kernel side (writes bytes into the buffer at head, advances head)
- Consumer: content thread (reads bytes from tail, advances tail)
- Exactly one writer of each index

For the **send ring**:
- Producer: content thread (writes bytes into the buffer at head, advances head)
- Consumer: io_uring kernel side (reads bytes from tail, advances tail)
- Exactly one writer of each index

Each ring has one producer and one consumer with disjoint roles. Neither side ever races the other for writes to the same index. The SPSC property falls out of the kernel/content layer split — it's not a separate design choice, it's a consequence of the architecture.

### Why memory barriers but NOT atomic CAS

In SPSC:
- The producer writes only to head, reads only tail
- The consumer writes only to tail, reads only head
- They both READ each other's index but WRITE only their own
- **No compare-and-swap is needed; only ordering matters**

The required ordering protocol:

```
Producer side (e.g., io_uring kernel writing recv data):
  1. write bytes into buffer[head ... head + N]
  2. release-barrier (ensures bytes are visible before head update)
  3. head = head + N  (single-writer, no CAS)

Consumer side (e.g., content thread reading recv data):
  1. observed_head = head  (acquire-load, sees writes preceding the release)
  2. read bytes from buffer[tail ... observed_head]
  3. release-barrier (optional, for any data the consumer writes back)
  4. tail = observed_head  (single-writer, no CAS)
```

### Why this matters for performance

`std::atomic<size_t>` with default `seq_cst` ordering would also work, but it would emit unnecessary full memory fences on every load and store. On x86 it's a small cost (LOCK-prefixed CAS instructions); on ARM it's significant.

The right primitives are direct `__atomic_*` builtins with explicit ordering — which is what `src/sync/atomic.h` already provides (`load_acquire`, `store_release`, etc.). The ring buffer should use these rather than `std::atomic`.

### x86 vs ARM specifics

- **x86 (TSO — Total Store Ordering)**: most ordering is automatic. Loads have implicit acquire semantics; stores have implicit release semantics. You usually only need a compiler barrier (`asm volatile("" ::: "memory")`) on the producer side to prevent reordering of the data writes after the head update. Acquire/release semantics in code are mostly free.
- **ARM (weaker model)**: requires actual `dmb ish` (data memory barrier, inner-shareable domain) instructions for acquire/release. Implementations using `__atomic_*` builtins with explicit ordering generate these correctly.

### Convention

This is the **same SPSC pattern io_uring uses internally** for its own SQ/CQ rings (kernel writes CQ tail, user reads CQ tail; user writes SQ head, kernel reads SQ head). The pattern is decades old, well-understood, and avoids the per-operation cost of LOCK-prefix atomics. Modeling our user-space session ring buffers the same way is the conventional choice.

### "Interlock atomic is not necessary"

The Windows `InterlockedXxx` family (and equivalent CAS-style atomic operations) are designed for the **multi-writer or contended-update case** — exactly what SPSC rings don't have. Avoiding them here isn't an optimization; it's correctness. Using `InterlockedCompareExchange` style operations on a single-writer index would be:
- More expensive (full memory fence on every update)
- Misleading (signals to readers of the code that contention is expected when it isn't)
- Wrong tool for the job

---

## Part 5: Packet handling

### Packets are channel-local content actions

Packets are content actions (move, attack, chat, broadcast intent) — produced and consumed entirely within the content loop, never crossing threads. Same TLS bifurcation category as entities and session buffers.

### Spec's current shape (zero-copy)

- **Inbound** = `frame_view` (non-owning slice into the recv ring buffer). Zero-copy, zero allocation. The recv ring IS the packet storage.
- **Outbound** = stack-local `SerialBuffer` (4 KiB scratchpad declared in the handler that's building the response).

This is leaner than pool-allocating packets — sidesteps the pool entirely.

### When pooled packets earn their keep over zero-copy

- Packets need to **outlive the immediate handler** (queued for delayed dispatch, fan-out broadcast, retained for replay).
- Packets are **too big for stack** (jumbo payloads, blob attachments).
- Handler is a coroutine that **suspends mid-packet-build**.
- You want **uniform allocation discipline** — every channel-local type goes through `ObjectPool<T>`.

### Why splitting packet generation into "workers" is wrong

Considered and rejected for two reasons:

**1. Kernel-side worker literally cannot do it.** Kernel doesn't know your packet format. Framing is application code by definition.

**2. User-managed worker (split model) breaks ownership.** Framing state is per-Session: where the next frame begins in the ring, partial-frame handling, malformed-frame decisions, backpressure. That state is intimate with Session state and content-handler state. If a worker manages framing, the Session object now straddles a thread boundary — exactly what the TLS bifurcation was designed to prevent.

### Performance analysis (per-packet overhead)

For simple 4-byte-header POD packets:

| Stage | Unified content thread | Split (POD-copy, clean variant) |
|---|---|---|
| Frame header check | ~5 ns | ~5 ns |
| Decode payload to handler input | ~10–30 ns | ~10–30 ns |
| Cross-thread handoff (inbound) | — | ~50–150 ns |
| Handler runs | variable (same) | variable (same) |
| Build response | ~10–20 ns | ~10–20 ns |
| Cross-thread handoff (outbound) | — | ~50–150 ns |
| **Total non-handler overhead** | **~25–55 ns** | **~125–355 ns** |

Splitting adds ~100–300 ns of pure overhead per packet. For real-time service that cares about p99 latency, it's a strict regression.

### When splitting WOULD win

Only when per-packet parsing cost exceeds ~500 ns. Crossover examples:

| Parse workload | Per-packet parse cost | Worth splitting? |
|---|---|---|
| 4-byte POD framing | ~5–30 ns | **No** |
| Simple XOR obfuscation | ~50 ns/KB | No |
| AES-GCM decrypt (1 KB, AES-NI) | ~1–3 μs | Yes |
| zstd decompress (1 KB) | ~2–3 μs | Yes |
| Protobuf with deep nesting | ~500 ns – several μs | Yes |
| JSON parse | ~1–5 μs | Yes |

For Stephen's design (simple POD, no crypto), splitting is pure overhead.

### Industry practice

io_uring-native systems are overwhelmingly unified: Seastar, Tigerbeetle, Redpanda, Cloudflare Pingora. Real-time game servers (WoW, EVE, Riot, Roblox, Source 2) all do single-thread-per-zone with I/O folded in.

The split pattern is mostly outside io_uring: LMAX Disruptor (HFT, JVM), HFT firms with kernel-bypass networking, TLS terminators. The IOCP-era "worker pool + logic thread" pattern was forced by Windows IOCP's fungible-thread-pool model — io_uring lets you escape it.

---

## Part 5.5: Within-tick parallelism for compute-heavy sub-steps

Even with single-thread-per-channel, the content thread's tick can use parallel sub-steps for embarrassingly-parallel work. The architecture explicitly permits this — workers operate on read-only snapshots and merge results back into the canonical state on the content thread.

Common candidates:
- **Pathfinding** for many monsters/units (each entity's path is independent)
- **FSM updates** per entity (each entity's state transition is independent)
- **AOI queries** per player (each query is independent)
- **Animation/cosmetic updates** (per-entity, read-only of game state)

The tick structure becomes:

```
content thread tick:
  1. read incoming packets             ─── serial (content thread)
  2. dispatch handlers (mutate intent) ─── serial (content thread)
  3. ┌─ PARALLEL FAN-OUT ─┐
     │ pathfind monster 1 │
     │ pathfind monster 2 │ ─── worker pool, read-only snapshot
     │ pathfind monster N │
     └─────── JOIN ───────┘
  4. ┌─ PARALLEL FAN-OUT ─┐
     │ FSM tick monster 1 │
     │ FSM tick monster 2 │ ─── worker pool, read-only snapshot
     │ FSM tick monster N │
     └─────── JOIN ───────┘
  5. merge results into world state    ─── serial (content thread)
  6. compute AOI + build broadcasts    ─── potentially parallel per recipient
  7. submit network SQEs               ─── serial (content thread)
```

This is exactly how AAA engines structure their tick (Unreal's Mass Entity, Unity DOTS, Bevy ECS).

### What this is NOT

This is **not** multi-threading the architecture. The architecture stays single-thread-per-channel. The thread pool is a *tool* used during compute-heavy sub-steps, the way SIMD or GPU offload would be used. The content thread still owns canonical state; workers operate on read-only snapshots and the merge step is serial.

### When this is worth doing

Only when profiling shows compute-heavy sub-steps (pathfinding, AI) dominating the serial budget. For v1 of `engine-uring` and the squad-commander game (~140 entities per zone), this is unnecessary — single-threaded tick handles the load comfortably.

### For the portfolio writeup

Document it as a known optimization path you considered and chose not to implement. The architecture explicitly permits it; you're holding it in reserve until profiling justifies the complexity. Something like:

> *"Within-tick parallelism (parallel pathfinding, parallel FSM updates) is a known optimization for raising per-zone entity capacity. Not implemented in v1; documented as a future direction once profiling shows AI/pathfinding dominating the serial budget. The architecture explicitly permits this — workers operate on read-only snapshots and merge back, so the single-thread-per-channel contract is preserved."*

Reviewers reading that paragraph see "this engineer can distinguish architectural from optimization concerns" — exactly the senior signal worth advertising.

---

## Part 6: Session ownership

### Content thread owns its sessions

Session is a channel-local type:

- Only the content thread reads/writes the Session's ring buffers, drains its CQEs, frames its packets, runs its handlers.
- Session contains channel-local primitives (ring buffers, possibly SerialBuffer scratch) that are already TLS.
- Session lifetime is bounded by the content thread's lifetime.
- Cross-thread `shared_ptr` deleter concern from `doc/memory/object_pool.md` evaporates — Session is single-thread-owned.

### Accept-handoff pattern

How sessions arrive at the content thread:

1. **Direct accept** — content thread owns the listening socket (or shares via `SO_REUSEPORT`), accepts new connections inline, constructs Session in its own pool. No cross-thread anything.
2. **Gateway hand-off** — a separate gateway/login thread accepts + authenticates + routes to the correct channel. Should hand off **only the accepted fd + connection metadata as a POD message** (not a constructed Session object). Content thread constructs its own Session from POD.

Either works; option 2 is normal for game servers because login/auth/zone-routing happens before the player lands in a zone. The key: never pass a Session pointer across threads.

### Session migration as a separate layer

If sessions need to move between content threads (player crosses zone boundary):

```
[content thread A]                           [content thread B]
  serialize Session state → POD msg
   (player_id, position, inventory, ...)
  destroy local Session                ───▶  receive POD msg
   (return to A's TLS pool)                  allocate new Session from B's TLS pool
                                             restore state from POD
                                             register Session in B's session table
```

The Session object itself never moves — only the player's persistent state does, as POD. No pointer crossing, no shared ownership.

For TCP connection migration: gateway-proxy model (TCP terminates at gateway, channel change updates routing table) is the standard production solution. Socket handoff via Unix domain sockets / `SCM_RIGHTS` is harder and rarely done.

---

## Part 7: Scaling limits

### Single CPU core is the ceiling

This is the fundamental scaling limit of the architecture. Per-channel capacity ≈ tick budget ÷ per-player cost.

Rough budget math:
- One core, ~30 Hz tick → 33 ms per tick
- Per-player tick cost: 10–100 μs depending on content
- Per-channel capacity: **300 to 3,300 players** realistically

### Real game numbers

| Game | Players per shared context | How they manage |
|---|---|---|
| MOBAs (LoL, Dota 2) | 10 | Trivially fits |
| Halo / CoD matches | 12–24 | Trivially fits |
| Battle royale | 100 | Comfortable fit |
| Battlefield | 128 | Tight; good AOI |
| WoW zone | 150–200 | Heavy AOI + instance sharding |
| EVE Online battle (peak) | ~5,000 | **Time dilation** — slow the tick |

### Ways to push past the ceiling

1. **Spatial sharding (zones/instances)** — standard MMO answer. The channel concept IS this.
2. **Time dilation (EVE)** — slow simulation tick under load.
3. **AOI culling** — each player only sees N nearest entities. Required above ~50 players.
4. **Distributed simulation** — Star Citizen Server Meshing, SpatialOS. Bandwidth + latency become new bottleneck.
5. **Feature sharding within a zone** — physics on one thread, AI on another. Breaks the no-MT principle; rarely worth it.
6. **GPU offload** — collision/AOI on GPU.

### Where this architecture stops applying

- MOBA-sized through battle-royale-sized (10–150 players): wildly under ceiling, idle CPU
- MMO zone-sized (150–500 with AOI): achievable, ceiling becomes design constraint
- Mega-event scale (1,000+ in one context): need time dilation or AOI brutality
- EVE-scale or persistent 10,000+ shared zones: distributed simulation territory; different problem

### Is the single-core limit fundamentally escapable?

Worth being explicit because this question came up in discussion. The honest answer: **practically yes, fundamentally no, but the escapes are either equivalent, worse, or radically more complex.**

### Why DB engines can do what game engines can't

DB engines can binary-level optimize concurrent access because their abstraction layer permits it. Games can't, for the inverse reasons:

| DB property | Game zone equivalent |
|---|---|
| Operations have known semantics (read/write by key) | Handlers do arbitrary computation |
| Data has natural sharding by key | Interactions are non-local by definition |
| Conflicts are rare in OLTP workloads | Conflicts ARE the gameplay (combat, trades, collisions) |
| Cost of conflict = transaction retry | Cost of conflict = gameplay desync (unacceptable) |
| Operations reorderable within isolation level | Ordering IS the gameplay |
| Optimistic concurrency works | Optimistic fails — interactions are the workload |
| Schema enables ahead-of-time optimization | Each game has unique state mutations |

DBs sidestep the problem because their abstraction layer is constrained — operations are well-defined, conflicts rare, isolation relaxable. Games have none of those constraints, which is precisely why the same techniques don't carry over.

### What CAN be done within current technology

You can squeeze more out of one core; you cannot multiply the serial portion:

| Technique | Realistic gain | What it changes |
|---|---|---|
| **ECS / struct-of-arrays / cache-friendly layout** | 5–10× | Per-core throughput up; logic still serial |
| **SIMD / AVX-512 for repetitive ops** | 2–8× on parallel sub-steps | Within-tick parallelism only |
| **GPU offload** (physics, batch raycasts, AOI queries) | 10–100× on parallel sub-steps | Decisions still on CPU |
| **Sub-step parallelism within a tick** (Part 5.5) | 2–4× | Independent per-entity work parallelized; mutation step stays serial |
| **Hyper-optimized single-thread code** (inlining, branch prediction, prefetch) | 20–50% | Per-core IPC marginally higher |
| **Better CPUs** (caches, clock, IPC) | ~20–30% per 5 years | Has plateaued |

These compound: well-optimized ECS engines do 10–100× the work per core vs naive implementations. Real and significant. But the **serial decision-making step at the heart of each tick stays single-threaded.** You're making the core faster, not escaping its limit.

### What you fundamentally cannot do

Causally coherent state mutation is **by definition** serial. If A's attack on B triggers B's reaction triggers C's attack, those decisions must happen in order to preserve causal coherence. This is a fundamental limit of classical computation on coherent shared state, not an engineering limitation that better effort can overcome.

### The honest escape hatches

| Escape | What it costs | Used by |
|---|---|---|
| **Spatial sharding (more zones)** | Players in different zones can't interact in real-time | All MMOs |
| **Time dilation** | Players perceive slow motion | EVE Online |
| **AOI brutality** | Players only see N neighbors; rest approximated | Required for >50 players in one zone |
| **Soft sharding within a zone** (spatial subzones with sync at borders) | Breaks under "deathball" — activity converges to one subzone | Some custom MMO architectures |
| **Distributed simulation** (multi-machine one zone) | Enormous complexity; network is new bottleneck | Star Citizen Server Meshing (~10 years in dev), SpatialOS (failed commercially) |
| **Lockstep peer-to-peer** | Vulnerable to cheating; requires perfect determinism; not server-authoritative | RTS games (StarCraft 1, AoE) |
| **Rollback netcode** (predict + verify + rollback) | Works for tiny player counts; rollback cost grows with entities | Fighting games (GGPO) |

### Why this is computer-science territory, not portfolio territory

For real-time interactive game zones: trying to push the framing layer into worker threads, or otherwise breaking the single-thread-per-channel principle, is **college / PhD systems research**, not portfolio work. The conclusions are negative (it doesn't help, often hurts) but the research path to learn that is months.

For portfolio purposes: stay with the proven architecture. Document the limit, document the escape hatches, ship the well-designed system. The decision to NOT pursue these alternatives is itself a portfolio signal — it demonstrates that you can distinguish architectural from research concerns.

---

## Part 8: Cross-channel coordination layer

The three "hard things" identified as deferred (login, channel change, broadcast) all sit *on top of* the channel-per-thread base. Detailed design below.

### The five-thread role hierarchy

| Thread role | Quantity | Hot path? | Owns | Pattern |
|---|---|---|---|---|
| **Content thread** | One per channel | Yes | Channel state, sessions, io_uring instance, ring buffers | Single-threaded, never blocks |
| **Accept / gateway thread** | 1+ | No | Listening socket(s), auth I/O | Single thread (or N via SO_REUSEPORT); writes to content threads' inboxes |
| **Session registry** (data structure, not necessarily a dedicated thread) | 0 dedicated threads (just a structure) | No (from content thread perspective) | player_id → location map | RCU-style snapshot for lock-free reads; writers serialize through a queue |
| **Broadcaster** | 1 dedicated | No | Faction/channel subscription lists, fan-out work | Pops broadcast inbox, fans out POD copies to content threads |
| **Janitor / lease checker** | 1 (low priority) | No | Health checks, orphan cleanup | Runs occasionally; evicts registry entries whose content thread didn't heartbeat |

The asymmetry from Part 1.5 applies: slow threads can hold locks, block on I/O, do whatever they need to coordinate. The content thread is sealed off.

### Accept / gateway thread

**Role:**
- Listen on TCP, accept new connections
- Run authentication (token check, DB query — possibly slow)
- Decide destination channel based on player state
- Hand off `{ fd, player_id, auth_payload, initial_state }` as a POD message to destination content thread's inbox

**Why it can't be inside any content thread:**
- Content threads are running their tick loop; accept latency would block ticks
- Auth might block on DB I/O; can't sit on a real-time thread
- The accept role is logically centralized — one place that knows "where do new players go"

**Threading detail:**
- One accept thread is sufficient for moderate load
- Multiple via `SO_REUSEPORT` if accept-side scaling is needed
- Accept thread → content thread inbox is **MPSC if multiple accept threads**, **SPSC if single accept thread**

This is where MPSC queue (not SPSC) first becomes necessary. SPSC ends here.

### Session registry — RCU style under the never-block principle

**Operations needed:**

| Op | Caller | Frequency |
|---|---|---|
| Register session | Content thread (on accept) | Rare (per login) |
| Unregister session | Content thread (on disconnect) | Rare (per disconnect) |
| Move session | Source + destination content threads (on channel change) | Rare (per zone transition) |
| Lookup by player_id | Content thread (for whispers) | Frequent |
| Iterate by faction / channel | Broadcaster (for fan-out) | Frequent |

Under the never-block principle (Part 1.5), content threads cannot acquire a `shared_mutex` (even shared/reader lock can briefly contend with a writer). Two correct designs:

**Option A — RCU-style snapshot:**

```cpp
// A registry writer thread owns the canonical map. Mutations build new immutable map.
// Atomically swap a global pointer. Old maps freed after grace period.

struct SessionLocation {
    uint32_t content_thread_id;
    uint32_t session_handle;
    uint32_t faction_id;
};

// Single atomic pointer; content threads load with __ATOMIC_ACQUIRE
std::atomic<const RegistryMap*> g_registry_snapshot;

// Content thread lookup is non-blocking:
auto snapshot = g_registry_snapshot.load(std::memory_order_acquire);
auto loc = snapshot->find(player_id);  // O(1), never blocks
```

The Linux kernel uses this pattern for routing tables and scheduler structures.

**Option B — Fully indirect (simpler):**

```
Content thread: push "lookup player X" request → registry inbox
Registry thread: processes request, pushes response → content thread's reply inbox
Content thread: reads reply on next tick (or whenever)
```

Higher latency (one round-trip), simpler implementation. Often acceptable for whispers (UX absorbs 1–10 ms).

**Why `std::shared_mutex` (originally sketched) is wrong:** Even shared/reader lock acquisition can briefly contend with an exclusive (writer) lock. That violates the never-block principle from Part 1.5. The Part 3 bifurcation table's "Lock?" column for globally-shared-mutable is misleading from the content thread's perspective — content threads access these via lock-free read patterns, not direct mutex acquisition. Writers can use the mutex; readers (content threads) cannot.

### Broadcaster thread

**Within-channel broadcast** (zone chat, in-zone events) — handled by content thread itself, no broadcaster needed:
- Iterate channel's session table, push to each session's send ring
- No locks, no queues, fast path

**Cross-channel broadcast** (faction radio, server-wide announcement) — broadcaster does the fan-out:
- Producer content thread pushes POD broadcast intent to broadcaster's inbox
- Broadcaster looks up subscriber list (faction_id → [content_thread_ids])
- Broadcaster pushes POD copies to each subscriber's inbox
- Each content thread receives at next tick

The broadcaster runs on a dedicated thread because fan-out work shouldn't block any one content thread (especially the producer).

### Janitor / lease thread

If a content thread crashes, its sessions are orphaned in the registry. Solution: heartbeat / lease.

- Each registry entry has a "last seen" timestamp
- Content threads heartbeat to registry inbox periodically
- Janitor wakes up periodically, evicts entries whose content thread hasn't heartbeated within threshold
- Janitor publishes a new registry snapshot to maintain RCU semantics

Low priority, runs occasionally. Not on the hot path.

### Latency budgets per cross-thread operation

(Derived from the never-block principle in Part 1.5.)

| Operation | Content-thread cost | Cross-thread total cost | UX |
|---|---|---|---|
| Content tick (handler, mutation, broadcast within channel) | Microseconds (hot path) | N/A | Hard real-time |
| Whisper cross-channel | < 1 μs (push to router) | 1–10 ms total | Imperceptible to player |
| Channel change | < 1 μs (push POD, destroy local) | 10–100 ms total | "Loading next zone..." absorbs it |
| Faction broadcast | < 1 μs (push to broadcaster) | 1–50 ms total | Acceptable for chat / announcements |
| New player login | 0 (no content involvement until handoff) | 100 ms – seconds | Brief login wait OK |

Every cross-thread operation has a generous millisecond-scale budget on the non-content side. Content thread spends microseconds in all of them.

### What can be deferred for v1

Not everything needs to ship in v1:

- **Sealed zones (no channel change)** → no `move_session`, much simpler registry
- **No whispers (zone chat only)** → no `lookup_by_player_id`, much simpler registry
- **No global broadcast** → no broadcaster thread at all
- **Single accept thread** → SPSC into content threads, no MPSC complexity

A reasonable v1 ships with: accept thread + minimal registry (player_id → content_thread_id, no channel change, no whispers, just login routing). A few hundred lines.

- **V2 adds**: channel change → registry update + POD handoff. Maybe whispers.
- **V3 adds**: cross-faction broadcast.

### Why the layering matters

If the content layer were multi-threaded, each cross-channel mechanism would also be multi-threaded — login races with content, channel change becomes a distributed transaction, broadcast needs locks at every recipient. The "easy if X is not considered" structure works precisely because the base doesn't leak threading concerns into the harder layers.

The architecture has **bounded** the multi-thread complexity to one specific layer (this one). Within that layer, the patterns are well-known (RCU snapshots, MPSC inboxes, broadcaster fan-out). The content threads themselves remain blissfully single-threaded.

---

## Part 9: Bug found — release-mode no-op in `lnx::mutex`

Surfaced during architectural review. Every pthread call in `src/sync/mutex.h` is wrapped in `LNX_DCHECK`:

```cpp
inline void lock() noexcept {
    LNX_DCHECK(pthread_mutex_lock(&_mtx) == 0);
}
```

`LNX_DCHECK(cond)` expands to `((void)0)` under `NDEBUG` — the macro discards its argument, so **the `pthread_mutex_lock` call is never made in release builds**. Same for `pthread_mutex_init`/`destroy`, `unlock`, `try_lock`, and every method in `shared_mutex`. In release, `lnx::mutex` is a no-op.

### The fix

```cpp
inline void lock() noexcept {
    int rc = pthread_mutex_lock(&_mtx);
    LNX_DCHECK(rc == 0);
}
```

Git history: commit `145b902` ("add LNX_DCHECK debug trap to lnx::mutex/shared_mutex") was the refactor that introduced the bug. Smoke tests likely run debug, so it hasn't surfaced. Worth a release-preset test before anything else lands.

---

## Memory files written during this discussion

Located at `~/.claude/projects/-Users-im-1702-CLionProjects-iouring-net-lib/memory/`:

| File | Type | Content |
|---|---|---|
| `user_profile.md` | user | Personal project, WSL2 on home Windows desktop, design-driven workflow |
| `project_port_origin.md` | project | Linux port of three Windows reference repos at `~/CLionProjects/` |
| `project_spec_first.md` | project | Wiki specs precede src/ implementation |
| `project_status.md` | project | Pre-v1 snapshot of what's landed vs spec-only |
| `project_no_content_layer_threading.md` | project | Root principle: no MT in content for real-time service |
| `project_channel_tls_pools.md` | project | Channel-per-thread architecture, TLS ObjectPool, thread entry pattern |
| `project_global_shared_state.md` | project | Three-category bifurcation, when global+lock applies |
| `project_error_model.md` | project | LNX_CHECK/LNX_DCHECK + tl::expected; no exceptions |
| `reference_windows_repos.md` | reference | Lookup table for `~/CLionProjects/` predecessor repos |

The wiki specs in `doc/memory/*.md`, `doc/runtime/job_queue.md`, `doc/network/io_uring_reactor.md`, and `doc/network/session.md` are **ahead of the actual design intent** on the TLS pool axis and owe updates to reflect channel-per-thread, shared-nothing-within-a-channel.

---

## Open questions still on the table

These came up during discussion but were not fully settled:

1. **Byte allocator scope** — Is `MemoryPool` global (with per-channel `ObjectPool<T>` routing into it), or also per-channel? Per-channel is more fully shared-nothing but doubles allocator memory cost.

2. **Sealed vs spanning channels** — Whether cross-channel messaging ships in v1 at all. Sealed-zone model (entities never cross channels) means `lock_free_stack` / MPSC inbox can defer to v2 entirely. Spanning channels (whispers, parties, raid invites that span zones) means MPSC inbox is v1.

3. **Type-system enforcement of object category** — Convention only / namespace separation / tag types or wrapper templates. Worth deciding before too many entity types exist.

4. **Coroutine vs state-machine for Session** — Spec mandates coroutine-driven Session. Stephen's instinct earlier was uncertainty about whether coroutine_task is needed. Both work; this is a syntax-preference choice within the channel that should be settled before implementing `Session`.

5. **Inbound packet allocation strategy** — Spec uses zero-copy `frame_view`. `ObjectPool<SerialBuffer>` for inbound is also valid, costs one memcpy per recv, gains uniformity. Decision is open.

6. **Namespace direction** — Wiki specs use `iouring_net::*` / `iouring_net::mem` / `iouring_net::rt`; landed code uses `lnx::` for the platform layer and `sds::` for data structures. Inconsistent and should be reconciled before more code lands.

7. **`session_handle.md` spec missing** — Referenced in `doc/00-overview.md:25` and `README.md:117` but the spec file was never written.

---

## Next concrete step

The discussion concluded with a proposal for the smallest end-to-end thing that proves the architecture:

1. One pthread → one stack-scoped `channel_context` → one `thread_local channel_context*`
2. One io_uring instance, one TCP listening socket
3. One Session (recv ring + send ring + socket fd)
4. One `ObjectPool<Session>` in TLS
5. Recv loop: `io_uring_wait_cqe` → frame packets from recv ring → dispatch → handler → build response → push to send ring → submit SQE
6. Drain on shutdown, destruct context, exit thread

Roughly 500–1000 lines of focused C++. After that, scaling is replication — more entity types, more sessions per channel, more channels — without architectural change. The hard part is the first build.

Plus, before more code lands: **fix the `lnx::mutex` / `lnx::shared_mutex` release-mode bug**.
