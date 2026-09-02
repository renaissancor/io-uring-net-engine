# Measured results

Numbers, and the conditions that make them mean anything. Kept apart from the
tools that produced them for one reason: **the tools change and the numbers
must not silently follow.** A result is only comparable to a later result if
the method that produced it is still written down beside it.

Until this repo existed, the epoll baseline lived in the load generator's
README, because `server-epoll` was then treated as disposable and anything
durable stored there died with it. That is no longer a constraint, and the
numbers now live where they belong.

| file | what it holds |
|---|---|
| this file | the single-threaded epoll baseline — the control group the io_uring engine gets compared against |
| [`2026-08-30-what-limits-the-server.md`](2026-08-30-what-limits-the-server.md) | what limits the server and why CPU% does not say. Findings that outlive any particular number, including the ones that turned out wrong |
| [`2026-09-01-where-the-epoll-server-saturates.md`](2026-09-01-where-the-epoll-server-saturates.md) | the ceiling reached at last — 1.78 GB/s byte-bound, and what the server does past it (it sheds clients, and the instrument said `[ OK ]`) |
| [`2026-09-02-stl-to-sds-the-measured-delta.md`](2026-09-02-stl-to-sds-the-measured-delta.md) | STL to `sds::` on both programs: the client's collapse was two `recv()` per socket, not its containers; the `sds::` control cuts server user-space at 8–10M and is lossless at the 1024 B rung where the STL server sheds |

Method and instrument: [`../client-bench/`](../client-bench/). The verdict
rules that decide whether a run counts at all are in
[`../client-bench/README.md`](../client-bench/README.md) under "Verdicts and
exit codes" — a run that voids is not a slow result, it is not a result.

---

## Baseline: single-threaded epoll server

`server-epoll`, 10k connections, rooms of 10, loopback, same machine.
Delivered messages per second is `conns × rate × per-room`. The server is
single-threaded, so one core is the ceiling.

**There are two baselines, and the difference between them is larger than
anything io_uring is expected to deliver.** The study server takes
`CHAT_FLUSH=immediate|batch`:

- `immediate` — a broadcast calls `send()` once per recipient, inline, while
  walking the room. One syscall per delivery. The naive shape.
- `batch` — the room walk only appends to each recipient's buffer, and one
  flush pass at the end of the epoll batch sends what accumulated. Several
  messages bound for the same connection collapse into one `send()`.

Publishing only the first would credit io_uring for batching that epoll can do
perfectly well. A weak control group is not a control group.

Numbers below are the **third** measurement, and the first one taken with a
fleet rather than a single client process. The single-process ladder recorded
earlier was wrong above rate 5, for two compounding reasons documented under
"One process cannot verify itself" — treat any number here that predates the
fleet as withdrawn.

Method: `python3 fleet.py --nodes 3 --conns 3334 --rate <r> --duration 20`
against a fresh server per point.

### immediate — one send() per delivery

| rate | delivered/s | server CPU | user/kernel | p50 | p99 | p99.9 |
|---:|---:|---:|---:|---:|---:|---:|
| 5 | 500k | 79% | 7 / 92 | 0.022 ms | 0.454 ms | **12.328 ms** |
| 7 | 700k | **100%** | 7 / 92 | **67.8 ms** | 187.4 ms | 210.4 ms |
| 14 | 1.4M | 100% | 7 / 92 | **460.1 ms** | >1000 ms | >1000 ms |

### batch — one send() per connection per epoll batch

Three processes, ~10k connections total, so the client is not in the way.

| rate | delivered/s | p50 | p99 | self-lag p99 | one process reported |
|---:|---:|---:|---:|---:|---:|
| 5 | 500k | 0.023 ms | 0.237 ms | 0.001 ms | 0.022 ms — agrees |
| 8 | 800k | 0.056 ms | 0.455 ms | 0.003 ms | 0.025 ms — **2x low** |
| 14 | 1.4M | 0.073 ms | 0.770 ms | 0.008 ms | 0.035 ms — **2x low** |
| 20 | 2.0M | 0.102 ms | 1.480 ms | 0.021 ms | 0.056 ms — **2x low** |
| 30 | 3.0M | 18.533 ms | 38.701 ms | 0.024 ms | 0.136 ms — **136x low** |

**This table's knee at 2M deliveries/s is not the server's ceiling.** It was
read as one when this section was written; the two later notes withdrew that.
[`2026-08-30`](2026-08-30-what-limits-the-server.md) § 2 pushed the same
server to 10M deliveries/s at 100% CPU with every collapse on the client's
side, and [`2026-09-01`](2026-09-01-where-the-epoll-server-saturates.md)
reached the real ceiling by making the server byte-bound: ≈1.78 GB/s. What
the rows above do show is the sweep-period latency and the instrument defect
the fleet corrected. The paragraph is kept as written, per the rule that a
recorded number is corrected beside, not over.

### One process cannot verify itself

This is the finding worth keeping, above any particular number.

A saturated load generator does not report that it is saturated. It reports
plausible server numbers that happen to be wrong, and the self-lag guard —
built exactly to catch this — did not fire, because it watches sends and the
damage was on the receive side.

Two separate mechanisms, found by carrying identical load with one process and
with three and asking why they disagreed:

**Receive-side coordinated omission.** `recv_ts` was stamped once per
`epoll_wait` batch and shared by every frame read in that batch. The frames at
the end of a long ready list are the late ones, and dating them from when the
walk began deletes precisely the delay that saturation caused. More
connections per process means longer batches means more deleted. At 3M/s the
batch stamp reported p50 0.109 ms from one process and 18.868 ms from three
carrying the same connections and the same load — with the server at 100% CPU
and the same user/kernel split in both, so the server was doing identical work.
Fixed: stamped per socket.

**The client is inside the system under test.** Even correctly stamped, a
client that cannot read fast enough closes its receive windows, and TCP
backpressure then stops the server from building the queue it would build
against a client that keeps up. Nothing is mis-measured; the server is simply
not being asked the question you thought you asked. This one has no fix in
code — it is what `fleet.py`, `--src-ips` and more machines are for.

**So: a single-process number at high rate is unverified, not wrong-by-default
but unverified.** Confirm it with a fleet run at the same connection count
before quoting it. The two agreed exactly at rate 5 and diverged by 2x from
rate 8 onward.
### Where the time goes

At the fair baseline the server spends **86–93% of its CPU in kernel time**,
and the number *falls as load rises* — 93% at rate 5, 90% at rate 14, 86% at
rate 30 — because coalescing removes syscalls while the memcpy work stays.
Application logic (framing, room lookup, string assembly) is the remaining
7–14%.

Read the direction, not just the value: the kernel share is highest where the
server is least busy. Quoting a single figure hides that, which is how the
first session ended up with "92%" as if it were a constant.

When this was written the kernel share read as "the io_uring argument
measured rather than assumed". [`2026-08-30`](2026-08-30-what-limits-the-server.md)
§ 2 then found the share *falling* from 86% to 77% while CPU held 100% from
3M to 10M deliveries/s, so it is not a saturation signal and not on its own
an argument. The argument that survives is the per-tick cost model in
[`design-notes/2026-09-02-where-io-uring-becomes-meaningful.md`](../design-notes/2026-09-02-where-io-uring-becomes-meaningful.md),
in which the syscall term is one of two terms and its weight depends on
connections per thread.

**Re-run this split first against the io_uring server, and against `batch`,
never against `immediate`.** If the split does not move, the port did not do
what it was for.

## Caveats

- Loopback only. No NIC, no driver path, and client and server share the CPU.
- The histogram tops out at 1 s. Rows showing `>1000 ms` have samples
  excluded, so past saturation the percentiles stop being comparable between
  rows.
- `batch` flushes at the end of an epoll batch — microseconds — not on a
  fixed-rate tick. A 30 Hz tick would add up to 33 ms and is a different
  experiment.
- Steady-state connections, not churn. Repeated connect/disconnect is a
  different and harder workload this does not touch.
- Fan-out is rooms of 10 in the tables above. Varying it 10 → 100 → 500 at a
  fixed 500k deliveries/s moved the user/kernel split only from 7/93 to 4/96,
  so the split is not an artifact of small rooms. It *is* an artifact of chat:
  per-recipient work here is a memcpy, where a game server would add AoI
  filtering and per-recipient serialization. Re-measure when gameplay packets
  land.
- `--size-mix` exists but every table above is fixed 64-byte filler.
