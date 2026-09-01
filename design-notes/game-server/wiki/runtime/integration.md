# `runtime/integration.md` — how the product wires into the library

## Purpose

The product is a thin shell on top of the library: the dispatcher, the
handlers, the codegen output, and `main`. *How* those pieces connect
to `iouring_net::service`, `session`, `listener`, `task<T>`, and
`packet_framing` is the subject of this page.

This is the wiki spec for the seam declared in
[`../../docs/03-cmake.md`](../../docs/03-cmake.md). The library wiki
specs (when written) are the authoritative source for each library
type's interface; this page documents the **consumption pattern**,
not the library types themselves.

## Library surfaces consumed

| Library symbol                              | Used by                       | Where                                                          |
|---------------------------------------------|-------------------------------|----------------------------------------------------------------|
| `iouring_net::service`                      | `server_lifecycle`, handlers  | Owns the reactor + the session table                           |
| `iouring_net::service::send_to()`           | proxy + cross-session helpers | Route bytes to a `session_handle`; reactor-correct under v2    |
| `iouring_net::service::for_each_session()`  | broadcast helpers             | Yields `session_handle` (NOT `session&`) for each live session |
| `iouring_net::listener`                     | `server_lifecycle`            | Multishot accept                                               |
| `iouring_net::session`                      | per-session coroutine         | The handler's own session, by reference                        |
| `iouring_net::session::recv_packet()`       | per-session coroutine         | Returns `expected<frame_view, framing_error>`                  |
| `iouring_net::session::send()`              | own-session send path         | Awaitable send (handler's own session, client's own session)   |
| `iouring_net::session::handle()`            | handler, client               | Returns the `session_handle` for this session                  |
| `iouring_net::session::service()`           | handler                       | Back-reference to the owning service                           |
| `iouring_net::session::reject_and_close()`  | generated stubs, `reject_unknown` | Closes the session with a `close_reason` + offending id    |
| `iouring_net::session_handle`               | proxies + cross-session paths | Opaque, copyable, non-owning; the **only** legal way to talk to a peer |
| `iouring_net::session_handle::id()`         | handler captures, logging     | Returns the session-id half of the handle (the generation half is implementation-internal) |
| `iouring_net::session_handle::send()`       | generated proxies             | Routes through the recipient's reactor (v1: direct; v2: hop). **Library takes ownership of bytes before any async/cross-reactor work** — proxies may pass a stack span and return as soon as the recipient queue has accepted (copied) the data. Reports stale-handle, closed-session, queue-full, stopped-service via `expected`. |
| `iouring_net::close_reason` (enum)          | reject paths                  | `bad_payload_size`, `unknown_packet_id`, etc.                  |
| `iouring_net::task<T>`                      | all coroutines                | Coroutine return type                                          |
| `iouring_net::run_until_complete()`         | `main`                        | Pumps the reactor until a task completes                       |
| `iouring_net::co_spawn()`                   | `server_lifecycle::run`       | Fire-and-forget a coroutine onto the reactor                   |
| `iouring_net::expected<T, E>`               | every error path              | Library alias of `tl::expected`                                |
| `iouring_net::packet_framing` (impl)        | (transitive)                  | Powers `recv_packet`; product never calls directly             |

### Forbidden library surfaces

The library **must not** expose any API that lets the product turn a
`session_handle` back into a `session&` (or `session*`). The whole
point of the handle/reference split is to make peer raw references
unreachable from handlers. Specifically forbidden:

- `service::get_session(session_handle) -> session&` or any synonym.
- `session_handle::operator->()` returning a session pointer.
- Any "current session" thread-local that handlers can read.

Logging, metrics, and debug hooks **must** receive a
`session_handle` or its `id()`, never a `session&`. If a future
feature seems to need a raw peer reference, the right move is to
add a handle-routed method on `service` instead.

### `service::for_each_session` semantics

Snapshot-based: the iteration is logically a snapshot of the handle
table at the moment of the call. Sessions joining during the
broadcast are not included; sessions that disconnect during the
broadcast produce a stale-handle error from
`session_handle::send`, which the broadcast helper logs and
continues past. Implementations must NOT hold the session-table
lock across `co_await`.

### Dependency status (2026-05-14)

`session_handle`, `session_handle::id()`, `session_handle::send()`,
`service::send_to`, `service::for_each_session`, `session::handle()`,
`session::service()`, `session::reject_and_close()`, and the
`close_reason` enum are required by this product but **not yet
implemented** in `iouring-net-lib`. They are prerequisites for the
network-layer milestone (see
[`../../../iouring-net-lib/docs/09-project-split.md`](../../../iouring-net-lib/docs/09-project-split.md)
§ "When to split"). The product cannot build until they land.

## Build seam

```
iouring-net-lib/                       installed → $PREFIX
├── lib/libiouring_net.a
├── include/iouring_net/...             ← only legal include path
└── lib/cmake/iouring_net/
    ├── iouring_netConfig.cmake
    ├── iouring_netConfigVersion.cmake
    └── iouring_netTargets.cmake

iouring-net-server/
├── CMakeLists.txt                      find_package(iouring_net …)
├── server/
│   └── #include <iouring_net/service.h>      ← public include
└── generated/
    └── #include <iouring_net/session.h>      ← public include
```

`#include <iouring_net/...>` paths are resolved through the imported
target's `INTERFACE_INCLUDE_DIRECTORIES`, which the library installs
at `${CMAKE_INSTALL_INCLUDEDIR}/iouring_net`. Adding the `iouring_net/`
prefix prevents header collisions with any system header named
`session.h` or similar.

Headers under `iouring-net-lib/src/` that are intended as *private*
must move under `src/internal/` or live in `.cpp` files only — they
ship in the install today (every `*.h` is exported). The product
**must not depend on private headers**; if a product file `#include`s
something the library's docs don't mention, the burden is on the
library to either document it as public or hide it.

## Link seam

```cmake
# server/CMakeLists.txt
target_link_libraries(server PRIVATE
  iouring_server_stub                  # our generated stubs
  iouring_net::iouring_net)            # PROPAGATES the rest
```

The single imported target propagates:

- `Threads::Threads`
- `fmt::fmt`
- `liburing::uring`
- `tl::expected`

The product never names these targets directly. If a refactor moves
`fmt` from PUBLIC to PRIVATE in the library, the product must also
add `find_package(fmt)` — that's a library API change and would be
flagged on the library side first.

## Runtime seam

```
                         Process boundary
┌──────────────────────────────────────────────────────────────┐
│ main()                                                       │
│   build dispatcher                                           │
│   register_all(dispatcher)        ← from generated stubs     │
│   build server_lifecycle(cfg, dispatcher)                    │
│   lifecycle.bind()                                           │
│   lifecycle.listen()                                         │
│   run_until_complete(lifecycle.run())                        │
│     │                                                        │
│     │  ┌───────────────────────────────────────────────────┐ │
│     │  │ lifecycle.run() — accept loop                     │ │
│     │  │                                                   │ │
│     │  │ loop on listener_.accept(stop_token):             │ │
│     │  │   service_.adopt_session(fd) → session            │ │
│     │  │   co_spawn(handle_session(session))               │ │
│     │  │     │                                             │ │
│     │  │     │  ┌──────────────────────────────────────┐   │ │
│     │  │     │  │ handle_session                       │   │ │
│     │  │     │  │  loop: frame = co_await s.recv_packet│   │ │
│     │  │     │  │        co_await dispatcher.dispatch  │   │ │
│     │  │     │  │            → stub_X(s, body)         │   │ │
│     │  │     │  │              → handle_X(s, body)     │   │ │
│     │  │     │  │                 → co_await s.send    │   │ │
│     │  │     │  └──────────────────────────────────────┘   │ │
│     │  └───────────────────────────────────────────────────┘ │
│     │                                                        │
│     └─ on stop: service_.drain() then run() returns          │
└──────────────────────────────────────────────────────────────┘
```

The library owns:
- The `io_uring` instance, SQE submission, CQE consumption.
- The recv buffer pool (fixed buffers registered with the kernel).
- The send queue per session.
- The framing primitive (where bytes become a `frame_view`).

The product owns:
- Argument parsing, signal handling, exit-code mapping.
- The `packet_dispatcher` table + the per-packet stubs.
- Handler logic.
- The `session` lifetime decision (when does this stop being
  interesting?) — currently "until peer FIN" or "until stop".

## Error flow

```
recv_packet error  →  unexpected(framing_error / errc)
                       │
                       ▼
                 per-session loop breaks
                       │
                       ▼
                 session destructor closes fd
                       │
                       ▼
                 handle_session task ends
                       │
                       ▼
                 spawned task list shrinks
```

```
send error in handler  →  unexpected propagates out of co_await
                          │
                          ▼
                    handler returns (task<void> ignores expected)
                          │
                          ▼
                    next packet attempt on same session
                          → recv_packet sees ECONNRESET or similar
                          → loop breaks (see above)
```

No retry logic in v1. A failed send doesn't get queued for redelivery;
the session is treated as broken and torn down. This is intentional —
correctness over throughput at this stage of the project.

## Versioning interaction

The product depends on **`iouring_net` API**, not on its internal
ABI. A library minor-version bump (`0.0.1 → 0.0.2`) that adds methods
to `session` is backward-compatible. A bump that *removes* a method
the product uses is **breaking**:

1. Library tag rolls; CI fails the product's `default-noble` job
   because the product no longer compiles.
2. Product's `LIB_TAG` stays pinned to the previous tag until a fix
   PR adapts the product to the new API.
3. When the fix lands, both `LIB_TAG` and the corrected source land
   in the same commit.

This is the same "pin by tag" guarantee from
[`../../docs/07-ci.md`](../../docs/07-ci.md). The library never floats
to `main` in CI, so the product is never broken by library `main`
changes between the time the dev pulls and the time CI runs.

## Cross-references

- [`../../docs/03-cmake.md`](../../docs/03-cmake.md) — CMake-side
  spelling of this seam.
- [`../../docs/01-architecture.md`](../../docs/01-architecture.md) §
  "Lifecycle" — the lifecycle diagram this expands on.
- [`../server/lifecycle.md`](../server/lifecycle.md) — server-side
  consumer of these library types.
- [`../client/main.md`](../client/main.md) — client-side consumer
  (smaller subset of library surface).
- [`../../../iouring-net-lib/docs/09-project-split.md`](../../../iouring-net-lib/docs/09-project-split.md)
  — the architectural constraint that defines what's allowed to cross
  this seam.
