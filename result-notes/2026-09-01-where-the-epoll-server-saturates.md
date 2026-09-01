# 2026-09-01 — Where the epoll server saturates, measured at last

> **What this closes.** [`2026-08-30`](2026-08-30-what-limits-the-server.md) §2
> ended with "the server's ceiling was never reached in any dimension" — every
> collapse it produced was the client. This note reaches it, on both sides of
> the cliff, with the server's own close counter agreeing with the client's.
>
> **What it costs.** The ceiling is not where the earlier fitted model put it,
> and past it the server does not degrade — **it sheds clients.** The
> instrument watched it do that and printed `[ OK ]`.

| # | Finding | Status |
|---|---|---|
| 1 | The server ceiling is **observed**, not fitted — 1.70M del/s at 1024 B, 3.50M at 512 B | New |
| 2 | Over 512–1024 B the server is **byte-bound at ≈1.78 GB/s** | New |
| 3 | The 64 B fit predicted **2.64 GB/s** — high by 36% at 1024 B, 19% at 512 B | Corrects `2026-08-30` §3 |
| 4 | Past the ceiling there is no graceful rung: **100% → shed** | New |
| 5 | Connection loss was advisory. Advisory is not a gate | Fixed — `[VOID]`, exit 3 |
| 6 | Fan-out is **structurally blind** to shedding | New; why 5 is not redundant |
| 7 | CPU% read 98–101% on **every rung of both ladders** | Third confirmation of `2026-08-30` §2 |

Same box and caveats as `2026-08-30`: one vCPU of an i5-13600K under WSL2,
loopback, `CHAT_FLUSH=batch`, `-O2 -DNDEBUG`, 10,008 connections, `--per-room
10`, 15 s windows. The WSL2 note there applies here unchanged.

---

## 0. First, the instrument still tells the truth

Before moving anything, the recorded 3M row was re-run on today's tree:

| | recorded 2026-08-30 | today |
|---|---|---|
| deliveries/s | 3,001,961 | **3,002,207** |
| fan-out | 10.00 | **10.00** |
| server CPU | 100% (user 14 / kernel 86) | **101% (user 14 / kernel 86)** |
| self-lag p99 | 0.016 ms | **0.020 ms** |

`chatcli.py verify 8×20`: 1,280/1,280, zero missing, duplicated or
misattributed. The apparatus is the same apparatus.

## 1. How the ceiling was brought into range

The obstruction is structural: at 64 B the server's fitted ceiling (13.4M
deliveries/s) sits above what this box's client fleet can drive, and
`2026-08-30`'s own sizing rule says why — client cores ≳ 1.4 × F × server
cores, so 14 client vCPUs against one server core broke at 10M. There are 20
vCPUs. You cannot buy your way to the ceiling here.

**Tried first, and unavailable: shrink the server.** cgroup v2 `cpu.max` would
cap the server below one core and move its ceiling down instead. The
controller is delegated and `cpu.max` is writable, but the server sits in
`/init.scope` and migrating a process between cgroups needs write access to
the common ancestor, which is root-owned. Recorded so nobody re-tries it on
this box; on a host with a user systemd session it is the cleaner lever.

**Used instead: raise the payload.** `2026-08-30` §3 predicts the server
becomes copy-bound above `B_out` ≈ 121 B, and a byte-bound server has a *lower*
delivery ceiling — which walks it down into the fleet's reach. It needs no
privilege, changes one controlled variable the instrument already exposes
(`--size`), and it tests a prediction that had never been checked.

## 2. The ladders

**1024 B, 6 nodes.** Wire bytes per delivery = 1028.

| rate | offered | achieved | | fan-out | GB/s | p50 | p99 | self-lag p99 | server closes |
|---|---|---|---|---|---|---|---|---|---|
| 12 | 1.20M | 1,203,442 | 100% | 10.02 | 1.22 | **0.416** | 28.7 | 0.013 | 0 |
| 15 | 1.50M | 1,503,045 | 100% | 10.01 | 1.53 | 50.9 | 115.5 | 0.258 | 0 |
| 17 | 1.70M | **1,699,942** | 100% | 9.99 | **1.73** | 70.8 | 153.6 | 0.175 | **0** |
| 20 | 2.00M | 1,766,982 | 88% | 9.87 | 1.80 | 77.0 | ≥1000 | 1.246 | **1,260** |

**512 B, 8 nodes.** Wire bytes per delivery = 516.

| rate | offered | achieved | | fan-out | GB/s | p50 | p99 | self-lag p99 | server closes |
|---|---|---|---|---|---|---|---|---|---|
| 25 | 2.50M | 2,500,588 | 100% | 9.99 | 1.36 | 52.8 | 126.2 | 0.029 | 0 |
| 30 | 3.00M | 3,000,834 | 100% | 9.99 | 1.63 | 65.5 | 279.6 | 0.063 | 0 |
| 35 | 3.50M | **3,500,943** | 100% | 9.99 | **1.81** | 133.8 | 873.9 | 0.459 | **0** |
| 40 | 4.00M | 3,253,969 | 81% | 9.79 | 1.68 | 79.9 | ≥1000 | 1.019 | **1,990** |

"Server closes" is `[drop] … send buffer over cap` counted in the server's own
log across each rung. It is not the client's inference: at 1024 B/rate 20 the
client reported `lost_conns=1260` against the server's 1,260, and at 512 B/rate
40, 1,984 against 1,990 (the six-frame gap is the drain boundary).

**This is the first time in this repository that a ceiling claim rests on the
server agreeing with the client rather than on the client alone.**

Both cliff rungs satisfy `2026-08-30`'s pair rule for a server ceiling —
achieved < offered with self-lag small (1.0–1.2 ms against a latency p99 past
a second). Neither is the client.

## 3. Three thresholds, not one

The question "where does it saturate" has three different answers depending on
what you need from the server, and they are far apart.

| | 1024 B | 512 B | what changes above it |
|---|---|---|---|
| **Interactive** — p50 stays sub-ms | ~1.20M del/s | below the ladder | p50 jumps 0.4 ms → 51 ms between two adjacent rungs |
| **Lossless** — 100% delivered, zero closes | **1.70M** | **3.50M** | the server starts closing clients |
| **Peak** — highest number it will print | 1.77M | 3.50M | offering more returns less |

The gap between the first two is the interesting one. Between rate 12 and rate
15 at 1024 B, p50 goes from 0.416 ms to 50.9 ms — a **122× latency increase for
25% more load**, with every gauge green and delivery still exactly 100%. A
server sized on throughput alone lands on the wrong side of that without
anything in the run saying so.

**Past the lossless rung there is no graceful degradation.** There is no rung
that delivers 95%. Both ladders go 100% → shed, and at 512 B offering 14% more
load (rate 35 → 40) *reduced* throughput from 3.50M to 3.25M. That is
congestion collapse, and its mechanism is `server.cpp:162`: a connection whose
outbound buffer passes `k_send_cap` (256 KiB) is closed, and the reap
broadcasts "X left" into rooms already under pressure — the cascade
`reap_doomed()`'s own comment warns about, arriving under load instead of from
a slow reader.

## 4. The model was optimistic about bytes

`2026-08-30` §3 fitted, at 64 B: `c_del ≈ 45.7 ns`, `c_byte ≈ 0.379 ns/B`,
asymptote 2.64 GB/s.

| | model | measured | |
|---|---|---|---|
| 1024 B | 2.31M del/s | 1.70M | model high by **36%** |
| 512 B | 4.17M del/s | 3.50M | model high by **19%** |

Measured cost per wire byte is **0.572 ns** at 1024 B and **0.554 ns** at
512 B — agreeing within 3.3%, and giving a byte ceiling of **≈1.78 GB/s**
against the predicted 2.64. The delivery ceiling ratio between the two sizes
is 2.059 where pure byte-binding predicts 2.00, so a small per-delivery term
survives but the regime is byte-bound.

The structure held; the constant did not, and the reason is in the earlier
note's own caveat. A fit taken at 64 B has the byte term contributing 24 ns of
a 70 ns budget — a minority share, poorly constrained, and its error is
invisible at the size it was fitted at and dominant at 16× that size. **One
payload size cannot constrain both terms.** The portable lesson is the one
`2026-08-30` already stated about `c_msg` and `c_del` not being separable,
extended: a capacity model needs points from both regimes or it only describes
the regime it was fitted in.

## 5. Connection loss is now a gate

At 1024 B/rate 20 the run lost 1,260 of 10,008 connections, delivered 88% of
its offered load, clamped p99 at the histogram's 1 s ceiling — and printed:

```
[merged] ... backpressed=0 lost_conns=1260
[ OK ] fleet self-lag p99 (1.246ms) is small against latency p99 (999.999ms);
       no node was the bottleneck
```

Every word true. The client was not the bottleneck. And it reads as approval
of a number describing a server shedding an eighth of its clients. This is
`2026-08-30` finding 5 recurring verbatim — *a printed fact nothing acts on is
not a gate* — on a different counter.

`loadgen` and `merge.py` now void a run whose connection loss exceeds **0.5%**,
checked directly after fan-out and for the same reason: a connection that dies
mid-run stops offering load, so what was measured is an average over a load
that changed while it was being measured. Every clean rung of both ladders
measured **exactly zero**, so the band is margin rather than tuning.

Verified both ways. The clean rung (512 B, rate 30) still reports `[ OK ]` and
exits 0 with `lost_conns=0`. The rung that used to print `[ OK ]` now prints:

```
[fleet] measured fan-out 9.91 (--per-room 10)
[VOID] 910 of 10008 connections died during the run (9.1%) — the offered load
       fell while it was being measured ...
fleet exit=3
```

and the server's log recorded 910 closes for that window.

### Why fan-out could not have caught this

Note the 9.91 above: **inside** the ±5% band, so the fan-out gate stayed
silent while 9.1% of the connections died. That is structural, not luck. Fan-out
is `frames_in / sent`, and a closed connection stops appearing in both the
numerator and the denominator — it leaves the ratio nearly untouched and only
the messages in flight to a doomed client perturb it. Fan-out measures the
*shape* of the load; it cannot see the load getting smaller. The two gates
catch disjoint failures, which is why this one is not redundant.

## 6. CPU% failed a third time, over a wider range

`--server-pid` reported **98–101% of one core on all eight rungs of both
ladders.** That range spans a 2.9× throughput difference, a 122× latency
difference, and both sides of a cliff where the server begins closing clients.
The rung delivering 1.20M at 0.416 ms p50 and the rung shedding 1,990
connections are indistinguishable by CPU.

The one-sided reading `2026-08-30` recommends — trust it only when it is *low*
— is confirmed again, and there is now no configuration on this box where a
high reading has ever meant anything.

## 7. What is still not measured

- **p99 is censored at both cliff rungs.** Both report 999.999 ms, the
  histogram's top bucket, so the true tail past the ceiling is unknown. The
  ladder measures where the cliff is, not how deep.
- **64 B was not re-laddered.** The 13.4M figure remains an extrapolation, and
  §4 says a fit from one regime does not travel — including this note's, in
  the other direction. `c_del` is unconstrained by these two sizes (a
  two-point fit puts the intercept slightly negative), so nothing here should
  be extrapolated down to small payloads.
- **Whether the cliff is the send cap or the CPU.** `k_send_cap` is a policy
  constant. Raising it would move the shed threshold and show whether the
  server has usable throughput above 1.78 GB/s that the cap is currently
  hiding, or whether it would simply queue further behind the same wall. One
  run with a larger cap answers it.
- **§4 of `2026-08-30` still stands.** Latency near capacity remains
  unattributable without server-side stamping; nothing here changes that, and
  the 51 ms and 70 ms p50 figures above are what a user experiences rather
  than "the server's latency".

## Rationale links

- `client-bench/src/traffic.cpp` — the connection-loss gate, next to the fan-out gate.
- `client-bench/merge.py` — the same gate on the fleet verdict.
- `server-epoll/server.cpp:162` — `queue_send`, `k_send_cap`, and the close that ends a shedding rung.
- [`2026-08-30-what-limits-the-server.md`](2026-08-30-what-limits-the-server.md) — §2 is the gap this closes, §3 the model this corrects, §5 the defect this repeats.
