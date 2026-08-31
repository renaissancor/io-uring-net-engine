# doc/ — Code Specification Index

**Entry point for building this project from spec.** Each source unit has a spec
at `doc/<path>.md` mirroring `src/<path>.h[/.cpp]` 1:1. `doc/` is the **source of
truth**: implement from the specs alone. The "why" lives in `../design-notes/` (dated
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
| `sds/ring_buffer` | `src/sds/ring_buffer.h` | landed | sync/atomic, types | [spec](sds/ring_buffer.md) |
| `sds/pipe` | `src/sds/pipe.h` | landed | ring_buffer, types | [spec](sds/pipe.md) |
| `sds/static_vector` | `src/sds/static_vector.h` | landed | types, check | _todo_ |
| `memory/packet_pool` | `src/memory/packet_pool.{h,cpp}` | landed | types, check | _todo_ |
| `runtime/thread` | `src/runtime/thread.h` | landed | check | _todo_ |

### Tiers 2+ — the app runtime (moved)

The app layer — identity & mesh vocabulary, mesh transport & authority,
thread control blocks & engines, supervisor, and the planned data path —
migrated to [`../../server-uring/`](../../server-uring/) together with its
sources and tests. Its build-order tiers continue in
[`../../server-uring/doc/INDEX.md`](../../server-uring/doc/INDEX.md); they
depend on tiers 0–1 here only through the engine's installed public headers.

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
`check`; plus `sds/pipe` and (now in `../../server-uring/doc/`) `mesh` (reference shape).

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

> Layout migration is **done**: brainstorm → `../design-notes/`, per-file specs and
> project guides → `doc/`. `../../server-uring/doc/mesh.md` is the filled
> reference shape; match it when reformatting the relocated specs.
