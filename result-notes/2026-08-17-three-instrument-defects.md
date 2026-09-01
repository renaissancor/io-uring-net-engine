# 2026-08-17 — Three defects in the instrument, and how each was caught

> **Why this file exists.** The numbers in `README.md` have a short life: the
> whole point of the baseline is that the io_uring server will replace it and
> the table gets rewritten. The *traps* do not get rewritten. Every one of them
> will exist verbatim in the next load generator, including the one that
> measures io_uring, so the finding outlives the finding's occasion.
>
> All three shared one property: **the instrument kept reporting plausible
> numbers while it was wrong.** None produced an error, a crash, or an outlier.
> That is the class of bug this file is about.

## Summary

| # | Defect | Caught by | Fixed? |
|---|---|---|---|
| 1 | `broadcast()` sent inline per recipient — a strawman baseline | Reading the server, asking if it was a fair opponent | Yes, `CHAT_FLUSH=batch` |
| 2 | `[VOID]` verdict was a bare ratio with no absolute floor | Three back-to-back identical runs disagreeing | Yes, three-level verdict |
| 3 | `recv_ts` stamped once per epoll batch — receive-side coordinated omission | 1 process vs 3 processes disagreeing by 136x | Yes, stamped per socket |
| 3b | The client sits inside the flow-control loop it measures | Same probe as 3 | **No fix in code — this is what fleet mode is for** |

---

## 1. The baseline was a strawman

**Symptom.** epoll knee at ~700k deliveries/s, 92% kernel time. Clean-looking
numbers, reproducible, no warnings.

**Cause.** `broadcast()` called `queue_send()` then `flush_send()` *inline*,
once per recipient, while walking the room's member set. One syscall per
delivery.

**Why it mattered.** io_uring's headline advantage is batched submission.
Measuring it against a server that makes one syscall per delivery credits
io_uring with batching that plain epoll can do for itself. The comparison would
have shown a large win and attributed it to the wrong thing.

**Found by** asking whether the baseline was a fair opponent — not by any
measurement. Nothing in the output suggested a problem, because the server was
doing exactly what it was written to do.

**Result after fixing.** At the same offered load and the same ~98% of one
core: p50 85.9 ms inline, 0.021 ms batched. Later measurement sharpened this
further — see below, the cost is not throughput at all.

**Lesson recorded as** `server-epoll` lesson 8.

---

## 2. The `[VOID]` verdict flipped on noise

**Symptom.** Three consecutive runs at the same rate, same connection count,
same server:

| run | self-lag p99 | latency p99 | verdict |
|---|---:|---:|---|
| 1 | 0.133 ms | 0.726 ms | `[ OK ]` |
| 2 | 0.143 ms | 0.658 ms | `[VOID]` |
| 3 | 0.131 ms | 0.604 ms | `[VOID]` |

Those are the same measurement three times.

**Cause.** The guard was `lag99 * 5 > lat99` — a pure ratio, no absolute floor.
Near the ceiling the server's p99 and the client's own jitter converge to the
same scale, so the ratio oscillates across the threshold while the actual
contamination stays at a tenth of a millisecond.

**The reasoning error.** A ratio answers "what fraction of the reported number
could be client artifact." That is not the question a verdict should answer.
Self-lag inflates measured latency roughly *additively*, so the question is
**whether subtracting it would change the conclusion**. At 0.143 ms of self-lag
on a 0.658 ms latency, the corrected floor is 0.515 ms — still sub-millisecond,
still the same conclusion, so the run is usable and discarding it loses
information for nothing.

**Fix.** Three levels, with a 1 ms absolute floor:

- `[ OK ]` — ratio passes.
- `[WARN]` — ratio fails but self-lag is under 1 ms. Report the corrected lower
  bound; the run is usable, the headroom is not.
- `[VOID]` — ratio fails and self-lag is at or above 1 ms. Here the correction
  *is* load-bearing: the genuine case measured 4.640 ms of self-lag against
  10.478 ms of latency, where correcting halves the number.

**Generalisable form.** A threshold on a ratio of two quantities that both
shrink together will oscillate wherever they converge. If the two are related
additively, the threshold belongs on the difference, not the ratio — or the
ratio needs an absolute floor beneath which it stops being asked.

---

## 3. Receive-side coordinated omission

This is the serious one.

**Symptom.** None. The instrument reported a clean `[ OK ]` at 3.0M
deliveries/s with p50 0.109 ms, and every self-check passed.

**Found by** carrying identical load with one process and with three, and
asking why they disagreed:

| | server CPU | user/kernel | deliveries/s | p50 | self-lag p99 |
|---|---:|---:|---:|---:|---:|
| 1 process x 10000 conns | 100% | 13/86 | 3.000M | **0.109 ms** | 0.836 ms |
| 3 processes x 3334 conns | 100% | 13/86 | 3.000M | **18.868 ms** | 0.024 ms |
| 5 processes x 2000 conns | 100% | 15/84 | 3.002M | 22.182 ms | 0.015 ms |

Same server, same connection count, same delivered rate, same CPU, same
user/kernel split. The server was doing identical work. The single process
reported a latency 170x lower — **and reported 35x more self-lag while doing
it**, which is why the self-lag guard stayed quiet: the guard compares the two,
and both moved in the direction that keeps the ratio small.

**Cause.** `recv_ts` was sampled once, immediately after `epoll_wait` returned,
and shared by every frame consumed anywhere in that batch:

```cpp
const int64_t recv_ts = now_ns();          // once
for (int i = 0; i < ready; ++i) {          // up to 4096 sockets
    ...
    consume_frames(c, recv_ts, ...);       // the last one gets the first one's time
}
```

The frames at the end of a long ready list are precisely the late frames.
Dating them from when the walk *began* deletes exactly the interval that
saturation caused. The error scales with how long the walk takes, so more
connections per process means more deletion — which is why splitting the same
connections across three processes made it visible.

**This is coordinated omission**, the same failure the tool was built to avoid
on the send side, wearing the other hat. The send side was handled from day one
(the payload carries the *intended* send time, not the actual one). The receive
side was not, and the self-lag histogram cannot see it by construction —
it watches sends.

**Fix.** Stamp per socket, immediately after the read:

```cpp
consume_frames(c, now_ns(), ...);
```

Costs one `clock_gettime` per readable socket rather than per batch. Via the
vDSO that is ~20 ns, and it is measurable at 10000 fds in one process — self-lag
p99 went 0.836 ms to 2.157 ms in that configuration, which is honest overhead
appearing rather than a regression: that configuration now correctly `[VOID]`s
itself.

### 3b. The client is inside the system under test

Fixing the stamp did not make one process agree with three. It narrowed the gap
and moved the disagreement down the ladder, but a real difference remains:

| rate | deliveries/s | 3 processes | 1 process | |
|---:|---:|---:|---:|---|
| 5 | 500k | 0.023 ms | 0.022 ms | agrees |
| 8 | 800k | 0.056 ms | 0.025 ms | 2x low |
| 14 | 1.4M | 0.073 ms | 0.035 ms | 2x low |
| 20 | 2.0M | 0.102 ms | 0.056 ms | 2x low |
| 30 | 3.0M | 18.533 ms | 0.136 ms | 136x low |

This part is **not a measurement bug and has no fix in code.** A client that
cannot read fast enough closes its TCP receive windows, and backpressure then
prevents the server from building the queue it would build against a client
that keeps up. Nothing is mis-measured. The server is simply not being asked
the question you believed you were asking.

The only remedy is more client capacity: more processes, then more machines.
That is what `fleet.py`, `--src-ips` and the node-namespacing exist for.

**Operating rule, now in the README and the commit trailer:** a single-process
number above ~500k deliveries/s is *unverified*. Confirm it against a fleet run
at the same connection count before quoting it.

---

## What generalises

**A self-check only covers the half it was built for.** The self-lag histogram
is a good idea and it worked — for sends. Its existence made the receive side
*less* likely to be examined, because the instrument appeared to have a
saturation guard. A guard that covers one direction reads, from outside, like a
guard that covers the thing.

**Two implementations of the same measurement are worth more than one careful
implementation.** Every one of these three was caught by disagreement, not by
inspection: a fair baseline vs a strawman, a run vs its own repeat, one process
vs three. None was found by reading the code and reasoning about it, and #3
survived a full session of exactly that.

**Reproducibility is not correctness.** The single-process ladder reproduced to
the third decimal across two sessions on different server processes. It was
wrong by two orders of magnitude at the top. Stability measures whether the
same wrong thing happens twice.

**The cheapest control group is the one that already exists.** Running the same
load as N processes instead of one costs a shell loop. It is the only reason
any of #3 is known, and it should be the default posture for any number that
matters, not a special investigation.

---

## Rationale links

- `README.md` § "One process cannot verify itself" — the operating rule.
- `README.md` § "What it measures" — the three-level verdict table.
- `../server-epoll/README.md` lesson 8 — defect 1 from the server's side.
- Commits `2dfe667` (fleet mode + defect 3), `674cda5` (defect 2),
  `5f5954d` in `server-epoll` (defect 1).
