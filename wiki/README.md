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
| [`sds/serial_buffer.md`](sds/serial_buffer.md)                      | `src/sds/serial_buffer.{h,cpp}`                              | sequential write buffer    |
| [`memory/memory_pool.md`](memory/memory_pool.md)                    | `src/memory/memory_pool.{h,cpp}`                             | 48-class size-bucket pool  |
| [`memory/object_pool.md`](memory/object_pool.md)                    | `src/memory/object_pool.{h,cpp}`                             | typed pool over the 48-class allocator |
| [`memory/leak_tracker.md`](memory/leak_tracker.md)                  | `src/memory/leak_tracker.{h,cpp}`                            | global allocation accounting |
| [`sync/sync_primitives.md`](sync/sync_primitives.md)                | `src/sync/{atomic,mutex,shared_mutex}.h` (`lnx::` namespace) | std::-shape primitive wrappers |
| [`sync/lock_free_stack.md`](sync/lock_free_stack.md)                | `src/sync/lock_free_stack.{h,cpp}`                           | Treiber stack (replaces Win32 `SLIST`) |
| [`diagnostic/profiler_deadlock.md`](diagnostic/profiler_deadlock.md) | `src/diagnostic/profiler_deadlock.{h,cpp}`                  | lock-order graph + cycle detection |
| [`diagnostic/profiler_scope.md`](diagnostic/profiler_scope.md)      | `src/diagnostic/profiler_scope.{h,cpp}`                      | RAII scope-timing profiler |
| [`runtime/coroutine_task.md`](runtime/coroutine_task.md)            | `src/runtime/task.{h,cpp}`                                   | C++20 coroutine `task<T>`  |
| [`runtime/job_queue.md`](runtime/job_queue.md)                      | `src/runtime/job_queue.{h,cpp}`                              | per-entity FIFO serializer |
| [`runtime/thread_context.md`](runtime/thread_context.md)            | `src/runtime/thread_context.{h,cpp}`                         | TLS context for reactor threads |
| [`network/io_uring_reactor.md`](network/io_uring_reactor.md)        | `src/network/reactor.{h,cpp}` (uses `liburing`)              | the io_uring event loop    |
| [`network/listener_and_service.md`](network/listener_and_service.md) | `src/network/{listener,service}.{h,cpp}`                    | bind/accept + reactor owner |
| [`network/session.md`](network/session.md)                          | `src/network/session.{h,cpp}`                                | one TCP connection's coroutine + buffers |
| [`network/packet_framing.md`](network/packet_framing.md)            | `src/network/packet_framing.{h,cpp}`                         | `[uint16 size │ uint16 id]` framer |
| [`network/packet_handler.md`](network/packet_handler.md)            | `src/network/packet_handler.{h,cpp}`                         | id → handler dispatch      |

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

`indexed_heap`, `malloc_vector`, `guard_overflow`, and the
`SelectServer` Python codegen pipeline are documented in
`docs/00-overview.md` § "Subsystem inventory" with status `Defer (v2)`
or noted as game-side use cases. They are not covered by wiki specs in v1.
