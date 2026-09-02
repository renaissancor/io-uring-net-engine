# 2026-09-02 — STL to `sds::`: the measured delta, on the client and on the control

> **Why this file exists.** Purpose item 1 is optimisation work that can be
> shown. This note shows it twice: the load generator was made able to drive
> the load it used to collapse under, and the epoll control group was rebuilt
> on the engine's `sds::` primitives so the STL-versus-custom difference is a
> row in a table rather than an assumption. The predictions were written in
> the plan before the code and are quoted beside what was measured.
>
> **The finding worth keeping:** the client's cycles were not in its
> containers. Its kernel half was two `recv()` calls per readable socket, one
> of which only ever returned `EAGAIN`; removing that call did more than every
> data-structure change combined. Profile first.

| # | Finding | Status |
|---|---|---|
| 1 | The client's hot loop was **syscall-bound, not container-bound**: containers were ~9 % of its user instructions; `recv()` was ~90 % of its syscalls and half of those returned `EAGAIN` | New; from callgrind |
| 2 | Stopping the drain at a short read **halved `recv()` calls** and moved the 10M deliveries/s point from a client collapse with 12 nodes to a clean run with 12 | New |
| 3 | The `sds::` server cuts its user-space CPU share at 8–10M (22–25 % → 17–18 %) and its sweep latency by ~8 % there; at 3M it costs ~4 % of user share and ~4 % of latency | New; STL vs `sds::` at equal load and equal syscall shape |
| 4 | **The 64 B ceiling is observed at last**: STL lossless at 10M, shedding at 12M; `sds::` lossless at 12M, shedding at 14M — +20 %, the top of the predicted band. The fitted 13.4M was high | New; corrects `2026-08-30` § 3 |
| 5 | At 1024 B the `sds::` server is lossless at **2.20M deliveries/s (2.27 GB/s)** where the STL server sheds from 1.70M — the "byte-bound, no change" prediction was wrong in the good direction | New |
| 6 | The short-read stop that halved the client's syscalls makes the **server slower** (p50 +35 % at 10M): on a backlogged server it defers bytes by a sweep instead of saving a call | New; prediction failed |
| 7 | **Run-to-run variance on this box is of the order of the deltas.** The same STL 3M row measured p50 18.6–20.6 ms and p99 39–204 ms across the day; nothing under ~15 % is claimed from a single pair | Method |
| 8 | A run can print `[ OK ]` with self-lag p99 25 ms when latency p99 is censored at 1 s — the ratio gate is blind above the histogram's range | Instrument; not fixed here |

Same box and caveats as [`2026-08-30`](2026-08-30-what-limits-the-server.md):
one vCPU of an i5-13600K under WSL2, loopback, `CHAT_FLUSH=batch`,
`-O2 -DNDEBUG`, `--per-room 10`, 15 s windows, fresh server per point. The
WSL2 note there applies unchanged, and finding 7 is its consequence.

## 0. Method, and which binary produced which row

- **Fleet:** `python3 fleet.py --nodes N --conns C --rate R --duration 15 -- --server-pid <pid>`,
  dumps merged by `merge.py`; the fleet verdict is the one quoted.
- **CPU:** the server's split from `--server-pid`; every loadgen's split from
  `tools/cpusample.py` (utime/stime from `/proc`, busiest 10 s), which also
  records the server's `comm` so `server` and `server-sds` rows cannot be
  confused.
- **Syscall counts:** callgrind `--collect-systime` on one loadgen at 100
  connections × 10 msg/s for 10 s — the only direct count available without
  `perf` or `strace`. Instruction counts from the same runs.
- **Servers:** `server` (STL, `server.cpp`, unchanged since `b15e077`) and
  `server-sds` (`server_sds.cpp`, commit `b8728c7`), both built for
  measurement, both `CHAT_MAX_CONNS=60000 CHAT_QUIET=1`.
- **Clients**, by commit of `client-bench/`:

| tag | commit | what changed |
|---|---|---|
| base | `cb3d114` | the instrument as the recorded baselines used it |
| C1 | `16d3ad8` | `read_available` stops at a short read instead of looping to `EAGAIN` |
| C2 | `c4c75dc` | receive into a per-fd slot in one `mmap` slab, parse in place; frame built once on the stack and sent directly |

Every client row below names its tag. Rows within one table were taken
back-to-back, alternating server, so the box was in the same state for both
sides of a pair.

## 1. Where the client's time went (before any change)

At 3M deliveries/s with 3 nodes, the recorded row reproduced (p50 18.5 ms,
server 100 %, user 13 / kernel 87) and **every loadgen was already at 100 % of
a core, split 50 / 50 user/kernel**. At 10M with 12 nodes the fleet collapsed
on its own side. Callgrind on one loadgen (`run_traffic` only):

| | base |
|---|---:|
| instructions | 290.4M |
| syscalls | 9.29M, of which `epoll_wait` 2.24M, `clock_gettime` 6.83M (a real syscall under valgrind; vDSO otherwise), **`recv` 200,022**, `send` 10,100 |
| frames received | 100,650 |
| `read_available` + `consume_frames` (incl. `std::string`) | ~9 % of instructions |

Two `recv()` per readable socket: `read_available` looped to `EAGAIN` under
level-triggered epoll, so every readiness cost a second syscall that only
ever said "nothing". `server.cpp` LESSON 1 says the loop is an optimisation
under level-triggered epoll; at this fan-out it was the opposite. The
`epoll_wait` and `clock_gettime` counts are the scheduling loop spinning at
low load (`slot < 1 ms` makes the wait timeout 0) and are not a target: at
load the loop has work every iteration.

## 2. The client, before and after

### 2.1 Syscalls and instructions (callgrind, 100 conns × 10/s × 10 s)

| client | instructions | `recv()` calls | frames | `recv()` per frame |
|---|---:|---:|---:|---:|
| base | 290.4M | 200,022 | 100,650 | 1.99 |
| C1 | 296.0M | **100,056** | 100,650 | **0.99** |
| C2 | **278.4M** | 100,071 | 100,650 | 0.99 |

C1 does exactly what it says. C2's −6 % of user instructions is the copy and
`std::string` bookkeeping it removed — the plan predicted ≤ 5 % of client CPU
and that is where it landed.

### 2.2 At load: the 10M point, 12 nodes × 834 conns × 100/s, STL server

| client | achieved | fan-out | self-lag p99 | server closes | loadgen user/kernel | verdict |
|---|---:|---:|---:|---:|---|---|
| base | 5.69M (57 %) | 7.41 | **90.4 ms** | 8,030 | 34 / 66 (19..48 user) | `[VOID]` fan-out |
| base (earlier run) | 8.29M (83 %) | 9.19 | 25.8 ms | 3,460 | 39 / 61 | `[VOID]` fan-out |
| C1 | **9.98M (100 %)** | 9.97 | **0.082 ms** | 0 | 49 / 51 | `[ OK ]` |
| C2 | **9.98M (100 %)** | 9.98 | 0.046 ms | 0 | 51 / 49 | `[ OK ]` |

The base client's collapse is the `2026-08-30` § 2 pattern exactly — achieved
< offered with self-lag large — and the server's closes are the shed that
follows a client that stopped reading. With 14 nodes the base client carried
10M (self-lag p99 1.45 ms, p50 122 ms); with C1, 12 do it with headroom.
Reading the kernel share alone would have missed this: at 100 % of a core the
saved `EAGAIN` calls are spent on the next batch, so the split barely moves;
the capacity did.

The 14-node points were noisy in both directions (one base run `[ OK ]`, one
C1 run `[VOID]` with a 20 % user-share node); 15 busy processes on a 20-vCPU
guest over 14 physical cores is where finding 7 starts. Rows above use 12.

### 2.3 Same shape at 3M — the semantics gate

3 nodes × 3334 × 30/s against the STL server, fan-out 10.00 and 0 lost on
every row:

| client | p50 | p90 | p99 | self-lag p99 | server user/kernel |
|---|---:|---:|---:|---:|---|
| recorded 2026-08-30 | 18.5 | 32.8 | — | 0.016 | 14 / 86 |
| base (today, 2 runs) | 18.5 / 19.6 | 33.3 / 34.7 | 39.3 / 75.7 | 0.025 / 0.164 | 13 / 87 |
| C1 (2 runs) | 19.6 / 18.7 | 34.8 / 33.2 | 43.5 / 38.9 | 0.026 / 0.030 | 14 / 86 |
| C2 (2 runs) | 19.1 / 19.0 | 34.2 / 33.9 | 42.3 / 41.1 | 0.028 / 0.021 | 15 / 85 |

p50 sits on the sweep period as before, the histogram shape is the same, and
the judge (`verify 8×20` exact, `dribble`, `slowreader`) passed after each
commit. The 75.7 ms p99 in a base run and the 18.5–20.6 ms p50 spread are the
box, not the client: the spread is present with the unchanged binary.

## 3. STL versus `sds::`, same load, same client (C2), same syscall shape

The `sds::` server holds the baseline's `recv()` request size and
drain-to-`EAGAIN` loop and its one `send()` per dirty connection per batch, so
these pairs differ in data structures only. Both sides at 100 % of one core
on every row (`2026-08-30` § 2: CPU % is not the signal; the split and the
latency are).

### 3.1 64 B, rooms of 10

| load | server | p50 | p90 | p99 | server user / kernel | closes |
|---|---|---:|---:|---:|---|---:|
| 3M, 3 nodes (a) | STL | 19.1 | 34.2 | 42.3 | 15 / 85 | 0 |
| | sds | 19.9 | 35.3 | 43.2 | 18 / 82 | 0 |
| 3M, 3 nodes (b) | STL | 19.0 | 33.9 | 41.1 | 14 / 86 | 0 |
| | sds | 19.7 | 35.3 | 44.2 | 18 / 82 | 0 |
| 8M, 12 nodes | STL | 54.5 | 97.6 | 130.0 | 22 / 78 | 0 |
| | sds | **49.6** | **93.9** | **122.6** | **17 / 83** | 0 |
| 10M, 12 nodes | STL | 85.6 | 161.1 | 209.1 | 25 / 75 | 0 |
| | sds | **78.4** | **142.0** | **185.3** | **18 / 82** | 0 |

Read the direction, as [`README.md`](README.md) says of the kernel share. The
STL server's user share **grows with load** (14 → 25 %) — its per-message
bookkeeping is proportional to messages — while the `sds::` server's is
**flat at 17–18 %** — its cost is per connection per sweep. So at 3M, where
sweeps are short and messages few, `sds::` pays about 4 % more user share
and 4 % more sweep latency; at 8–10M it pays 5–7 points less and the sweep
is 8–11 % shorter. The crossing is somewhere between 3M and 8M.

**Prediction check.** The plan predicted "10–20 % 64 B ceiling gain from data
structures alone". The user-share cut at 10M is −7 points of 100, i.e. −28 %
of user-space, in line with the callgrind estimate that half of the STL
server's user instructions were bookkeeping; the sweep gain at 10M is −8 %
p50. Whether that reaches the ceiling is § 4.

### 3.2 1024 B, 6 nodes × 1668 — the byte-bound regime

| rate | server | achieved | p50 | p99 | user / kernel | closes | verdict |
|---:|---|---:|---:|---:|---|---:|---|
| 17 | STL | 1.70M | 59.5 | 131.5 | 15 / 85 | 0 | `[ OK ]` |
| 17 | sds | 1.70M | 69.8 | 240.0 | 17 / 83 | 0 | `[ OK ]` |
| 17 (C1 client) | STL | 1.70M | 52.6 | 116.0 | 17 / 83 | 0 | `[ OK ]` |
| 17 (C1 client) | sds | 1.70M | 74.8 | 166.4 | 17 / 83 | 0 | `[ OK ]` |
| 20 (C1 client) | STL | 1.76M (88 %) | 80.8 | 999.999+ | 17 / 83 | **1,310** | `[VOID]` loss 13.1 % |
| 20 (C1 client) | sds | **2.00M (100 %)** | 115.2 | 323.1 | 18 / 82 | **0** | `[ OK ]` |

Two things, pulling opposite ways. Below the cliff the `sds::` server is
**slower** at 1024 B (p50 +17–40 %). At the cliff it does not shed: the STL
server's rate-20 rung — 1,310 closes, exactly the [`2026-09-01`](2026-09-01-where-the-epoll-server-saturates.md)
congestion collapse — becomes a lossless 2.0M deliveries/s, 2.06 GB/s on the
wire against the 1.78 GB/s ceiling that note recorded.

The mechanism is `std::string`. Near the cap a connection's `out` holds
100–250 KB, and every `send()` that does not drain it pays `erase(0, n)` — a
memmove of what remains — plus a reallocation-and-copy each time `append`
crosses a capacity doubling. The linear queue pays neither: it resets to
offset 0 when drained and compacts at most once per region wrap. That cost
is only visible where queues are deep, which is why the plan's "byte-bound,
near zero" prediction held at rate 17 and failed at rate 20. The `sds::`
server's own 1024 B ceiling is above 2.0M; the rungs that find it are in § 4.

### 3.3 The layout question the first pair raised

The very first STL/`sds::` pair at 3M read p50 18.7 vs **23.4 ms**, and at
1024 B rate 17 52.6 vs 74.8, which looked like a memory-layout cost: every
connection has its own 292 KiB slot and a 32 KiB receive ring whose cursor
advances forever, so a sweep touches a fresh cache line per connection and,
over time, every page of every ring — where the STL heap packs the same
state into a few thousand pages. Variants were built and run back-to-back:

| server at 3M | p50 | p99 | user / kernel |
|---|---:|---:|---|
| STL (two runs) | 18.6 / 18.7 | 38.9 / 41.9 | 13 / 87 |
| sds, 32 KiB ring (as shipped) | 19.1 | 40.7 | 18 / 82 |
| sds, 2 KiB ring | 19.2 | 39.8 | 17 / 83 |
| sds, 32 KiB ring re-constructed when empty | 30.5 | 61.9 | 36 / 64 |
| sds, 4 KiB ring re-constructed when empty (two runs) | 21.0 / 20.2 | 43.1 / 42.3 | 16 / 84 |

Within noise, the ring's size does not matter at 3M: the +25 % of the first
pair was finding 7, not layout (the repeat read +3 %). The "re-construct the
ring to rewind it" variant is a **wrong experiment, recorded so it is not
re-run**: `sds::ring_buffer`'s defaulted constructor value-initialises its
storage, so re-constructing zero-fills 32 KiB per parse — that is the 36 %
user share, not TLB reach. The layout hypothesis is neither confirmed nor
excluded by this; what is excluded is that the shipped 32 KiB ring costs
anything measurable at 64 B. The 1024 B below-cliff gap (§ 3.2) is real
across three pairs and remains unexplained; the candidates are the per-sweep
page footprint of 6 × 1028 B arriving per connection into a cycling ring
versus a hot 6 KB `std::string`, and it needs `perf` on bare metal.

## 4. The 64 B ladder with the new client — the ceiling, observed

16 nodes × 626 conns (10,016), C2 client, fresh server per rung. 12 nodes
carried 10M cleanly for both servers (§ 3.1); the ladder continues from there.

| offered | server | achieved | fan-out | self-lag p99 | server closes | server CPU (user / kernel) | verdict |
|---:|---|---:|---:|---:|---:|---|---|
| 12M | STL | **5.64M (47 %)** | 7.80 | 0.096 ms | **7,400** | 99 % (27 / 73) | `[VOID]` fan-out |
| 12M | sds | **11.96M (99.7 %)** | 9.95 | 3.83 ms | **0** | 100 % (18 / 82) | `[ OK ]` (p99 censored; see finding 8) |
| 14M | STL | 2.23M | 4.38 | 7.4 ms | 9,398 | 91 % | `[VOID]` |
| 14M | sds | 4.87M (35 %) | 7.17 | 0.66 ms | **7,420** | 100 % (16 / 84) | `[VOID]` fan-out |
| 16M | STL | 1.40M | 3.26 | 1.8 ms | 9,700 | 68 % | `[VOID]` |
| 16M | sds | 4.79M | 7.11 | 0.94 ms | 7,740 | 100 % | `[VOID]` |

Read each collapse with the `2026-08-30` § 2 pair rule. **STL at 12M:** achieved
< offered, self-lag p99 0.096 ms (small), 7,400 closes in the server's own
log — the server's, not the client's. That is the 64 B ceiling this
repository never reached before: the STL server is **lossless at 10M and sheds
at 12M deliveries/s**; the fitted 13.4M of `2026-08-30` § 3 was high, as
`2026-09-01` § 7 warned a one-regime fit would be. **sds at 14M:** the same
pattern, self-lag 0.66 ms, 7,420 closes — its ceiling is **lossless at 12M,
shedding at 14M**, one rung higher, +20 % against the STL server's, at the top
of the plan's predicted 10–20 % band.

Two caveats. The sds 12M row is at the instrument's edge as well as the
server's: self-lag p99 3.8 ms with latency p99 censored at 1 s, so the ratio
gate could not have failed (finding 8); it is a lossless rung, not a
comfortable one. And once a server is shedding, the STL rows' falling CPU
(91 %, 68 %) is the collapse itself — a server that has closed most of its
clients has less to do — not evidence that it was below its ceiling.

### 4.1 The 1024 B ceiling of the `sds::` server

| rate | achieved | p50 | p99 | closes | verdict |
|---:|---:|---:|---:|---:|---|
| 20 | 2.00M | 74.6 | 170.2 | 0 | `[ OK ]` |
| 22 | **2.20M** | 132.4 | 397.7 | **0** | `[ OK ]` |
| 25 | 2.13M (85 %) | 90.0 | 879.7 | **1,630** | `[VOID]` loss 16.3 % |

Lossless at 2.20M deliveries/s — 2.27 GB/s on the wire against the STL
server's 1.70M / 1.75 GB/s — shedding at 2.5M. The cliff is as sharp as
`2026-09-01` § 3 described, one rung higher, and the STL server's rate-20 rung
re-measured today shed again (450 closes, `[VOID]`). +29 % at 1024 B, from a
change the plan predicted would be worth nothing there.

## 5. `CHAT_SHORT_READ=1` on the server — prediction failed

The plan predicted the client's short-read stop would shorten the server's
sweep too. It does the opposite:

| load | sds, drain to `EAGAIN` | sds, stop at short read |
|---|---|---|
| 3M, 3 nodes | p50 19.7–19.9, p99 43–44 | p50 **20.8**, p99 43.4 |
| 10M, 12 nodes | p50 78.4, p99 185.3 | p50 **106.5**, p99 **271.4** |

Same achieved load, same CPU split (17–18 / 82–83). The mechanism is the
sweep model itself: on the server every connection has fresh bytes every
sweep, so a short read that leaves bytes behind does not save a syscall — it
moves those bytes to the *next* sweep and adds a sweep period to their
latency. The client is the opposite shape (few frames per socket per pass,
most sockets idle), which is why the same change halved its syscalls. Kept as
a knob because the row is worth having; not the default.

## 6. What is still not measured

- **The 64 B ceiling is bracketed, not pinned**: lossless at 10M / shedding at
  12M for STL, 12M / 14M for sds, on 20 % rungs. Finer rungs need more client
  than this box has: the sds 12M row already ran with self-lag p99 3.8 ms.
- **Why `sds::` is slower below the 1024 B cliff.** § 3.3 rules out the ring
  size at 64 B and nothing else; the answer needs hardware counters.
- **Whether the `sds::` server's send queue changes the shape of the cliff**,
  not only its position: with no memmove tax the connection still hits the
  256 KiB cap and is closed, so the collapse should be as sharp, one rung
  higher. Rate 22 and 25 at 1024 B are in § 4.
- **Finding 8.** `lag99 * 5 > lat99` cannot fire when `lat99` is the
  histogram's ceiling; the 8M/12-node base row printed `[ OK ]` with self-lag
  p99 25 ms and p90 latency beyond 1 s. A censored `lat99` should void the
  ratio test or at least say so. Recorded here; the instrument is unchanged in
  this note so its rows stay comparable.

## Rationale links

- [`../design-notes/2026-09-02-control-group-on-engine-primitives.md`](../design-notes/2026-09-02-control-group-on-engine-primitives.md) — the decisions: seam not copy, sibling file, existing binary, no engine headers in the instrument, syscall shape held equal.
- [`../server-epoll/doc/server_sds.md`](../server-epoll/doc/server_sds.md) — the `sds::` server as built.
- `client-bench/src/netutil.cpp` — `read_available` (C1), `rx_slab_*` and `send_frame` (C2); `client-bench/tools/cpusample.py` — the client-side CPU evidence.
- [`2026-08-30-what-limits-the-server.md`](2026-08-30-what-limits-the-server.md) § 2 — the pair rule every collapse here was read with; § WSL2 — finding 7's cause.
- [`2026-09-01-where-the-epoll-server-saturates.md`](2026-09-01-where-the-epoll-server-saturates.md) — the 1024 B cliff the `sds::` server moved.
