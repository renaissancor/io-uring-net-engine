---
status: accepted
note: no disposition note yet; DECIDED items (state fold, renames, kernel floor 6.0) are not yet reflected in doc/10 or the engine guides
---
# 2026-08-21 — Phase 2 fundamental-architecture pass

Decisions proposed before any content implementation (io_uring, packets,
rooms), covering four interlocking areas: ownership wiring, event-loop
model, session model, protocol layer. Recommendations below are grounded
in the built headers; **OPEN** items need the owner's call before code.

Two code facts shaped everything:

1. `sds::ring_buffer<N>` embeds `byte buffer_[N]` **inline**
   (`ring_buffer.h:82`) and has no external-storage variant. Since
   `engine_worker` is a TLS singleton, a member array of rings would land
   in `.tbss` — the exact BSS blowup the locked SoA rule bans. So
   "mmap-backed ring storage" must mean **mmap a region and placement-new
   the ring objects into it**, not "hand a ring a pointer".
2. **The supervisor has no io_uring ring** (`request_stop()` runs on
   `main()`), so `IORING_OP_MSG_RING` cannot serve the stop path. eventfd
   is mandatory anyway → use it for the mailbox doorbell too, one wake
   mechanism instead of two.

---

## 1. Ownership wiring

- **session_table: `engine_acceptor` owns it by value.** It is
  category-5 state ("acceptor thread ALONE", `session_table.h:11-14`)
  and category 5 lives in the engine. LANDLORD is for *cross-thread*
  pinned storage only. Rejected: supervisor-owned + borrowed pointer
  (re-opens reachability); handle-embedded (handle is the cross-thread
  surface); global singleton (enforcement-by-convention).
  At the 16K-session target the ~512 KiB table moves into the acceptor's
  mmap arena.
- **Listen fd: supervisor creates/binds/listens; acceptor only accepts.**
  Already locked (design/2026-06-06 #7). `main()` opens pre-spawn,
  `install_listen_fd()` before `start()` (category 3), `main()` closes
  after join. bind() failure is reportable from `main()`; on the engine
  it could only trap. Rejected: engine-created socket; fd inside the
  copied `config` POD (double-close invite).
- **Mailbox integration:** introduce `app::mesh_outbox` (mailbox ptr +
  peer eventfd) so a `post()` can never ship without its doorbell write.
  Worker loop: drain inbox first (bounded batch of 32 so a flooded inbox
  can't starve CQEs), then CQE parse/dispatch, then arm SQEs, tick,
  `submit_and_wait_timeout`. During `draining` the worker keeps draining:
  each remaining adopt → `close(fd)` + `session_closed{reason=drain}`
  (else an in-flight adopt strands an fd). Acceptor: `session_closed`
  must `validate(id, gen)` before `remove()`; failed validate is a
  normal discard, not a trap. Backpressure: full mailbox or full table →
  close at the door (locked drop-at-the-door), never spin.
- **Gap found:** nothing ever calls `session_table::mark_in_world` — no
  worker→acceptor adoption confirmation exists, so sessions would sit in
  `assigning` forever. **DECIDED 2026-08-21:** add
  `app_msg_type::session_adopted = 4` `{session_id, generation,
  worker_id}`, posted once post-install.
- **Worker index:** no new field — `handle_worker::base._id` is the
  index. Acceptor grows `mesh_outbox _to_worker[config::k_worker_max]`;
  delete the `worker_count == 1` LNX_CHECK when fan-out lands. Routing
  hook `pick_worker(room_id)` = `room_id % worker_count`.
  **OPEN** (1b): when fan-out lands, mesh storage as function-local
  `static` in `main()` (BSS, still LANDLORD-owned) vs stack? (~640 KiB.)
  **OPEN** (1c): on rejection, silent close vs `S_SERVER_BUSY` frame
  (the latter needs acceptor-side write, which doc/10 §7 avoids —
  recommend silent close).

## 2. Event-loop model

- **One ring per engine — confirmed.** Acceptor 64 SQEs, worker 1024.
  `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN`. The ring is
  category-5 state; sharing forfeits SINGLE_ISSUER and invariant #1.
  Rejected: shared ring; ringless blocking `accept4()` acceptor (racy
  stop path, breaks the uniform completion-driven shape).
- **Wait: `io_uring_submit_and_wait_timeout`.** Worker timeout = next
  tick deadline (1 ms placeholder); acceptor coarse (50 ms). Key insight:
  a realtime loop needs a periodic tick anyway, so the bounded wait is
  *load-bearing*, not a compromise — the eventfd exists to cut mailbox/
  stop latency below the tick, not to make the loop wakeable at all.
  End state: worker keeps this shape; acceptor eventually goes untimed
  `wait_cqe` + eventfd. Rejected: peek+yield spin (two cores burned,
  destroys tail-latency measurement); TIMEOUT SQE (synthetic CQE per
  tick); untimed wait with no wake (documented deadlock).
- **Mailbox wake: one eventfd per consumer, armed as a multishot poll
  SQE.** The mailbox is plain user memory — not pollable, ever. The
  **handle** creates the eventfd in its ctor (must exist before
  `start()` — same race window `handle_thread.cpp:30-56` fixed) and
  closes it in its dtor. eventfd counter semantics coalesce N posts into
  one wake. Park/unpark skip-write optimization deferred until profiled.
  Rejected: MSG_RING for Phase 2 (supervisor has no ring → would mean
  two wake paths; remains the end-state acceptor→worker doorbell
  optimization, payload too small to ever be the transport); shared
  eventfd (thundering herd).
- **Multishot accept: yes.** Armed once; re-arm when `CQE_F_MORE` clear.
  **Trap flagged:** direct descriptors (`FILE_INDEX_ALLOC`) on accept
  are per-ring and can't be handed to a worker's ring — actively wrong
  for the acceptor; viable later *inside* a worker.
- **Stop: the wake is the handle's job.**
  `handle_*::request_stop() { base.request_stop(); wake(); }` —
  `handle_thread` stays a role-agnostic pure CAS. Engine checks the flag
  after *every* wait return; acceptor drain cancels the multishot accept
  (`prep_cancel_fd`) so the ring is quiescent before `main()` closes the
  listen fd. Rejected: pthread_kill (signals masked by design);
  timeout-only stop; cross-thread ring-fd close.
- **DECIDED 2026-08-21:** raise the kernel floor to **6.0**
  (SINGLE_ISSUER/DEFER_TASKRUN used unconditionally; update doc/00 and
  doc/06 floor statements). **OPEN** (2b): worker tick 1 ms now vs
  coarse until the tick has real work?

## 3. Session model

- **Two non-overlapping state enums.** Acceptor authority
  (`session_state`): `accepted → assigning → owned → closing → closed`
  (renames: `in_world`→`owned`, `disconnecting`→`closing`). Worker
  connection (`conn_state`, new): `connected → selecting → in_world →
  closing`, mapping 1:1 to doc/10 §6; "Disconnected" = slot absence.
  Rationale: the acceptor cannot observe selecting→in_world without
  per-transition mesh traffic; "authority is the mapping, not the I/O".
  `owned` removes the lie; distinct names keep `grep in_world`
  worker-only. Rejected: one shared enum; deriving state from
  `owner_worker == -1` (loses the `assigning` limbo backpressure
  accounting needs); identical value names.
  **DECIDED 2026-08-21:** rename is a go (`in_world`→`owned`,
  `disconnecting`→`closing` in session_record/session_table/test,
  mechanical, before Phase 2 code lands on the old names).
- **Worker-side SoA layout (Phase 2):** dense columns
  `_fd/_sid/_gen/_account/_room/_state/_recv_inflight/_send_inflight`
  (+ intrusive `u16` free list, O(1) alloc) sized `k_slots = 256 ==
  k_max_sessions` so the acceptor can't over-admit; rings = one mmap
  region (`MAP_NORESERVE`, ~8 MiB/worker), placement-new'd
  `ring_buffer<16384, ring_sync::single>` recv+send arrays, dtor
  reverse-destroys + munmap. One stitch buffer per **engine**, not per
  session. No per-slot alignas (single-threaded cursors — superseded per
  design/2026-05-23). **No session_id→slot map needed**: CQE user_data
  carries the slot; adopt allocates from the free list — say it so
  nobody adds a hash map defensively. Ordering hazard: arm the next recv
  SQE **after** the parse loop (direct_* cursors shift as parsing
  consumes). Adoption: worker echoes `{id, gen}` (token, not
  self-validation); duplicate resident id → trap.
- **session_id stays flat u64; generation stays a separate field.**
  Packing channel/worker/slot breaks id stability across migration
  (locked-rejected in design/2026-05-19). The bits that *should* pack are
  **CQE `user_data`**: `(op:8 << 56) | (gen16 << 16) | slot16` — a late
  CQE for a recycled slot is discarded on gen mismatch before touching
  the ring (extends the echo-smoke precedent). Rejected: pointer
  user_data (kernel may still own the memory).
  **OPEN** (3b): acceptor-side `generation` may be redundant
  (`_next_id` is monotonic, so `validate()` could be existence-only) —
  keep for symmetry or drop from the authority record?
  **OPEN** (3c): send ring 16 KiB symmetric vs 4 KiB — note broadcast
  amplification (64-member room × 256 B ≈ 16 KiB send per message).

## 4. Protocol layer

- **Location: `src/app/protocol.h`, `namespace app`.** `message.h` is
  the precedent; `app::` is already the private tier (absent from the
  CMake public-header fileset); a `net::` tier for one enum is a
  namespace created to group a folder (peer tier permitted *later* —
  "not yet", not "never"). Generic ring→frame loop in
  `src/app/frame_parser.h`, first resident of a future `src/net/`.
  Wire opcodes stay out of `message.h`: mesh messages are trusted by
  construction, wire frames are attacker-controlled — different trust
  boundaries, different files. Split rule if needed:
  `protocol_header.h` / `protocol_cs.h` / `protocol_sc.h`.
- **Header: the existing 8-byte layout, reconciled to project style.**
  `{u16 size (total, incl. header); u16 opcode (high bit = direction);
  u16 sequence; u08 flags(=0); u08 version(=1)}`, `alignas(8)`.
  8 not 4: bodies start 8-aligned for in-place POD casts + a versioning
  escape hatch. **No magic byte** (locked non-goal, doc/00). LE-on-wire
  = host order, `static_assert` LE. Validate every frame (size bounds,
  version, flags, opcode range, direction bit); all failures close.
  `size > k_max_frame` closes immediately — such a frame can never
  assemble; waiting is an unrecoverable stall. Adopting this means
  reconciling `doc/network/packet_header.md` (namespace + u16/u08) and
  updating its banner.
- **Parsing: in place from the recv ring; copy out only on
  wrap-straddle** (Option A, locked in design/2026-05-19; ~1-in-250
  stitch rate). `peek()` already reassembles a straddling *header*
  across the wrap. Framing state = the dequeue cursor alone.
  **Handler hard rule:** the body pointer aliases the ring and dies when
  `dispatch` returns — retain-by-copy only; document on the handler
  signature. Ring full with no complete frame → close (malformed peer).
  Rejected: copy-per-frame via packet_pool (pre-pivot design);
  double-mapped rings (documented future optimization); acceptor-side
  parse (explicitly rejected by doc/10 §7).
- **Gate: static per-opcode table**
  `k_cs_table[op] = {handler, min_state, min_body}`; one dispatch site
  checks bounds → state → body size → call. Invariant #5 becomes a
  readable data fact. Requires ordered `conn_state` (`<` meaningful) —
  comment the enum; insertion silently breaks it. Rejected: per-state
  handler-table swap (loses unknown-opcode vs out-of-state distinction);
  in-handler checks (convention = the bug class the gate prevents);
  bitmask (clean upgrade path if a non-monotonic state appears).
  Illegal packet policy: one `close_session(slot, close_reason)` fn,
  always disconnect (drop-at-the-door extended past the door); this
  defines `session_closed_msg.reason`. Close sequence: state=closing →
  cancel SQEs → last completion → close(fd) → **bump gen at slot free,
  before reuse** (what makes the user_data gen check work) → free-list →
  post session_closed.
- **v1 packets (7):** C_SELECT_ROOM, C_CHAT (len ≤ 512), C_LEAVE_ROOM;
  S_WELCOME, S_ENTER_WORLD_OK, S_ENTER_WORLD_FAIL (not optional — the
  alternative to "room full" is disconnect), S_CHAT. S_PEER_JOIN/LEAVE
  deferred. Rooms: `{u16 member_count; u16 members[64]}` × 4, members
  are **slot indices** so broadcast is a direct column walk. Send path:
  all-or-nothing `enqueue()`; 0 → `close(send_backpressure)`; **max one
  send SQE in flight per session** preserves ordering, no IO_LINK.
  **DECIDED 2026-08-21:** fold `S_ENTER_SELECTING` into `S_WELCOME`
  (update doc/10 §6 accordingly).
  **OPEN** (4b): maintain `sequence` in v1 (recommended) or leave 0?
  **OPEN** (4c): `k_max_frame` = 1024 (recommended — tight bound turns a
  length-field attack into an immediate close) vs ring capacity?

## Doc conflicts flagged

- design/2026-06-06 #3/#5 (acceptor-as-lobby owns pre-adopt I/O)
  conflicts with doc/10 §7 (v1 immediate-adopt). doc/10 wins per
  CLAUDE.md; the lobby model is the *long-term* shape — banner the
  2026-06-06 doc accordingly.
- Adopting the 8-byte header revives part of the banned
  `doc/network/packet_header.md` — reconcile that spec (namespace,
  aliases, banner) in the same change.
