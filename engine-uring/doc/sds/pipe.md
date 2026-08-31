# pipe — bounded byte stream between two threads

> **Status:** landed
> **Source:** `src/sds/pipe.h`
> **Namespace:** `sds`
> **Depends:** `ring_buffer`, `types`

## Purpose

A bounded byte stream between exactly two threads — one writer, one reader.
Semantically a `pipe(2)`: no message boundaries, no growth, and a full pipe
refuses the write rather than blocking or resizing.

This is the **same mechanism** as `sds::ring_buffer`, named for its other role:

| Type | Role | Sync |
|---|---|---|
| `sds::ring_buffer<N, ring_sync::single>` | socket I/O — io_uring fills the enqueue region, the owning worker parses off the dequeue region | one thread, plain cursors |
| `sds::pipe<N>` | thread mesh — a peer writes frames in, the owner reads them out | two threads, atomic64 acquire/release |

## API

```cpp
namespace sds {

template <usize N>
using pipe = ring_buffer<N, ring_sync::spsc>;

}  // namespace sds
```

An alias, not a wrapper — zero indirection, and the full `ring_buffer` byte API
is available unchanged.

## Invariants

- **SPSC contract:** at most one thread writes, at most one reads. No runtime
  guard.
- All `ring_buffer` invariants apply: power-of-two `N`, all `N` bytes usable,
  all-or-nothing `enqueue`/`enqueue2`, bounded (never resizes).

## Notes

- **Why a separate name.** In a network engine, `ring_buffer` reads as "the
  socket recv/send buffer" — that is its dominant use. Overloading the name for
  thread-to-thread traffic buries the distinction at exactly the use sites where
  it matters most. Keeping `ring_buffer` for the io_uring side and `pipe` for
  the mesh side means a declaration never has to be read twice to learn which
  traffic it carries.
- **Framing is not this layer's job**, exactly as it is not TCP's. A pipe moves
  bytes; `app/mesh.h` puts `[header|body]` frames on top. Use `enqueue2()` to
  publish a header and body as one atomic frame with no staging buffer.
- Because the byte API is identical across both roles, write the frame-parse
  loop as a **template over the byte API**, not against a concrete type — then
  one loop serves socket bytes and peer-thread bytes alike.

## Test plan

Covered by `tests/sds/ring_buffer_test.cpp` (the `ring_sync::spsc` cases,
including the 1M-frame FIFO stress) and `tests/app/mesh_test.cpp` (framing over
`sds::pipe`, including wrap-straddling frames).

## Done when

- [x] Builds on `default` and `floor` presets
- [x] Mesh edges (`acceptor_to_worker_pipe` / `worker_to_acceptor_pipe`) use it
- [x] This spec matches the built API

## Rationale

- `doc/sds/ring_buffer.md` — the shared mechanism
- `doc/app/mesh.md` — the framing layer on top
