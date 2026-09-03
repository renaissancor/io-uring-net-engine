---
status: accepted
note: stage B of 2026-09-02 § 7 — the io_uring data path in server-uring, built to the stage C shape so the same load generator and the same instrument read both servers; protocol parity with the study server is a measurement decision, recorded here with what it defers
---
# 2026-09-03 — Stage B: the io_uring data path, built to be measured against stage C

`server-uring/src` has the roles, the mesh, and the authority table, and no
byte has ever crossed a socket in it. Stages A and C fixed the control it will
be compared against: `server-epoll` with `CHAT_FLUSH=batch CHAT_TICK_HZ=30
CHAT_TICK_MODE=coalesce`, ~3 µs of I/O per active connection per tick, one
wake and two `recv()` in, one `send()` per player per tick out
([`result-notes/2026-09-03-the-tick-budget-stage-c.md`](../result-notes/2026-09-03-the-tick-budget-stage-c.md)
§ 4). This note decides what the io_uring side builds first, and what it
does not, so that the first number is comparable.

## 1. The decision: protocol parity, measurement first

The io_uring server speaks the **study protocol** — `server-epoll`'s wire
format, frame types (1/2/3 in, 100/101/102 out), `"nick: text"` payloads,
and the type-102 tick frame — byte for byte. `loadgen --proto study` then runs
against both servers unmodified, `fleet.py`/`merge.py` gate both the same
way, and `ticksweep.py --server` points at either binary.

This defers the product protocol of
[`../server-uring/doc/10-realtime-server-architecture.md`](../server-uring/doc/10-realtime-server-architecture.md)
§ 7 (`S_WELCOME`, `C_SELECT_ROOM`, `S_ENTER_WORLD_OK`, the header whose
length includes itself). That protocol is the product's; this stage is the
instrument's. Building the product protocol first would mean a second client
path, a second verdict calibration, and a comparison whose two sides differ
in protocol as well as mechanism. The seam already exists for later:
`--proto iouring` in the client switches framing, and the handler table is
one switch statement. § 7 of doc/10 gets a status line saying so in the same
commit.

## 2. What the worker owns

One `io_uring` per worker, and everything the study server keeps in
process-globals, in the worker's engine body — the same structures
`server_sds.cpp` already proved on the engine's primitives, ported rather
than redesigned:

- **sessions**: a flat, fd-indexed slab of records inside one
  `mmap(MAP_NORESERVE)`; per session an `sds::ring_buffer` for inbound
  bytes and a linear outbound queue with the 256 KiB cap and the same
  `[drop] … over cap` line the saturation notes count;
- **rooms**: records in the slab found through `sds::cstr_hash_map`,
  membership an intrusive doubly-linked list through the sessions, one
  tick buffer per room;
- **no STL** — the project rule, and the reason the port is from
  `server_sds.cpp` and not from `server.cpp`.

`config::k_session_capacity` rises from 256 to 16,384 (the 10k row plus
headroom); the mesh pipes that its static_assert sizes move from `main`'s
stack frame to static storage.

## 3. The I/O shape, and the knobs it deliberately does not turn yet

The hypothesis' `c_cqe` assumed multishot receive with provided buffers. That
is what lands, because a one-shot-recv first cut would understate the
difference and § 6.2 of the hypothesis says so.

- **Receive**: `IORING_OP_RECV` multishot per session, into a **provided
  buffer ring** (kernel 6.6, liburing 2.5 on this box). A completion names
  the session and a buffer; the bytes are committed to the session's ring,
  frames parsed, handlers run, the buffer returned. No re-arm per input.
- **Send**: one `IORING_OP_SEND` SQE per player per tick from the room walk,
  **one `io_uring_submit` for the batch**. A completion advances the session's
  outbound queue; a short send resubmits the remainder. Bytes stay in the
  session's queue until the completion says the kernel is done with them.
- **Wake**: the loop waits with `io_uring_wait_cqe_timeout` bounded by the
  tick deadline — the same shape as `epoll_pwait2` in the control, and the
  bounded-timeout option doc/10 § 9 allows for a first pass. The acceptor's
  `adopt_session` arrives through the mesh pipe as today, plus an eventfd
  the acceptor writes and the worker keeps a read SQE on, so an adoption
  wakes the worker without waiting for a tick.
- **Not yet, as named knobs for later rows**: `SQPOLL`, registered files,
  registered/fixed buffers, `SEND_ZC`, `DEFER_TASKRUN`/`COOP_TASKRUN`,
  NAPI busy-poll. Each is a row of its own once the plain shape is
  measured; turning any on before the first number would make the first
  number un-attributable.

## 4. The acceptor

Plain `accept4` in a loop behind a 100 ms `poll()` on the listen socket, so
`request_stop()` is observed without an eventfd on that side. For each
connection: mint the session, `mesh_post` `adopt_session`, write the
worker's eventfd. A post that fails for lack of pipe room closes the fd and
counts it — the existing contract. Multishot accept on the acceptor's own
ring is a later refinement; accept is not on the path being measured.

## 5. The tick and the instrument

`tick_logic.h` moves from `server-epoll/` to `common/` at the repository
root and both servers include it from there — the byte-identity rule of
`2026-09-02` § 7.2 as a file location rather than a promise. The worker's
loop calls the same hooks in the same order: `timeout_ns()` before the
wait, `drain_begin/end` around CQE processing, `maybe_tick(flush_rooms)`,
`flush_begin/end` around the submit of the tick's sends. "Wakes" counts
returns from the wait. The dump format is the one `ticksweep.py` already
parses.

`CHAT_TICK_MODE=coalesce` is the only delivery mode the io_uring server
implements in this stage; `immediate` (stage A's shape) is not ported,
because § 4 of the stage C note established that comparing against it would
credit the mechanism with the tick's batching.

## 6. Predictions, before the loop exists

From the stage C constants and the hypothesis' per-completion range, at
`W = 64`, 30 Hz, one input per connection per tick:

| N | epoll, stage C (measured) | io_uring, predicted | predicted share |
|---:|---:|---:|---:|
| 300 | 3.2 µs / input, 3 % | 0.6–1.0 µs | ~1 % |
| 1,000 | 2.9–3.6 µs, 9–11 % | 0.6–1.0 µs | 2–3 % |
| 3,000 | 3.2–3.6 µs, 29–32 % | 0.6–1.0 µs | 6–9 % |
| 10,000 | 1.5–2.5 µs, 44–75 %, burst 11–19 ms | 0.6–1.0 µs, burst 3–6 ms | 20–30 % |

- **P8 — the receive side.** Drain per input falls from ~1.1–1.4 µs (one
  `epoll_pwait2` return, two `recv()`) to **0.3–0.5 µs** (one CQE, one
  buffer return, the same parse and handler). Above 0.7 µs the `c_cqe`
  estimate of the hypothesis was low and § 3 of it is corrected.
- **P9 — the send side.** One SQE plus a shared submit for a ~1 KB frame
  costs **0.3–0.5 µs** against `send()`'s ~2 µs; the copy into the queue is
  common to both. Above 1 µs, the per-op saving is smaller than assumed.
- **P10 — wakes.** Wakes per period stay ≈ `N_a` at 300–3,000: each input
  is one CQE and the loop wakes per CQE unless it is told to wait for
  several. This is a property of the wait, not the mechanism; the syscall
  saving is one `io_uring_enter` per wake against three syscalls per input.
  If wakes fall well below `N_a`, the kernel is coalescing completions and
  the row says so.
- **P11 — the burst.** At 10k, 10,000 sends leave as SQEs in one submit; the
  flush phase (the submit and the completions' bookkeeping) is **3–6 ms**
  against 11–19, and client p50 falls back toward half a period. If the
  flush stays above 10 ms, the kernel's per-op send cost, not the syscall
  boundary, was the burst.
- **P12 — CPU.** Server CPU at 3,000 falls from 49 % to **20–30 %**; at
  10,000 it leaves saturation for the first time on this box.
- The two-tier verdict of `2026-09-02` § 5 is then read as: at 300 per
  thread the difference is 2 % of a tick and invisible next to any logic;
  at 3,000 it is 20–25 % of the tick; at 10,000 it is the difference
  between a saturated core and a server with headroom.

## 7. Order of work

1. `common/tick_logic.h` (move; `server-epoll` Makefile points at it).
2. Acceptor: listen, accept, mint, `adopt_session`, eventfd wake.
3. Worker: ring init, provided buffer ring, adopt → session slot →
   multishot recv; CQE loop; framing and the study handlers; sends.
4. Tick, coalesce, the hooks, `CHAT_*` knobs read from the environment as
   the control reads them.
5. Smoke: `loadgen` at 200 connections against the ASan build; then the
   verify path the control used.
6. `ticksweep.py --server uring-server --mode coalesce --only-w64`, two
   passes; result note against the stage C rows; doc/10 § 7 and § 12
   status lines; one doc per new unit, same commit as the unit.

## 8. Rejected

- **Product protocol first.** § 1: a second client path and an
  un-attributable comparison.
- **One-shot recv first.** Understates the difference by the hypothesis'
  own § 6.2; the multishot path is the claim under test.
- **A separate `uring` build of `tick_logic.h`.** The header's value is
  being the same bytes; `common/` makes that a path, not a promise.
- **Porting `immediate` delivery.** The stage C note already settled which
  shape is the control.
- **Turning on `SQPOLL` or fixed buffers for the first row.** Each is a row;
  none is the baseline.

## Rationale links

- [`2026-09-02-where-io-uring-becomes-meaningful.md`](2026-09-02-where-io-uring-becomes-meaningful.md)
  § 3 for `c_cqe`, § 6.2 for why multishot lands first, § 5 for the verdict
  the predictions are read against.
- [`2026-09-03-stage-c-tick-coalesced-delivery.md`](2026-09-03-stage-c-tick-coalesced-delivery.md)
  and [`../result-notes/2026-09-03-the-tick-budget-stage-c.md`](../result-notes/2026-09-03-the-tick-budget-stage-c.md)
  — the control's shape and constants.
- [`2026-09-02-control-group-on-engine-primitives.md`](2026-09-02-control-group-on-engine-primitives.md)
  — `server_sds.cpp`, the structures § 2 ports.
- [`../server-uring/doc/10-realtime-server-architecture.md`](../server-uring/doc/10-realtime-server-architecture.md)
  § 7–9 — the handoff protocol kept, the product protocol deferred, the
  blocking-wait caveat § 3 answers with a bounded timeout and an eventfd.
