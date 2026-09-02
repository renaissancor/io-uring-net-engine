---
status: superseded
reconstructed: 2026-09-02
superseded_by:
  - 2026-05-19-chat-server-data-layout.md
  - 2026-05-21-chat-only-scope-reconstructed.md
still_holds: coroutines rejected; no-exceptions error model; single-thread content layer
---
# 2026-05-17 — Architecture pivot and monorepo decision (reconstructed 2026-09-02)

> **Reconstructed, not contemporaneous.** No note was written on 2026-05-17.
> Five later notes cite "the 2026-05-17 architecture" and the wiki pages that
> recorded it lived under `.omc/wiki/`, which was gitignored and did not
> survive. This note is transcribed on 2026-09-02 from the memory records the
> sessions of that day left behind:
> `~/.claude/projects/-home-stephen-code-iouring-net-lib/memory/project_architecture_v1.md`
> and `.../project_library_product_split.md`. Where those records are silent,
> this note says so rather than filling the gap.

Two decisions were taken on this day. They are unrelated in content and are
kept in one note only because they share a date.

## 1. The two-repo split is rejected

[`2026-05-14-project-split.md`](2026-05-14-project-split.md) had argued for
two repositories: `iouring-net-lib` (the library) and `iouring-net-server`
(the product), with `docs/09` describing the boundary and a partial design
tree already under `~/code/iouring-net-server/`.

**Decision: one repository, one binary.** The memory record quotes the
reasoning in the author's words: *"perhaps I was not supposed to divide repo
let's just do it here and test."* For a portfolio project, one repo and one
binary is easier to deploy, demo, and review. The library/product distinction
stays *conceptual* — which code is reusable infrastructure and which is this
specific server's logic — but it is not a deliverable boundary.

Consequences recorded at the time: do not create or maintain a sibling
`iouring-net-server` repo; do not write wiki entries that reference a
separate product repo; the wiki docs that pointed to a "product side"
(`wiki/network/packet_handler.md` and others) were deleted.

What happened next, for the reader following the timeline: this decision was
itself reversed four days later by
[`2026-05-21-chat-only-scope-reconstructed.md`](2026-05-21-chat-only-scope-reconstructed.md),
which sent game work back to a separate repo. That repo was scaffolded on
2026-07-04 and finally folded back into this monorepo on 2026-08-31, where
its documents now sit as [`game-server/`](game-server/). The 05-14 note's
boundary criteria outlived every one of those moves.

## 2. The architecture pivot: from single-tier coroutines to a two-tier reactor

The memory record states that on this day the architecture "superseded an
earlier single-tier coroutine design" and was locked as v1. What was locked:

**Topology — two-tier reactor.**
- Network thread pool (~2–4), each owning one `io_uring` ring, copying bytes
  between sockets and per-session ring buffers. Does not inspect packet
  contents, does not run game logic.
- Content thread pool (~16–32), each owning a partition of sessions and
  running a tick loop (input → logic → output). Does not touch io_uring.
- Sessions assigned to one (network thread, content thread) pair at accept
  time by consistent hash on the interaction-unit key; never migrate.

**Memory — three tiers.** TLS memory for game state (alloc-thread ==
free-thread), a flat session array with embedded 64 KiB recv and 16 KiB send
rings, and a per-content-thread packet pool for `cs_packet` / `sc_packet`.
Driving principle, quoted: *"server must perform normally when burdened;
memory inefficiency from 10K idle ring buffers is fine; allocation latency
growing with load is NOT."*

**Wire format.** 8-byte header `{size:u16, opcode:u16, sequence:u16,
flags:u8, version:u8}`, little-endian, opcode high bit splits direction.

**Explicitly rejected on this day**, per the record: single-tier per-worker
io_uring ("equivalent to running N single-thread processes"); coroutine-driven
per-session `task<T>`; `job_queue` with drainer election; a 4-byte
Windows-parity header; `shared_ptr<session>`; domain-sharded workers; a
dedicated send-only thread; a `session_pool` class with a mutex.

**Code state on this day**, per the record: `lnx::thread`, `lnx::mutex`,
`lnx::atomic*`, `LNX_CHECK`, `sds::ring_buffer`, `sds::cstr_hash_map`,
`profiler_scope` landed. Network and memory pool not yet implemented.

## 3. How long it held

Two days. The memory record carries an update dated 2026-05-19 evening: the
separate network thread pool, the three-tier memory framing, the asymmetric
ring sizes, and "sessions never migrate" were superseded by the data-layout
pivot recorded in
[`2026-05-19-chat-server-data-layout.md`](2026-05-19-chat-server-data-layout.md).
That note in turn says the *morning* 2026-05-19 architecture doc "had already
moved to channel = pthread + io_uring shard" — that is, the two-tier reactor
was abandoned in favour of content threads owning their own ring within 48
hours of being locked, and the rationale for that move is the one in
[`2026-05-19-server-architecture.md`](2026-05-19-server-architecture.md)
Parts 1–2 (no multithreading inside the content layer; one thread owns a
channel end to end).

What the record says survived the 05-19 pivot: the 8-byte packet header,
content-thread-local `sc_packet` / `cs_packet` lifetime, the
no-exceptions error model (`LNX_CHECK` + `tl::expected`), and the
single-thread content layer principle. Of those, the header width was later
disputed again — see the drift review,
[`2026-09-02-design-notes-drift-review.md`](2026-09-02-design-notes-drift-review.md).

## Why this note exists

Until 2026-09-02 the engine's `doc/runtime/threading_model.md` described the
two-tier reactor of this day as "what v1 implements", under a banner that
said it was replaced, and nothing in the repository said where the model came
from, why it was chosen over single-tier, or that it lasted two days. That
spec now sits with the other unbuilt May designs in
[`unbuilt-specs-2026-05/`](unbuilt-specs-2026-05/), and this note is the
record it lacked.
