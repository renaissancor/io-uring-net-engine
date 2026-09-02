# io-uring-net-engine

A Linux-native C++20 network engine for realtime servers, built on `io_uring` —
and the measurement apparatus that has to exist before any claim about it means
anything.

What this repository is for, in the author's words: **to show optimisation
work, loop structure worth reading, and a network engine a game server could
use.** Everything here serves one of those three.

The measurement question is the instrument for the first of them:

> For an MMORPG-shaped server, **at what point does `io_uring` become
> meaningful compared to `epoll`?** Not "is it faster" — where, in connections
> per thread and logic cost per tick, does the I/O layer's share of the tick
> budget start to matter, and how much of it does `io_uring` give back?

The prediction, written before the io_uring data path exists so it cannot be
fitted to the result, is
[`design-notes/2026-09-02-where-io-uring-becomes-meaningful.md`](design-notes/2026-09-02-where-io-uring-becomes-meaningful.md).
Answering it honestly needs three things, not one: the engine, a control group
worth beating, and an instrument that knows when it is measuring itself. All
three are here, which is the reason this is one repository.

## Layout

| | | |
|---|---|---|
| [`engine-uring/`](engine-uring/) | **the engine** | C++20 primitive layer the runtime is built from — `sds::` containers (the STL is banned), `lnx::` atomics and mutex, a per-thread packet pool over `mmap`, a scope profiler, the `LNX_CHECK` + `expected` error model. Thirteen units, each with a doc that describes it. |
| [`server-epoll/`](server-epoll/) | **the control group** | Single-threaded, level-triggered `epoll` chat server. One file, STL, no abstractions — deliberately. It exists to be beaten fairly. |
| [`client-bench/`](client-bench/) | **the instrument** | Load generator (C++) plus a fleet runner and a correctness judge (Python). Measures connection scale and delivery latency, and **refuses to report a number when the run measured the client instead of the server.** |
| [`server-uring/`](server-uring/) | **the product** | The server on top of the engine — supervisor/acceptor/worker runtime, SPSC thread mesh, session authority — consuming it through `find_package(iouring_net)` against an install prefix, never a relative include. The per-worker `io_uring` data path is the current work; room chat is its first workload. |
| [`result-notes/`](result-notes/) | **what was measured** | The epoll baseline, and findings that outlive any particular number. |
| [`design-notes/`](design-notes/) | **why it is shaped this way** | Dated decision logs and cross-cutting design records — rationale, alternatives, and the ideas that were rejected. |

Code documentation lives with its component: one `doc/<unit>.md` per source
unit, describing the code that exists. Deliberation, including designs that
were never built, is dated and lives in `design-notes/`; measurements in
`result-notes/`, regardless of which component they touch.

## Where the line currently is

Single-threaded `epoll`, 10k connections, rooms of 10, loopback, measured with
a 3-process fleet:

| | |
|---|---|
| **Ceiling** | **byte-bound at ≈1.78 GB/s** — 1.70M deliveries/s at 1024 B, 3.50M at 512 B. At 64 B, **observed on 2026-09-02**: lossless at 10M deliveries/s, shedding at 12M (the fitted 13.4M was high). The same server rebuilt on the engine's `sds::` primitives (`server-sds`) is lossless at 12M / 2.20M at 1024 B — +20 % and +29 %, [`result-notes/2026-09-02`](result-notes/2026-09-02-stl-to-sds-the-measured-delta.md) |
| Latency below the ceiling | set by the **sweep period**, i.e. connection count, not by load — sub-millisecond p50 at 10k connections up to the client's limit |
| CPU | 100% of one core from 3M to 10M deliveries/s — **100% is not a saturation signal** here |
| Connection scale | 40k established, 0 attrition — but only across 4 source IPs |

An earlier version of this table stated a 2M deliveries/s ceiling and read the
86–93% kernel share as "the io_uring argument measured". Both were withdrawn:
the 2M knee was the single-process client measuring itself, and kernel share
fell as load rose while CPU held 100%. What the epoll data actually supports
is a per-tick cost model, and the io_uring argument is stated against that
model in the hypothesis note above. Full tables, method, and caveats in
[`result-notes/`](result-notes/).

**How the ceiling was reached:** by raising the payload until the server
became byte-bound and its ceiling fell into the load generator's reach, with
the server's connection-close log agreeing with the client's count. Past it
the server does not degrade gracefully — it closes clients. Two
of the three thresholds that matter are far below the peak, and the whole
ladder read 98–101% CPU. See
[`result-notes/2026-09-01-where-the-epoll-server-saturates.md`](result-notes/2026-09-01-where-the-epoll-server-saturates.md).

**Two numbers on this page are corrections of earlier ones.** A single client
process reported latency up to 136× too low near the knee, and 100% CPU turned
out not to mean saturation — the same server held 100% of a core from 3M to 10M
deliveries/s. Both are recorded in
[`result-notes/2026-08-30-what-limits-the-server.md`](result-notes/2026-08-30-what-limits-the-server.md)
rather than quietly fixed, because the traps outlive the numbers.

## What makes a number count

The instrument voids its own run and exits **3** when the measurement does not
describe the server:

- **fan-out drift** past ±5% of the requested room size — the offered load is
  then not the requested load, so asking whether the client kept up answers
  nothing. Orphaned load-generator processes are the usual cause, and they read
  as *higher* throughput.
- **connection loss** past 0.5% of `--conns` — a connection that dies mid-run
  stops offering load, so the number is an average over a load nobody chose.
  Usually the server shedding clients, which fan-out cannot see: a closed
  connection leaves the numerator and denominator of `frames_in/sent` at the
  same time.
- **self-lag** too large against latency p99 — the client's own scheduling
  delay is contaminating the histogram.

A verdict printed in prose that nothing can act on is not a gate, so it leaves
through the exit status too. `[WARN]` and `[ OK ]` are both 0: a run with no
headroom is still a run.

## Reading order

1. **This file** — the claim.
2. [`result-notes/`](result-notes/) — the baseline the engine must beat, how it was taken, and where the control group's own ceiling is.
3. [`client-bench/doc/INDEX.md`](client-bench/doc/INDEX.md) — the instrument's code map: what each unit owns and what it gets wrong.
4. [`engine-uring/doc/INDEX.md`](engine-uring/doc/INDEX.md) — the engine's code map, then [`10-realtime-server-architecture.md`](server-uring/doc/10-realtime-server-architecture.md) for the runtime shape and [`server-uring/doc/INDEX.md`](server-uring/doc/INDEX.md) for its units.
5. [`design-notes/2026-09-02-design-notes-drift-review.md`](design-notes/2026-09-02-design-notes-drift-review.md) — how the documents relate, and which earlier statements are superseded.

## Status

| component | state |
|---|---|
| `server-epoll` | complete; baseline measured three times, earlier numbers withdrawn; a second build, `server-sds`, on the engine's primitives through the `find_package` seam |
| `client-bench` | complete; fleet mode, correctness judge, verdict gating; 2026-09-02: half the `recv()` calls, carries 10M deliveries/s per 12 vCPUs |
| `engine-uring` | in progress — primitives and transport land; 96 tests green |
| `server-uring` | in progress — runtime spine and thread mesh moved in, 17 tests green; data path is the current work |

The baseline table gets rewritten the day the io_uring server is measured. The
traps recorded alongside it will still be true.

## Build

Each component builds independently.

```bash
cd client-bench && make            # loadgen
cd server-epoll && make            # server  (make asan for the checked build)
cd engine-uring && make test       # cmake presets; 96 tests
```

`server-uring` builds against an *installed* engine — see its README for the
two-step prefix round trip, which CI (`.github/workflows/ci.yml`) exercises on
every push. `engine-uring` needs `liburing >= 2.5`, `fmt`, and Catch2. The default preset is
ASan+UBSan — never measure with it, it roughly halves throughput. In every
component `make` builds the binary you *measure* with and a separate target
builds the sanitised one; that convention was the other way round once, and it
silently cost a set of numbers.
