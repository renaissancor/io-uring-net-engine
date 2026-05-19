# Chat Server Data Layout Discussion — 2026-05-19 Evening

Companion record to `2026-05-19-server-architecture.md` and
`2026-05-19-portfolio-strategy.md` (same date, morning session).
Captures the evening pivot from the morning's "channel-per-thread +
ObjectPool\<Session\>" framing to a **structure-of-arrays linear layout
with session-as-handle**, plus the compile-time caps, ring buffer
design, framing protocol, backpressure policy, allocator strategy, and
dispatch posture that flow from it.

---

## TL;DR

Locked across the evening discussion:

1. **Session is a `uint32_t` handle**, not a class with embedded state.
2. **One linear `mmap` region** holds all ring storage (recv + send across all sessions, all channels) at known offsets.
3. **Structure-of-arrays metadata** — parallel arrays of `head_recv[]`, `tail_recv[]`, `head_send[]`, `tail_send[]`, `fd[]`, `channel_owning_id[]`, etc., indexed by `session_id`.
4. **Compile-time caps:** `CHANNEL_COUNT = 2~4` (start with 2), `SESSION_COUNT_PER_CHANNEL = 16384`, `RECV_RING_BUFFER_BYTES = SEND_RING_BUFFER_BYTES = 16384` (separate constants).
5. **SPSC rings** with `uint64_t` free-running head/tail counters (Lamport style; matches io_uring's own SQ/CQ pattern), `alignas(64)` per slot to prevent false sharing.
6. **Frame straddling: stitch on demand** (Option A). Option B (mmap double-mapping) is a documented future optimization.
7. **Send-ring backpressure: drop frame + close session.** No block-and-wait. No per-session resize. Global resize = redeploy with bigger constant.
8. **Accept-thread-owned session_id allocator** (strategy locked; primitive design deferred).
9. **Posture A: library = pure transport.** Chat server provides a frame-dispatch callback; library never inspects opcodes.
10. **Framing: stateless** — `recv_ring.tail` IS the only per-session framing state.

This is a structural pivot from the 2026-05-17 morning architecture
(which had `ObjectPool<Session>`, three-tier memory, separate
network/content thread pools). The 2026-05-19 morning architecture
doc had already moved to "channel = pthread + io_uring shard"; this
evening discussion lands the data layout that fits.

**Significant side outputs:**
- Bug fix landed: `LNX_DCHECK` release-mode no-op in `lnx::mutex`/`lnx::shared_mutex` closed (commit `8930c59`); `LNX_DCHECK` macro removed entirely from `src/check.h`.
- Doc cleanup landed: stale coroutine references removed from `docs/*.md` (commit `738d2bb`).
- Policy update: `std::` namespace usage rule extended from sync-primitives-only scope to project-wide "pragmatic no-STL" (view types + traits + C-library funcs OK; containers/smart pointers/function/exceptions/streams/format/sync types banned).

---

## Part 1: Session-as-handle + linear SoA storage

### The pivot

The 2026-05-17 architecture and the morning 2026-05-19 architecture doc
both implied **Session is an object** with embedded ring buffers, owned
by a per-channel `ObjectPool<Session>`. Stephen's insight during the
evening discussion turned that inside out: if total session count is
fixed at compile time, **the entire ring storage region can be
allocated as one linear `mmap` block at server boot.**

This is more than an optimization. It changes what Session *is*:

- **No longer a class with state.** Session is a `uint32_t session_id`
  index into parallel arrays.
- **No longer pool-allocated.** Slots are pre-allocated as array
  positions; there's nothing to "construct" — initialization is
  zeroing a few fields.
- **No `Session*` pointers anywhere.** Cross-thread communication
  carries the `session_id` integer.

### The two-region layout

```
Region 1 — session metadata, structure-of-arrays:
  head_recv[SESSION_COUNT_TOTAL]            ← producer-only (kernel)
  tail_recv[SESSION_COUNT_TOTAL]            ← consumer-only (content)
  head_send[SESSION_COUNT_TOTAL]            ← producer-only (content)
  tail_send[SESSION_COUNT_TOTAL]            ← consumer-only (kernel)
  fd[SESSION_COUNT_TOTAL]
  channel_owning_id[SESSION_COUNT_TOTAL]
  (other small per-session metadata fields)
  → small, hot in content thread's cache

Region 2 — ring storage, one giant mmap:
  storage[SESSION_COUNT_TOTAL × 2 × RING_BUFFER_BYTES]
  → large; accessed by io_uring kernel side; doesn't share cache
    with metadata
```

### Why this beats the embedded approach

1. **SoA cache behavior.** A content thread iterating active sessions
   per tick touches sequential cache lines per metadata array. AoS
   (`Session[N]`) would stride by `sizeof(Session)` per access,
   wasting line capacity on fields the loop doesn't use.
2. **No `ObjectPool<Session>` complexity.** Slot reuse is a free-list
   of `uint32_t` indices, not a typed pool with object lifetime
   management.
3. **No `Session*` pointer hazards.** Cross-thread refs pass an
   integer; no use-after-free, no dangling pointer concern.
4. **Migration becomes trivial.** Session is a handle; moving session
   X from channel A to channel B is flipping `channel_owning_id[X]`
   plus a drainage protocol. **No serialize/reconstruct cycle.** No
   memcpy of session state. No second allocation. See "Migration is
   single-thread-handleable" in §1.2 below.

### Migration is single-thread-handleable

A bonus that fell out of SoA + flat `session_id` space (discussed but
deferred as a v2+ topic):

```
Migration of session X from channel A to channel B:
  1. Thread A stops submitting new recv SQEs for session X
  2. Thread A drains in-flight CQEs for X, processes them
  3. Thread A flushes pending send_ring[X] data
  4. Thread A pushes POD {session_id: X, new_room: ...} → B's inbox
  5. Thread B pops the message, registers X as its own
  6. Thread B submits a recv SQE for X on B's io_uring ring
  7. Normal service resumes
```

The fd doesn't move (process-wide handle; both rings can submit SQEs
against it). The session metadata never moves (lives in the SoA arrays
at index X regardless of owner). Only *ownership* flips — and quiescence
during the flip is enforced by drainage in steps 2-3.

The original 2026-05-17 architecture had migration as a known-hard
"serialize → POD message → reconstruct on B" problem. SoA + flat
`session_id` makes it nearly trivial — a structural property of the
layout rather than a layered protocol.

### What's NOT possible

If we ever encoded `session_id = (channel_id << bits) | local_index`,
we'd lose this — migrating X from channel 0 to channel 2 would force
its `session_id` to change. Stable `session_id` across migration
(needed for logs, debug, persistent references, client reconnect
tokens) requires a **flat** `session_id` space and a separate
`channel_owning_id[]` field. The trade-off: lookup "which channel owns
this session" is now one extra array read instead of bit shifts. Cheap
vs the migration-without-realloc win.

---

## Part 2: Compile-time caps

### The locked values

```cpp
constexpr uint32_t CHANNEL_COUNT             = 2;     // start; expand to 4 if RAM allows
constexpr uint32_t SESSION_COUNT_PER_CHANNEL = 16384;
constexpr uint32_t SESSION_COUNT_TOTAL       = CHANNEL_COUNT * SESSION_COUNT_PER_CHANNEL;
constexpr uint32_t RECV_RING_BUFFER_BYTES    = 16384;
constexpr uint32_t SEND_RING_BUFFER_BYTES    = 16384;   // separate constant; may diverge later
```

`UPPER_SNAKE_CASE` matches Linux/POSIX convention (`PIPE_BUF`, `IOV_MAX`,
`PATH_MAX`) and the project's macro style (`LNX_CHECK`, `LNX_TRAP`).
Constants live in an `iouring_net::config` namespace (or equivalent
header).

### Memory budget

At `CHANNEL_COUNT = 2`:

| Item | Size |
|---|---|
| Ring storage | 32768 sessions × 32 KiB = **1 GiB** |
| SoA metadata | ~32 B/session × 32768 ≈ 1 MiB |
| Stitch buffers | 2 channels × 64 KiB = 128 KiB |
| **Total** | **~1 GiB** |

At `CHANNEL_COUNT = 4`: ~2 GiB. Stephen's dev box has 64 GiB RAM —
plenty of headroom to experiment with 4096-user rooms (broadcast
stress test) without memory pressure.

### Naming rationale

- `_COUNT` over `_MAX` — modern project; reads as fixed quantity (which it is). POSIX leans `_MAX`; either works.
- `SESSION_COUNT_PER_CHANNEL` over `CHANNEL_SESSION_COUNT` — prioritizes grep-locality for the SESSION_* cluster (`SESSION_COUNT_PER_CHANNEL`, `SESSION_COUNT_TOTAL`, future `SESSION_COUNT_PER_ROOM`). Slightly longer; better discoverability.
- Separate `RECV_` / `SEND_` ring constants — symmetric in v1, but split anticipated; one may need to diverge based on broadcast patterns or measured asymmetry.

### Resize policy

**Resize = redeploy with new constant.** Runtime growth would require
allocating a new linear region, pausing all content threads, migrating
every active session, and updating SoA pointers — a stop-the-world
event. For a portfolio-scale server, redeploy is simpler and the
right trade-off.

---

## Part 3: Ring buffer design

### SPSC by construction

Each session has two ring buffers:

```
recv_ring (SPSC):
  Producer: io_uring kernel side (writes recv'd bytes into ring)
  Consumer: content thread (frames bytes, dispatches)

send_ring (SPSC):
  Producer: content thread (writes frame bytes for transmission)
  Consumer: io_uring kernel side (drains bytes onto socket)
```

Single-producer / single-consumer falls out of the architecture's
kernel/content split. No CAS needed — only memory barriers via
`__atomic_*` builtins (acquire/release ordering through `lnx::atomic`).

### Index style: free-running uint64_t counters (Lamport SPSC)

```
uint64_t head;  // never resets — keeps incrementing as bytes are written
uint64_t tail;  // never resets — keeps incrementing as bytes are read

// To index into storage of size N (must be power of 2):
storage[ head & (N-1) ]
storage[ tail & (N-1) ]

// Occupancy:
bytes_in_ring = head - tail   // unsigned arithmetic; correct across wrap

// States:
empty: head == tail
full:  head - tail == N
```

Why uint64_t instead of uint32_t: at 16 KiB ring, uint32_t wraps every
4 GiB of throughput per session — the unsigned arithmetic still works
across the wrap as long as occupancy never exceeds 2^31, which is
guaranteed since N is way below that. But uint64_t removes the entire
concern (16 EiB wrap = never), and the size cost is zero — each
head/tail is in its own 64-byte cache line via `alignas(64)`, so the
extra 4 bytes inside that line are free.

This is the **same SPSC pattern io_uring's own SQ/CQ rings use
internally**. Decades old, well-understood, no CAS, no LOCK-prefixed
instructions.

### Cache layout: `alignas(64)` MUST

False sharing on head/tail is brutal for SPSC. If `head` and `tail`
share a 64-byte cache line:

```
Cache line [head][tail][... 32 more bytes ...]
            ^producer-only  ^consumer-only
```

When the producer writes `head`, the CPU's coherency protocol
invalidates that line in the consumer's cache. The consumer's next read
of `tail` (logically hot in its cache) must refetch the whole line from
L3 or RAM. Symmetric problem in reverse.

**Fix:**
```cpp
alignas(64) uint64_t head;   // padded to own cache line
alignas(64) uint64_t tail;
alignas(64) std::byte storage[N];
```

Cost: ~128 bytes of padding per ring's index pair instead of 16 bytes.
For 65536 rings (recv + send × 32768 sessions), ~15 MiB of padding —
rounding error against the 1 GiB of storage.

With the SoA pivot, padding must apply **per slot**:

```cpp
struct alignas(64) head_slot { uint64_t v; };
head_slot head_recv[SESSION_COUNT_TOTAL];
```

Otherwise `head_recv[i]` and `head_recv[i+1]` share a line, and if
sessions i and i+1 land on different content threads (different channel
ranges), the kernel-side write to `head_recv[i]` invalidates
`head_recv[i+1]` for the other thread. Cross-session-within-array false
sharing is the SoA-specific version of the problem.

---

## Part 4: Frame straddling

### Why it's unavoidable

io_uring's recv writes bytes from the socket into a user buffer.
The kernel doesn't know what a "frame" is — it just writes raw bytes
to the address you gave it. TCP delivers a byte stream that doesn't
get re-delivered. **Once 4 bytes of a 100-byte frame end up at ring
positions [16380..16383] and the next 96 at [0..95], the consumer must
handle the split.** There's no producer trick that avoids this with
TCP + io_uring + a wrap-around ring.

### Three options considered

**Option A — Stitch on demand (CHOSEN for v1):**

The ring is one flat 16384-byte buffer. Per channel, a small "stitch
buffer" (~64 KiB) holds the rare stitched frame.

```
peek 8-byte header (stitch the header itself if it straddles wrap)
if recv_ring has < h.size bytes: break (wait for more next tick)

if frame [tail, tail+h.size) does NOT cross the wrap point:
    frame_view fv = { ptr = &storage[tail & (N-1)], len = h.size }
    (zero-copy — bytes stay in recv_ring)

else (frame straddles wrap):
    memcpy first segment from [tail & (N-1), N) into stitch_buf
    memcpy second segment from [0, h.size - first_len) into stitch_buf + first_len
    frame_view fv = { ptr = stitch_buf, len = h.size }
    (one memcpy per straddle; rare)

dispatch(opcode, fv)
recv_ring.advance_tail(h.size)
```

Cost: one memcpy per straddled frame. Straddle probability ≈
frame_size / ring_size ≈ 64 / 16384 = 0.4%. Roughly 1 in 250 frames
takes the memcpy path; ~50ns per memcpy. Unmeasurable at chat scale.

**Option B — Double-mapping (mmap trick, always zero-copy):**

`mmap` the ring's storage such that the same physical pages are mapped
at BOTH `[base, base+N)` and `[base+N, base+2N)`. A read from
`&storage[N-4]` reads the last 4 bytes; a read from `&storage[N]`
automatically reads byte 0 of the ring (MMU resolves the VA back to the
same physical page).

`frame_view{ptr = &storage[tail & (N-1)], len = L}` is then always
valid as contiguous memory, even when L extends past the wrap. The
wrap is invisible to the consumer.

Cost at runtime: zero. Cost at boot: one extra mmap per ring (~65k
syscalls; few hundred ms). VA cost: 2× per ring (same physical RAM,
twice the VA range — free on 64-bit). Complexity: requires
`memfd_create` + `mmap MAP_FIXED` dance.

**Documented as future optimization.** Beautiful but not v1.

**Option C — Forbid wrap-straddle:**

Considered and rejected as fundamentally impossible. The producer is
io_uring (kernel side) writing raw bytes from TCP. It can't insert
"skip to wrap" markers or pad to frame boundaries — it doesn't know
what a frame is. Application-level framing can only happen on the
consumer side, by which point the bytes are already split.

### Why Option A for v1

- Standard implementation; many production SPSC protocol libs use this
- Straddle is genuinely rare at chat scale
- Easy to test (no platform-specific mmap dance)
- Memcpy cost is invisible against the broadcast + syscall costs that
  actually dominate per-message work
- Future migration to Option B doesn't change the ring buffer's
  external API — the framing layer's branch on straddle just goes away

---

## Part 5: Send-ring backpressure

### The problem

The content thread writes outgoing frame bytes into `send_ring`. The
kernel drains them via io_uring send completions. If the content
thread writes faster than the kernel drains (slow client, congested
network, TCP window collapse), the send_ring fills. **What does the
handler do when send_ring is full and another frame needs to go out?**

### The four candidate policies

| Policy | Pros | Cons |
|---|---|---|
| Drop frame, log | Simple; no session impact | Silent data loss; chat messages disappear |
| **Drop frame AND close session** | Eliminates slow drainers; bounds RAM by construction | Aggressive; transient slowness = disconnect |
| Block the handler (spin/yield until drain) | No data loss | **Violates the never-block principle** |
| Apply backpressure upstream (stop recving) | TCP's own flow control kicks in | Doesn't help if the session itself is the problem |

### The pick: drop frame + close session

Locked. Rationale (in Stephen's words): *"if send_buffer is full that
client should be disconnected, this is not purposed for finance trading
project."*

Reasoning:
- The content thread never blocks (compliant with the never-block
  principle from architecture doc Part 1.5)
- A client whose send_ring saturates is too slow to matter for
  real-time chat anyway
- Reconnect logic on the client side absorbs the disconnect
- Bounds RAM by construction — no session holds more than
  `SEND_RING_BUFFER_BYTES` worth, ever

### What about resize?

Two resize patterns were considered:

1. **Per-session dynamic resize** (give slow drainers a bigger ring).
   **Rejected.** Fights the SoA layout — every session's ring is at a
   known offset in the linear region; making one ring bigger requires
   AoS-style indirection or overflow-buffer bookkeeping. Lose the
   simplicity of the design.

2. **Global resize** (double `SEND_RING_BUFFER_BYTES` if pressure
   shows up across the fleet). **Accepted as the "we mis-sized"
   remediation, but it's a compile-time constant change + redeploy,
   not a runtime knob.** Runtime resize of a linear region with
   active sessions is a stop-the-world event.

"RAM is cheap" applies to **sizing** (we'll happily ship with 32 KiB
or 64 KiB send rings if the workload warrants it), NOT to
**misbehaving-client tolerance** (slow clients get disconnected,
period).

### Recv-side asymmetry (no policy needed)

For recv_ring, the dynamic is inverted: the producer is the kernel,
the consumer is the content thread. If the content thread can't drain
fast enough, recv_ring fills → io_uring writes pause → TCP socket
buffer fills → ACK window shrinks → sender slows. **TCP's own flow
control handles it.** No explicit recv-side policy needed.

---

## Part 6: session_id allocator — accept-thread-owned

### Strategy (locked; primitive design deferred)

The accept thread owns a free-list of available `session_id` values
(stack of `uint32_t` indices). On accept:
1. Pop a session_id from the free-list
2. Initialize the SoA metadata for that slot
3. Pick a destination channel (round-robin for v1)
4. Push a POD message to the destination channel's inbox: "session X
   is yours"

On disconnect (handled by content thread):
1. Close the fd
2. Push a "release session X" message back to the accept thread's
   inbox
3. Locally forget the session

The accept thread, when it drains its inbox, pushes released
session_ids back onto the free-list.

### What this means for cross-thread primitives

Two primitives are *implied* by this strategy:
1. Accept thread → content thread inbox (carries new-session POD)
2. Content threads → accept thread inbox (carries release-session POD; multi-producer)

**Both deferred for now.** Designing them is the natural next step
when implementing the accept path — but until then, don't shop for
MPSC variants or commit to ring vs Vyukov queue. The strategy doesn't
depend on which primitive lands.

### Why accept-thread-owned beats alternatives

- **Lock-free MPMC free-list:** more implementation complexity. The
  free-list isn't hot (one push/pop per session lifetime). Doesn't
  pay back.
- **Per-channel cache:** each channel pre-claims K session_ids;
  refills from a global structure. Lower contention on the global
  structure. Reasonable optimization if accept becomes a bottleneck,
  but premature for v1.
- **Accept-thread-owned:** single-thread mutation of the free-list
  (no atomics on the list itself; atomics only on the inbox). Clean.
  Standard.

### Subtleties

- **Crashed content thread leaks session_ids.** If a content thread
  dies, its sessions never send "release" messages, and their
  session_ids stay claimed. Architecture doc Part 8's "janitor"
  thread is the fix (heartbeat + lease eviction). **Deferred for v1.**
- **Free-list empty = accept refusal.** Pop fails → `close(fd)` on
  the incoming connection. Signals capacity clearly; client retries.
- **Routing policy.** Round-robin for v1. Revisit if profiling shows
  channel skew. Least-loaded routing requires an atomic per-channel
  counter — possible but premature.

---

## Part 7: Framing — stateless

### The pitch

The session's framing state is **just `recv_ring.tail`.** No
HEADER_PENDING / BODY_PENDING / DISPATCH FSM. No per-session
`framing_state[]` array in the SoA.

Each tick, per session:

```
while recv_ring.available() >= sizeof(packet_header):
    packet_header h;
    peek_header(&h)                          # stitch if header straddles wrap

    if h.flags   != 0:  close_session(MALFORMED); break
    if h.version != 0:  close_session(VERSION_MISMATCH); break
    if h.size    <  sizeof(packet_header):  close_session(MALFORMED); break
    if h.size    >  recv_ring.capacity():   close_session(OVERSIZED); break

    if recv_ring.available() < h.size:
        break                                # wait for next tick

    frame_view fv = recv_ring.read_frame(h.size)   # stitch if wrap-straddled
    dispatch(opcode, fv)                     # zero-copy view passed in
    recv_ring.advance_tail(h.size)
```

### Why stateless

1. **Tail IS the state.** Tail position tells you "where the next
   frame's header starts." That's all the framing state any session
   needs.
2. **Re-parsing the header for slow-arriving frames is fine.** If only
   4 bytes arrived this tick, break out; next tick peek the (now
   8-byte) header and continue. The "wasted" parse is ~5 ns —
   imperceptible against the inter-tick gap.
3. **One less field in the SoA layout.** No `framing_state[]` array
   needed. Less metadata, more cache.
4. **Easier to reason about.** "Given the bytes in the ring, what
   frames can I extract right now" is a pure function of the ring
   contents. No state machine to draw on a whiteboard.
5. **Malformed-frame recovery is just `close_session`.** No RECOVERY
   state — the session is gone.

### Practical implications

- The recv_ring needs a **peek** operation that returns the next N
  bytes without advancing tail (since we peek the header, validate,
  then decide whether to advance).
- The **stitch buffer is channel-local, not per-session.** Header
  stitches are 8 bytes; frame stitches up to `MAX_FRAME_SIZE`. One
  scratch buffer per channel, used inside a single `read_frame` call,
  never crosses ticks. ~64 KiB scratch per channel covers any
  reasonable frame size.

---

## Part 8: Dispatch — Posture A (library = pure transport)

### The architectural question

Two postures for how the network library handles opcodes:

**Posture A: library is pure transport.**
```
network library:  recv → frame → call user-provided callback(session_id, frame_view)
chat server:      provides callback → inside callback, switch(opcode) { ... }
```
Library doesn't know what `CMSG_CHAT_MSG` means. The opcode switch lives entirely in the chat server.

**Posture B: library provides dispatch infrastructure.**
```
network library:  recv → frame → look up opcode in dispatch_table → call handler
chat server:      registers handlers: register(CMSG_CHAT_MSG, &chat_msg_handler)
```
Library owns the dispatch table; chat server populates it.

### Why A

- **Library stays content-agnostic.** Library has one callback
  signature and never inspects opcode meanings.
- **Different content layers don't change the library.** Chat server,
  future game server, future MMO — each provides its own callback
  with its own switch.
- **Library doesn't grow a registration API or a sparse opcode
  table.** Less infrastructure to ship.
- **Test mocking still possible at the *callback* level** (inject a
  test-stub callback instead of `chat_dispatch`).

### Library API shape

```cpp
using frame_dispatch_fn = void (*)(uint32_t session_id, frame_view& fv);
channel_init(..., frame_dispatch_fn on_frame);
```

Chat server implements:

```cpp
void chat_dispatch(uint32_t sid, frame_view& fv) {
    packet_header h = read_header(fv);
    switch (h.opcode) {
        case CMSG_JOIN_ROOM:    join_room_handler(sid, fv); break;
        case CMSG_SEND_MESSAGE: send_message_handler(sid, fv); break;
        // ...
        default:                close_session(sid, BAD_OPCODE);
    }
}
```

### switch vs function pointer table

**They generate the same code at chat scale.** A `switch (uint16_t)`
with consecutive-or-near-consecutive cases compiles to a jump table —
literally a function pointer table the compiler builds for you. Same
indirect call, same branch predictor behavior, same icache pressure.

The choice is purely code organization:

| | switch-case | function pointer table |
|---|---|---|
| Codegen | Jump table (compiler-managed) | Jump table (you manage) |
| Where dispatch logic lives | One place, easy to grep | Distributed across registrations |
| Adding an opcode | Touch the switch + add handler | Touch the table + add handler |
| Type safety | Compiler verifies each case's signature | All handlers must share one uniform sig |
| Memory | Cases compile into ~10 bytes each | Pointer per opcode-space entry (32K × 8 B = 256 KB sparse table) |
| Test mocking | Can't replace at runtime | Swap a pointer to install a stub |

For chat with ~10–20 opcodes: switch-case in the chat server.
X-macro pattern is available if the opcode count grows past ~30, but
not needed for v1.

---

## Part 9: What this supersedes

### Wiki/spec staleness

Several `wiki/*.md` files predate this pivot and now have stale
sections. Each has been annotated with a banner pointing at this
discussion + the `project-chat-server-v1` memory entry:

- `wiki/network/packet_header.md` — reception flow uses obsolete
  `mem::alloc(h.size) + drain into serial_buffer` pattern; the
  8-byte header layout itself is unchanged.
- `wiki/network/cs_packet.md` — described as packet_pool allocation;
  current state is a zero-copy `frame_view`. The typed read API
  surface is unchanged.
- `wiki/network/sc_packet.md` — packet_pool staging + memcpy-to-ring
  may be replaced with direct staging into a per-channel scratch.
  The typed write API surface is unchanged.
- `wiki/network/packet_pool.md` — the "Tier 3 of three memory tiers"
  framing is obsolete; bucket-sized free-list pattern remains useful
  for content-thread-local packet-shaped objects but is no longer
  mandatory on the hot path.
- `wiki/runtime/threading_model.md` — "two-tier reactor" framing
  (separate network thread pool + content thread pool) was replaced
  by content-threads-own-io_uring + single accept thread. The
  three-tier memory framing also obsolete. Single-thread content
  layer, never-block-on-cross-thread-locks, per-session SPSC ring,
  `LNX_CHECK` / `tl::expected` are unchanged.

### Memory entry updates

- `project-chat-server-v1.md` — new entry, captures all 10 TL;DR locks
- `project-architecture-v1.md` — annotated at top with "see
  project-chat-server-v1 for current shape"
- `feedback-defer-primitives.md` — new entry: don't shop for
  cross-thread primitives until the consuming layer is being built
- `feedback-no-std-primitives.md` — extended from sync-primitives-only
  scope to project-wide "pragmatic no-STL" policy

---

## Part 10: Side decisions made today

### PLAYER_COUNT_PER_ROOM

A separate cap (room concept; deferred as a topic) from
`SESSION_COUNT_PER_CHANNEL`. The discussion landed on **1024 as the
sweet spot** for chat v1:

- Comfortably within broadcast CPU budget at any realistic chat rate
- send_ring drop risk stays low even with bursty chatter
- "1024-user rooms" is impressive without sounding fanciful
- Maps cleanly: channel with 16384 sessions can host 16 max-cap rooms,
  or many smaller ones

**Stretch to 2048** for a single eye-catching number. Works, but
broadcast eats into tick budget noticeably at chatty workloads.

**4096 as test-only.** Stephen's 64 GiB desktop can run it; produces a
useful stress test. send_ring drops become a real concern under burst
load. Real systems serving rooms this large use pubsub fan-out.

**Coupled scaling:** when PLAYER_COUNT_PER_ROOM scales up,
SEND_RING_BUFFER_BYTES should scale proportionally — broadcast
amplifies, drop risk per recipient rises with broadcast count.
Worth treating these constants as a paired tuning knob.

### Coroutine doc cleanup

Coroutine-driven per-session `task<T>` was explicitly rejected at the
2026-05-17 pivot ("doesn't fit tick loop, conflicts with
pre-allocation"). The wiki specs were updated then; user-facing docs
in `docs/*.md` never were, leaving misleading claims that the project
is "coroutine-based" and references to `task<T>` in subsystem
inventory and test plans. **Cleaned up in commit `738d2bb`.** Eight
docs touched; coroutine references removed except in
`docs/discussions/` (historical snapshots).

### `LNX_DCHECK` release-mode no-op bug

Found during this discussion's review of `src/sync/mutex.h`:
`LNX_DCHECK` expanded to `((void)0)` under NDEBUG, so every
`pthread_mutex_*` and `pthread_rwlock_*` call wrapped in it was
elided in release builds. `lnx::mutex` was a no-op in production.

**Fixed in commit `8930c59`.** All 13 sites converted to `LNX_CHECK`
(which always evaluates its expression). `LNX_DCHECK` macro removed
entirely from `src/check.h` (no other callers). Both debug and
release presets pass 73/73 tests, including the read-heavy
shared_mutex stress test that exercised the previously broken path.

Decision rationale: the user pointed out the original 2-line "extract
rc to a variable, DCHECK on it" fix preserved the macro's
side-effect-elision bug class. The clean fix was to swap the macro
choice — `LNX_CHECK` is the right macro for mutex ops (always
evaluates; the perf delta vs DCHECK is rounding error against the
~20ns pthread syscall). Both `libstdc++` and `libc++`
`std::mutex::lock()` also unconditionally check the pthread return.

### `std::` namespace policy extension

Previous memory (`feedback_no_std_primitives.md`, written 2026-05-15)
scoped the "no std::" rule to `src/sync/` only — higher layers were
allowed to use `std::` freely. `docs/04-coding-style.md` even
contained the explicit guidance *"Prefer `std::` over project-rolled
where semantics match"* + a table endorsing `std::unique_ptr`,
`std::shared_ptr`, `std::vector` off hot path.

Stephen flagged this as contradicting the actual project goal:
*"my goal is nearly not using STL and namespace std."*

**Policy extended project-wide:**
- Permitted: type traits, typedef aliases (`std::size_t`,
  `std::byte`, etc.), C-library funcs (`std::memcpy`,
  `std::aligned_alloc`, etc.), lightweight view types
  (`std::span`, `std::string_view`), `<bit>` helpers.
- Banned: owning containers, smart pointers, callable wrappers,
  sync primitives (already covered by `lnx::`), error/fatal types
  (already covered by `LNX_TRAP` + `tl::expected`), streams, format
  (already covered by `{fmt}`).

`docs/04-coding-style.md` and the memory entry both updated in this
commit.

---

## Open questions / deferred topics

These came up during the discussion but were not closed:

1. **Channel context structure** — Where do the SoA arrays actually
   live? One global state singleton, or per-channel
   `channel_context` with each channel owning its session_id range?
   Either works; decision deferred to implementation time.
2. **`std::array` borderline case** — Banned for stylistic
   consistency, but it's lightweight enough that it could move to
   "permitted." Revisit if a concrete need shows up.
3. **`std::chrono`** — Not explicitly settled. Mostly compile-time
   types, no allocation. Probably permitted; flag for confirmation
   when next encountered.
4. **Room concept inside a channel** — How "channel hosts N rooms"
   lays out: per-channel `room_table`, room membership lists,
   broadcast iteration mechanics. Deferred to next session.
5. **Content thread's tick loop concrete shape** — Drain CQEs, frame,
   dispatch, submit SQEs — the order and any intra-tick parallelism.
   Deferred to implementation.
6. **io_uring setup specifics** — ring depth, IORING_SETUP flags,
   single-shot vs multishot recv, fixed-buffer registration. Deferred
   to implementation.
7. **MPSC inbox primitive design** — Exact shape (bounded ring vs
   Vyukov queue) deferred per `feedback-defer-primitives` rule.
8. **Migration drainage protocol** — Outlined in §1.2 above but not
   fully fleshed out. Deferred per Stephen's "migration is another
   topic later."
9. **Listen socket and accept loop concrete shape** — How the accept
   thread sets up listen socket, accepts connections, optionally uses
   SO_REUSEPORT for multi-accept. Deferred to implementation.

---

## Next concrete step

Largely unchanged from the morning architecture doc's recommendation,
just refined by today's locks:

1. **Define caps in one header** (`src/iouring_net/limits.h` or
   `src/iouring_net/config.h`)
2. **Build the smallest end-to-end thing** — single channel, single
   session_id slot, echo over io_uring with the SoA layout
3. **Iterate to v1** — multiple sessions, accept thread, multiple
   channels, MPSC inboxes, framing layer
4. **Chat semantics on top** — opcode switch, room concept, broadcast
5. **Polish + benchmarks** — what makes it a portfolio piece

The data model is the part that's now hard to change without
re-engineering. The mechanism layers on top.
