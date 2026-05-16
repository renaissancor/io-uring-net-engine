# Threading Model — per-worker io_uring + TLS-only memory

## Purpose

Formalize the project-wide threading rules that the foundational primitives
(`lnx::thread`, `lnx::mutex`, `mem::Memory`, future `io_uring_ring`, future
`job_queue`) all depend on. Every other design doc in `wiki/memory/`,
`wiki/network/`, `wiki/runtime/` MUST be consistent with this document.

This is a **project-level constraint**, not a per-subsystem implementation
detail. Changing it ripples through every other doc in this wiki, so
revisions to this doc should be deliberate and reviewed.

## Intended scope

This threading model is designed for game servers with **bounded interaction
units** — where the game itself imposes a maximum population per shared
field (channel, match, room, instance). The design's TLS memory invariant
and per-worker topology are predicated on each interaction unit fitting
inside one CPU core's worth of work.

### Fits naturally

| Workload                         | Interaction unit       | Typical max members |
|----------------------------------|------------------------|---------------------|
| Match-based (BR / MOBA / FPS)    | match                  | 10–100              |
| Lobby / session co-op            | session                | 4–8                 |
| Party / casual                   | room                   | 16–60               |
| **Channel-based MMO**            | **channel**            | **70–300**          |
| Instanced-dungeon MMO            | dungeon instance       | 8–40                |
| Turn-based / 1v1                 | match                  | 2                   |

Concrete examples of channel-based MMOs that fit this model:

- **Lost Ark** — server/channel split, capped per channel.
- **MapleStory** — channels per world.
- **Black Desert Online** — channels per zone.
- **Dungeon Fighter Online** — channels.
- **FFXI** — multiple worlds, no seamless cross-world play.
- **RuneScape** — world-hopping.
- **Mu Online** — channels.
- **Diablo Immortal** — channels per region.

### Out of scope

- **Seamless open-world MMOs** — AION 2, ArcheAge, ESO megaserver, Albion
  Online. These deliberately reject channel boundaries, allowing 5K–10K+
  players to occupy the same map. Requires spatial sub-sharding with
  replication and distributed simulation across nodes — studio-scale
  engineering, explicitly out of scope for this project.
- **Single-shard universes** — EVE Online's "Tranquility" model. Bespoke
  architecture; not something a general-purpose library should attempt.

### Boundary case

WoW-style "phased" MMOs sit between the two. Phasing is essentially fine-
grained channeling driven by player progression, and the phase is the
interaction unit. If we needed to support that, the model still works — a
phase is just a more dynamic version of a channel.

### Why this scoping matters

The single-core ceiling per interaction unit is the design's hardest
scaling cliff (see [Hotspot failure mode](#hotspot-failure-mode) below). For
the in-scope workloads above, the game's design **already caps interaction
units at sizes well below one core's capacity**, so the ceiling never binds
in practice. For out-of-scope workloads, the ceiling becomes the central
engineering challenge — and demands a different topology than this document
specifies.

If the project ever needs to support a seamless open-world workload, the
foundational primitives (`lnx::thread`, `mem::Memory`, `lnx::mutex`, the
inbox transport) remain useful, but the topology layer must be redesigned.
Treat that as a fork, not an extension.

## Topology — per-worker io_uring ring, end-to-end

Each worker thread owns its own io_uring ring and runs every stage of its
connections' lifecycle on the same thread:

```
[accept thread]
   |
   v   (connection accepted, hashed/round-robin to worker N)
[worker thread N]
   |- io_uring submit (recv, send, timeout, close, ...)
   |- io_uring completion poll
   |- packet framing / parsing
   |- business logic / content handler
   |- response build
   |- io_uring submit (send)
   ...
```

A connection is bound to a worker thread at accept time (via consistent hash
of session id, round-robin, or load-aware policy) and **never migrates**.

This matches:

- IOCP_Rookiss's per-worker GQCS topology (the reference repo).
- Seastar (Scylla's shard-per-core io_uring framework).
- nginx's per-worker model.
- envoy's per-worker thread isolation.

This **does not** match:

- The classical reactor + worker-pool pattern (separate I/O thread, separate
  handler thread pool with cross-thread buffer handoff).
- Workstealing thread pools.

The classical reactor + worker-pool pattern is rejected because cross-thread
buffer handoff conflicts with the TLS memory model below. Workstealing is
rejected for the same reason.

## Memory model — TLS singleton, alloc-thread == free-thread

Memory allocation is per-thread. Each thread has its own `thread_local
mem::Memory` instance (see [[memory_pool]]) backed by a single `mmap` region
sized at construction.

The central invariant:

> **A block allocated on thread A MUST be released on thread A.**

This invariant is what makes the alloc/release fast paths synchronization-free
(no atomics, no Treiber stack, no ABA). It is also what constrains us to the
topology above — if business logic ran on a different thread than I/O, the
buffers allocated for I/O would have to cross threads on free, breaking the
invariant.

Foundational primitives that follow this discipline:

- `mem::Memory` (TLS singleton, 48-ish buckets, mmap-backed)
- `profiler_scope` (TLS singleton, per-thread record map)
- `logger` (TLS singleton, per-thread ring buffer)
- [[thread_context]] (TLS state — role, name, current job)

## Cross-thread interaction — copy via inbox, NOT pointer passing

When data must move between threads (e.g., worker N produces a chat message
that worker M's connection should receive), it does so by **copying the
bytes** through a lock-free per-thread inbox, **not** by sharing a pointer.

```
[worker N]                            [worker M]
  send_to(M, msg)                       poll_inbox()
   |- serialize msg into bytes           |- pop bytes from inbox
   |- push bytes onto M's inbox  -->     |- allocate fresh object in M's TLS Memory
                                         |- deserialize bytes into it
                                         |- hand to handler
```

The inbox is a per-thread MPSC ring buffer (one inbox per destination thread,
many senders write, only the destination reads). Multiple senders can push
without locking each other; only the destination pops.

### Why copy and not pointer-pass

- Preserves alloc-thread == free-thread strictly. The bytes' final object form
  is allocated on the destination thread.
- Cross-thread interaction is **visible at the call site** — no spooky-
  action-at-a-distance via shared `shared_ptr` or atomic ref-counts.
- The receiver can log, replay, or audit the bytes; raw pointers can't be
  replayed across runs.
- The pattern is well-validated at C100K+ scale by Seastar and Erlang/BEAM.

The cost is serialization + copy. For most content payloads (player position
delta, item update, chat message), the payload is small (<256 B) and copying
is cheap compared to the cache-coherence traffic a cross-thread pointer
dereference would incur anyway.

### Carveout — packets / serial_buffers

Network packet payloads (`serial_buffer` and friends) may be large (up to MTU
or jumbo-frame size). Bytes-in-inbox semantics are wasteful for those.

**Packet handling is exempt from the copy-cross rule.** Packets stay on their
connection's owning worker thread; if processing needs to happen elsewhere,
route the **work** to the owning thread (RPC pattern), not the bytes.

The packet path has a separate design — deferred and likely to live under
`wiki/network/`. See [Open question 4](#open-questions).

## Sharding rule — interaction unit, not user

The TLS memory invariant naturally implies an application-level routing rule:

> **Shard by interaction unit, not by user.**

If two sessions interact with the same shared field (chat room state, game
zone, match instance, guild ledger), they MUST be routed to the **same worker
thread at accept time**. The shared field then lives in that worker's TLS,
all interactions stay TLS-local, and no synchronization is needed even though
logically multiple users are touching the same data.

| Shared field        | Routing key            | Lands on...        |
|---------------------|------------------------|--------------------|
| Chat room           | `room_id`              | One worker         |
| Game zone / map     | `zone_id`              | One worker         |
| Match instance      | `match_id`             | One worker         |
| Guild               | `guild_id`             | One worker (cold)  |
| Trade / auction     | `auction_id`           | One worker         |

A user's *individual* session state (own inventory, settings, login record)
doesn't need to co-locate with their chat room — it can stay on whatever
worker accepted their connection. Updates between the session worker and
the shared-field worker travel via the inbox copy mechanism.

### When a user is in multiple interaction units

The realistic case: a user simultaneously belongs to a game zone, a chat
room, a guild, and several smaller groups. Two resolutions:

1. **Pick a dominant unit.** Route the session to the worker that owns its
   hottest interaction unit (usually the spatial zone for real-time games,
   the chat room for chat-heavy apps). Other units' updates cross via inbox.
   Optimizes for the common case.
2. **Disaggregate fully.** Session state lives wherever it connected; every
   interaction unit lives wherever its key hashes. Every interaction crosses
   threads via inbox. More uniform, more inbox traffic.

Most production game servers choose (1) with the dominant unit being the
spatial zone. Resolution is per-application policy, not a primitive.

### Hotspot failure mode

If an interaction unit gets hot enough to saturate a single worker thread
(a 50K-active-typer chat room, a 5K-player game zone in one cell), you have
a **single-core ceiling**. Resolutions:

- **Split the unit** — sub-shard with replication. Big chat room becomes N
  small shards; spatial zone becomes a quad-tree.
- **Migrate the unit** — move the shared field to a less-loaded worker.
  Expensive (drain inbox, copy TLS state, redirect new arrivals). Best
  avoided in steady state.
- **Cap the unit** — enforce max members per room / zone / match. Common in
  practice.

This is the design's most fundamental scaling ceiling: **a single interaction
unit cannot outgrow one core's worth of work**. Plan capacity accordingly.

## Project-wide rule

> **Contents (game state, per-user interactions, business objects):**
> TLS-allocated, single-thread, copy-via-inbox when crossing threads.
>
> **Application routing:** shard by interaction unit (`room_id`, `zone_id`,
> `match_id`, …), not by user / session id. Sessions in the same interaction
> unit land on the same worker.
>
> **Packets (`serial_buffer`, network framing):** separate design,
> connection-pinned to owning worker, exempt from copy-cross.
>
> **Foundational primitives (sync/runtime/memory/sds):** no `std::` STL,
> minimize glibc malloc surface.

## Application design guidance — minimize cross-thread interaction

The design makes cross-thread interaction **explicit and rare**, not cheap.
Building correct cross-thread message flow is genuinely complicated.
Application code should be designed to minimize cross-thread paths, not to
use them liberally.

### Why it's expensive

Each cross-thread content message carries costs that intra-thread calls
don't:

- **Serialization schema.** Every cross-thread message type needs a versioned
  binary encoding (player update, chat msg, trade offer, …). Tens to hundreds
  of message types accumulate in a real game.
- **State drift handling.** By the time worker M receives a message, the
  world state on M may have changed (target logged off, item already sold,
  room closed). Every cross-thread message needs a "target is gone" branch.
- **1→N fanout.** Receiving worker may need to forward updates to other
  workers (chat → all members' workers); each hop is another serialize +
  inbox push + deserialize.
- **Cross-thread query fanout.** Queries like "list online friends" either
  fanout across workers (slow) or require denormalized state somewhere
  (consistency cost).
- **Multi-worker transactional operations.** "Trade item X from A to B and
  gold from B to A" spans two workers. No locks, no 2PC, so you need either
  eventual consistency with rollback or a designated arbiter worker. Both
  are nontrivial.
- **Debugging.** Tracing a cross-thread interaction needs correlation IDs
  propagated through inboxes and merged logs across workers, not a single
  stack trace.

**Practical rule of thumb:** a feature that takes 1 day intra-thread takes
3–5 days cross-thread. Plan accordingly.

### Engineering rules

1. **Default to intra-thread.** When designing a new feature, first ask
   "can this stay inside one interaction unit?" If yes, it does.
   Cross-thread requires explicit justification.
2. **Co-locate aggressively.** A player's inventory, settings, quest log,
   friends list live on **the player's worker**, NOT on a dedicated service
   worker. Denormalize state to keep reads local; pay the denormalization
   cost (occasional inconsistency, update fanout on rare writes) instead
   of the cross-thread cost (every read crosses threads).
3. **Reserve cross-thread for explicit, bounded edges.** Typical legitimate
   cases: global / cross-channel chat, friend status notifications,
   matchmaking handoff (one-time at match start), auction house post / bid,
   login / logout broadcast, global announcements. Aim for "this could fit
   on one page of documentation."
4. **Catalog the cross-thread message types.** Maintain a list (e.g.,
   `wiki/network/cross_thread_messages.md`) of every cross-thread message
   type. New entries require deliberate review. If the list grows past
   ~30 types, the application is leaking complexity into the cross-thread
   layer — refactor toward co-location.

### Anti-pattern: domain-sharded workers

> **Shard by interaction unit** (rooms, matches, channels) — **NOT by
> domain** (inventory service, chat service, friends service).

The two look superficially similar but have opposite load behavior:

- **Sharding by interaction unit** keeps related state together. Most
  operations stay intra-thread. Cross-thread traffic is 1–5% of total.
- **Sharding by domain** separates related state. Most operations cross
  threads. Cross-thread traffic balloons to 80%+. Complexity explodes,
  and the design's performance advantage disappears.

If service-style architecture is desired, the right place for it is
**across processes / machines** (microservices), not across threads within
one process. Within a process, this design assumes interaction-unit
sharding throughout.

## Foundational-layer discipline

The `src/sync/`, `src/runtime/`, `src/memory/`, `src/sds/` layers
intentionally avoid:

- `std::` STL containers (`std::vector`, `std::unordered_map`, `std::list`,
  `std::map`, etc.).
- `std::` synchronization primitives (`std::mutex`, `std::shared_mutex`).
  `std::atomic<T>` for primitive integral / pointer types is acceptable
  where it maps directly to `__atomic_*` builtins.
- `std::abort`, `std::terminate`, `std::system_error`, exceptions in
  general.
- Calls into `::malloc` / `::free` / `::calloc` / `::realloc` for our own
  data. libc still calls these internally for things like `errno` setup;
  that's an accepted tax.

Rationale: **reducing glibc allocator surface area is a long-term performance
goal.** Foundational code is where it pays off most — the deeper a layer is
in the stack, the more callers it has, and the more allocations it can
intercept. Third-party glue (openssl, zlib, eventual DB driver) is an
accepted tax; we don't try to eliminate it.

Compatibility shims like `tl::expected` and `{fmt}` are allowed because they
don't allocate on the hot path. `std::string_view`, `std::span`, `std::byte`
and other non-owning views are allowed.

## Open questions

1. **Inbox shape.** Bytes-in-queue (Seastar style — fixed-size MPSC ring of
   raw bytes, sender serializes inline) vs. handle-in-queue (sender allocates
   bytes in a small shared region, queue carries pointer + size). Leaning
   bytes-in-queue. Decide when the first cross-thread message lands.
2. **Inbox queue-full policy.** Drop, retry, block? Probably per message
   class: best-effort updates drop oldest, control messages block briefly.
   Decide alongside the inbox design.
3. **Worker assignment policy at accept.** Resolved in principle by the
   [sharding rule](#sharding-rule--interaction-unit-not-user): consistent-hash
   by the **interaction-unit key** (`room_id`, `zone_id`, `match_id`, …), NOT
   round-robin and NOT consistent-hash on session id. For sessions that
   participate in multiple interaction units, the policy picks a dominant
   unit (typically the hottest / spatial unit) and hashes on that. Still open:
   specific hashing function (xxhash, FNV-1a, …) and the dominant-unit
   selection mechanism — deferred until the network layer / session
   registration design lands.
4. **Packet path design** (the carveout). Deferred to `wiki/network/`. Likely
   options: connection-pinned with RPC-style work routing; or large-block
   raw-`mmap` escape hatch.
5. **Shutdown discipline.** Workers must drain their inboxes and run TLS
   destructors before process exit. `lnx::thread::join()` from main provides
   the synchronization point. The process should not call `::exit()` directly
   while workers are alive — TLS destructors don't run cleanly during
   `::exit()`.

## See also

- [[memory_pool]] — TLS `Memory` singleton, alloc-thread == free-thread invariant
- [[object_pool]] — typed wrapper; cross-thread `shared_ptr` forbidden here
- [[thread]] — `lnx::thread` primitive (pthread wrapper)
- [[thread_context]] — per-thread role + state
