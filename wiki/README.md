# `wiki/` — per-source-file design specs

One markdown file per planned source file in `src/`. The wiki tree
mirrors `src/<category>/` exactly. Each file is self-contained: it
documents the contract, design choices, concurrency/ownership
constraints, test plan, and open questions for one type or one tightly
scoped subsystem.

Generic data structures (`sds/`) live in their own `sds::` namespace,
deliberately decoupled from `iouring_net::` so they can be reused in
other projects. Domain-coupled subsystems (`network/`, `runtime/`, etc.)
live in `iouring_net::`. See `wiki/sds/README.md` if/when sds/ grows
its own index.

For cross-cutting topics (build, kernel, CI, coding style), see
`docs/`.

## Layout — wiki ↔ src mapping

| Wiki spec                                                           | Source files (planned)                                       | Subsystem                  |
|---------------------------------------------------------------------|--------------------------------------------------------------|----------------------------|
| [`sds/ring_buffer.md`](sds/ring_buffer.md)                          | `src/sds/ring_buffer.{h,cpp}`                                | growable circular byte buffer (v1 landed) |
| [`sds/cstr_hash_map.md`](sds/cstr_hash_map.md)                      | `src/sds/cstr_hash_map.{h,cpp}`                              | hash map keyed by `.rodata` literals |
| [`sds/malloc_vector.md`](sds/malloc_vector.md)                      | `src/sds/malloc_vector.h`                                    | malloc/free-backed vector for trivial records |
| [`sds/serial_buffer.md`](sds/serial_buffer.md)                      | `src/sds/serial_buffer.{h,cpp}`                              | sequential write buffer    |
| [`memory/memory_pool.md`](memory/memory_pool.md)                    | `src/memory/memory_pool.{h,cpp}`                             | 48-class size-bucket pool  |
| [`memory/object_pool.md`](memory/object_pool.md)                    | `src/memory/object_pool.{h,cpp}`                             | typed pool over the 48-class allocator |
| [`memory/leak_tracker.md`](memory/leak_tracker.md)                  | `src/memory/leak_tracker.{h,cpp}`                            | global allocation accounting |
| [`sync/sync_primitives.md`](sync/sync_primitives.md)                | `src/sync/{mutex,shared_mutex}.h` (`lnx::` namespace)        | sync primitive overview |
| [`sync/atomic.md`](sync/atomic.md)                                  | `src/sync/atomic.h`                                          | Interlocked-style atomic wrappers (v1 landed) |
| [`sync/lock_free_stack.md`](sync/lock_free_stack.md)                | `src/sync/lock_free_stack.{h,cpp}`                           | Treiber stack (replaces Win32 `SLIST`) |
| [`diagnostic/profiler_deadlock.md`](diagnostic/profiler_deadlock.md) | `src/diagnostic/profiler_deadlock.{h,cpp}`                  | lock-order graph + cycle detection |
| [`diagnostic/profiler_scope.md`](diagnostic/profiler_scope.md)      | `src/diagnostic/profiler_scope.{h,cpp}`                      | RAII scope-timing profiler |
| [`runtime/thread.md`](runtime/thread.md)                            | `src/runtime/thread.{h,cpp}`                                 | pthread-backed native thread wrapper |
| [`runtime/thread_context.md`](runtime/thread_context.md)            | `src/runtime/thread_context.{h,cpp}`                         | TLS context per network/content thread |
| [`runtime/threading_model.md`](runtime/threading_model.md)          | *(project-wide constraint)*                                  | two-tier reactor + three-tier memory model |
| [`network/packet_header.md`](network/packet_header.md)              | `src/network/packet_header.h`                                | 8-byte wire header on every packet |
| [`network/session.md`](network/session.md)                          | `src/network/{session,session_pool}.{h,cpp}`                 | pre-allocated session slot with embedded recv/send ring buffers |
| [`network/packet_pool.md`](network/packet_pool.md)                  | `src/network/packet_pool.{h,cpp}`                            | per-content-thread pre-allocated cs_packet/sc_packet pool |
| [`network/cs_packet.md`](network/cs_packet.md)                      | `src/network/cs_packet.{h,cpp}`                              | typed reader over an incoming packet (client → server) |
| [`network/sc_packet.md`](network/sc_packet.md)                      | `src/network/sc_packet.{h,cpp}`                              | typed writer for an outgoing packet (server → client) |

## Conventions

- Each wiki file's section structure: `Public surface` → `Linux design`
  / `Behavior` → `Concurrency & ownership` → `Test plan` → `Open
  questions`. Not all sections are required, but if a file has them
  they appear in this order.
- Cross-references use `wiki/<category>/<file>.md` paths from the
  repo root, so they resolve in any markdown viewer.
- Code snippets are illustrative C++20 — they may use the project
  `expected` alias and `fmt::` formatting; see `docs/04-coding-style.md`
  for the binding declarations.
- Open questions in a wiki file are *that subsystem's* open
  questions. Cross-cutting open questions live in `docs/02-build-and-toolchain.md`.

## Out of scope for v1

`indexed_heap`, `guard_overflow`, and the `SelectServer` Python codegen
pipeline are documented in
`docs/00-overview.md` § "Subsystem inventory" with status `Defer (v2)`
or noted as game-side use cases. They are not covered by wiki specs in v1.
