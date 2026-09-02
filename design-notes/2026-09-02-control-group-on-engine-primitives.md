---
status: accepted
---
# 2026-09-02 — The control group on the engine's primitives, and how the instrument was optimised

The repository's first purpose is optimisation work that can be shown
([`2026-09-02-design-notes-drift-review.md`](2026-09-02-design-notes-drift-review.md)
§ 0). Two STL programs bounded what could be shown: every collapse in
`result-notes/` was the load generator's, and the epoll control differed from
the io_uring server in its data structures as well as its I/O mechanism, so a
future io_uring-versus-epoll delta would not have been attributable to the I/O
layer. This note records the decisions behind fixing both. The numbers are in
[`../result-notes/2026-09-02-stl-to-sds-the-measured-delta.md`](../result-notes/2026-09-02-stl-to-sds-the-measured-delta.md);
the code is described by
[`../server-epoll/doc/server_sds.md`](../server-epoll/doc/server_sds.md) and
[`../client-bench/doc/INDEX.md`](../client-bench/doc/INDEX.md).

## 1. Two constraints, fixed before anything was read

- **The published baseline does not change.** `server-epoll/server.cpp` is
  not edited. The optimised server is a sibling file and a second build
  target, so the STL-versus-custom delta is itself a measured row.
- **The instrument's measurement semantics do not change.** Intended-send
  timestamp, per-socket recv stamp, node stamp, fan-out and connection-loss
  gates, verdict exit codes, dump format v1. Proof is the correctness judge
  and a fixed low-rate run producing the same histogram shape before and after.

## 2. Profile first: where the cycles actually were

The prompt's list of STL suspects was checked against the source and against
callgrind before anything was replaced, and the profile changed the order of
work.

- **The client's time was not in its containers.** At 3M and 10M deliveries/s
  every `loadgen` was at 100 % of a core, split roughly half user, half
  kernel. Under callgrind the receive-parse path (`read_available` +
  `consume_frames`, `std::string` included) was ~9 % of user instructions.
  The kernel half was ~90 % `recv()`, and there were **exactly two `recv()`
  per readable socket** — one with data, one returning `EAGAIN` — because the
  drain loop ran to `EAGAIN` under level-triggered epoll. Stopping at a short
  read halved them (200,022 → 100,056 for 100,650 frames) and moved the 10M
  point from a client collapse with 12 nodes to a clean run. The data-structure
  work on the client (a per-fd receive slot in an `mmap` slab, direct sends)
  was done second and predicted to be worth ≤ 5 %.
- **The server's user-space share was 13–26 % of its CPU**, and about half of
  that was STL bookkeeping: `unordered_map<int, conn>` lookups (~14 % of user
  instructions), the room `unordered_map<string, unordered_set>` (~8 %),
  `std::string` append/erase/growth (~25–30 %), a `malloc`/`free` pair per
  inbound message (~4 %). Prediction written before the build: 64 B ceiling
  +10–20 % from data structures alone, ~0 at 512–1024 B where the server is
  byte-bound.

## 3. Decisions

### 3.1 The sds server links the installed engine through `find_package` — the seam, not a copy

Copying the five headers into `server-epoll/` would have kept the study build
standalone. Rejected: the point of the second build is that the control and
the product share primitives, and the seam is the thing that guarantees it.
`server-uring/README.md` also still owed "a probe that round-trips bytes
through the *installed* `sds::ring_buffer`, so the seam fails loudly" — the
sds server is that probe, and it did fail loudly once: the July prefix in
`~/.local` carried the retired non-template `ring_buffer`, and the wrong include
spelling (`<iouring_net/sds/…>` instead of the installed `<sds/…>`) resolved
against it. CI's `engine-and-server` job now builds `server-sds` against the
same prefix it installs.

Cost accepted: `make sds` needs cmake and an installed engine, and the
engine's imported target drags `fmt`, `liburing` and `tl::expected` link
interfaces along for header-only templates. `make` stays plain: it is the
published baseline's build path and part of what was measured.

### 3.2 Sibling file, not a compile-time variant of `server.cpp`

`#ifdef`s would edit the published file's text and break the LESSON markers
that `README.md` cross-references. `server_sds.cpp` is `server.cpp` with the
containers swapped and nothing else moved; the diff between the two files is
the portfolio artefact.

### 3.3 The client optimisation lands on the existing `loadgen`, staged as commits

A `loadgen-sds` variant would have left the recorded baselines on the old
binary and every future row on the new one, with the note having to carry two
instruments. Instead each change is its own commit, each measured row names the
commit that produced it, and the gate is the judge plus the same-shape check of
the recorded 3M row. The row reproduced within noise after each commit.

### 3.4 The instrument does not include engine headers

The repository's rule is that an instrument living inside one of the two things
it compares is not an instrument. Linking the engine into `loadgen` would
couple a `ring_buffer` defect to both the io_uring server and the thing
measuring it. The client got the *techniques* — a fixed per-fd receive slot in
one `mmap(MAP_NORESERVE)` slab, `recv()` straight into it, in-place parsing, a
frame built once on the stack and handed to `send()` — with no engine
dependency. `client-bench/README.md`'s "the STL stays" now reads "the STL stays
out of the hot loop".

### 3.5 Same send cap, same drop-and-close, same syscall shape

For the delta to be attributable to data structures, three things had to be
held equal: the 256 KiB cap and its `[drop] … over cap` line (the saturation
note counts it to find the ceiling), the `recv()` request size (64 KiB) and
drain-to-`EAGAIN` loop, and one `send()` per dirty connection per batch.

Two consequences:

- **The send side is a linear queue, not `sds::ring_buffer`.** A 256 KiB ring
  per connection cycles through all its pages — 2.5 GB resident at 10k
  connections. The linear `[head, tail)` queue resets to offset 0 when it
  drains (after nearly every batch flush), compacts once if a permanently
  part-sent connection walks its tail to the region end, and touches only its
  peak depth.
- **The receive ring is filled by `enqueue()` from a 64 KiB scratch, not
  through `direct_enqueue_ptr()`.** The zero-copy fill is the io_uring path's
  shape; its contiguous run shrinks to a few bytes near the wrap and would turn
  one baseline `recv()` into several. One copy, as `std::string::append` did.
  `CHAT_SHORT_READ=1` applies the client's short-read stop to the server as a
  separately measured row.

### 3.6 What the first measurement said, and the rule it forced

With the syscall shape held equal, the `sds::` server cut its user-space share
where load is high (22–25 % → 17–18 % at 8–10M deliveries/s, sweep p50 −8 %)
and lifted the 1024 B lossless rung (2.0M deliveries/s with zero closes where
the STL server shed 1,310 connections — the byte-bound "no change" prediction
was wrong in the good direction, because `std::string::erase(0, n)` on a
200 KB send queue is a memmove per `send()` and the linear queue has none).
At 3M it costs about 4 % more user share: its cost is per connection per
sweep, the STL server's is per message, and the two cross between 3M and 8M.

The first 3M pair read +25 % latency for `sds::`, which looked like a layout
cost (a 32 KiB ring per connection cycling through its pages). A repeat read
+3 %, and ring-size variants were within noise. The rule that survives is
methodological and is now in the result note's method: **on this box a single
pair cannot support a delta under ~15 %; alternate the two sides back-to-back
and repeat.** The layout question is open, not answered, and it matters for
the io_uring server's per-session rings, which have exactly this shape.

## 4. Rejected

- Copying the `sds::` headers into `server-epoll/` (§ 3.1).
- A `#ifdef` variant inside `server.cpp` (§ 3.2).
- A separate `loadgen-sds` binary (§ 3.3).
- The client including engine headers (§ 3.4).
- A 256 KiB `sds::ring_buffer` per connection for the send side (§ 3.5).
- Filling the receive ring through `direct_enqueue_ptr()` in the control (§ 3.5).
- `fmt` for the control's log lines: a new variable in a comparison; `printf`
  is off the hot path.
- Reading server CPU % as the outcome: it read 100 % on every rung of every
  pair, exactly as [`../result-notes/2026-08-30-what-limits-the-server.md`](../result-notes/2026-08-30-what-limits-the-server.md)
  § 2 says it would.

## Rationale links

- [`../result-notes/2026-09-02-stl-to-sds-the-measured-delta.md`](../result-notes/2026-09-02-stl-to-sds-the-measured-delta.md) — the numbers, and the layout variants.
- [`../server-epoll/doc/server_sds.md`](../server-epoll/doc/server_sds.md) — the built code.
- [`../server-epoll/server.cpp`](../server-epoll/server.cpp) LESSON 1 — the level-triggered drain loop the client's change rests on.
- [`2026-09-02-where-io-uring-becomes-meaningful.md`](2026-09-02-where-io-uring-becomes-meaningful.md) § 8 — why the control for io_uring is this server in `batch` mode.
