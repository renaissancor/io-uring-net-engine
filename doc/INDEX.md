# doc/ — Code Specification Index

**Entry point for building this project from spec.** Each source unit has a spec
at `doc/<path>.md` mirroring `src/<path>.h[/.cpp]` 1:1. `doc/` is the **source of
truth**: implement from the specs alone. The "why" lives in `design/` (dated
journal) — read it for context, never as a build dependency.

- **Project guides** (build, style, CI, architecture) live at `doc/` root.
- **Per-file specs** mirror `src/` under `doc/`.
- **Template:** [`TEMPLATE.md`](TEMPLATE.md). **Sync rule:** a unit is not "done"
  until its spec matches the built API.

---

## Build order (tiers — build a tier only after every tier above it)

Dependencies point upward: a unit may depend only on units in the same or a
higher tier. Pick any order within a tier.

### Tier 0 — foundation (no intra-project deps)
| Unit | Source | Status | Spec |
|---|---|---|---|
| `types` | `src/types.h` | landed | _todo_ |
| `check` | `src/check.h` | landed | _todo_ |

### Tier 1 — primitives
| Unit | Source | Status | Depends | Spec |
|---|---|---|---|---|
| `sync/atomic` | `src/sync/atomic.h` | landed | types | _todo_ |
| `sds/ring_buffer` | `src/sds/ring_buffer.h` | landed | sync/atomic, types | _todo_ |
| `sds/static_vector` | `src/sds/static_vector.h` | landed | types, check | _todo_ |
| `memory/packet_pool` | `src/memory/packet_pool.{h,cpp}` | landed | types, check | _todo_ |
| `runtime/thread` | `src/runtime/thread.h` | landed | check | _todo_ |

### Tier 2 — app identity & mesh vocabulary
| Unit | Source | Status | Depends | Spec |
|---|---|---|---|---|
| `app/config` | `src/app/config.h` | landed | types | _todo_ |
| `app/session_id` | `src/app/session_id.h` | landed | types | _todo_ |
| `app/message` | `src/app/message.h` | landed | session_id, types | _todo_ |
| `app/session_record` | `src/app/session_record.h` | landed | session_id, types | _todo_ |
| `app/detail/thread_role` | `src/app/detail/thread_role.{h,cpp}` | landed | types | _todo_ |

### Tier 3 — mesh transport & authority
| Unit | Source | Status | Depends | Spec |
|---|---|---|---|---|
| `app/spsc_mailbox` | `src/app/spsc_mailbox.h` | landed | message, ring_buffer, check, types | [spec](app/spsc_mailbox.md) |
| `app/session_table` | `src/app/session_table.{h,cpp}` | landed | config, session_id, session_record | _todo_ |

### Tier 4 — thread handles & engines
| Unit | Source | Status | Depends | Spec |
|---|---|---|---|---|
| `app/handle_thread` | `src/app/handle_thread.{h,cpp}` | landed | runtime/thread, sync/atomic, types | _todo_ |
| `app/handle_worker` | `src/app/handle_worker.{h,cpp}` | landed | handle_thread, config, spsc_mailbox | _todo_ |
| `app/handle_acceptor` | `src/app/handle_acceptor.{h,cpp}` | landed | handle_thread, config, spsc_mailbox | _todo_ |
| `app/engine_worker` | `src/app/engine_worker.{h,cpp}` | in-progress | handle_worker, thread_role | _todo_ |
| `app/engine_acceptor` | `src/app/engine_acceptor.{h,cpp}` | in-progress | handle_acceptor, thread_role | _todo_ |

### Tier 5 — supervisor
| Unit | Source | Status | Depends | Spec |
|---|---|---|---|---|
| `app/main` | `src/app/main.cpp` | landed | config, handles, spsc_mailbox, static_vector | _todo_ |

### Tier 6 — data path (planned; see `doc/10-realtime-server-architecture.md` + `handoff.md`)
| Unit | Source | Status | Depends | Spec |
|---|---|---|---|---|
| `app/protocol` | `src/app/protocol.{h,cpp}` | planned | message, types | _todo_ |
| `app/session` (worker-side, SoA/mmap) | `src/app/session.*` | planned | ring_buffer, session_id | _todo_ |
| `app/world_room` | `src/app/world_room.{h,cpp}` | planned | session_id | _todo_ |
| engine data-path loop | `engine_worker` / `engine_acceptor` | planned | all of tiers 0–4 | _todo_ |

---

## Legend

- **Status:** `landed` (built + tested) · `in-progress` (skeleton exists) ·
  `planned` (spec only / not yet spec'd).
- **Spec `_todo_`** = a spec for this unit is not yet in `TEMPLATE.md` form.
  Many per-file specs were **relocated from the old `wiki/` tree** and now live
  at `doc/<path>.md` in their original (pre-template) prose — useful already,
  but not yet reformatted to the normative template. Reformatting those, and
  writing new specs for the `app/` layer, is the remaining spec work.

## Spec reconciliation status

Reconciling relocated specs to `TEMPLATE.md` surfaced that several described
**retired or nonexistent** code — the code is the current source of truth.

**Reconciled to the built API** (template form, verified against the header):
`sds/ring_buffer`, `sds/static_vector`, `sds/cstr_hash_map`, `sds/malloc_vector`,
`sync/atomic`, `sync/mutex`, `runtime/thread`, `diagnostic/profiler_scope`,
`check`; plus `app/spsc_mailbox` (reference shape).

**Drift corrected along the way:**
- `ring_buffer` — old spec described a fully **retired** growable `char*` ring.
- `check` / `mutex` — advertised **`LNX_DCHECK`**, which does not exist.
- `thread` — old spec's API sketch over-promised (no name param / cached TID).

**Deferred — `memory/packet_pool` (needs a design decision, not reconciliation):**
The landed `src/memory/packet_pool` is a 3-bucket (64/256/1024 B) TLS/mmap byte
pool, but no spec matches it — `doc/memory/memory_pool.md` (48-class allocator),
`doc/memory/object_pool.md`, and `doc/network/packet_pool.md` (cs/sc-typed) all
describe **different, unbuilt** designs. Deciding which design is the future
(and whether the landed pool supersedes the 48-class one) is a separate
discussion; until then, `doc/memory/packet_pool.md` is intentionally unwritten
and those three specs are treated as **planned**, not landed.

**Not yet written — `app/` layer specs:** `message`, `session_id`,
`session_record`, `config`, `session_table`, `handle_thread/worker/acceptor`,
`engine_worker/acceptor`, `main`, `detail/thread_role` (only `spsc_mailbox`
exists). These document current work and have no relocated predecessor.

> Layout migration is **done**: brainstorm → `design/`, per-file specs and
> project guides → `doc/`. `doc/app/spsc_mailbox.md` is the filled reference
> shape; match it when reformatting the relocated specs.
