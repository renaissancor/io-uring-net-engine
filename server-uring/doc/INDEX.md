# server-uring doc/ — code map

**One file per source unit, mirroring `src/` 1:1, each describing the code
that exists.** Same contract as the engine's
[`doc/INDEX.md`](../../engine-uring/doc/INDEX.md): written from the header,
stale the moment the header changes, and never a specification of something
not yet built. [`mesh.md`](mesh.md) is the filled reference shape;
[`../../engine-uring/doc/TEMPLATE.md`](../../engine-uring/doc/TEMPLATE.md) is
the template.

The runtime architecture — thread roles, ownership invariants, glossary,
handoff protocol — is
[`10-realtime-server-architecture.md`](10-realtime-server-architecture.md).
Tiers 0–1 (foundation and primitives) live in the engine and are consumed
here only through its installed public headers via `find_package(iouring_net)`.

---

## Units, in dependency order

Tiers depend only upward. Status is `landed` (built and tested) or
`in-progress` (skeleton exists; the doc describes the skeleton).

### Tier 2 — identity & mesh vocabulary

| Unit | Source | Status | Depends | Doc |
|---|---|---|---|---|
| `config` | `src/config.h` | landed | types | [config.md](config.md) |
| `session_id` | `src/session_id.h` | landed | types | [session_id.md](session_id.md) |
| `message` | `src/message.h` | landed | session_id, types | [message.md](message.md) |
| `session_record` | `src/session_record.h` | landed | session_id, types | [session_record.md](session_record.md) |
| `detail/thread_role` | `src/detail/thread_role.{h,cpp}` | landed | types | [detail/thread_role.md](detail/thread_role.md) |
| `roster` | `src/roster.h` | landed | types | [roster.md](roster.md) |

### Tier 3 — mesh transport & authority

| Unit | Source | Status | Depends | Doc |
|---|---|---|---|---|
| `mesh` | `src/mesh.h` | landed | message, sds::pipe, check, types | [mesh.md](mesh.md) |
| `session_table` | `src/session_table.{h,cpp}` | landed | config, session_id, session_record | [session_table.md](session_table.md) |

### Tier 4 — thread control blocks & engines

| Unit | Source | Status | Depends | Doc |
|---|---|---|---|---|
| `thread_ctl` | `src/thread_ctl.{h,cpp}` | landed | runtime/thread, sync/atomic, types | [thread_ctl.md](thread_ctl.md) |
| `worker_ctl` | `src/worker_ctl.{h,cpp}` | landed | thread_ctl, config, mesh | [worker_ctl.md](worker_ctl.md) |
| `acceptor_ctl` | `src/acceptor_ctl.{h,cpp}` | landed | thread_ctl, config, mesh | [acceptor_ctl.md](acceptor_ctl.md) |
| `worker_engine` | `src/worker_engine.{h,cpp}` | in-progress | worker_ctl, thread_role | [worker_engine.md](worker_engine.md) |
| `acceptor_engine` | `src/acceptor_engine.{h,cpp}` | in-progress | acceptor_ctl, thread_role | [acceptor_engine.md](acceptor_engine.md) |

### Tier 5 — supervisor

| Unit | Source | Status | Depends | Doc |
|---|---|---|---|---|
| `main` | `src/main.cpp` | landed | roster, config, ctls, mesh, static_vector | [main.md](main.md) |

Fourteen units, fourteen docs.

## What is not here

The data path — protocol framing, worker-side session storage, rooms, the
per-worker `io_uring` recv/send loop — has no source yet and therefore no doc.
Its shape is [`10-realtime-server-architecture.md`](10-realtime-server-architecture.md)
§ 7–8; the measurement it will be judged by is
[`../../design-notes/2026-09-02-where-io-uring-becomes-meaningful.md`](../../design-notes/2026-09-02-where-io-uring-becomes-meaningful.md).
When a unit lands, its doc lands in the same commit and joins the table above.

## Tests

| file | covers |
|---|---|
| `tests/mesh_test.cpp` | `mesh` framing, `enqueue2` atomicity, cross-thread torture (`[app][mesh]`, `[app][mesh][stress]`) |
| `tests/session_table_test.cpp` | `session_table` authority map (`[app][session_table]`) |
| `tests/worker_ctl_skeleton_test.cpp` | `worker_ctl` + `worker_engine` lifecycle through `entry` (`[app][skeleton][worker_ctl]`) |

Each unit's own doc lists the cases that exercise it, or says "No dedicated
test."
