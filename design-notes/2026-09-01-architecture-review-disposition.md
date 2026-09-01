# 2026-09-01 — Disposition of the pre-data-path review

What happened to the six items in
[`2026-08-21-architecture-review.md`](2026-08-21-architecture-review.md).
That note is not edited — it records what was true on the day it was
written, and "recording without acting; discuss individually" was true then.
This is the acting.

Three items needed no decision, only work, and they are done. Two are still
open decisions. One is half-done by nature.

## Landed

| item | commit | what changed |
|---|---|---|
| 3 — close-notify cannot drop | `0c52218` | `static_assert` in `mesh.h` ties `k_session_capacity` × frame size to the pipe's capacity, with the protocol rule that makes the bound sufficient written beside it |
| 5 — `start()` traps on missing pipes | `0c52218` | `LNX_CHECK` on the mesh edges in `worker_ctl::start()` and across the whole roster in `acceptor_ctl::start()` |
| 6 — TSan in the verify loop | `253e88a` | 200k-frame cross-thread `enqueue2` torture test; `concurrency-tsan` CI job over the install-prefix round trip |

Two things are worth keeping from doing them.

**The item-3 assert was not vacuous.** Shrinking the pipe to 4 KiB fails the
build with its own message. An assert nobody has watched fail is a comment
with extra syntax.

**The item-5 check found its own callers.** Three `worker_ctl` lifecycle
tests could not start once the guard existed, because they had never wired
their edges — the exact silent-race shape the review predicted, sitting in
the test suite. They now supply edges.

Item 6 also settled the side question the review left open: tests are
exempt from the no-STL rule, on purpose, because the STL is the oracle they
check against. Recorded in
[`../engine-uring/doc/04-coding-style.md`](../engine-uring/doc/04-coding-style.md)
§ "Tests are exempt, on purpose". Neither of the two files the review
compared needs changing.

## Still open

**Item 1 — every wake reason is a CQE.** Undecided, but it is hard to see
the other side: a worker that cannot be woken either busy-polls (burning a
core per worker, in a repository whose headline claim is about where the CPU
budget goes) or carries the admission-latency hole the review names. Cheap
now — `worker_engine::run_loop()` is six lines — and expensive after the
data path lands on top of it.

**Item 2 — fd ownership in flight.** Half of this is one invariant sentence
in `server-uring/doc/10-realtime-server-architecture.md` §4 and can land any
time. The other half is drain logic in a `run_loop()` that has no drain to
do yet, so it belongs with the data path rather than ahead of it.

**Item 4 — mirrored vs modular recv ring.** One premise of the review has
since been checked and does not hold. It argued the decision had to precede
the parse loop because retrofitting would be expensive; but
`ring_buffer.h`'s zero-copy path is already a separate API
(`direct_dequeue_ptr` / `direct_dequeue_size` / `commit_dequeue`) whose
contiguity limit lives in one line:

```cpp
return umin(avail, to_end);   // mirroring makes this `return avail;`
```

Every caller already has to handle "the contiguous run is shorter than the
frame" by falling back to `dequeue()`. Under a mirrored mapping that branch
simply stops being taken. The retrofit is local, so the decision is not
actually blocking the parser, and choosing the complex option before there
is a single io_uring measurement to justify it is the trap
[`../result-notes/2026-08-30-what-limits-the-server.md`](../result-notes/2026-08-30-what-limits-the-server.md)
was written about.

## The thing none of the six is

All six are hardening. `server-uring` still cannot accept a TCP connection:
no listen socket, no `io_uring_queue_init`, no session storage.
`worker_engine::run_loop()` bumps a heartbeat and yields. The baseline table
in the root README says it "gets rewritten the day the io_uring server is
measured", and that day needs a vertical slice — listen, multishot accept,
`adopt_session` over the mesh, per-worker recv/send, and the same chat
protocol `server-epoll` speaks — not more hardening. That slice would also
answer which of items 1, 2 and 4 actually mattered, which is currently a
guess.
