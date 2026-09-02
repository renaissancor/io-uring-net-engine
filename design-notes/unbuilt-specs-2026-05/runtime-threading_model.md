# Threading Model — two-tier reactor + three-tier memory

> **2026-05-19 evening pivot — partially superseded.** The "two-tier
> reactor" framing here (separate network thread pool ~2–4 + content
> thread pool ~16–32) was replaced: content threads own `io_uring`
> directly (per-channel reactor, one ring per channel), and a single
> accept thread handles new connections without owning a ring. The
> three-tier memory framing also gave way to SoA + linear `mmap` ring
> storage with session-as-handle. What is UNCHANGED: single-thread
> content layer, never-block-on-cross-thread-locks principle,
> per-session SPSC ring primitive, `LNX_CHECK` / `tl::expected` error
> model. See `../design-notes/2026-05-19-chat-server-data-layout.md`
> and the `project-chat-server-v1` memory entry for the current
> shape.

## Purpose

Formalize the project-wide threading rules that the foundational primitives
(`lnx::thread`, `lnx::mutex`, `mem::Memory`, future `io_uring_ring`,
`session_pool`, `packet_pool`) all depend on. Every other design doc in
`doc/memory/`, `doc/network/`, `doc/runtime/` MUST be consistent with
this document.

This is a **project-level constraint**, not a per-subsystem implementation
detail. Changing it ripples through every other doc in this wiki, so
revisions to this doc should be deliberate and reviewed.

## Intended scope

This threading model is designed for game servers with **bounded interaction
units** — where the game itself imposes a maximum population per shared
field (channel, match, room, instance). The design's invariants and tier
boundaries are predicated on each interaction unit fitting inside one CPU
core's worth of work.

### Fits naturally

| Workload                         | Interaction unit       | Typical max members |
|----------------------------------|------------------------|---------------------|
| Match-based (BR / MOBA / FPS)    | match                  | 10–100              |
| Lobby / session co-op            | session                | 4–8                 |
| Party / casual                   | room                   | 16–60               |
| **Channel-based MMO**            | **channel**            | **70–300**          |
| Instanced-dungeon MMO            | dungeon instance       | 8–40                |
| Turn-based / 1v1                 | match                  | 2                   |

Concrete examples of channel-based MMOs that fit this model: Lost Ark,
MapleStory, Black Desert Online, Dungeon Fighter Online, FFXI, RuneScape,
Mu Online, Diablo Immortal.

### Out of scope

- **Seamless open-world MMOs** — AION 2, ArcheAge, ESO megaserver, Albion
  Online. Studio-scale engineering, out of scope.
- **Single-shard universes** — EVE Online's "Tranquility" model.

Boundary case: **WoW-style phased MMOs** — phasing is fine-grained
channeling driven by player progression and fits this model.

## v1 scope — what is NOT implemented yet

The architecture described in this document is the **complete intended
design**. v1 implements a subset; everything else is documented here for
forward reference and to anchor future contributors' mental models — not
because v1 ships it.

### What v1 implements

- Two-tier reactor (network threads + content threads).
- Three-tier memory (TLS Memory + Session pool + Packet pool).
- **Network ↔ Content** cross-thread path via SPSC ring buffers per session.
- Sharding rule (sessions assigned to a content thread by interaction-unit
  key at accept time; never migrate).
- Foundational-layer discipline (no `std::` STL in `src/sync/`,
  `src/runtime/`, `src/memory/`, `src/sds/`, `src/network/`).

### What v1 explicitly does NOT implement

**Content ↔ Content cross-thread messaging (inbox)** — deferred. The
inbox is a substantial separate subsystem in its own right: lock-free
MPSC ring, message format, queue-full policy, per-thread sizing,
serialization schema for cross-thread messages, cross-thread dispatch
table. It earns its own design pass when the first cross-interaction-
unit feature actually lands in product scope. **Discussing the inbox
under "dispatcher" scope is wrong** — it's a whole project topic of its
own.

Features that **require** the inbox and are therefore out of v1 scope:

- Global chat across channels
- Cross-channel whisper / private message
- Friend status notifications across channels
- Cross-channel guild chat
- Cross-channel auction-house notifications
- Server-wide broadcast announcements ("server reboot in 5 minutes")

For v1, the threading model is: **each content thread is fully isolated.
All interactions stay within one channel / match / room / instance.** No
inter-channel features. The application is designed to live within this
constraint; if it ever needs a cross-channel feature, that is the trigger
for the inbox design conversation, not a v1 work item.

The application-design guidance section below ("minimize cross-content-
thread interaction") tells you how to structure features so they do not
need the inbox in the first place — and is therefore even more relevant
for v1 than it would be for v2.

## Topology — two-tier reactor

The server runs **three thread types**, each with a single role:

```
[Accept thread (1)]
       │
       │  new connection arrives
       │  hash interaction-unit key → (network_thread_id, content_thread_id)
       │  claim a free session slot from the session pool
       ▼
[Network thread pool (N, typically 2–4)]
   - each owns one io_uring ring
   - polls io_uring CQEs (recv, send, timer)
   - on recv CQE: writes raw bytes into session.recv_ring_buffer (SPSC W)
   - on send CQE: advances read pointer of session.send_ring_buffer (SPSC R)
   - does NOT inspect packet contents
   - does NOT run game logic
       │
       │  session.recv_ring_buffer (SPSC bytes: network writes, content reads)
       │  session.send_ring_buffer (SPSC bytes: content writes, network reads)
       ▼
[Content thread pool (M, typically 16–32)]
   - each owns a set of sessions (partitioned by content_thread_id)
   - tick loop:
       drain own sessions' recv_ring_buffer → cs_packet → handler
       handlers may build sc_packet → copy bytes → send_ring_buffer → free sc_packet
       poll cross-content inbox for inter-channel messages
       run timed systems (AI, regeneration, timers)
   - does NOT touch io_uring directly
```

Sessions are **assigned to one content thread at accept time** (consistent
hash on interaction-unit key — `room_id`, `match_id`, etc.) and never
migrate. Each session is also paired with one network thread for I/O; this
mapping can be different from the content thread mapping (e.g., 4 network
threads each serve sessions belonging to several content threads).

This is the **classical reactor + worker-pool pattern** with shared-memory
SPSC queues between tiers, refined for io_uring and for the project's
predictable-latency goals.

### Why two-tier and not single-tier (per-worker io_uring end-to-end)

A single-tier model (one thread per worker doing everything from io_uring
to handlers) is the high-performance choice at C100K+ scale — Seastar,
BEAM, modern io_uring servers converge on it. We deliberately do NOT use
single-tier because:

- The portfolio scope is 10K–100K connections, where the per-packet latency
  difference between single-tier and two-tier is invisible.
- Two-tier demonstrates real cross-thread coordination (SPSC ring buffers,
  controlled access patterns) — valuable engineering content for a portfolio.
- Two-tier decouples I/O concurrency (NIC-bound) from logic concurrency
  (CPU-bound), giving an actual tunable lever.
- Network threads can be CPU-pinned to NIC-near cores for NUMA locality.
- Two-tier matches the IOCP / Boost.Asio tradition that most C++ network
  code reviewers will be familiar with.

## Memory model — three tiers

Different lifetimes and threading patterns require different allocators.
The project uses three distinct memory tiers, each tuned for its workload:

### Tier 1 — TLS Memory (per content thread)

**Scope:** Game state, content objects.

Examples: `Player`, `Inventory`, `Item`, `Buff`, `Spell`, `Quest`,
`Channel`, `Room`, `Match`.

Each content thread has its own `mem::Memory` instance (TLS singleton, mmap
region, 48-bucket free lists — see [[memory_pool]]). Allocations are
dynamic; lifetimes vary from milliseconds to hours.

The **alloc-thread == free-thread invariant** applies here: a game-state
object allocated on content thread A is freed on content thread A. Cross-
thread interaction copies bytes via the content↔content inbox.

### Tier 2 — Session Pool (process-wide, pre-allocated at startup)

**Scope:** Network I/O state.

Examples: `session` struct, `recv_ring_buffer` (64 KiB), `send_ring_buffer`
(16 KiB), sequence windows, socket fd, network/content thread mapping.

All session slots are allocated **once at server startup** as a single
mmap region sized for `MAX_SESSIONS × slot_size`. Slots are reused — when
a session closes, its slot returns to the pool's free list, but the memory
itself is never freed.

**Predictability principle:** the server's worst-case memory footprint is
fixed at startup. There is no allocation latency growth under load.

Slots are accessed by both network thread and content thread, with
controlled access:

- `recv_ring_buffer` — SPSC, network writes, content reads.
- `send_ring_buffer` — SPSC, content writes, network reads.
- Other session fields — owned by content thread; network thread reads
  only the SPSC ring pointers and the socket fd.

See [[session]] for the full slot layout.

### Tier 3 — Packet Pool (process-wide, pre-allocated at startup)

**Scope:** Transient packet objects.

Examples: `cs_packet`, `sc_packet`.

Pre-allocated buckets per packet size class, organized as per-content-
thread free lists. At server startup, each content thread receives a
preset count of pre-built packet structures in each bucket.

Packet objects are **content-thread-local** in lifetime:

- `cs_packet` allocated when bytes drain from `recv_ring_buffer`, freed
  after handler dispatch.
- `sc_packet` allocated when handler builds outgoing, freed after bytes
  copy to `send_ring_buffer`.

Both have microsecond lifetimes. Pre-allocated buckets eliminate any
chance of allocator exhaustion under packet bursts.

See [[packet_pool]] for sizing and bucket structure.

### Why three tiers, not one

| Tier | Why not TLS Memory? |
|------|----------------------|
| Sessions / ring buffers | Cross-thread (network + content). TLS invariant doesn't apply. Pre-allocation gives predictable latency. |
| Packets | Could fit TLS Memory, but a dedicated pre-allocated pool gives O(1) alloc with no chance of TLS bucket exhaustion under bursts. Predictability over memory efficiency. |
| Game state | Variable, can't be pre-allocated by count. TLS Memory's dynamic allocation is exactly right. |

**The driving principle is predictable latency over memory efficiency.**
RAM is cheap; allocation hiccups under load are not.

> "Server should perform normally when burdened. Memory inefficiency to
> have 10000 ring buffers while 100 are used is not a problem in a real-
> time server. But when a server is fast at 1000 clients and slow at
> 8000 clients, that IS wrong for a real-time server where speed matters
> most."

This is the real-time server philosophy. Apply it to every per-session
and per-packet resource: pre-allocate at startup, recycle slots, never
grow under load.

## Cross-thread interaction

Two distinct cross-thread paths exist, with different mechanisms suited
to different traffic patterns:

### Network ↔ Content (per session, high frequency)

Mediated by the session's two SPSC ring buffers:

- `session.recv_ring_buffer` — network thread writes raw bytes; content
  thread reads them. SPSC means single producer (one network thread per
  session) and single consumer (one content thread per session).
- `session.send_ring_buffer` — content thread writes raw bytes; network
  thread reads them. Same SPSC shape, reversed.

Both rings are lock-free with cache-line-padded head/tail to avoid false
sharing. See `doc/sync/spsc_ring.md` (deferred design).

### Content ↔ Content (per cross-interaction-unit message, low frequency) — DEFERRED to v2+

> **This subsystem is NOT implemented in v1.** See the
> [v1 scope](#v1-scope--what-is-not-implemented-yet) section above for the
> explicit deferral and the list of features that require it. The design
> below is documented for forward reference only; do not implement in v1.

Mediated by per-content-thread MPSC inboxes:

- Each content thread has one inbox.
- Other content threads push serialized bytes; the owning content thread
  pops in its tick loop.
- Used for chat across channels, friend status, matchmaking handoff, etc.

The application design guidance below still applies: minimize content↔
content traffic by sharding aggressively by interaction unit. For v1
specifically, **the application must be designed so cross-channel
features are not needed at all** — there is no inbox to use.

## Sharding rule — assign by interaction unit

The TLS memory invariant and the per-content-thread tick loop together
imply an application-level routing rule:

> **Shard sessions to content threads by interaction unit, not by user.**

If two sessions interact with the same shared field (chat room state,
game zone, match instance, guild ledger), they MUST be routed to the
**same content thread** at accept time. The shared field then lives in
that content thread's TLS Memory, all interactions stay TLS-local, and
no synchronization is needed even though logically multiple users are
touching the same data.

| Shared field        | Routing key            | Content thread        |
|---------------------|------------------------|-----------------------|
| Chat room           | `room_id`              | One per room          |
| Game zone / map     | `zone_id`              | One per zone          |
| Match instance      | `match_id`             | One per match         |
| Guild               | `guild_id`             | One per guild (cold)  |
| Trade / auction     | `auction_id`           | One per auction       |

A user's *individual* session state (own inventory, settings, login)
doesn't need to co-locate with their chat room — it can stay on whatever
content thread accepted their connection. Updates between the session's
content thread and the shared-field's content thread travel via the
content↔content inbox.

### When a user is in multiple interaction units

The realistic case: a user simultaneously belongs to a game zone, a chat
room, a guild, and several smaller groups. Two resolutions:

1. **Pick a dominant unit.** Route the session to the content thread that
   owns its hottest interaction unit (usually the spatial zone for real-
   time games, the chat room for chat-heavy apps). Other units' updates
   cross via inbox. Optimizes for the common case.
2. **Disaggregate fully.** Session state lives wherever it connected;
   every interaction unit lives wherever its key hashes. Every interaction
   crosses content threads via inbox. More uniform, more inbox traffic.

Most production game servers choose (1) with the dominant unit being the
spatial zone. Resolution is per-application policy, not a primitive.

### Hotspot failure mode

If an interaction unit gets hot enough to saturate a single content
thread (a 50K-active-typer chat room, a 5K-player game zone in one cell),
you have a **single-core ceiling**. Resolutions:

- **Split the unit** — sub-shard with replication.
- **Migrate the unit** — move shared field to a less-loaded thread.
- **Cap the unit** — enforce max members per room / zone / match.

This ceiling does NOT bind for the intended scope (channel-based MMOs
and match-based games cap interaction units below one core's capacity by
game design).

## Project-wide rule

> **Tier 1 (game state):** TLS Memory per content thread, single-thread
> alloc==free, copy-via-inbox when crossing content threads.
>
> **Tier 2 (sessions + ring buffers):** Pre-allocated process-wide at
> server startup. Slots reused, never freed.
>
> **Tier 3 (packets):** Pre-allocated process-wide at startup. Per-
> content-thread free lists. O(1) alloc/free. Content-thread-local
> lifetime.
>
> **Application routing:** assign content thread by interaction-unit key
> at accept time.
>
> **Network ↔ content:** SPSC ring buffers per session (`recv_ring_buffer`,
> `send_ring_buffer`).
>
> **Content ↔ content:** MPSC inbox per content thread (DEFERRED to v2+;
> v1 has no cross-channel features and no inbox subsystem).
>
> **Foundational primitives (sync/runtime/memory/sds):** no `std::` STL,
> minimize glibc malloc surface.

## Application design guidance — minimize cross-content-thread interaction

The design makes content↔content interaction **explicit and rare**, not
cheap. Building correct content↔content message flow is genuinely
complicated. Application code should be designed to minimize these paths,
not to use them liberally.

### Why it's expensive

Each content↔content message carries costs that intra-thread calls don't:

- **Serialization schema.** Every content↔content message type needs a
  versioned binary encoding. Tens to hundreds of message types accumulate
  in a real game.
- **State drift handling.** By the time content thread M receives a
  message, the world state on M may have changed (target logged off,
  item already sold, room closed). Every message needs a "target is gone"
  branch.
- **1→N fanout.** Receiving content thread may need to forward updates
  to other content threads (chat → all members' content threads); each
  hop is another serialize + inbox push + deserialize.
- **Cross-thread query fanout.** Queries like "list online friends" either
  fanout across content threads (slow) or require denormalized state
  somewhere (consistency cost).
- **Multi-thread transactional operations.** "Trade item X from A to B
  and gold from B to A" spans two content threads. No locks, no 2PC, so
  you need either eventual consistency with rollback or a designated
  arbiter thread. Both are nontrivial.
- **Debugging.** Tracing a content↔content interaction needs correlation
  IDs through inboxes and merged logs across content threads.

**Practical rule of thumb:** a feature that takes 1 day intra-thread
takes 3–5 days cross-content-thread. Plan accordingly.

### Engineering rules

1. **Default to intra-thread.** When designing a new feature, first ask
   "can this stay inside one interaction unit?" If yes, it does. Cross-
   content-thread requires explicit justification.
2. **Co-locate aggressively.** A player's inventory, settings, quest log,
   friends list live on **the player's content thread**, NOT on a
   dedicated service content thread. Denormalize to keep reads local.
3. **Reserve content↔content for explicit, bounded edges.** Typical
   legitimate cases: global / cross-channel chat, friend status
   notifications, matchmaking handoff (one-time at match start), auction
   house post / bid, login / logout broadcast, global announcements.
4. **Catalog the cross-content-thread message types.** Maintain a list
   (e.g., `doc/network/cross_thread_messages.md`) of every type. New
   entries require deliberate review. If the list grows past ~30 types,
   refactor toward co-location.

### Anti-pattern: domain-sharded content threads

> **Shard by interaction unit** (rooms, matches, channels) — **NOT by
> domain** (inventory service, chat service, friends service).

The two look similar but have opposite load behavior. Sharding by
interaction unit keeps related state together; most operations stay
intra-thread; cross-thread traffic is 1–5% of total. Sharding by domain
separates related state; most operations cross threads; cross-thread
traffic balloons to 80%+ and the design's performance advantage
disappears.

If service-style architecture is desired, the right place for it is
**across processes / machines** (microservices), not across threads within
one process.

## Foundational-layer discipline

The `src/sync/`, `src/runtime/`, `src/memory/`, `src/sds/` layers
intentionally avoid:

- `std::` STL containers (`std::vector`, `std::unordered_map`, `std::list`,
  `std::map`, etc.).
- `std::` synchronization primitives (`std::mutex`, `std::shared_mutex`).
  `std::atomic<T>` for primitive integral / pointer types is acceptable
  where it maps directly to `__atomic_*` builtins.
- `std::abort`, `std::terminate`, `std::system_error`, exceptions.
- Calls into `::malloc` / `::free` / `::calloc` / `::realloc` for our own
  data. libc still calls these internally; that's an accepted tax.

Rationale: reducing glibc allocator surface area is a long-term
performance goal. Foundational code is where it pays off most.

Compatibility shims like `tl::expected` and `{fmt}` are allowed.
`std::string_view`, `std::span`, `std::byte` and other non-owning views
are allowed.

## Open questions

1. **Inbox shape** — DEFERRED to v2+. Bytes-in-queue (Seastar style —
   fixed-size MPSC ring of raw bytes, sender serializes inline) vs.
   handle-in-queue (sender allocates bytes in a small shared region,
   queue carries pointer + size). Leaning bytes-in-queue. Earns its own
   design conversation when the first cross-channel feature appears.
2. **Inbox queue-full policy** — DEFERRED to v2+. Drop, retry, block?
   Probably per message class. Decide alongside the inbox itself.
3. **Network ↔ content thread mapping.** N:M = how many of each, and how
   are sessions assigned (which network thread serves which sessions)?
4. **Packet pool sizing.** How many `cs_packet` / `sc_packet` pre-
   allocated per content thread per bucket? Measurement-driven.
5. **Session pool max size.** What's `MAX_SESSIONS`? Likely 10K–100K for
   portfolio scope. Affects startup memory footprint directly.
6. **Shutdown discipline.** Network threads finish in-flight io_uring
   ops, content threads finish in-flight ticks, then process can exit
   cleanly. (In v2+ this also includes draining inboxes before exit.)
7. **Session slot reuse cleanup.** When a session closes, what cleanup
   runs before the slot returns to the pool? Sequence-window reset,
   buffer drain, fd close. Define precisely.

## See also

- [[memory_pool]] — TLS Memory tier for game state objects
- [[object_pool]] — typed wrapper on TLS Memory; single-thread only
- [[session]] — Session pool tier (pre-allocated network I/O state)
- [[packet_pool]] — Packet pool tier (pre-allocated cs/sc_packet)
- [[packet_header]] — 8 B wire header on every packet
- [[thread]] — `lnx::thread` primitive (pthread wrapper)
- [[thread_context]] — per-thread role + state
