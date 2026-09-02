# Unbuilt specs, May 2026

Fourteen specification documents written between 2026-05-14 and 2026-05-19
for code that was never built. They lived in `engine-uring/doc/` until
2026-09-02, alongside the specs that do describe built code, and the doc
tree could not tell a reader which was which: nine carried no status line at
all, and five carried a "partially superseded" history banner.

They are moved here whole, like [`../game-server/`](../game-server/), because
they cross-reference each other and read as one body: the 2026-05-17 two-tier
reactor and its three memory tiers, the 8-byte packet header with its typed
`cs_packet` / `sc_packet` readers and writers, the session object those
assumed, and five primitives ported on paper from the Windows reference
library. The architecture they describe is recorded in
[`../2026-05-17-architecture-pivot-and-monorepo-reconstructed.md`](../2026-05-17-architecture-pivot-and-monorepo-reconstructed.md);
what replaced it is in
[`../2026-05-19-chat-server-data-layout.md`](../2026-05-19-chat-server-data-layout.md)
and [`../2026-05-23-session-account-data-model.md`](../2026-05-23-session-account-data-model.md).

**None of these is a description of code in this repository.** A `doc/` file
in a component describes the code that exists beside it; that rule is stated
in [`../../engine-uring/doc/README.md`](../../engine-uring/doc/README.md).
These are deliberation about a plan, which is what this directory holds.

| file | described | what exists instead |
|---|---|---|
| `runtime-threading_model.md` | two-tier reactor: network pool + content pool, SPSC rings per session | single-tier per-worker loop, [`server-uring/doc/10`](../../server-uring/doc/10-realtime-server-architecture.md) |
| `runtime-thread_context.md` | per-thread context object | `runtime/thread` + `server-uring` `thread_ctl` |
| `network-session.md` | `Session` object with embedded rings, `ObjectPool<Session>` | planned SoA session, not built |
| `network-packet_header.md` | 8-byte `{size, opcode, sequence, flags, version}` | nothing in the engine; 4-byte `[u16 len][u16 type]` on the wire in `server-epoll` and `client-bench` |
| `network-cs_packet.md`, `network-sc_packet.md` | typed reader / writer over the 8-byte header | not built |
| `network-packet_pool.md` | typed cs/sc packet pool as "tier 3" memory | `mem::packet_pool`, a 3-bucket byte pool — [`engine-uring/doc/memory/packet_pool.md`](../../engine-uring/doc/memory/packet_pool.md) |
| `memory-memory_pool.md` | 48-class TLS byte allocator | not built |
| `memory-object_pool.md` | generic object pool over a lock-free stack | not built |
| `memory-leak_tracker.md` | debug allocation tracer | not built |
| `sync-lock_free_stack.md` | Treiber stack with ABA tag | not built |
| `sync-sync_primitives.md` | overview of atomic / mutex / guards | superseded by the per-unit `sync/atomic.md` and `sync/mutex.md` |
| `sds-serial_buffer.md` | fixed contiguous serialization scratch | not built |
| `diagnostic-profiler_deadlock.md` | lock-order cycle detector | not built |

File names are the old `doc/<category>/<unit>.md` paths flattened with `-`.
Internal links between them still use the old relative paths and will not
resolve; they are left as written, per the directory's rule that dated
material is not revised.
