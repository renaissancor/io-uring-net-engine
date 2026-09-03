# Design notes

Dated records of thinking. What was considered, what was chosen, and — more
useful later — what was rejected and why. These are not specifications. A
document here describes the state of an argument on the day it was written,
and nothing edits its body afterwards to keep it true.

**One thing is allowed to change: the status block.** Every dated note opens
with YAML front matter (the ADR convention, rendered by GitHub as a table):

```yaml
---
status: superseded            # proposed | accepted | superseded
superseded_by:                # required when superseded; files that replaced it
  - 2026-05-23-session-account-data-model.md
still_holds: ...              # what a reader may still rely on, one line
amended_by: [...]             # for accepted notes a later note adjusted
reconstructed: 2026-09-02     # when the note was written after the fact
---
```

That block is the only edit a note receives after its day, and
[`check_status.py`](check_status.py) fails if a superseded note names no
existing successor or the journal below disagrees with it. Before 2026-09-02
supersession was marked by ad-hoc banners on two notes and by nothing on six
others; the blocks replaced that.

**The rule that keeps this directory honest: it is flat and chronological.**
No `decisions/` and no `discussions/` subdirectory. A two-level split would
force a judgment on every future file — is this a decision or a discussion? —
and the judgment ages badly. `2026-05-14-project-split.md` argued for shipping
the library and the product as two repositories; that split was later
rejected. It would be sitting in `decisions/` right now, and the folder name
would be lying about it.

So the split is by *kind of claim*, not by folder:

| where | what belongs there |
|---|---|
| `design-notes/` | dated deliberation. True as of its date, never revised |
| a component's `doc/` | the binding spec the code must satisfy. Revised whenever the code moves |
| [`../result-notes/`](../result-notes/) | measurements, and the method that makes them comparable |

That is why [`../server-uring/doc/10-realtime-server-architecture.md`](../server-uring/doc/10-realtime-server-architecture.md)
lives next to the code rather than here: it is normative, and something is
supposed to break when the code drifts from it. The same reasoning keeps
[`../engine-uring/doc/01-windows-to-linux-mapping.md`](../engine-uring/doc/01-windows-to-linux-mapping.md)
in the engine — it is a reference table you read *while* reading engine code,
not an argument anyone had on a particular afternoon.

## The journal

| date | note |
|---|---|
| [`2026-05-14`](2026-05-14-project-split.md) | **Superseded** — Library / product split — argued for two repositories; this monorepo is the answer that won. Kept for the boundary criteria, which still hold |
| [`2026-05-15`](2026-05-15-session-log.md) | **Superseded** — Session log — its build order never happened; `LNX_DCHECK` was deleted |
| [`2026-05-17`](2026-05-17-architecture-pivot-and-monorepo-reconstructed.md) | **Superseded** — Two-repo split rejected; single-tier coroutines → two-tier reactor locked. **Reconstructed 2026-09-02** from surviving memory records; the wiki that held it did not survive |
| [`2026-05-19`](2026-05-19-server-architecture.md) | **Superseded** — Server architecture — data layout, roles, and inboxes replaced by 05-19 evening, 05-23, and `doc/10`; Part 7 budget and Part 5.5 still hold |
| [`2026-05-19`](2026-05-19-chat-server-data-layout.md) | **Superseded** — Chat server data layout — constants, global mmap, and kernel-producer SPSC replaced by 05-23; session-as-handle and drop-and-close still hold |
| [`2026-05-19`](2026-05-19-portfolio-strategy.md) | **Superseded** — Portfolio strategy — three-layer plan narrowed on 05-21; purpose restated 2026-09-02 |
| [`2026-05-21`](2026-05-21-packet-pool-review.md) | **Accepted** — `packet_pool` review |
| [`2026-05-21`](2026-05-21-chat-only-scope-reconstructed.md) | **Superseded** — Chat-only scope; game server to a separate repo. **Reconstructed 2026-09-02** — the decision that narrowed the deliverable had no note of its own |
| [`2026-05-23`](2026-05-23-session-account-data-model.md) | **Accepted** — Session / account data model |
| [`2026-05-25`](2026-05-25-handle-engine-split.md) | **Superseded** — Handle / engine split — names became `*_ctl` / `*_engine`, registry became the compile-time roster |
| [`2026-06-06`](2026-06-06-supervisor-init-and-acceptor-lobby.md) | **Superseded** — Supervisor init, acceptor as lobby — `doc/10` § 7: v1 gives the acceptor no pre-world I/O |
| [`2026-08-21`](2026-08-21-architecture-review.md) | **Accepted** — Pre-data-path architecture review |
| [`2026-08-21`](2026-08-21-phase2-architecture-pass.md) | **Accepted** — Phase 2 architecture pass — no disposition note yet |
| [`2026-09-01`](2026-09-01-architecture-review-disposition.md) | **Accepted** — Disposition of the pre-data-path review — what landed, what is still open |
| [`2026-09-02`](2026-09-02-where-io-uring-becomes-meaningful.md) | **Proposed** — Where io_uring becomes meaningful vs epoll — the hypothesis, cost model, predicted two-tier verdict, and the tick-budget experiment, written before the data path exists |
| [`2026-09-02`](2026-09-02-design-notes-drift-review.md) | **Accepted** — Drift review — 67 cited findings grouped into six root causes, the purpose statement that closes the first, the recovered sources for the second, and the one convention decision still open |
| [`2026-09-02`](2026-09-02-control-group-on-engine-primitives.md) | **Accepted** — The control group on the engine's primitives — `server-sds` through the `find_package` seam, the client optimised on the existing binary with the judge as gate, syscall shape held equal so the delta is data structures, and the layout lesson the first measurement forced |
| [`2026-09-03`](2026-09-03-working-set-knob-for-the-tick-budget-experiment.md) | **Accepted** — A working-set knob for the tick-budget experiment — content accumulates as memory, not only cycles; the knob lets the sweep falsify "logic term identical by construction"; predictions P1–P3; stage A is the epoll half with chat semantics untouched |

## `game-server/`

One exception to the flat rule, and it is provenance rather than taxonomy:
[`game-server/`](game-server/) is an imported document set that arrived whole,
from the separate `iouring-net-server` repository that the rejected two-repo
split created. Its 20 documents cross-link each other and read as one body;
scattering them by date would break that and gain nothing. It is kept intact,
and its own [`README`](game-server/README.md) says which parts the library
pivot invalidated.
