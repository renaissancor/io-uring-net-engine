# doc/ — code map

**One file per source unit, mirroring `src/` 1:1, each describing the code
that exists.** A `doc/<path>.md` says what `src/<path>.h[/.cpp]` is, its
exact public API, the invariants callers must hold, what happens on each
failure, and which existing tests prove it. It is written from the header,
and when the two disagree the header is right and the doc is out of date.

What it is not: a specification to build from, or a record of what was
planned. Deliberation, including specs for code that was never written, is
dated and lives in [`../../design-notes/`](../../design-notes/). The
[`TEMPLATE.md`](TEMPLATE.md) gives the shape;
[`../../server-uring/doc/mesh.md`](../../server-uring/doc/mesh.md) is the
filled reference.

- **Project guides** (build, style, CI, architecture) are at `doc/` root;
  [`README.md`](README.md) indexes them.
- **Sync rule:** a unit is not done until its doc matches the built API. A
  changed header means a changed doc in the same commit.

---

## Units, in dependency order

Tiers depend only upward. Pick any order within a tier.

### Tier 0 — foundation (no intra-project deps)

| Unit | Source | Doc |
|---|---|---|
| `types` | `src/types.h` | [types.md](types.md) |
| `check` | `src/check.h` | [check.md](check.md) |
| `error/expected` | `src/error/expected.h` | [error/expected.md](error/expected.md) |

### Tier 1 — primitives

| Unit | Source | Depends | Doc |
|---|---|---|---|
| `sync/atomic` | `src/sync/atomic.h` | types | [sync/atomic.md](sync/atomic.md) |
| `sync/mutex` | `src/sync/mutex.h` | check | [sync/mutex.md](sync/mutex.md) |
| `sds/ring_buffer` | `src/sds/ring_buffer.h` | sync/atomic, types | [sds/ring_buffer.md](sds/ring_buffer.md) |
| `sds/pipe` | `src/sds/pipe.h` | ring_buffer, types | [sds/pipe.md](sds/pipe.md) |
| `sds/static_vector` | `src/sds/static_vector.h` | types, check | [sds/static_vector.md](sds/static_vector.md) |
| `sds/malloc_vector` | `src/sds/malloc_vector.h` | types, check | [sds/malloc_vector.md](sds/malloc_vector.md) |
| `sds/cstr_hash_map` | `src/sds/cstr_hash_map.h` | types, check | [sds/cstr_hash_map.md](sds/cstr_hash_map.md) |
| `memory/packet_pool` | `src/memory/packet_pool.{h,cpp}` | types, check | [memory/packet_pool.md](memory/packet_pool.md) |
| `runtime/thread` | `src/runtime/thread.h` | check | [runtime/thread.md](runtime/thread.md) |
| `diagnostic/profiler_scope` | `src/diagnostic/profiler_scope.{h,cpp}` | types | [diagnostic/profiler_scope.md](diagnostic/profiler_scope.md) |

Thirteen units, thirteen docs. `src/_placeholder.cpp` exists only so the
static library target has a translation unit and has no doc.

### Tiers 2+ — the runtime (in `server-uring`)

Identity and mesh vocabulary, mesh transport and authority, thread control
blocks and engines, and the supervisor live in
[`../../server-uring/`](../../server-uring/) with their own code map at
[`../../server-uring/doc/INDEX.md`](../../server-uring/doc/INDEX.md). They
consume tiers 0–1 only through the engine's installed public headers.

---

## History of this tree

Until 2026-09-02 this directory also held fourteen specs for units that were
never built — the 2026-05-17 two-tier reactor, its memory tiers, the 8-byte
packet header and typed packet readers, and five primitives ported on paper
from the Windows reference. They were written under a build-from-spec
contract that the tree later abandoned without saying so, and nine of them
carried no status line. They are now dated deliberation:
[`../../design-notes/unbuilt-specs-2026-05/`](../../design-notes/unbuilt-specs-2026-05/).

Reconciling the surviving docs to their headers, done earlier, corrected
three drifts worth remembering: `ring_buffer`'s doc described a retired
growable ring; `check` and `mutex` advertised an `LNX_DCHECK` that does not
exist; `thread`'s API sketch promised a name parameter and cached TID it
does not have.
