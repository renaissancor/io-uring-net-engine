---
status: proposed
amended_by:
  - 2026-09-03-working-set-knob-for-the-tick-budget-experiment.md
  - 2026-09-03-stage-c-tick-coalesced-delivery.md
  - 2026-09-03-stage-b-the-io-uring-data-path.md
note: a hypothesis; becomes accepted or moved when the tick-budget experiment in section 7 has run. 2026-09-03 adds a working-set knob to § 7 and stages the epoll half first
---
# 2026-09-02 — Where io_uring becomes meaningful: the hypothesis, written before the data path exists

The question this repository exists to answer, stated plainly for the first
time: **for an MMORPG-shaped server, at what point does io_uring become
meaningful compared to epoll?** Not "is io_uring faster" — the chat bench has
already shown that the epoll server's ceiling is a delivery-rate ceiling that
epoll reaches perfectly well ([`2026-09-01`](../result-notes/2026-09-01-where-the-epoll-server-saturates.md)).
The product question is about a server whose thread spends most of its time
on game logic, and it asks how much of that thread's budget the I/O layer
takes, and whether io_uring gives enough of it back to matter.

Where this sits: the repository's purpose, as the author stated it the same
day (recorded in
[`2026-09-02-design-notes-drift-review.md`](2026-09-02-design-notes-drift-review.md)
§ 0), is to show optimisation work, loop structure worth reading, and an
engine a game server could use. The epoll-versus-io_uring comparison is the
instrument for the first of those, not a purpose of its own, and this note
is the hypothesis that instrument tests.

This note writes the answer down as a prediction, with the formula and the
numbers, *before* the io_uring data path is built. `server-uring/src` today
contains one `io_uring_peek_cqe` and no recv or send submission. That is the
right moment. The 100% CPU misreading recorded in
[`2026-08-30`](../result-notes/2026-08-30-what-limits-the-server.md) § 2
happened because a claim was asserted from a gauge and explained afterwards.
A hypothesis written first can only be confirmed or moved; it cannot be
fitted to whatever comes out.

## 1. What the chat bench measures, and what it does not

`server-epoll` and `client-bench` together measure one thing well: the cost of
the I/O layer with the content cost held at approximately zero. Every inbound
frame costs a string concatenation and a room walk, and the fitted content
term `c_msg` in the sweep model was not separable from the delivery term. That
is why the server held 100% of one core from 3M to 10M deliveries/s and why
its ceiling is invariant in connection count.

A game server is the opposite shape. [`2026-05-19-server-architecture.md`](2026-05-19-server-architecture.md)
§ Part 7 budgets **10–100 µs of content cost per player per tick**. Against the
constants fitted in the saturation note — **~46 ns per delivery, ~0.4 ns per
byte, 0.75–1.65 µs per syscall** — content outweighs the network work by one
to two orders of magnitude the moment real logic exists. The chat bench has
therefore measured the term that will matter *least* in the product. It has
not measured the wrong thing; it has isolated one term so that the term can
now be placed inside a budget. This note is the placing.

## 2. The claim

> Per tick, a worker thread pays an I/O term and a logic term. The logic term
> is identical under epoll and io_uring by construction. The I/O term differs
> only in its per-active-connection constant. io_uring is meaningful exactly
> where that constant, multiplied by active connections per thread, is a
> material fraction of the tick period after logic is subtracted — and
> nowhere else.

## 3. The cost model

Take one thread that owns `N` connections, of which `N_a` have traffic in a
given tick, and a tick period `T` (33 ms at 30 Hz).

```
T_io(epoll)     ≈ 2 · c_sys · N_a            recv + send per active connection
T_io(io_uring)  ≈ c_submit + c_cqe · N_a     one submit per batch, one completion per op
T_logic         = N · c_player               identical for both

budget returned by io_uring per tick  ≈ (2·c_sys − c_cqe) · N_a
```

Notes on the terms:

- `c_sys` is the epoll server's fitted per-syscall cost, 0.75–1.65 µs on WSL2.
  Batching frames into fewer `send()` calls (the `CHAT_FLUSH=batch` control)
  does not remove the per-connection call; it only stops there being more
  than one per connection per sweep.
- `c_cqe` is the per-completion cost under io_uring, expected in the low
  hundreds of nanoseconds, and lower again with multishot recv, provided
  buffers, and registered files. None of those are in use yet, so `c_cqe` is
  a range, not a number.
- The **idle** connection costs nothing per tick under either mechanism.
  That is the step epoll already took over `poll()`; see § 8.
- `N_a` is bounded by `N`. In a chat or presence tier a few percent of `N`
  are active per tick; in a simulation tier every player sends input most
  ticks, so `N_a ≈ N`.

## 4. Predicted crossover

Using 1 µs per syscall, 100 ns per completion, and a 33 ms tick:

| active connections per thread | `T_io` epoll | `T_io` io_uring | budget returned | as share of tick |
|---:|---:|---:|---:|---:|
| 300 | 0.6 ms | 0.06 ms | 0.54 ms | under 2 % |
| 1,000 | 2 ms | 0.2 ms | 1.8 ms | about 5 % |
| 3,000 | 6 ms | 0.6 ms | 5.4 ms | about 16 % |
| 10,000 | 20 ms | 2 ms | 18 ms | about 55 % |

Set against the logic term:

| players per thread | `T_logic` at 10 µs | `T_logic` at 100 µs |
|---:|---:|---:|
| 300 | 3 ms | 30 ms |
| 3,000 | 30 ms | 300 ms |

**At 300 players per thread**, which is a normal MMORPG zone, io_uring returns
half a millisecond of a 33 ms tick while logic takes 3–30 ms. The I/O layer is
invisible. **At 3,000 connections per thread**, io_uring returns a sixth of the
tick, which is the difference between fitting and overrunning once logic is
non-trivial — but at 3,000 players the logic term alone already overruns at
any content cost above ~10 µs, so the thread could not be a simulation thread.
**At 10,000 per thread** epoll cannot hold the tick at all and io_uring can,
and a thread with 10,000 connections is a gateway or relay, not a zone.

## 5. The predicted conclusion

io_uring becomes meaningful where **one thread owns thousands of connections
with light per-connection logic**, and stays irrelevant where **one thread
owns hundreds of connections with heavy logic**. In MMORPG architecture those
are two different tiers:

| tier | connections per thread | logic per connection | predicted verdict |
|---|---|---|---|
| zone / channel simulation | hundreds | heavy, 10–100 µs per tick | epoll and io_uring within noise |
| gateway, lobby, chat, presence, cross-zone relay | thousands to tens of thousands | light, near the chat server's shape | io_uring materially reclaims tick budget |

So the expected shape of the portfolio claim is: **io_uring matters at the
front end and the relay, not in the simulation.** The experiment in § 7 exists
to put a number on the boundary — connections per thread, at each logic cost —
rather than to decide whether the boundary exists.

This is a stronger claim than "io_uring is faster", and more useful to a
reader deciding where to spend engineering effort. It is also what the
architecture in [`../server-uring/doc/10-realtime-server-architecture.md`](../server-uring/doc/10-realtime-server-architecture.md)
already implies: the acceptor-as-lobby and the per-worker ring are the
front-end shape; the room/world tick placeholder is the simulation shape.

## 6. What would move the boundary

Three things could falsify or shift the prediction, and the experiment must
be able to see each of them:

1. **Tail, not mean.** Even where the mean I/O term is small, epoll's
   per-connection wakeup and `epoll_ctl` re-arming pattern may produce tick
   overruns at p99.9 that io_uring's batched completion does not. A mean or
   even a p90 would hide this. The instrument has to be a per-tick histogram
   with an explicit overrun count.
2. **The completion constant.** Multishot recv and provided buffers change
   `c_cqe` substantially. If the io_uring data path lands with plain one-shot
   recv per connection first, the first measurement will understate the
   difference, and that has to be said next to the number.
3. **Environment.** Every constant here was fitted on WSL2 with a synthetic
   CPU topology ([`2026-08-30`](../result-notes/2026-08-30-what-limits-the-server.md)
   § WSL2). The *structure* of the prediction — linear in `N_a`, invariant in
   idle `N`, two-tier verdict — does not depend on the environment. The
   crossover *numbers* do, and must be re-fitted on bare metal before being
   quoted as anything more than a shape.

## 7. The experiment: a tick-based load simulation inside both servers

The chat bench cannot produce the number this note needs, because it has no
tick and its latency is intended-send to echo-receive. The change is to give
both servers a tick and measure the tick.

### 7.1 Loop shape

The wait becomes bounded by the next tick deadline instead of blocking
indefinitely; at the deadline a tick function runs and the deadline advances.
Everything else — accept, recv, parse, handler, flush — is unchanged.

```
while running:
    wait for readiness/completions, timeout = next_tick_deadline − now
    for each ready/completed recv:
        parse frames; run the handler immediately   (records intent only)
    if now ≥ next_tick_deadline:
        t0 = now
        run tick: integrate entities, interest management, build snapshots
        t1 = now
        flush snapshot sends
        t2 = now
        record (t0−tick_start_of_io, t1−t0, t2−t1); count overrun if t2 > deadline + period
        next_tick_deadline += period
```

For `server-epoll` this is a timeout argument and one function call. For
`server-uring` it is a timeout SQE or a timerfd on the ring, and it fills the
"run room/world tick placeholder" line that
[`10-realtime-server-architecture.md`](../server-uring/doc/10-realtime-server-architecture.md)
§ 8 already reserves.

### 7.2 What the synthetic tick does

The point is to make cost *scale the way game logic scales*, not to be a
game. Three components cover the shape, and they touch memory, which a spin
loop does not — real logic evicts the socket buffers and session table from
cache, and the I/O term after logic is dearer for it.

- **Per-session entity state.** Position, velocity, a few counters. Every
  tick integrates movement for every session: O(`N`) per tick.
- **Per-room interest management.** Pairwise distance per room: O(`F²`) per
  room, the shape of area-of-interest filtering. At `F = 10` it is 100
  checks per room per tick; it grows the way it would in a game if
  `--per-room` is raised.
- **Per-tick state broadcast.** Every session receives a snapshot of its
  room's entity positions every tick. This is the traffic pattern that
  actually dominates game servers — periodic outbound state, not echoes of
  input. At 10k connections, 30 Hz, `F = 10` it is 3M deliveries/s, inside
  the envelope the epoll server already handled, so the bench's existing
  range covers it.

On top of the shaped work, **two calibrated burn knobs**, so cost can be
dialled rather than only fitted:

- `LOGIC_NS_PER_MSG` — spun on the handler path per inbound frame. Models the
  `c_msg` slot the sweep model could not separate.
- `LOGIC_NS_PER_ENTITY_TICK` — spun per entity per tick. Models `c_player`,
  the 10–100 µs budget from Part 7.

Spin, never sleep: sleeping yields the core and the kernel does I/O work in
the gap, the opposite of what heavy logic does to a tick. Calibrate once at
startup to iterations per nanosecond and burn by count; a clock check inside
the loop is a vDSO call per check and skews small values. Keep the result in a
volatile sink so the loop survives optimisation.

**The tick function, the entity layout, and the burn routine must be
byte-identical between the two servers**, shared as one header or one
translation unit. A few percent of drift in the logic term and the comparison
is void.

### 7.3 Inputs become intents

Chat frames become move commands. The handler validates and records the
intent on the session's entity; the tick applies it. Handler cost stays tiny
and independent of what the simulation does, which is the discipline
[`2026-05-19-server-architecture.md`](2026-05-19-server-architecture.md)
already argues for on determinism grounds.

### 7.4 What is measured

**Primary instrument: the server's own per-tick phase histogram.** Time in
I/O drain, time in the tick function, time in flush, and an overrun count.
This answers "how much of the tick did the I/O layer take, at logic cost L,
with N connections" directly, and it is the only instrument that can compare
epoll against io_uring with the logic term held exactly equal. The client
cannot see this and the chat bench structurally cannot produce it.

**Secondary: client-observed latency, redefined.** Intended send to the first
snapshot that reflects the input. Each entity's snapshot entry carries the
intended timestamp of the last input applied to it; the client samples once
per new timestamp on its own entity. This latency includes the wait for the
next tick, so its p50 sits near half a period and **is not comparable with
any number in `result-notes/` today.** It is the number a player experiences,
and it is what turns "tick budget" into a latency claim a reader can feel.

**Sweep:** `N ∈ {300, 1000, 3000, 10000}` per thread × `L_entity ∈ {0, 10, 30,
100} µs` × both servers, at a fixed offered input rate below the ceiling. The
result is the surface on which the gap between epoll and io_uring closes; § 4
predicts where.

## 8. A `poll()` baseline, and what it is for

A `select`/`poll` study build was proposed on the reasoning that epoll's
red-black tree and ready list avoid the O(`N`) descriptor scan. The reasoning
is half right and the baseline is worth having, with a correction and a
constraint.

**The correction.** The tree is not what makes epoll fast — it makes
registration O(log N), and registration is rare. What matters is that epoll
is a persistent kernel object: each socket has a wait-queue callback
installed once that pushes it onto the ready list, so `epoll_wait` returns
O(ready) and never touches idle sockets. `poll()` has no persistent state.
Every call copies the whole `pollfd` array in, installs a wait-queue entry on
every one of `N` sockets, sleeps, tears all `N` down, copies the array out,
and userspace scans `N` `revents`. That tax is paid per call whether or not
anything happened. `select` is the same with a bitmask and a hard 1,024
descriptor ceiling, so it cannot join the 10k rows at all.

**When the tax matters.** Poll's penalty is worst when connections are mostly
*idle*: it pays for all `N` and serves few. In the backlogged regime of the
saturation note, where every connection has data each sweep, the sweep
touches every socket anyway and poll's disadvantage shrinks to the copy and
wait-queue overhead. So a poll baseline does not mainly show "slow under
load". It shows **the idle-connection tax per tick**, which is exactly the
presence / chat / gateway shape: tens of thousands connected, a few percent
active.

That gives the table this note actually wants, cost per connection per tick:

| | idle connection | active connection |
|---|---|---|
| `poll` | tens of ns — copy + wait-queue setup + scan | 2 syscalls + its share of the scan |
| `epoll` | zero | 2 syscalls, + `epoll_ctl` when arming `EPOLLOUT` |
| `io_uring` | zero (multishot recv) | ~1 completion, hundreds of ns |

epoll removed the idle tax; io_uring removes most of the active tax. Two
different steps on two different terms, and the poll row makes the first step
visible so the second is not confused with it.

**One thing poll may win.** At a few hundred connections the interest set
travels for free in the array, so poll never pays the `epoll_ctl` calls the
epoll server needs to arm and disarm `EPOLLOUT` (its LESSON 3). At
zone-thread scale all three mechanisms are likely within noise, and that
result supports § 5 more than another epoll number would.

**The constraint.** A poll-versus-io_uring gap at 10k connections would be
enormous and completely unsurprising — the C10K problem was documented in
1999. If that number appears anywhere near a headline it credits io_uring for
what epoll fixed twenty years ago, and
[`result-notes/README.md`](../result-notes/README.md) already records the
principle: a weak control group is not a control group. **The control for
io_uring remains `server-epoll` in `CHAT_FLUSH=batch` mode.** Poll is context,
one row above it on the ladder; select is a footnote for the sub-1,000 rows.

## 9. Rejected

- **Build real game logic first, then measure.** The synthetic tick is a cost
  model, not a game. It reproduces scaling and memory footprint, not branchy
  AI or pathfinding. That is sufficient for the crossover question and it
  costs a fraction of the effort; real content belongs to the product layer
  and would make the logic term unrepeatable between runs.
- **Compare io_uring against `CHAT_FLUSH=immediate`.** Its tail is ten times
  worse than `batch` at the same load. Rejected in the result notes already;
  restated here because a tick-based sweep is a new place to make the same
  mistake.
- **Use client-side latency as the primary comparison.** It includes half a
  tick of wait by construction and cannot separate the I/O term from the
  logic term. It is the secondary, felt-latency number, not the instrument.
- **A fixed-rate tick with a real world update as the first step.** It
  changes the latency floor by half a period and would swamp the I/O
  difference. The burn knobs get the crossover with a small diff; the shaped
  tick follows once the knobs have said where to look.

## Rationale links

- [`../result-notes/2026-08-30-what-limits-the-server.md`](../result-notes/2026-08-30-what-limits-the-server.md)
  — the sweep model and the fitted constants this note's arithmetic rests on;
  § 2 for why CPU% is not a signal; § WSL2 for why the numbers are shape only.
- [`../result-notes/2026-09-01-where-the-epoll-server-saturates.md`](../result-notes/2026-09-01-where-the-epoll-server-saturates.md)
  — the delivery-rate ceiling, which stands, and which answers a different
  question from this one.
- [`2026-05-19-server-architecture.md`](2026-05-19-server-architecture.md)
  § Part 7 — the 10–100 µs per-player budget and the per-channel capacity
  ceiling that put the logic term on the scale.
- [`../server-uring/doc/10-realtime-server-architecture.md`](../server-uring/doc/10-realtime-server-architecture.md)
  § 8 — the worker loop with the tick placeholder § 7.1 fills.
- [`../server-epoll/server.cpp`](../server-epoll/server.cpp) — LESSON 3
  (`EPOLLOUT` arming) and LESSON 8 (`batch`), the two epoll costs § 8 refers to.
