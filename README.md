# io-uring-net-engine

A Linux-native C++20 network engine for realtime servers, built on `io_uring` —
and the measurement apparatus that has to exist before any claim about it means
anything.

The question the whole repo answers:

> A single-threaded `epoll` server saturates one core somewhere. **Where?** And
> how far does an `io_uring` engine with per-worker rings and real threading
> move that line?

Answering it honestly needs three things, not one: the engine, a control group
worth beating, and an instrument that knows when it is measuring itself. All
three are here, which is the reason this is one repository.

## Layout

| | | |
|---|---|---|
| [`engine-uring/`](engine-uring/) | **the engine** | C++20 `io_uring` runtime — supervisor/acceptor/worker threads, per-worker rings, memory and object pools, lock-free structures, `sds::` containers (the STL is banned), profiler and leak tracker. The largest body of work here. |
| [`server-epoll/`](server-epoll/) | **the control group** | Single-threaded, level-triggered `epoll` chat server. One file, STL, no abstractions — deliberately. It exists to be beaten fairly. |
| [`client-bench/`](client-bench/) | **the instrument** | Load generator (C++) plus a fleet runner and a correctness judge (Python). Measures connection scale and delivery latency, and **refuses to report a number when the run measured the client instead of the server.** |
| [`server-uring/`](server-uring/) | **the product** | Reserved. The chat/game server on top of the engine, consuming it through `find_package(iouring_net)` against an install prefix — never a relative include. Not yet written. |
| [`result-notes/`](result-notes/) | **what was measured** | The epoll baseline, and findings that outlive any particular number. |
| [`design-notes/`](design-notes/) | **why it is shaped this way** | Dated decision logs and cross-cutting design records — rationale, alternatives, and the ideas that were rejected. |

Code documentation lives with its component (`engine-uring/doc/` is normative
and is meant to be read without opening the source). Decision logs and measured
findings sit in `*-notes/`, regardless of which component they touch.

## Where the line currently is

Single-threaded `epoll`, 10k connections, rooms of 10, loopback, measured with
a 3-process fleet:

| | |
|---|---|
| **Ceiling** | **2M deliveries/s** — sub-millisecond p50 below it, 18.5 ms at 3M |
| CPU at the ceiling | 100% of one core, **86–93% of it in the kernel** |
| Connection scale | 40k established, 0 attrition — but only across 4 source IPs |

That kernel share is the io_uring argument stated as a measurement rather than
an assumption: the cost being attacked is syscall transitions, and that is
where the budget actually sits. Full tables, method, and caveats in
[`result-notes/`](result-notes/).

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
- **self-lag** too large against latency p99 — the client's own scheduling
  delay is contaminating the histogram.

A verdict printed in prose that nothing can act on is not a gate, so it leaves
through the exit status too. `[WARN]` and `[ OK ]` are both 0: a run with no
headroom is still a run.

## Reading order

1. **This file** — the claim.
2. [`result-notes/`](result-notes/) — the baseline the engine must beat, and how it was taken.
3. [`client-bench/doc/INDEX.md`](client-bench/doc/INDEX.md) — the instrument's code map: what each unit owns and what it gets wrong.
4. [`engine-uring/doc/00-overview.md`](engine-uring/doc/00-overview.md) — the engine's layered design, then [`10-realtime-server-architecture.md`](engine-uring/doc/10-realtime-server-architecture.md) for the runtime shape.

## Status

| component | state |
|---|---|
| `server-epoll` | complete; baseline measured three times, earlier numbers withdrawn |
| `client-bench` | complete; fleet mode, correctness judge, verdict gating |
| `engine-uring` | in progress — primitives, runtime and thread mesh land; 113 tests green |
| `server-uring` | not started |

The baseline table gets rewritten the day the io_uring server is measured. The
traps recorded alongside it will still be true.

## Build

Each component builds independently.

```bash
cd client-bench && make            # loadgen
cd server-epoll && make            # server  (make asan for the checked build)
cd engine-uring && make test       # cmake presets; 113 tests
```

`engine-uring` needs `liburing >= 2.5`, `fmt`, and Catch2. The default preset is
ASan+UBSan — never measure with it, it roughly halves throughput. In every
component `make` builds the binary you *measure* with and a separate target
builds the sanitised one; that convention was the other way round once, and it
silently cost a set of numbers.
