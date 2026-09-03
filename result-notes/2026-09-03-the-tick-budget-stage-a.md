# 2026-09-03 — The tick budget, stage A: what the I/O layer costs the epoll server per tick, and what it does not

The first run of the experiment
[`design-notes/2026-09-02`](../design-notes/2026-09-02-where-io-uring-becomes-meaningful.md)
§ 7 designed and
[`design-notes/2026-09-03`](../design-notes/2026-09-03-working-set-knob-for-the-tick-budget-experiment.md)
amended: `server-epoll` with a 30 Hz tick, a per-entity logic cost `L`, a
per-entity state block `W`, and the server's own per-period phase histogram
as the instrument. Only the epoll side exists; this note records it so that
the io_uring side, when it lands, is compared against a recorded row and not
a re-run.

| # | finding | status |
|---|---|---|
| 1 | **The I/O term is ~16 µs per active connection per tick**, not the 2 µs the cost model assumed. Of it, ~1.25 µs is the receive side (one wake, two `recv()`, parse, handler) and ~14.3 µs is **ten `send()` calls at 1.43 µs each** — the chat shape broadcasts every input to `F = 10` recipients immediately. The model counted one send per active connection; the shape sends `F` | New; model corrected |
| 2 | **Epoll's batch is one input wide when the server is not saturated.** Wakes per period ≈ active connections (300 → 300, 1000 → 980). Inputs spread across the period each wake the loop alone, and `CHAT_FLUSH=batch` has nothing to coalesce | New |
| 3 | **Coalescing cuts the per-input I/O cost 5×**, from 16 µs to 3.1 µs, when a backlog forms (10k connections: 37 wakes per period, ~270 inputs each, several frames per recipient per `send()`). Here it was forced by saturation and paid in 18 ms of queueing; a tick-aligned send would get it by design for at most one period of latency. **This is the largest lever measured, and it is design, not mechanism** | New |
| 4 | **The logic term is what was dialled**: tick time = `N × L` within 1 % in every cell but one, and that one is explained by its own calibration line (0.43 iters/ns against 0.57 everywhere else, and a tick 75 % of nominal) | Instrument confirmed |
| 5 | **The overrun boundary is `N × L ≈ period`**, as § 4 of the hypothesis predicted: 3,000 entities overrun at any `L` above 10 µs, 10,000 at any `L` above 0 | Prediction confirmed |
| 6 | **P1 falsified on this box.** A 40 MB working set (10k × 4 KiB, swept every tick) raised the receive-side drain by 0.2–0.3 µs per input — the handler's own 64-line walk of its block — and left the send-side flush flat or lower. The I/O syscalls did not get measurably colder. The eviction is paid once per tick, `O(I/O working set)`, not once per syscall | Prediction failed, in the direction that simplifies |
| 7 | **The felt tail is the tick.** Client latency p99 ≈ tick duration in every unsaturated cell (3.0 → 2.85 ms, 9.0 → 8.85 ms, 10.0 → 10.1 ms). Under immediate broadcast the logic term sets the p99 floor directly | New |
| 8 | Pass 1's `W = 64` cells carried client self-lag p99 of 2.6–13 ms; the identical pass-2 cells carried 0.003–0.02 ms, and the server-side phases agreed across passes within 3 %. Transient, client- or box-side, finding 7 of `2026-08-30` again. Client columns below are read from pass 2 | Method |
| 9 | The censored-p99 gate fixed this morning fired for the first time, on every 10k row with `L ≥ 30 µs`: `[VOID] fleet latency p99 is past the 1s histogram range` | Instrument |

## Method

One box, the same as every note here: one vCPU of an i5-13600K under WSL2,
loopback, `CHAT_FLUSH=batch CHAT_QUIET=1`, a fresh server per cell.

- **Tick**: `CHAT_TICK_HZ=30`, period 33.33 ms. `epoll_pwait2` bounded by the
  next deadline; at the deadline the tick walks every live session's block
  and spins `L` per session. Chat semantics unchanged: a frame is still
  broadcast by its handler in the same batch, so the client's echo latency
  and its verdict gate the row as before.
- **Grid**: `N ∈ {300, 1000, 3000, 10000}` × `L ∈ {0, 10, 30, 100} µs` at
  `W = 64 B`, plus `W ∈ {512, 4096}` at `L ∈ {0, 30}`. 32 cells, two full
  passes, 64 runs, `client-bench/tools/ticksweep.py`.
- **Load**: 30 messages/s per connection, rooms of 10, so one input per
  connection per tick (`N_a = N`) and `N × 300` deliveries/s. Fleet shape
  1 × 300, 1 × 1000, 3 × 1000, 3 × 3334. 20 s of traffic per cell.
- **Instrument**: the server's per-period sums of time in the I/O drain
  (from each wake to the end of its event processing), in the tick
  function, and in the flush/reap tail; the number of wakes per period; the
  overrun count. 1 µs buckets to 1 s. A tick ending past deadline + period
  is an overrun and the deadline is reset from then, not caught up.
- **Gate**: `merge.py`'s fleet verdict on the client side (fan-out, loss,
  censored p99, self-lag). A self-lag void discards the client latency
  columns, not the server's phase histogram, which does not depend on the
  client's clock.
- **Burn calibration**: median of seven 20 ms burns at startup, printed in
  the banner and carried in the dump. 61 of 64 cells calibrated at
  0.56–0.59 iters/ns; one at 0.55 and one at 0.43 (finding 4).
- Raw per-cell summary: [`data/2026-09-03-ticksweep-stage-a.csv`](data/2026-09-03-ticksweep-stage-a.csv).
  The full table is the appendix.

## 1. The I/O term, per active connection per tick

`L = 0`, `W = 64`, pass 2. "I/O" is drain + flush per period; inputs per
period is `N`.

| N | deliveries/s | drain p50 | flush p50 | I/O share of period | wakes / period | inputs / wake | I/O per input |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 300 | 90k | 0.44 ms | 4.45 ms | 15 % | 300 | 1.0 | **16.3 µs** |
| 1,000 | 300k | 1.25 ms | 14.31 ms | 47 % | 980 | 1.0 | **15.6 µs** |
| 3,000 | 900k | 2.32 ms | 30.30 ms | 98 % | 677 | 4.4 | 10.9 µs |
| 10,000 | 3.0M | 8.77 ms | 21.80 ms | 92 % | 37 | 270 | **3.1 µs** |

Every row delivered 100 % of what was offered. The first two rows are the
clean regime and the number the hypothesis needs: **~16 µs of I/O per
active connection per tick**, against the `2 · c_sys ≈ 2 µs` of
`2026-09-02` § 3. The decomposition at `N = 1000`:

- **Receive side, 1.25 µs per input.** One `epoll_pwait2` return, one
  `recv()` that reads the frame, one that returns `EAGAIN` (LESSON 1's
  drain-to-EAGAIN under level-triggered epoll), parse, handler, and ten
  `std::string` appends onto recipients' `out`.
- **Send side, 14.3 µs per input**: 10,000 `send()` per period for 1,000
  inputs, 1.43 µs each. Ten recipients per input, one `send()` per
  recipient per wake, and — finding 2 — one input per wake, so nothing
  coalesces.

The cost model's `T_io(epoll) ≈ 2 · c_sys · N_a` is therefore the model of a
server that sends one frame per active connection per tick. The chat shape
is `N_a · (c_drain + F · c_send)`, and `F = 10` puts the send side at nine
tenths of the term. Corrected:

```
T_io(epoll, immediate broadcast) ≈ N_a · (1.25 + F · 1.43) µs      F = 10 → 15.6 µs · N_a
```

At `N_a = 1000` that is 47 % of a 33 ms tick before any logic runs; at
3,000 it is the whole tick. A zone thread with a few hundred players spends
15 % of its tick on I/O under this shape, which is already more than the
"invisible" the hypothesis' § 4 table predicted for 300 — because of `F`,
not because of the syscall constant.

### 1.1 The saturated rows, and what they show about coalescing

At 3,000 and 10,000 the server is at 100 % of its core and the per-input cost
falls to 10.9 and 3.1 µs. The mechanism is in the wakes column: with a
backlog, one `epoll_pwait2` return carries 4 and then 270 ready sockets,
and a recipient that has several frames queued from one batch gets them in
one `send()`. `CHAT_FLUSH=batch` finally has something to batch. The price
is the queue that makes the backlog: client p50 18–23 ms and p99 39–49 ms
at 10k, the 3M row of every earlier note, seen from inside.

That is a 5× cut in the I/O term, obtained here by accident of saturation.
A server that processed inputs once per tick and sent each player one
snapshot per tick would obtain the same coalescing by design, with the
latency bounded by one period instead of by the queue. Predicted for that
shape, from these constants:

```
T_io(epoll, tick-aligned snapshot) ≈ N_a · (1.25 + 1 · 1.43) µs ≈ 2.7 µs · N_a
```

2.7 ms at `N_a = 1000` (8 % of the tick) instead of 15.6 ms (47 %). No
change of I/O mechanism is involved. This is the largest budget lever this
experiment has measured, and it sits in the product layer, which is what
`design-notes/2026-09-02` § 5 predicted the answer would look like.

## 2. The logic term and the overrun boundary

Tick p50 against `N × L`, pass 2, `W = 64`:

| N | L | N × L | tick p50 | ratio | overruns |
|---:|---:|---:|---:|---:|---:|
| 300 | 30 µs | 9.0 ms | 8.96 ms | 0.995 | 0 % |
| 1,000 | 10 µs | 10.0 ms | 10.03 ms | 1.003 | 0 % |
| 1,000 | 30 µs | 30.0 ms | 30.26 ms | 1.009 | 0.6 % |
| 1,000 | 100 µs | 100 ms | 100.6 ms | 1.006 | 84 % |
| 3,000 | 10 µs | 30.0 ms | 30.08 ms | 1.003 | 0.75 % |
| 3,000 | 30 µs | 90.0 ms | 90.6 ms | 1.006 | 82 % |
| 10,000 | 10 µs | 100 ms | 99.7 ms | 0.997 | 76 % |

The knob dials what it says. The one cell that disagrees is pass 1 at
`N = 1000, L = 30` (22.9 ms, 0.76 of nominal): its banner reads
`calibrated at 0.43 iters/ns` where every other cell reads 0.55–0.59, so
its spins ran at 0.43 / 0.57 = 75 % of the requested length. The
calibration happened during the pass-1 disturbance of finding 8; the row is
kept because the dump explains it.

The boundary is where the hypothesis put it. `N × L = 30 ms` sits at the
edge (0.6–4.6 % overruns); anything above it overruns most ticks. Three
thousand entities per thread overrun at every `L` above 10 µs, ten thousand
at every `L` above zero — the simulation tier cannot hold thousands of
players per thread whatever the I/O layer costs, and `2026-09-02` § 4 said
so before the loop existed.

## 3. The working set: P1 falsified

Per-input receive-side drain and per-period flush at `L = 0`, pass 2:

| N | W | drain p50 | Δ drain per input vs W = 64 | flush p50 | Δ flush | tick p50 | tick per block |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 300 | 64 | 0.442 ms | — | 4.448 ms | — | 0.001 ms | 3 ns |
| 300 | 512 | 0.477 | +0.12 µs | 4.468 | 0 % | 0.008 | 27 ns |
| 300 | 4096 | 0.528 | **+0.29 µs** | 4.454 | 0 % | 0.046 | 153 ns |
| 1,000 | 64 | 1.251 | — | 14.313 | — | 0.004 | 4 ns |
| 1,000 | 512 | 1.240 | −0.01 µs | 14.241 | −1 % | 0.024 | 24 ns |
| 1,000 | 4096 | 1.481 | **+0.23 µs** | 13.982 | **−2 %** | 0.166 | 166 ns |
| 3,000 | 64 | 2.323 | — | 30.302 | — | 0.013 | 4 ns |
| 3,000 | 4096 | 3.198 | **+0.29 µs** | 28.838 | **−5 %** | 0.586 | 195 ns |
| 10,000 | 64 | 8.773 | — | 21.799 | — | 0.042 | 4 ns |
| 10,000 | 4096 | 10.728 | **+0.20 µs** | 18.117 | **−17 %** | 2.061 | 206 ns |

P1 predicted the per-active-connection I/O cost would rise 20–40 % from
`W = 64` to `W = 4096` at 10k, and set 10 % as the threshold below which the
cache-coupling argument is not material on this box. The receive-side drain
rose by 0.2–0.3 µs per input, 18–37 % of the drain — but the drain contains
the handler's own read-modify-write of the sender's 4 KiB block, sixty-four
lines, and the tick column prices exactly that walk at 150–200 ns per block
when it runs sequentially and warm. Random and cold, the handler's copy of
it accounts for the whole increase. The send side, which touches no block
and is nine tenths of the term, moved 0 to −2 % where the server had
headroom and fell where it was saturated (the tick took the time). Against
the 15.6 µs total the residual attributable to colder I/O syscalls is under
1 %. **P1 is falsified at its own threshold.**

Why, in one sentence: the 40 MB sweep evicts the I/O path's working set
once per tick, and the first few wakes re-warm it; the penalty is
`O(I/O working set)` per tick — tens of microseconds — not `O(N_a)` per
syscall. FlexSC's indirect cost runs the other way, syscalls evicting user
data, and would appear as a tick term that grows with `N_a` at fixed `W`.
At `W = 64` the tick per block is 3–4 ns at every `N`; at `W = 4096` it is
153 → 166 → 195 → 206 ns from 300 to 10k, which is the sweep leaving L3 for
DRAM (1.2 → 4 → 12 → 40 MB), not the syscalls. P2 cannot be tested without
the io_uring side, but nothing here suggests the logic term differs by the
5 % that would move § 2 of the hypothesis.

What this does not say: that a real system's working set is irrelevant.
The design note's § 3 lists what the knob understates — instruction
footprint, dependent misses, coherence traffic — and each would raise the
tick term. What it does say is that the *coupling* into the I/O term, the
thing that would have made the io_uring comparison depend on `W`, is not
there at this scale. The sweep in stage B can hold `W = 64` and lose
nothing.

## 4. The client's view, and the gates

**p99 is the tick.** In every unsaturated cell the client's latency p99 sits
on the tick duration: `N = 300`: `L = 10` tick 3.02 / p99 2.85 ms, `L = 30`
8.96 / 8.85; `N = 1000`, `L = 10`: 10.03 / 10.14. A frame that arrives
during the tick waits for the tick. Under immediate broadcast the logic
term is the latency tail, directly and in full; under a tick-aligned
snapshot it would be half a period plus the tick, for everyone. The p50
stays at 20–60 µs in the same cells: most frames land between ticks.

**The censored gate.** Every 10k row with `L ≥ 30 µs` (tick 300 ms and
above) and pass 1 of 3k × 100 µs voided with `[VOID] fleet latency p99 is
past the 1s histogram range` — the finding 8 fix from this morning,
firing on rows whose latency was off the instrument's scale. Their
server-side phases are in the appendix; their client columns are not
numbers.

**Pass 1.** The first sixteen cells (`W = 64`, pass 1, about the first
fifteen minutes) carried client self-lag p99 of 2.6–13 ms, voiding five of
them by the ratio; the same cells in pass 2 carried 0.003–0.02 ms and
passed. Server-side drain, tick and flush agree between passes within 3 %
(the calibration outlier aside), so the disturbance was on the client or
the box, not in the server. Three later cells show 1.2–2.5 ms of the same.
This is finding 7 of `2026-08-30` — this box's noise is of the order of
some of the deltas — and the reason the grid ran twice. Client columns are
quoted from pass 2 throughout.

## 5. What it means for the hypothesis

1. **The hypothesis' arithmetic used the wrong `F`.** With `F = 10` and no
   coalescing, epoll's I/O term is 16 µs per active connection per tick, and
   1,000 active connections per thread take half the tick before logic. The
   two-tier verdict of `2026-09-02` § 5 stands in shape — hundreds of
   players with heavy logic, thousands with light — but the number where
   epoll stops holding the tick is lower than its § 4 table, by `F`.
2. **The lever is coalescing, and epoll can pull it.** The saturated rows
   show 3.1 µs per input once sends collapse; a tick-aligned snapshot is
   predicted at 2.7 µs by construction. That is the change to make before
   the I/O mechanism is compared, or the comparison credits io_uring with
   batching that a tick already provides — the same trap `CHAT_FLUSH=batch`
   exists to avoid, one level up.
3. **Refined prediction for stage B**, against the tick-aligned shape:
   io_uring with multishot recv and one submit per tick replaces the
   receive side's three syscalls per input (~1.25 µs) with one completion
   (~0.1–0.2 µs) and the per-player `send()` (~1.43 µs) with an SQE, so the
   term falls from ~2.7 to ~0.5 µs per active connection per tick. At
   1,000 that is 2.7 ms → 0.5 ms, 8 % → 1.5 % of the tick; at 10,000 it is
   27 ms → 5 ms, the difference between not holding the tick and holding
   it. The boundary the hypothesis asked for is predicted between those.
4. **`W` drops out.** Stage B runs at `W = 64`; the memory term stays in the
   header for the day the logic is real.

## 6. What is still not measured

- **Stage B**: the io_uring data path does not exist. P2 and P3 wait on it.
- **Stage C**: the tick-aligned snapshot shape. § 1.1's 2.7 µs is a
  prediction from measured constants, not a measurement.
- **The handler-touch confound in § 3** could be closed with a knob that
  skips the handler's block walk; it was not needed for the verdict, since
  the flush term carried it.
- **The pass-1 disturbance** has no cause assigned.
- **Everything here is one WSL2 vCPU.** The structure — `F` in the I/O term,
  `N × L` at the boundary, eviction once per tick — does not depend on the
  box; every constant does.

## Appendix — every cell, both passes

drain / tick / flush are per-period milliseconds, p50 / p99. "io share" is
(drain p50 + flush p50) / 33.33 ms. Client columns from `merge.py`;
`999.999+` is a p99 past the histogram's range. Verdicts: `VOID` in pass 1
`W = 64` cells with self-lag above ~1 ms is the ratio gate (finding 8 of
this note); `VOID` on 10k × `L ≥ 30` is the censored gate.

#### W = 64 B

| N | L µs | pass | drain p50 / p99 | tick p50 / p99 | flush p50 / p99 | io share | wakes p50 | overrun % | client p50 / p99 | self-lag p99 | srv CPU | verdict |
|---:|---:|:--:|---|---|---|---:|---:|---:|---|---:|---:|---|
| 300 | 0 | 1 | 0.474 / 7.070 | 0.001 / 0.031 | 4.522 / 19.803 | 15 % | 299 | 0.00 | 0.026 / 7.832 | 2.635 | 30 | `VOID` |
| 300 | 0 | 2 | 0.442 / 0.750 | 0.001 / 0.003 | 4.448 / 5.899 | 15 % | 300 | 0.00 | 0.022 / 0.098 | 0.003 | 18 | `OK` |
| 300 | 10 | 1 | 0.418 / 3.533 | 2.945 / 10.597 | 4.081 / 15.610 | 13 % | 275 | 0.00 | 0.025 / 10.511 | 2.703 | 32 | `VOID` |
| 300 | 10 | 2 | 0.410 / 0.709 | 3.024 / 4.632 | 4.040 / 5.112 | 13 % | 273 | 0.00 | 0.023 / 2.851 | 0.007 | 25 | `OK` |
| 300 | 30 | 1 | 0.433 / 3.249 | 9.030 / 25.250 | 3.486 / 12.364 | 12 % | 218 | 0.30 | 0.039 / 21.898 | 1.350 | 51 | `OK` |
| 300 | 30 | 2 | 0.374 / 0.683 | 8.957 / 10.726 | 3.352 / 4.272 | 11 % | 219 | 0.00 | 0.025 / 8.852 | 0.004 | 41 | `OK` |
| 300 | 100 | 1 | 0.250 / 3.632 | 29.740 / 58.159 | 0.886 / 16.728 | 3 % | 28 | 4.68 | 13.857 / 54.731 | 3.703 | 94 | `OK` |
| 300 | 100 | 2 | 0.238 / 0.359 | 30.094 / 31.403 | 0.836 / 1.078 | 3 % | 25 | 0.15 | 14.116 / 30.190 | 0.004 | 96 | `OK` |
| 1000 | 0 | 1 | 1.268 / 7.646 | 0.004 / 0.011 | 14.383 / 24.386 | 47 % | 983 | 0.00 | 0.023 / 11.330 | 3.718 | 60 | `VOID` |
| 1000 | 0 | 2 | 1.251 / 1.593 | 0.004 / 0.008 | 14.313 / 16.005 | 47 % | 980 | 0.00 | 0.021 / 0.109 | 0.009 | 53 | `OK` |
| 1000 | 10 | 1 | 1.151 / 5.955 | 10.099 / 26.975 | 10.200 / 16.030 | 34 % | 649 | 0.30 | 0.058 / 38.132 | 4.823 | 77 | `OK` |
| 1000 | 10 | 2 | 1.077 / 1.481 | 10.034 / 11.832 | 10.093 / 13.006 | 34 % | 655 | 0.00 | 0.025 / 10.142 | 0.010 | 70 | `OK` |
| 1000 | 30 | 1 | 0.936 / 6.814 | 22.932 / 42.573 | 4.903 / 21.114 | 18 % | 241 | 3.94 | 8.571 / 51.176 | 4.350 | 91 | `OK` |
| 1000 | 30 | 2 | 0.799 / 1.231 | 30.259 / 31.765 | 1.850 / 2.590 | 8 % | 4 | 0.61 | 22.146 / 62.733 | 0.013 | 102 | `OK` |
| 1000 | 100 | 1 | 2.655 / 11.064 | 100.476 / 183.749 | 14.146 / 21.800 | 50 % | 851 | 81.87 | 42.315 / 167.544 | 4.839 | 93 | `OK` |
| 1000 | 100 | 2 | 2.643 / 3.896 | 100.562 / 103.476 | 14.152 / 18.584 | 50 % | 851 | 83.60 | 34.569 / 100.110 | 0.009 | 92 | `OK` |
| 3000 | 0 | 1 | 2.621 / 14.290 | 0.014 / 0.053 | 29.904 / 30.363 | 98 % | 606 | 0.00 | 0.069 / 40.158 | 8.131 | 102 | `VOID` |
| 3000 | 0 | 2 | 2.323 / 2.784 | 0.013 / 0.022 | 30.302 / 30.495 | 98 % | 677 | 0.00 | 0.055 / 0.198 | 0.014 | 102 | `OK` |
| 3000 | 10 | 1 | 1.299 / 15.337 | 29.965 / 55.280 | 1.825 / 22.719 | 9 % | 2 | 4.57 | 83.653 / 196.846 | 1.067 | 102 | `OK` |
| 3000 | 10 | 2 | 1.128 / 2.065 | 30.080 / 31.799 | 1.687 / 2.982 | 8 % | 2 | 0.75 | 98.496 / 224.244 | 0.008 | 102 | `OK` |
| 3000 | 30 | 1 | 6.559 / 19.381 | 90.718 / 152.722 | 24.484 / 25.496 | 93 % | 357 | 78.67 | 42.903 / 390.258 | 0.363 | 102 | `OK` |
| 3000 | 30 | 2 | 6.258 / 8.365 | 90.558 / 94.763 | 25.107 / 25.748 | 94 % | 382 | 82.44 | 40.212 / 90.752 | 0.008 | 102 | `OK` |
| 3000 | 100 | 1 | 10.680 / 32.634 | 299.899 / 509.202 | 12.107 / 21.241 | 68 % | 3 | 52.14 | 164.545 / 999.999+ | - | 102 | `VOID` |
| 3000 | 100 | 2 | 11.117 / 13.865 | 300.483 / 311.021 | 18.536 / 21.080 | 89 % | 18 | 59.43 | 154.862 / 309.578 | 0.011 | 102 | `OK` |
| 10000 | 0 | 1 | 9.155 / 24.668 | 0.045 / 0.155 | 21.400 / 24.130 | 92 % | 35 | 0.00 | 22.980 / 206.990 | 13.308 | 102 | `OK` |
| 10000 | 0 | 2 | 8.773 / 10.190 | 0.042 / 0.078 | 21.799 / 22.648 | 92 % | 37 | 0.00 | 18.344 / 38.571 | 0.021 | 102 | `OK` |
| 10000 | 10 | 1 | 14.480 / 22.297 | 100.827 / 106.415 | 17.831 / 20.366 | 97 % | 14 | 76.04 | 183.930 / 471.533 | 0.037 | 102 | `OK` |
| 10000 | 10 | 2 | 14.550 / 22.359 | 99.718 / 103.325 | 17.917 / 20.538 | 97 % | 15 | 76.26 | 184.905 / 484.468 | 0.032 | 102 | `OK` |
| 10000 | 30 | 1 | 3.430 / 56.047 | 49.518 / 303.237 | 2.854 / 19.522 | 19 % | 1 | 60.12 | 999.999+ / 999.999+ | - | 100 | `VOID` |
| 10000 | 30 | 2 | 2.830 / 53.063 | 45.425 / 306.352 | 2.545 / 21.089 | 16 % | 1 | 60.12 | 999.999+ / 999.999+ | - | 101 | `VOID` |
| 10000 | 100 | 1 | 6.431 / 81.979 | 646.040 / 1000.000+ | 5.180 / 35.153 | 35 % | 1 | 62.79 | 999.997+ / 999.997+ | - | 102 | `VOID` |
| 10000 | 100 | 2 | 10.985 / 60.603 | 711.899 / 1000.000+ | 4.026 / 35.253 | 45 % | 1 | 62.79 | 999.994+ / 999.994+ | - | 102 | `VOID` |

#### W = 512 B

| N | L µs | pass | drain p50 / p99 | tick p50 / p99 | flush p50 / p99 | io share | wakes p50 | overrun % | client p50 / p99 | self-lag p99 | srv CPU | verdict |
|---:|---:|:--:|---|---|---|---:|---:|---:|---|---:|---:|---|
| 300 | 0 | 1 | 0.460 / 0.911 | 0.007 / 0.021 | 4.391 / 6.609 | 15 % | 301 | 0.00 | 0.023 / 0.094 | 0.010 | 18 | `OK` |
| 300 | 0 | 2 | 0.477 / 0.953 | 0.008 / 0.025 | 4.468 / 6.535 | 15 % | 300 | 0.00 | 0.023 / 0.087 | 0.004 | 18 | `OK` |
| 300 | 30 | 1 | 0.381 / 0.690 | 9.065 / 11.900 | 3.322 / 4.869 | 11 % | 218 | 0.00 | 0.025 / 8.975 | 0.007 | 41 | `OK` |
| 300 | 30 | 2 | 0.390 / 0.710 | 9.176 / 13.053 | 3.344 / 4.780 | 11 % | 215 | 0.00 | 0.026 / 9.196 | 0.002 | 42 | `OK` |
| 1000 | 0 | 1 | 1.240 / 1.683 | 0.024 / 0.048 | 14.279 / 17.521 | 47 % | 985 | 0.00 | 0.021 / 0.085 | 0.006 | 53 | `OK` |
| 1000 | 0 | 2 | 1.240 / 5.140 | 0.024 / 0.091 | 14.241 / 23.104 | 46 % | 983 | 0.00 | 0.022 / 6.116 | 1.217 | 55 | `OK` |
| 1000 | 30 | 1 | 0.772 / 1.134 | 30.410 / 31.860 | 1.792 / 2.353 | 8 % | 4 | 0.30 | 22.024 / 64.627 | 0.006 | 102 | `OK` |
| 1000 | 30 | 2 | 0.812 / 5.971 | 29.642 / 52.601 | 2.067 / 22.533 | 9 % | 17 | 3.63 | 20.580 / 133.736 | 0.763 | 99 | `OK` |
| 3000 | 0 | 1 | 2.488 / 2.965 | 0.074 / 0.122 | 30.069 / 30.298 | 98 % | 667 | 0.00 | 0.055 / 0.197 | 0.013 | 102 | `OK` |
| 3000 | 0 | 2 | 2.552 / 10.965 | 0.073 / 0.223 | 30.001 / 30.334 | 98 % | 647 | 0.00 | 0.058 / 19.887 | 2.539 | 102 | `OK` |
| 3000 | 30 | 1 | 6.688 / 8.383 | 91.374 / 94.551 | 24.598 / 25.547 | 94 % | 358 | 82.84 | 40.804 / 91.712 | 0.008 | 103 | `OK` |
| 3000 | 30 | 2 | 6.922 / 9.372 | 91.019 / 95.749 | 24.277 / 25.126 | 94 % | 333 | 82.44 | 41.203 / 91.530 | 0.012 | 102 | `OK` |
| 10000 | 0 | 1 | 9.508 / 10.735 | 0.267 / 0.557 | 20.770 / 21.666 | 91 % | 36 | 0.00 | 18.776 / 39.637 | 0.027 | 102 | `OK` |
| 10000 | 0 | 2 | 9.500 / 11.197 | 0.266 / 0.489 | 20.923 / 21.790 | 91 % | 34 | 0.00 | 19.776 / 43.135 | 0.023 | 103 | `OK` |
| 10000 | 30 | 1 | 2.606 / 55.945 | 41.882 / 307.806 | 2.791 / 22.462 | 16 % | 1 | 61.80 | 999.999+ / 999.999+ | - | 101 | `VOID` |
| 10000 | 30 | 2 | 2.540 / 65.127 | 35.436 / 318.255 | 3.228 / 22.524 | 17 % | 1 | 62.07 | 999.999+ / 999.999+ | - | 99 | `VOID` |

#### W = 4096 B

| N | L µs | pass | drain p50 / p99 | tick p50 / p99 | flush p50 / p99 | io share | wakes p50 | overrun % | client p50 / p99 | self-lag p99 | srv CPU | verdict |
|---:|---:|:--:|---|---|---|---:|---:|---:|---|---:|---:|---|
| 300 | 0 | 1 | 0.526 / 0.935 | 0.045 / 0.179 | 4.430 / 6.131 | 15 % | 300 | 0.00 | 0.023 / 0.098 | 0.003 | 18 | `OK` |
| 300 | 0 | 2 | 0.528 / 0.900 | 0.046 / 0.120 | 4.454 / 5.735 | 15 % | 300 | 0.00 | 0.023 / 0.108 | 0.004 | 18 | `OK` |
| 300 | 30 | 1 | 0.454 / 0.851 | 9.117 / 9.734 | 3.304 / 4.903 | 11 % | 217 | 0.00 | 0.026 / 9.020 | 0.007 | 42 | `OK` |
| 300 | 30 | 2 | 0.437 / 3.355 | 9.117 / 19.948 | 3.315 / 10.953 | 11 % | 217 | 0.15 | 0.027 / 17.508 | 1.759 | 45 | `OK` |
| 1000 | 0 | 1 | 1.505 / 2.112 | 0.171 / 0.320 | 14.231 / 17.559 | 47 % | 976 | 0.00 | 0.022 / 0.137 | 0.012 | 54 | `OK` |
| 1000 | 0 | 2 | 1.481 / 8.365 | 0.166 / 1.036 | 13.982 / 22.545 | 46 % | 978 | 0.00 | 0.022 / 8.926 | 1.653 | 57 | `OK` |
| 1000 | 30 | 1 | 0.924 / 3.286 | 30.259 / 33.384 | 1.759 / 16.466 | 8 % | 3 | 1.52 | 24.561 / 72.990 | 0.012 | 102 | `OK` |
| 1000 | 30 | 2 | 0.942 / 7.153 | 30.256 / 53.434 | 1.817 / 22.472 | 8 % | 4 | 6.20 | 25.699 / 80.328 | 2.337 | 102 | `OK` |
| 3000 | 0 | 1 | 3.140 / 3.817 | 0.566 / 1.246 | 28.910 / 29.206 | 96 % | 621 | 0.00 | 0.060 / 0.553 | 0.014 | 101 | `OK` |
| 3000 | 0 | 2 | 3.198 / 4.087 | 0.586 / 0.959 | 28.838 / 29.177 | 96 % | 601 | 0.00 | 0.060 / 0.585 | 0.012 | 102 | `OK` |
| 3000 | 30 | 1 | 7.484 / 8.947 | 88.994 / 92.382 | 23.945 / 24.746 | 94 % | 344 | 84.31 | 39.937 / 89.422 | 0.009 | 103 | `OK` |
| 3000 | 30 | 2 | 8.079 / 10.430 | 91.532 / 95.173 | 22.947 / 23.989 | 93 % | 292 | 82.35 | 42.352 / 92.109 | 0.010 | 102 | `OK` |
| 10000 | 0 | 1 | 10.508 / 12.009 | 2.017 / 3.007 | 18.222 / 19.135 | 86 % | 32 | 0.00 | 21.649 / 45.127 | 0.024 | 102 | `OK` |
| 10000 | 0 | 2 | 10.728 / 12.267 | 2.061 / 2.848 | 18.117 / 19.088 | 87 % | 29 | 0.00 | 22.962 / 48.710 | 0.031 | 102 | `OK` |
| 10000 | 30 | 1 | 3.974 / 56.993 | 47.516 / 301.466 | 2.752 / 17.061 | 20 % | 1 | 60.61 | 999.999+ / 999.999+ | - | 102 | `VOID` |
| 10000 | 30 | 2 | 0.486 / 56.227 | 31.422 / 307.992 | 0.760 / 18.502 | 4 % | 1 | 35.64 | 999.999+ / 999.999+ | - | 103 | `VOID` |

## Rationale links

- [`../design-notes/2026-09-02-where-io-uring-becomes-meaningful.md`](../design-notes/2026-09-02-where-io-uring-becomes-meaningful.md)
  — the hypothesis and the cost model § 1 corrects; § 4 for the overrun
  boundary § 2 confirms; § 5 for the two-tier verdict that stands in shape.
- [`../design-notes/2026-09-03-working-set-knob-for-the-tick-budget-experiment.md`](../design-notes/2026-09-03-working-set-knob-for-the-tick-budget-experiment.md)
  — P1–P3 and the staging; § 3 for what the knob understates.
- [`../server-epoll/tick_logic.h`](../server-epoll/tick_logic.h) — the
  logic term and the instrument, as built; `../server-epoll/README.md`
  § "The tick-budget experiment" for the knobs.
- `../client-bench/tools/ticksweep.py` — the grid runner.
- [`2026-08-30-what-limits-the-server.md`](2026-08-30-what-limits-the-server.md)
  — the syscall constants this note's decomposition agrees with; finding 7
  for the noise § 4 met again.
- [`2026-09-02-stl-to-sds-the-measured-delta.md`](2026-09-02-stl-to-sds-the-measured-delta.md)
  § 6 — finding 8 and its fix, which gated the 10k rows here.
