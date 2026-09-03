# 2026-09-03 — The tick budget, stage C: one send per player per tick, measured against stage A

The second run of the tick-budget experiment, the same evening as
[stage A](2026-09-03-the-tick-budget-stage-a.md). The server is the same
binary with `CHAT_TICK_MODE=coalesce`
([`design-notes/2026-09-03-stage-c`](../design-notes/2026-09-03-stage-c-tick-coalesced-delivery.md)):
a chat frame is appended to its room's tick buffer instead of being broadcast
in the batch it arrived in, and at the tick every member receives the whole
buffer as one frame. Same grid at `W = 64`, same instrument, same gates, two
passes, 32 runs. Read every row against its stage A twin.

| # | finding | status |
|---|---|---|
| 1 | **The I/O term fell from ~16 µs to ~3 µs per active connection per tick** where the server has headroom: 16.3 → 3.2 µs at 300, 15.6 → 2.9–3.6 µs at 1,000, 10.9 → 3.2–3.6 µs at 3,000. I/O share of the period 15 → 3 %, 47 → 9–11 %, 98 → 29–32 %. Predicted 3.1 µs | **P4 confirmed** |
| 2 | **3,000 players per thread left saturation.** Stage A had the row at 100 % CPU with 98 % of the tick in I/O; stage C has it at 49 % CPU, 30 % I/O, client p99 39 ms. The regime change the design note predicted for this row happened | Prediction confirmed |
| 3 | **Wakes per period are still ≈ active connections** (296, 912–929, 1,265–1,447). Coalescing moved the send side only; the receive side pays one wake and two `recv()` per input as before. That is the term left for the io_uring comparison | **P5 confirmed** |
| 4 | **Fan-out 10.00–10.04 and zero loss in every cell with headroom.** Coalescing changed no delivery count; the gate kept its meaning without bending | **P7 confirmed** |
| 5 | **At 10,000 the tick shape is not better on this box, and it has a cost stage A did not: the burst.** Ten thousand ~1 KB frames leave in one flush after the tick, 11–19 ms of `send()` back to back; CPU stays at ~100 %, client p50 rises from 18–23 to 34–47 ms, and the two passes disagree by 2× on every phase. Predicted "same number, opposite meaning"; measured worse and noisy | **P6 failed** — a new term, not a wash |
| 6 | **Client latency is half a period plus the tick, as redefined**: p50 16.8 ms at `L = 0`, 19.8 at 3 ms of tick, 26.4 at 9 ms, 28.1 at 10 ms; p99 sits at one period plus the tick (33.5, 36.4, 42.5, 43.9 ms). Not comparable with any per-message-broadcast row, by construction | New; instrument confirmed |
| 7 | **Past the boundary the per-period histogram measures the sliver, not the period.** When `N × (L + c_io) > period` the tick re-fires with a few ms of drain between, wakes p50 reads 1, and drain/flush p50 read a fraction of a millisecond for thousands of inputs. Both stages show it (stage A: 3,000 × 10 µs, wakes 2; stage C: the same cell, wakes 1). The overrun count and the client's latency carry the verdict there, not the phase p50s | Instrument caveat |
| 8 | The tick phase gained the room walk, 0.03–0.13 ms at 300–1,000 and 0.5 ms at 3,000, where the design note allowed 0.2 ms | Within prediction |

## Method

Everything as in stage A's method section, with one change and one
restriction: `CHAT_TICK_MODE=coalesce`, and the `W = 64` half of the grid
only, since stage A § 3 showed `W` does not couple into the I/O term.
`N ∈ {300, 1000, 3000, 10000}` × `L ∈ {0, 10, 30, 100} µs`, two passes,
`ticksweep.py --mode coalesce --only-w64`. Client-side, the tick frame
(type 102) is parsed entry by entry and each entry is one delivery, so
fan-out and loss keep their definitions. Raw per-cell summary:
[`data/2026-09-03-ticksweep-stage-c.csv`](data/2026-09-03-ticksweep-stage-c.csv).

## 1. Stage A against stage C, `L = 0`

Per-period p50s, pass 2 for stage A (its clean pass), both passes for stage
C where they differ.

| N | stage | drain | flush | tick | I/O share | wakes | I/O per input | srv CPU | client p50 / p99 | verdict |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| 300 | A | 0.44 ms | 4.45 ms | 0.001 | 15 % | 300 | 16.3 µs | 18 % | 0.02 / 0.10 ms | `OK` |
| 300 | **C** | 0.37–0.38 | **0.59–0.63** | 0.03 | **3 %** | 296 | **3.2 µs** | **6 %** | 16.8 / 33.1–33.5 | `OK` |
| 1,000 | A | 1.25 | 14.31 | 0.004 | 47 % | 980 | 15.6 µs | 53 % | 0.02 / 0.11 | `OK` |
| 1,000 | **C** | 0.95–1.38 | **1.95–2.19** | 0.10–0.13 | **9–11 %** | 912–929 | **2.9–3.6 µs** | **17–24 %** | 17.8–18.2 / 33.7–35.5 | `OK` |
| 3,000 | A | 2.32 | 30.30 | 0.013 | 98 % | 677 | 10.9 µs | 102 % | 0.06 / 0.20 | `OK` |
| 3,000 | **C** | 2.74–3.43 | **6.84–7.39** | 0.49–0.53 | **29–32 %** | 1,265–1,447 | **3.2–3.6 µs** | **49 %** | 21.7–22.7 / 38.7–57.2 | `OK` |
| 10,000 | A | 8.77 | 21.80 | 0.04 | 92 % | 37 | 3.1 µs | 102 % | 18.3 / 38.6 | `OK` |
| 10,000 | **C** | 3.69–6.36 | 10.9–18.8 | 0.99–1.42 | 44–75 % | 16–30 | 1.5–2.5 µs | 98–101 % | **34.2–47.4** / 82.7–92.0 | `OK` |

The first three rows are the result. The send side went from `F` calls per
input to one per player per tick, and its cost went from 14.3 to 2.0 ms at
1,000 — 10,000 sends of ~100 B became 1,000 sends of ~1 KB at ~2 µs each,
the 0.4 ns/byte of `2026-08-30` showing up as the +0.5 µs over the 100 B
send. The receive side did not move (0.95–1.38 against 1.25 ms), and the
wakes column says why: one input still wakes the loop once. The tick phase
picked up the room walk — building one frame per room and appending it to
each member's `out` — at 0.1 ms per 1,000 players.

The 3,000 row changes regime rather than degree. Stage A had it at 100 % of
the core with the whole period in I/O and finding 3's accidental coalescing
already in play; stage C has half the core idle, 30 % of the period in I/O,
and `L ≈ 7 µs` of headroom before the tick would overrun (33 − 10 − 0.5 =
23 ms for 3,000 entities). That headroom is the number a zone-thread
designer wants, and the per-message-broadcast shape did not have it.

### 1.1 The 10k row: the burst

At 10,000 the total bytes per second are identical in both shapes (3M
deliveries of ~100 B, 300 MB/s) and the core is saturated in both. What
differs is *when* the bytes leave. Stage A spread 30,000 `send()` across the
period, each carrying whatever had accumulated for one recipient since the
last wake. Stage C emits 10,000 frames of ~1 KB in one flush after the tick,
and that flush takes 11–19 ms. A player whose input arrived just after a
tick waits half a period for the next tick and then up to the whole burst
for its frame: p50 34–47 ms against stage A's 18–23, p99 83–92 against
39–49. The two passes disagree by two to one on drain and flush, which at
100 % CPU is the noise of a saturated loop, not a measurement of either.

P6 predicted the two shapes would cost the same at 10k and the row would
say nothing. It cost the same in CPU, said something, and the something is a
cost: **coalescing to the tick concentrates the output into a burst whose
length adds to everyone's latency.** At 300–3,000 the burst is 0.6–7 ms and
hides inside the period; at 10,000 it is a third of the period and the
dominant term. A real server spreads the burst — sends in slices across the
period, or a second worker — and either is a design change, not an I/O
mechanism. What io_uring could do with the burst is a stage B question:
10,000 SQEs and one `io_uring_enter` instead of 10,000 `send()`, if the
per-op cost is where the hypothesis put it.

## 2. The logic term and the boundary, again

Tick p50 against `N × L`: 3.01 / 3.0 ms, 8.9–9.9 / 9.0, 9.7–10.0 / 10.0,
29.5–30.4 / 30.0, 30.4–30.7 / 30.0 (+ 0.5 ms room walk), 91.3–91.8 / 90.0,
99–100 / 100, 304–306 / 300. The knob still dials what it says, with the
room walk on top.

The boundary moved the way the I/O term moved. Stage A's 1,000 × 30 µs cell
overran 0.6–3.9 %, stage C's 1.5–2.0 % — the same cell, the same 30 ms tick,
and the I/O it competes with shrank from 15.6 to 2.6 ms, so the cell sits
just as close to the edge for a different reason (the room walk added
0.4 ms). At 3,000 × 10 µs stage C overruns 40 % where stage A overran 1–5 %,
which reads as worse and is finding 7: with 30.4 ms of tick, the deadline
leaves 2.9 ms for the drain of 3,000 sockets, the drain runs late into the
next deadline, and lateness plus tick exceeds the period on a coin flip.
Stage A's version of the same cell was already saturated by its I/O term
(78 ms of work per 33 ms period) and was spending its slivers differently.
Neither number describes a server that fits; the row is past the boundary
in both stages, and the overrun percentage at the boundary is not a
comparable quantity.

## 3. The client's numbers, and the instrument past the edge

**Latency is now a tick latency.** p50 = half a period + the tick's share,
p99 = a period + the tick: `N = 300`, `L = 0 / 10 / 30 µs` gives p50
16.8 / 19.8 / 26.4 ms and p99 33.5 / 36.4 / 42.5 ms; `N = 1000`, `L = 10`
gives 28.1 / 43.9. These are the numbers a player would feel under a
30 Hz server with that much logic, and the reason `design-notes/2026-09-02`
§ 7.4 said the client's latency would stop being comparable the day the
tick shaped the sends. It has.

**Every cell with headroom passed every gate**: fan-out 10.00–10.04, zero
lost connections, zero closes, self-lag p99 ≤ 0.07 ms except three cells at
1.0–1.8 ms and one at 0.5 ms (the box). The 10k × `L ≥ 30 µs` cells voided on **fan-out 5.18
below `--per-room`** — deliveries sent that never arrived inside the run —
and the 10k × 100 µs cells on the censored p99. Their server-side phases are
finding 7's slivers and are not read.

**Finding 7 is an instrument limit worth stating once.** The per-period
histogram assumes the tick fits in the period. When `N × (L + c_io)` exceeds
it, the loop alternates a full tick with a sliver of drain, each sliver is
one wake of up to 256 events (0.3 ms — the drain p50 of every such cell,
in both stages), and the histogram faithfully reports a period that is not
one. The overrun percentage says "most ticks", the client's p50 says "many
periods of queue", and those two are the verdict. A future instrument
could record the tick-to-tick interval alongside; this one did not.

## 4. What it means for the hypothesis

1. **The I/O term of a tick-shaped epoll server is ~3 µs per active
   connection per tick on this box**, one wake and two `recv()` on the way
   in and one ~2 µs `send()` on the way out. That is the control the
   io_uring server is compared against — not 16 µs, and not 2 µs.
2. **The 5× came from design.** It was available under epoll, it is the same
   coalescing a tick-based game server does by nature, and the comparison
   would have credited io_uring with it had stage B run against stage A's
   shape. `design-notes/2026-09-02` § 5 predicted the answer would sit in
   the product layer; the first 80 % of the budget did.
3. **What is left for io_uring, with numbers.** Receive side: three
   syscalls per input (~1.0–1.4 µs) against one completion under multishot
   recv (~0.1–0.2 µs). Send side: one `send()` per player per tick (~2 µs
   at 1 KB) against one SQE and a shared `io_uring_enter` (~0.3–0.5 µs if
   the copy dominates). Predicted: ~3 µs → ~0.5–0.7 µs per active connection
   per tick, 9 % → ~2 % of the period at 1,000, 30 % → ~6 % at 3,000, and at
   10,000 the burst of § 1.1 becomes 10,000 SQEs in one submit — the one
   place the mechanism might change the shape and not only the constant.
4. **The burst is a term the cost model did not have.** `T_io` per tick is
   not the only quantity; *when* in the period it is paid moves the client's
   p50 by up to the flush duration. Stage B measures it the same way: flush
   p50 is the burst length.

## 5. What is still not measured

- **Stage B**: the io_uring data path, against this shape and these rows.
- **The burst spread.** Whether slicing the flush across the period at 10k
  recovers stage A's p50 without its per-input cost. A design question with
  a small diff; not done, because it changes the shape stage B compares to.
- **Tick-to-tick interval** as an instrument column, so finding 7's cells
  read as what they are.
- **The self-lag excursions** (0.5–1.8 ms in four cells) and the 10k
  pass-to-pass 2× disagreement: the box, again, and unassigned.
- **Everything here is one WSL2 vCPU**; the shape of the result (`F`
  gone from the send term, the burst, the boundary) does not depend on it,
  the constants do.

## Appendix — every stage C cell, both passes

Same columns as stage A's appendix. `VOID` rows are the 10k collapse cells:
fan-out below band at `L = 30 µs`, censored p99 at `L = 100 µs`.

#### W = 64 B

| N | L µs | pass | drain p50 / p99 | tick p50 / p99 | flush p50 / p99 | io share | wakes p50 | overrun % | client p50 / p99 | self-lag p99 | srv CPU | verdict |
|---:|---:|:--:|---|---|---|---:|---:|---:|---|---:|---:|---|
| 300 | 0 | 1 | 0.373 / 0.739 | 0.029 / 0.063 | 0.593 / 0.924 | 3 % | 296 | 0.00 | 16.826 / 33.538 | 0.003 | 6 | `OK` |
| 300 | 0 | 2 | 0.382 / 0.849 | 0.035 / 0.064 | 0.625 / 0.959 | 3 % | 295 | 0.00 | 16.848 / 33.096 | 0.003 | 6 | `OK` |
| 300 | 10 | 1 | 0.350 / 0.615 | 3.012 / 6.500 | 0.589 / 0.769 | 3 % | 269 | 0.00 | 19.795 / 36.435 | 0.001 | 15 | `OK` |
| 300 | 10 | 2 | 0.309 / 1.576 | 3.016 / 7.677 | 0.557 / 1.280 | 3 % | 269 | 0.00 | 19.885 / 36.479 | 0.008 | 15 | `OK` |
| 300 | 30 | 1 | 0.302 / 1.720 | 8.917 / 12.994 | 0.598 / 1.654 | 3 % | 216 | 0.00 | 26.380 / 42.451 | 0.040 | 34 | `OK` |
| 300 | 30 | 2 | 0.298 / 0.592 | 9.881 / 10.479 | 0.591 / 0.813 | 3 % | 206 | 0.00 | 26.772 / 43.428 | 0.003 | 34 | `OK` |
| 300 | 100 | 1 | 0.167 / 1.738 | 29.892 / 37.506 | 0.575 / 1.664 | 2 % | 26 | 2.64 | 47.797 / 100.735 | 0.018 | 92 | `OK` |
| 300 | 100 | 2 | 0.181 / 0.285 | 29.607 / 31.192 | 0.596 / 0.828 | 2 % | 28 | 0.30 | 46.491 / 63.159 | 0.004 | 92 | `OK` |
| 1000 | 0 | 1 | 0.952 / 2.914 | 0.101 / 0.253 | 1.951 / 4.110 | 9 % | 929 | 0.00 | 17.771 / 33.742 | 0.005 | 17 | `OK` |
| 1000 | 0 | 2 | 1.376 / 3.408 | 0.127 / 0.423 | 2.188 / 6.674 | 11 % | 912 | 0.00 | 18.194 / 35.522 | 0.465 | 24 | `OK` |
| 1000 | 10 | 1 | 0.882 / 1.972 | 9.705 / 11.555 | 2.045 / 3.143 | 9 % | 633 | 0.00 | 28.136 / 43.889 | 0.007 | 45 | `OK` |
| 1000 | 10 | 2 | 1.073 / 1.984 | 10.021 / 11.939 | 2.191 / 3.794 | 10 % | 610 | 0.00 | 28.633 / 44.472 | 0.011 | 48 | `OK` |
| 1000 | 30 | 1 | 0.533 / 3.693 | 29.451 / 33.558 | 2.013 / 2.996 | 8 % | 32 | 1.53 | 48.828 / 107.493 | 0.017 | 98 | `OK` |
| 1000 | 30 | 2 | 0.574 / 3.406 | 30.353 / 34.003 | 2.103 / 3.195 | 8 % | 4 | 1.99 | 54.931 / 114.738 | 0.009 | 99 | `OK` |
| 1000 | 100 | 1 | 1.724 / 6.532 | 98.927 / 146.730 | 2.407 / 6.198 | 12 % | 878 | 81.77 | 169.137 / 260.138 | 0.044 | 82 | `OK` |
| 1000 | 100 | 2 | 2.122 / 3.717 | 100.106 / 111.822 | 2.737 / 4.549 | 15 % | 856 | 82.20 | 170.254 / 240.354 | 0.010 | 81 | `OK` |
| 3000 | 0 | 1 | 2.737 / 13.574 | 0.525 / 3.085 | 6.836 / 23.988 | 29 % | 1447 | 0.00 | 22.732 / 57.211 | 1.020 | 49 | `OK` |
| 3000 | 0 | 2 | 3.425 / 5.054 | 0.494 / 0.973 | 7.394 / 12.236 | 32 % | 1265 | 0.00 | 21.727 / 38.707 | 0.009 | 49 | `OK` |
| 3000 | 10 | 1 | 0.318 / 13.534 | 30.351 / 58.689 | 0.788 / 22.886 | 3 % | 1 | 39.87 | 84.938 / 172.262 | 1.777 | 85 | `OK` |
| 3000 | 10 | 2 | 0.375 / 8.758 | 30.747 / 36.029 | 0.890 / 10.772 | 4 % | 1 | 41.39 | 83.412 / 262.207 | 0.012 | 80 | `OK` |
| 3000 | 30 | 1 | 4.743 / 6.375 | 91.327 / 95.379 | 7.490 / 9.393 | 37 % | 663 | 82.93 | 157.870 / 220.476 | 0.005 | 88 | `OK` |
| 3000 | 30 | 2 | 5.356 / 7.850 | 91.757 / 96.380 | 8.087 / 10.426 | 40 % | 934 | 82.35 | 159.394 / 222.288 | 0.003 | 88 | `OK` |
| 3000 | 100 | 1 | 6.271 / 8.370 | 303.583 / 308.465 | 11.093 / 13.915 | 52 % | 389 | 59.43 | 480.028 / 645.743 | 0.011 | 98 | `OK` |
| 3000 | 100 | 2 | 7.071 / 10.240 | 305.581 / 320.985 | 12.477 / 16.815 | 59 % | 363 | 57.94 | 489.528 / 662.885 | 0.023 | 98 | `OK` |
| 10000 | 0 | 1 | 6.356 / 15.373 | 1.422 / 2.797 | 18.785 / 27.774 | 75 % | 30 | 0.00 | 34.206 / 82.717 | 0.014 | 101 | `OK` |
| 10000 | 0 | 2 | 3.686 / 22.973 | 0.990 / 5.121 | 10.875 / 34.451 | 44 % | 16 | 0.00 | 47.438 / 91.950 | 0.033 | 98 | `OK` |
| 10000 | 10 | 1 | 0.378 / 29.001 | 77.514 / 133.226 | 0.735 / 52.104 | 3 % | 1 | 73.64 | 228.878 / 731.955 | 1.841 | 101 | `OK` |
| 10000 | 10 | 2 | 0.523 / 28.093 | 103.422 / 113.443 | 1.198 / 57.408 | 5 % | 1 | 75.12 | 286.881 / 469.288 | 0.070 | 100 | `OK` |
| 10000 | 30 | 1 | 0.000 / 26.027 | 0.001 / 323.955 | 0.000 / 27.264 | 0 % | 1 | 49.26 | 999.999+ / 999.999+ | - | 102 | `VOID` |
| 10000 | 30 | 2 | 3.893 / 31.092 | 294.293 / 329.882 | 4.927 / 36.809 | 26 % | 2 | 55.91 | 999.999+ / 999.999+ | - | 100 | `VOID` |
| 10000 | 100 | 1 | 0.040 / 92.643 | 642.545 / 1000.000+ | 3.275 / 36.342 | 10 % | 1 | 60.98 | 0.000+ / 0.000+ | - | 102 | `VOID` |
| 10000 | 100 | 2 | 9.512 / 57.102 | 804.983 / 1000.000+ | 3.664 / 34.056 | 40 % | 1 | 61.90 | 998.794+ / 998.794+ | - | 100 | `VOID` |

## Rationale links

- [`2026-09-03-the-tick-budget-stage-a.md`](2026-09-03-the-tick-budget-stage-a.md)
  — every row here is read against its twin there; § 1.1 for the 2.7 µs
  prediction, § 3 for why `W` was dropped from the grid.
- [`../design-notes/2026-09-03-stage-c-tick-coalesced-delivery.md`](../design-notes/2026-09-03-stage-c-tick-coalesced-delivery.md)
  — the shape, why coalesced and not latest-wins, and P4–P7.
- [`../design-notes/2026-09-02-where-io-uring-becomes-meaningful.md`](../design-notes/2026-09-02-where-io-uring-becomes-meaningful.md)
  § 5 and § 7.4 — the product-layer prediction and the latency redefinition.
- [`../server-epoll/server.cpp`](../server-epoll/server.cpp) —
  `flush_tick_rooms()` and `queue_tick_frame()`; `client-bench/src/traffic.cpp`
  — the type-102 entry parser.
- [`2026-08-30-what-limits-the-server.md`](2026-08-30-what-limits-the-server.md)
  — the per-byte constant the 1 KB send cost agrees with.
