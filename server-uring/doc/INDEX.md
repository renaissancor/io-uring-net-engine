# server-uring doc/ — Code Specification Index

**Continuation of [`engine-uring/doc/INDEX.md`](../../engine-uring/doc/INDEX.md).**
The tier numbering is shared: tiers 0–1 are the engine's foundation and
primitives, consumed here strictly through the engine's installed public
headers (`find_package(iouring_net)`). Tiers 2+ below are the runtime this
component owns. Same rules: specs mirror `src/` 1:1, `TEMPLATE.md` form is
normative (see [`../../engine-uring/doc/TEMPLATE.md`](../../engine-uring/doc/TEMPLATE.md)),
and a unit is not "done" until its spec matches the built API.

The runtime architecture — shape, ownership invariants, v1 milestone — is
[`10-realtime-server-architecture.md`](10-realtime-server-architecture.md).

---

### Tier 2 — app identity & mesh vocabulary
| Unit | Source | Status | Depends | Spec |
|---|---|---|---|---|
| `config` | `src/config.h` | landed | types | _todo_ |
| `session_id` | `src/session_id.h` | landed | types | _todo_ |
| `message` | `src/message.h` | landed | session_id, types | _todo_ |
| `session_record` | `src/session_record.h` | landed | session_id, types | _todo_ |
| `detail/thread_role` | `src/detail/thread_role.{h,cpp}` | landed | types | _todo_ |

### Tier 3 — mesh transport & authority
| Unit | Source | Status | Depends | Spec |
|---|---|---|---|---|
| `mesh` | `src/mesh.h` | landed | message, sds::pipe, check, types | [spec](mesh.md) |
| `session_table` | `src/session_table.{h,cpp}` | landed | config, session_id, session_record | _todo_ |

### Tier 4 — thread control blocks & engines
| Unit | Source | Status | Depends | Spec |
|---|---|---|---|---|
| `thread_ctl` | `src/thread_ctl.{h,cpp}` | landed | runtime/thread, sync/atomic, types | _todo_ |
| `worker_ctl` | `src/worker_ctl.{h,cpp}` | landed | thread_ctl, config, mesh | _todo_ |
| `acceptor_ctl` | `src/acceptor_ctl.{h,cpp}` | landed | thread_ctl, config, mesh | _todo_ |
| `worker_engine` | `src/worker_engine.{h,cpp}` | in-progress | worker_ctl, thread_role | _todo_ |
| `acceptor_engine` | `src/acceptor_engine.{h,cpp}` | in-progress | acceptor_ctl, thread_role | _todo_ |

### Tier 5 — supervisor
| Unit | Source | Status | Depends | Spec |
|---|---|---|---|---|
| `main` | `src/main.cpp` | landed | config, ctls, mesh, static_vector | _todo_ |

### Tier 6 — data path (planned; see `10-realtime-server-architecture.md` + `handoff.md`)
| Unit | Source | Status | Depends | Spec |
|---|---|---|---|---|
| `protocol` | `src/protocol.{h,cpp}` | planned | message, types | _todo_ |
| `session` (worker-side, SoA/mmap) | `src/session.*` | planned | ring_buffer, session_id | _todo_ |
| `world_room` | `src/world_room.{h,cpp}` | planned | session_id | _todo_ |
| engine data-path loop | `worker_engine` / `acceptor_engine` | planned | all of tiers 0–4 | _todo_ |

---

## Spec status

**Written:** `mesh` ([mesh.md](mesh.md), the filled reference shape).

**Not yet written:** `message`, `session_id`, `session_record`, `config`,
`session_table`, `thread_ctl/worker/acceptor`, `worker_engine/acceptor`,
`main`, `detail/thread_role`. These document current work and have no
relocated predecessor.
