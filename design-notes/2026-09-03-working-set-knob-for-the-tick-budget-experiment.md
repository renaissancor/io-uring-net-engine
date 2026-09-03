---
status: accepted
note: amends 2026-09-02 § 7 — adds a working-set knob and the staging for the epoll side. Stage A ran the same day (result-notes/2026-09-03) — P1 falsified on this box, P2/P3 wait on the io_uring side
---
# 2026-09-03 — A working-set knob for the tick-budget experiment: content accumulates as memory, not only as cycles

[`2026-09-02-where-io-uring-becomes-meaningful.md`](2026-09-02-where-io-uring-becomes-meaningful.md)
§ 7 gives both servers a tick and dials the logic term with two spin knobs,
`LOGIC_NS_PER_MSG` and `LOGIC_NS_PER_ENTITY_TICK`. This note adds a third knob
and says why the experiment is not honest without it. It also fixes the order
in which the epoll side gets built, because the io_uring data path does not
exist yet and the epoll half can be measured on its own.

## 1. What a spin knob cannot show

The question behind § 7 was put plainly on 2026-09-03: in a shipped MMO server
the logic does not stay constant, it accumulates, system by system, for years.
A spin loop models the part of that which costs cycles. It misses the part
that costs memory, and that part is the one that couples the logic term to
the I/O term.

Every system that lands adds per-entity state: buffs, cooldowns, inventory
deltas, quest flags, area-of-interest lists, AI blackboards. The per-tick
pass over `N` entities then touches `N × W` bytes, and `W` grows with the
content. Three things follow, none of which a spin loop reproduces:

1. **The I/O term after logic is cold.** A tick that sweeps 40 MB has evicted
   the connection table, the receive buffers, and the kernel's socket
   structures from L2 and most of L3. The next drain pays cache misses on
   every one of them. § 7.2 already says this in one sentence — "real logic
   evicts the socket buffers and session table from cache" — and then fixes
   the footprint at a few counters per entity, which evicts nothing.
2. **The logic term is not identical between the two servers.** § 2 claims it
   is "identical under epoll and io_uring by construction". In instructions,
   yes. In cycles, no: a syscall pollutes the user-space cache and TLB on the
   way through the kernel, and epoll issues two of them per active connection
   per tick where io_uring issues a handful per batch. Soares and Stumm
   measured this as the *indirect* cost of system calls (FlexSC, OSDI 2010):
   user-mode IPC falls after a syscall-heavy phase, and the fall scales with
   how much the user code had in cache to lose. With `N_a = 10,000` and
   `W = 4 KiB`, epoll's 20,000 syscalls per tick run against a 40 MB working
   set. Whether that costs the logic term 1 % or 20 % on this box is exactly
   the kind of number § 6 says the experiment must be able to see, and with a
   fixed tiny footprint it cannot.
3. **The regime changes with `N × W`, not with `N`.** 10,000 entities at
   64 B fit in one P-core's L2 (2 MB on the i5-13600K). At 512 B they are
   5 MB, out of L2 and inside the 24 MB L3. At 4 KiB they are 40 MB and the
   tick streams from DRAM. The § 4 crossover table is a function of `N_a`
   alone; if the I/O constants move with the cache regime, the crossover
   moves with `W` as well, and a portfolio number quoted at one `W` is a
   number quoted at one point in a game's life.

So the knob is not a refinement. It is what lets the experiment falsify the
"identical by construction" premise instead of assuming it.

## 2. The knob

`LOGIC_BYTES_PER_ENTITY` = `W`, the size of each entity's state block.

- **Arena.** One `mmap` of `slots × W` bytes, `MAP_POPULATE`d and touched
  once at startup, so the first ticks do not measure page faults. One slot per
  connection slot (`CHAT_MAX_CONNS`), indexed by fd, so a session's block is
  found without a lookup that would itself be a cache event under study.
- **Per-entity tick work.** For every live session, walk its block one cache
  line (64 B) at a time: load, fold into a running checksum, store. A
  read-modify-write, not a read: a read-only sweep lets the compiler and the
  prefetcher do what no game system does. The checksum is stored back into
  the block's first line so the work is observable and cannot be eliminated.
- **Per-message handler work.** The handler for an inbound frame touches the
  sender's own block the same way. That is the random-access half: the tick
  is a linear sweep in slot order, the way an ECS pass runs; handlers arrive
  in socket-readiness order, the way input does.
- **Values.** `W ∈ {64, 512, 4096}`, chosen for the three regimes in § 1.3
  at `N = 10,000`, not for realism: 64 B is one line and isolates the spin
  knobs; 4 KiB is a page and streams from DRAM. A 16 KiB row (160 MB) is
  allowed if the 4 KiB row still shows a slope.
- **Separability.** At `W = 64` the memory term is one line per entity and the
  spin knobs measure alone. At `L = 0` the spin term is zero and `W` measures
  alone. The two are additive in the model; whether they are additive in the
  measurement is one of the things the sweep reports.

The header that holds the arena, the walk, and the two spin routines is one
translation unit with no engine headers and no STL, included unchanged by
every server that joins the experiment. § 7.2's byte-identity rule applies to
all three knobs.

## 3. What it does not model, said now

- **Instruction footprint.** Accumulated systems also grow code: i-cache and
  branch-predictor pressure. A spin loop plus a line walk fits in a few
  hundred bytes of text. The experiment is therefore a *lower bound* on how
  much real logic degrades the I/O term, and a result that says "no coupling"
  at `W = 4 KiB` says nothing about a 30 MB binary.
- **Pointer chasing.** The walk is sequential within a block and sequential
  over blocks. Real per-entity state is a graph, and its misses are dependent
  misses the prefetcher cannot hide. Same direction: the knob understates.
- **Sharing.** One thread, one arena, no false sharing, no cross-thread
  coherence traffic. Out of scope until there is a second worker.

Each of these would make the coupling larger, not smaller. If the knob at
4 KiB already moves the I/O term, the direction is settled and the size is a
floor.

## 4. Predictions, written before the loop exists

Same discipline as the parent note: the numbers below can be confirmed or
moved, not fitted.

- **P1 — the I/O drain gets dearer with `W`.** At `N = 10,000`, `L = 0`, the
  per-active-connection cost of the epoll drain rises from `W = 64` to
  `W = 4096` by **20–40 %**: one DRAM miss on the connection record, one on
  the receive buffer, some on kernel socket state, against a ~1 µs syscall.
  Under 10 % and the cache-coupling argument is not material on this box.
- **P2 — the logic term differs between servers.** At equal `L` and `W`, the
  tick-phase time under epoll exceeds the same phase under io_uring, and the
  excess grows with both `N_a` and `W`. Prediction: **under 5 % at `W = 64`,
  measurable above it**. If it exceeds 5 % in any cell, § 2 of the parent
  note is amended from "identical by construction" to "identical in
  instructions, not in cycles", and the budget-returned formula gains a term
  for the logic cycles io_uring gives back by not polluting the cache. This
  prediction cannot be tested until the io_uring data path lands; the epoll
  half of it (the absolute tick-phase time per cell) is recorded first so
  the comparison does not depend on re-running the control later.
- **P3 — `W` moves the crossover.** The `N_a` at which io_uring's returned
  budget becomes a material share of the tick is lower at `W = 4096` than at
  `W = 64`. Direction only; the parent note's § 6.3 caveat about WSL2 applies
  to every constant here.

## 5. Staging: the epoll half first, chat semantics untouched

The parent note's § 9 rejects "a fixed-rate tick with a real world update as
the first step" and says the knobs go first. This note keeps that and makes
it concrete, because the io_uring side has no data path and the experiment
should not wait for it.

**Stage A — `server-epoll` only.**

- `epoll_wait` gets a timeout of `next_tick_deadline − now` instead of −1.
  At the deadline the tick runs: the entity walk over every live session with
  `LOGIC_NS_PER_ENTITY_TICK` spun per entity, then the deadline advances by
  the period. `CHAT_TICK_HZ` selects it; unset, the loop is byte-for-byte the
  one that produced every row in `result-notes/`.
- The chat semantics do not change. A frame is still broadcast by the
  handler in the same batch; the tick is *added* work on a timer, not a
  redefinition of when sends happen. This is the smallest diff, and it is
  the one that keeps the existing client and its verdict as the gate: the
  client's echo latency now includes whatever the tick steals, and the
  self-lag / fan-out / loss / censored-p99 verdict still says whether the
  row counts. The snapshot protocol and the redefined client latency of
  § 7.4 come with the shaped tick, later.
- **The primary instrument lands in this stage**: the server's per-tick
  phase histogram — I/O drain, tick, flush, and an overrun count — printed at
  shutdown in the same 1 µs-bucket format the client uses, so `merge.py`'s
  reader can be reused for it. Without this the stage produces only client
  numbers, and § 7.4 explains why those cannot answer the question.
- The tick goes into `server.cpp`, the stated control for io_uring
  (`CHAT_FLUSH=batch`). `server_sds.cpp` is a sibling loop and takes the same
  three includes when the io_uring server does, so the two engine-primitive
  builds are compared under one header revision.

**Stage A sweep.** `N ∈ {300, 1000, 3000, 10000}` × `L_entity ∈ {0, 10, 30,
100} µs` at `W = 64` (the parent note's grid), plus `W ∈ {512, 4096}` at
`L_entity ∈ {0, 30}` (the memory row and one realistic row). 32 cells, each
at a fixed input rate below the ceiling, 30 Hz, alternated and repeated per
the 2026-09-02 measurement rules. `LOGIC_NS_PER_MSG` stays at 0 in stage A;
it is the `c_msg` slot and belongs to the stage that redefines the handler.

**Stage B** — the same header in the io_uring server when its data path
lands; P2 becomes testable. **Stage C** — the shaped tick and the snapshot
protocol of § 7.2–7.4, if the knobs say the boundary is where § 5 predicts.

## 6. Rejected

- **Model accumulation as a bigger spin.** That is the parent note's design,
  and § 1 is why it is insufficient: cycles do not evict caches.
- **Random access for the tick sweep.** A random walk over entities would
  defeat the prefetcher and make the memory term larger, but it is not how a
  per-system pass runs, and it would confound the handler's random access
  with the tick's. The handler path already supplies the random half.
- **A real-sized entity struct instead of a knob.** Picks one point in the
  game's life and calls it representative. The knob's whole purpose is the
  slope.
- **Waiting for the io_uring data path to run anything.** The epoll half is
  the control, it can be measured now, and recording it first means the
  comparison later is against a recorded row rather than a re-run.

## Rationale links

- [`2026-09-02-where-io-uring-becomes-meaningful.md`](2026-09-02-where-io-uring-becomes-meaningful.md)
  — the hypothesis, cost model, and § 7 experiment this note amends; § 9 for
  the knobs-first staging this note keeps.
- [`2026-05-19-server-architecture.md`](2026-05-19-server-architecture.md)
  Part 7 — the 10–100 µs per player per tick budget the `L` knob spans.
- [`../result-notes/2026-08-30-what-limits-the-server.md`](../result-notes/2026-08-30-what-limits-the-server.md)
  — the fitted syscall constant P1 is measured against; § WSL2 for why every
  number here is shape only.
- [`../result-notes/2026-09-02-stl-to-sds-the-measured-delta.md`](../result-notes/2026-09-02-stl-to-sds-the-measured-delta.md)
  § 6 — the measurement rules (alternate, repeat, never rebuild under a fleet)
  the stage A sweep runs under, and finding 8, whose fix on 2026-09-03 is the
  verdict gate stage A relies on.
- Soares, L. and Stumm, M., "FlexSC: Flexible System Call Scheduling with
  Exception-Less System Calls", OSDI 2010 — the measured indirect cost of
  system calls on user-mode IPC that § 1.2 rests on.
