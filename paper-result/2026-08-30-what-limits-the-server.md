# 2026-08-30 — What limits the server, and why CPU% does not say

> **Why this file exists.** The question was operational — *I want the server to
> reach its limit before the client does.* Answering it killed three plausible
> explanations and one of my own conclusions. The baseline table in `README.md`
> gets rewritten when io_uring lands; the traps below do not.
>
> One property runs through all of it: **every gauge read green during the runs
> that were wrong.** `self-lag` 0.008–0.019 ms, `backpressed=0`, `lost_conns=0`,
> and server CPU at 100% — in runs whose answer moved 60% when the fleet grew,
> and across a 3.3× throughput range.

| # | Finding | Status |
|---|---|---|
| 1 | Latency is the loop's **sweep period**, not a load backlog | Architectural |
| 2 | **100% CPU is not saturation** — 3M → 10M deliveries/s at 100% | Corrects an earlier draft of this file |
| 3 | Capacity is a **delivery-rate** ceiling, independent of connection count | Model, fitted |
| 4 | Near capacity, latency is **not attributable** to any one component | Needs server-side stamping |
| 5 | Fan-out drift was advisory; advisory is not a gate | Fixed — `[VOID]`, exit 3 |
| 6 | Nothing in the instrument observed the server at all | Fixed — `--server-pid` |

Every number: one vCPU of an i5-13600K under **WSL2**, loopback,
`CHAT_FLUSH=batch`, `-O2 -DNDEBUG`, fan-out validated at 10.0× per row. See the
WSL2 note at the end — it is not a footnote.

---

## 1. Latency is a sweep period, not a backlog

Hold offered load **constant** and vary only how it spreads across connections.
Same arrival rate, same service rate, same ~100% server CPU:

| conns | send interval | p50 | p90 | deliveries/s |
|---|---|---|---|---|
| 5,004 | 16.7 ms | 9.65 / 10.00 | **16.79 / 17.62** | 3,001,280 |
| 10,008 | 33.3 ms | 18.35 / 18.77 | **32.82 / 33.46** | 3,004,007 |
| 20,016 | 66.7 ms | 44.55 / 45.88 | **78.98 / 74.96** | 3,001,925 |

`p90` lands on the per-connection send interval every time, and latency varies
**4.5× under identical pressure**. A load-driven queue cannot do that — all
three rows apply the same load. What varies is how many connections the
single-threaded loop walks before returning to any one of them.

**So "raise pressure until latency degrades" does not find a bottleneck here.**
It finds the sweep period, which connection count sets.

Three attractive explanations, tested and dead — recorded so nobody re-tests:

- **`epoll_wait` maxevents (256).** 256 / 1024 / 4096 left the tail in the same
  10–17 ms band. It had to: more events per call is the same work in fewer
  syscalls, so the sweep *period* is unchanged.
- **P/E-core scheduling.** Pinned p99.9 11.2/13.6/11.6 vs unpinned
  15.7/9.6/10.7. No effect — and see the WSL2 note for why it could not have.
- **The `batch` flush deferral.** `immediate` has no deferral and its tail is
  ten times worse (66 ms vs 0.24 ms at p99, 10k conns), which re-confirms
  [`2026-08-17`](../client-bench/design/2026-08-17-three-instrument-defects.md) defect 1.

## 2. 100% CPU is not saturation

An earlier draft of this file claimed the server was "at its limit" because
`--server-pid` read 100% of one core. That was asserted without ever running
past the range `README.md` records. Pushing past it:

| target | achieved | fan-out | srv CPU | p50 | self-lag p99 |
|---|---|---|---|---|---|
| 3M | 3,001,961 (100%) | 10.00 | 100% | 23.5 | 0.016 |
| 8M | 7,998,943 (100%) | 9.99 | 100% | 50.9 | 0.072 |
| **10M** | **9,990,408 (100%)** | 9.98 | 100% | 63.5 | 0.982 |
| 15M | 3,728,900 (25%) | 5.83 | 97% | 188.7 | **19.359** |

**100% CPU held from 3M to 10M deliveries/s** while kernel share fell 86% → 77%.
The server gets *more efficient* under load: a longer sweep accumulates more
messages per connection, so each `read()` and `send()` carries more of them.
Batching factor (`rate × sweep`) went 0.7 → 6.4.

**The 15M collapse is the client.** `sent=6,393,289` against a 15,012,000
target — the fleet could not issue the sends; self-lag 19.4 ms; server CPU
*falls* to 44% at higher rates because it is starved. The server's ceiling was
never reached in any dimension: ≥1.0M msg/s, ≥10M deliveries/s, ≥1.37 GB/s, all
client-limited.

The signal that works is a pair, not a gauge:

- **server ceiling** = achieved < offered **and** self-lag small
- **client ceiling** = achieved < offered **and** self-lag large

Every collapse produced here was the second. The first was never observed.

## 3. Capacity is a delivery-rate ceiling

Per-message cost falls as load rises, so capacity is not one throughput number.
When backlogged, the server walks `N` connections in period `T`, paying
per-connection syscall cost per sweep plus content cost for what arrived:

```
T = 2·c_sys·N + T·φ        φ = M_in·(c_msg + F·(c_del + c_byte·B_out))

  T = 2·c_sys·N / (1 − φ)                    capacity ⟺  φ < 1
```

Fitted on a rate ladder at 10,008 conns, F=10, 64 B (`1/T` is linear in `M_in`,
so the x-intercept is capacity): **c_del ≈ 45.7 ns/delivery**,
**c_byte ≈ 0.379 ns/byte**, `c_sys` 0.75–1.65 µs. Model tracks measured `p90`
within +4% / −10% / +14% across the ladder. Predicted ceiling **1.34M msg/s
(13.4M deliveries/s)** — clean at 1.0M, client broke at 1.5M, so it sits in the
unmeasured gap. Cross-checks out of sample against the fan-out runs.

**`N` is absent from the capacity condition.** Connection count sets latency —
the numerator — never the ceiling, which is why §2's CPU readings said nothing.
Consequences:

- **Fan-out is free at the ceiling.** Capacity in *deliveries/s* is invariant:
  13.4M at F=1 and at F=100. Fan-out only changes the input rate that fills it.
- **Payload size trades messages for bytes.** 16 B → 1.78M msg/s / 474 MB/s;
  1024 B → 228k msg/s / 2,254 MB/s. Asymptotes at ~21.9M deliveries/s
  (bookkeeping-bound) and ~2.64 GB/s (copy-bound), crossing near `B_out` ≈ 121 B.

**The model predicts capacity, not latency.** The sweep form assumes a backlog
and misapplies silently without one: at 3,000 conns × 30/s it predicts 10.6 ms
against a **measured p90 of 0.040 ms — wrong by 250×**. `c_msg` is also not
separable from `c_del` here, so capacity comes out exactly `∝ 1/F`, which
flatters high fan-out. Treat the constants as this box's, and the structure as
the portable part.

## 4. Near capacity, latency is not attributable

`delivery latency = recv_ts − intended_send_ts` sums five stages, of which one
is instrumented:

| stage | measured? |
|---|---|
| `intended_send` → actual send | ✅ **self-lag** |
| send → server read | ❌ |
| server read → `flush_dirty()` | ❌ |
| flush → client socket | ❌ |
| client socket → `recv_ts` | ❌ |

Near capacity all five grow at once — sweep lengthens, buffers fill, windows
tighten, the client is scheduled later, the hypervisor moves a vCPU — and the
sum cannot be decomposed afterwards. This is why splitting the same load across
more nodes moved reported latency 41 → 67 ms **while self-lag fell**, with
`backpressed=0` throughout: self-lag watches sends, and the damage is on the
receive side. That is `2026-08-17` defect 3b at a larger scale, and **both
stamps being client-side means no client-side experiment can separate them.**

The operating rule that follows:

- **Capacity** comes from throughput — achieved vs offered, paired with
  self-lag to say which side fell short. That question has a clean answer.
- **Latency** is attributable only well below capacity, where one source
  dominates. Near saturation it is a real number — it is what a user
  experiences — but it is not "the server's latency."

The fix is a **server-side queue-delay histogram**: the interval between
`on_readable()` accepting a frame and `flush_dirty()` writing it. `CLOCK_MONOTONIC`
is system-wide, so on one box no clock sync is needed; cross-machine it
degrades to what exists now, which the `node_id` ownership stamp already
guards.

Note the client is deliberately **multi-process, not multi-threaded**: separate
address spaces are what make the N-vs-2N disagreement check trustworthy, the
model is identical on one box and many, and ephemeral ports are a per-source-IP
resource threads would not buy. So "add client capacity" means add nodes.
Sizing rule from these runs: the client does `F` reads and parses per message it
sends against the server's one read and `F` appends, so **client cores ≳ 1.4 ×
F × server cores** — 14 client vCPUs against one server core at F=10 broke at
10M deliveries/s, which is exactly that ratio.

## 5–6. The two fixes

**Fan-out is now a gate.** Orphaned `loadgen` processes stayed connected and
in-room; measured fan-out hit 17.4 against `--per-room 10` and the fleet
reported **860k deliveries/s against a 500k target**. An open-loop generator
cannot exceed its target, so throughput above target can only be contamination
— but it presents as a *better* number, which is the kind a human confirms
rather than questions. Two sweeps were discarded before the advisory line was
noticed. Now `[VOID]` and exit 3 in both `traffic.cpp` and `merge.py`, checked
*before* the self-lag verdict. Drift is systematic and small (10.26 at 50
conns, 10.00 by 3k), so the ±5% band does not false-positive.

**`--server-pid`** samples the server's utime+stime across the offered-load
window only — never the drain, a second of deliberate near-idle that would
understate it — and travels in the dump so `merge.py` reports it fleet-wide,
averaged rather than summed since every node measured the same server. **Read
it for its low values**: under 95% the run is definitively below the ceiling;
at 100% you have learned almost nothing (§2).

## What WSL2 costs this file

The guest's CPU topology is **synthetic** — WSL2 reports 10 uniform cores × 2
threads; the i5-13600K is 6 P-cores plus 8 E-cores. Verified consequences:
`taskset` inside the guest pins to a vCPU, not a physical core (all 20 vCPUs
benchmark identically at 474–485 ms on a fixed loop, so they are floating host
threads); the sibling map is fiction (saturating vCPU 3, nominal sibling of the
server's vCPU 2, changed nothing against a far control); and the 9- and
12-node runs had 10–13 busy threads, so the host was certainly using E-cores in
exactly the runs of §4 that were hardest to read.

So `c_sys`, the absolute throughput figures and the fitted ceiling are
**environment-bound**. The structure — capacity independent of `N`, a
delivery-rate ceiling, latency as sweep period, CPU% as a non-signal — is not,
because each rests on a ratio measured under identical conditions. Re-fit on
bare metal, where `perf` exists and can settle whether the core is compute-bound
or stalled.

## Rationale links

- `src/traffic.cpp` — `read_proc_cpu()`, the fan-out gate, `report()`.
- `merge.py` — fleet fan-out gate, server-CPU averaging, exit 3.
- [`2026-08-17-three-instrument-defects.md`](../client-bench/design/2026-08-17-three-instrument-defects.md)
  — defect 3b is §4 here, with the fleet on the wrong side of it.
- `README.md` "One process cannot verify itself" — §4 quantifies it, and the
  recorded baseline is understated by 3–5×.
