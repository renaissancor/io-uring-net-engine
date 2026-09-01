# Design notes

Dated records of thinking. What was considered, what was chosen, and — more
useful later — what was rejected and why. These are not specifications. A
document here describes the state of an argument on the day it was written,
and nothing edits it afterwards to keep it true.

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
| [`2026-05-14`](2026-05-14-project-split.md) | Library / product split. **Superseded** — argued for two repositories; this monorepo is the answer that won. Kept for the boundary criteria, which still hold |
| [`2026-05-15`](2026-05-15-session-log.md) | Session log |
| [`2026-05-19`](2026-05-19-server-architecture.md) | Server architecture |
| [`2026-05-19`](2026-05-19-chat-server-data-layout.md) | Chat server data layout |
| [`2026-05-19`](2026-05-19-portfolio-strategy.md) | Portfolio strategy |
| [`2026-05-21`](2026-05-21-packet-pool-review.md) | `packet_pool` review |
| [`2026-05-23`](2026-05-23-session-account-data-model.md) | Session / account data model |
| [`2026-05-25`](2026-05-25-handle-engine-split.md) | Handle / engine split |
| [`2026-06-06`](2026-06-06-supervisor-init-and-acceptor-lobby.md) | Supervisor init, acceptor as lobby |
| [`2026-08-21`](2026-08-21-architecture-review.md) | Pre-data-path architecture review |
| [`2026-08-21`](2026-08-21-phase2-architecture-pass.md) | Phase 2 architecture pass |
| [`2026-09-01`](2026-09-01-architecture-review-disposition.md) | Disposition of the pre-data-path review — what landed, what is still open |

## `game-server/`

One exception to the flat rule, and it is provenance rather than taxonomy:
[`game-server/`](game-server/) is an imported document set that arrived whole,
from the separate `iouring-net-server` repository that the rejected two-repo
split created. Its 20 documents cross-link each other and read as one body;
scattering them by date would break that and gain nothing. It is kept intact,
and its own [`README`](game-server/README.md) says which parts the library
pivot invalidated.
