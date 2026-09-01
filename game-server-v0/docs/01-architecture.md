# 01 — Architecture

> **⚠ Superseded in its runtime model (2026-07-04).** The
> coroutine/`task<T>`/`session` API this document builds on was
> dropped by the library; the locked model is supervisor + acceptor +
> N workers with per-worker io_uring and SPSC copy-via-inbox. The
> module decomposition, codegen split, and dispatcher design remain
> directionally valid (dispatcher becomes per-worker). See
> [`08-architecture-pivot.md`](08-architecture-pivot.md).

Component-level architecture of `iouring-net-server`. Where
`00-overview.md` introduces the application layer at a sketch level,
this document specifies the concrete components, their interfaces,
the lifecycle that ties them together, and the threading model.

---

## Module decomposition

```
iouring-net-server/
├── proto/
│   └── packets.json             # wire schema (hand-edited)
│
├── codegen/
│   ├── rpc_gen.py               # orchestrator
│   ├── stub_gen.py              # emits generated/server_stub.{h,cpp}
│   └── proxy_gen.py             # emits generated/client_proxy.{h,cpp}
│
├── generated/                   # build artifact (gitignored)
│   ├── packet_ids.h             # enum of packet IDs
│   ├── server_stub.h / .cpp     # parse + dispatch
│   └── client_proxy.h / .cpp    # serialize + send
│
├── server/
│   ├── main.cpp                 # entry point
│   ├── lifecycle.{h,cpp}        # server_lifecycle: bind/listen/run/shutdown
│   ├── dispatch.{h,cpp}         # packet_dispatcher (owns the id→handler table)
│   └── handlers/
│       ├── move_handler.{h,cpp}
│       ├── attack_handler.{h,cpp}
│       └── sync_handler.{h,cpp}
│
├── client/
│   └── main.cpp                 # test driver
│
└── tests/
    ├── unit/                    # codegen output correctness
    ├── integration/             # server + client in one process
    └── e2e/                     # two-process replay tests
```

The directory layout is **planned**, not yet on disk. The CMake target
graph below names what each directory will compile into.

---

## CMake target graph

```
                      ┌──────────────────────┐
                      │ iouring_net::iouring_│
                      │ net (find_package'd) │
                      └──────────┬───────────┘
                                 │ PUBLIC link
                  ┌──────────────┴──────────────┐
                  ▼                             ▼
        ┌─────────────────┐           ┌───────────────────┐
        │ generated lib   │           │ generated lib     │
        │ (server_stub)   │           │ (client_proxy)    │
        └────────┬────────┘           └─────────┬─────────┘
                 │                              │
                 ▼                              ▼
        ┌─────────────────┐           ┌───────────────────┐
        │ server          │           │ client            │
        │ executable      │           │ executable        │
        └─────────────────┘           └───────────────────┘
```

- `generated/` is split into two interface libraries
  (`iouring_server_stub`, `iouring_client_proxy`) so the codegen output
  is testable independently of the server/client mains.
- Both stub and proxy `PUBLIC`-link `iouring_net::iouring_net`. The
  generated headers `#include <iouring_net/session.h>` and the
  fixed-size types from `iouring_net/types.h`.
- `server` and `client` executables `PRIVATE`-link their respective
  generated library plus `iouring_net::iouring_net`. They never
  include each other.

---

## Component specs

### `server_lifecycle` (`server/lifecycle.{h,cpp}`)

Owns the runtime objects for one server process: an
`iouring_net::service`, an `iouring_net::listener`, and a
`packet_dispatcher`. Knows nothing about packet *content* — only the
order of operations.

```cpp
class server_lifecycle {
public:
  server_lifecycle(config cfg, packet_dispatcher& dispatcher);
  expected<void, std::error_code> bind();      // socket(), bind()
  expected<void, std::error_code> listen();    // listen()
  iouring_net::task<void> run();               // accept loop coroutine
  void request_stop();                         // wired to signal handler
private:
  iouring_net::service service_;
  iouring_net::listener listener_;
  packet_dispatcher& dispatcher_;
  std::stop_source stop_;
};
```

Run loop is a single coroutine: accept → spawn per-session coroutine →
back to accept. The per-session coroutine reads packets via
`iouring_net::session::recv_packet()` (which internally uses the
library's `packet_framing` deframer) and hands each to the dispatcher.

### `packet_dispatcher` (`server/dispatch.{h,cpp}`)

The runtime side of the codegen split. Maintains a table of
`packet_id → on_packet(session&, uint16_t id, std::span<const std::byte>)`
function pointers populated at startup by the generated stub
registration code.

```cpp
class packet_dispatcher {
public:
  using handler_fn =
      iouring_net::task<void>(*)(iouring_net::session&,
                                  uint16_t id,
                                  std::span<const std::byte>);

  packet_dispatcher() noexcept;        // fills every slot with reject_unknown
  void register_handler(uint16_t id, handler_fn fn);
  iouring_net::task<void> dispatch(iouring_net::session& s,
                                    uint16_t id,
                                    std::span<const std::byte> body) const;
private:
  std::array<handler_fn, 65536> table_;  // every slot non-null
};
```

Dense table over sparse map: 65 536 × 8 bytes = 512 KB, paid once. The
reference uses an `unordered_map<uint16_t, fn>` lookup at
`SelectServer/.../Stub.cpp:54`; we trade memory for one less
indirection on the hot path.

**Every slot is non-null at all times.** The constructor fills the
table with a sentinel `reject_unknown` handler that closes the
session on dispatch; `register_handler` swaps a slot from
`reject_unknown` to a real generated stub. `dispatch()` is therefore
unconditional — no `is_registered()` precheck in the caller, no
null-guard in the dispatcher, one indirect call on the hot path.
See [`../wiki/server/dispatch.md`](../wiki/server/dispatch.md) §
"Why default-rejector over null + caller-precheck" for the rationale.

### Generated stub (`generated/server_stub.{h,cpp}`)

Emitted by `codegen/stub_gen.py`. For each `c2s` entry in
`packets.json` it generates:
- A POD `struct CS_MOVE_START_body { uint8_t dir; uint16_t x; uint16_t y; };`
- A free function `on_CS_MOVE_START(session&, body)` that reads the
  POD off the wire (with bounds + alignment checks) and forwards to a
  user-implemented `handle_CS_MOVE_START(session&, body)`.
- A `register_all(packet_dispatcher&)` entry point called from
  `server/main.cpp` at startup.

The `handle_*` functions are forward-declared in the generated header;
the linker resolves them against the concrete handler objects.

### Concrete handlers (`server/handlers/*.{h,cpp}`)

Each file implements one or more `handle_X` functions referenced by
the generated stub. They are free functions, not class methods,
because the dispatch table is a flat array and the handler-class
pattern from the Windows reference (`Logic::GetInstance()->OnMoveStart`)
doesn't compose with coroutines as cleanly.

Coroutine signature (canonical — every spec MUST use this exact shape):
```cpp
iouring_net::task<void>
handle_CS_MOVE_START(iouring_net::session& s,
                      uint16_t id,
                      CS_MOVE_START_body body);
```

- `s` is the originating session (own-session reference; stable for
  the handler's coroutine lifetime).
- `id` is the dispatcher-passed packet id; equals
  `packet_id::CS_MOVE_START` for this handler. Used for diagnostics
  and for handler families that intentionally share an
  implementation across IDs.
- `body` is the parsed POD passed by value.

Handlers `co_await` library send primitives. They never block, never
spawn threads, never touch raw sockets.

### Generated proxy (`generated/client_proxy.{h,cpp}`)

Mirror of the stub for client code. For each `c2s` packet it generates:
- `iouring_net::task<expected<void, std::error_code>>
   send_CS_MOVE_START(iouring_net::session_handle h,
                        uint8_t dir, uint16_t x, uint16_t y);`

The function serializes its arguments into a stack-allocated wire
buffer and `co_await`s `h.send(...)`. The library's
`session_handle::send` is responsible for taking ownership / copying
the bytes before any async / cross-reactor work; the proxy returns
when `send` accepts the bytes into the recipient's send queue, not
when the peer has actually received them.

For `s2c` packets the proxy generates a parallel `on_SC_*` registration
set so the client side can hook server-pushed packets.

### Client (`client/main.cpp`)

A scripted test driver. Connects to the server, fires a sequence of
proxy calls, asserts that expected `SC_*` packets arrive. Not a
user-facing CLI — it is the integration-test client when the server
binary is the system under test.

---

## Lifecycle

```
parse args
  ↓
build packet_dispatcher
  ↓
server_stub::register_all(dispatcher)        # populates table_
  ↓
construct server_lifecycle(cfg, dispatcher)
  ↓
lifecycle.bind() → lifecycle.listen()
  ↓
co_await lifecycle.run()                     # accept loop
   ┌─────────────────────────────────────────────────┐
   │ per accepted fd:                                │
   │   construct iouring_net::session                │
   │   spawn per-session coroutine:                  │
   │     loop:                                       │
   │       co_await session.recv_packet()            │
   │         ↳ uses library's packet_framing         │
   │       co_await dispatcher.dispatch(s, id, body) │
   │         ↳ jumps to handle_X via table_[id]      │
   │       handle_X co_awaits session.send(...)      │
   └─────────────────────────────────────────────────┘
  ↓
SIGINT / SIGTERM → lifecycle.request_stop()
  ↓
service drains, sessions close, run() returns
  ↓
main returns 0
```

Shutdown is cooperative: the stop token unblocks the multishot accept,
new connections are refused, in-flight sessions finish their current
co_await round-trip, then the run loop exits. No connection is
forcibly killed unless `--force-shutdown` is set.

---

## Threading model

**v1: single thread.** One `io_uring` instance, one thread submitting
SQEs and consuming CQEs, one coroutine scheduler. This matches the
library's "one reactor per thread, optionally one thread" tenet.

**v1 reasoning:** Coroutine + io_uring already lets a single thread
saturate a 10 GbE link for this workload. Multi-threading buys
throughput at the cost of cross-thread session migration, locked
handler state, and harder integration tests. Wait for a profile that
demands it.

**Multi-thread story (v2 design only, not implemented):**
- N reactor threads, each owning a partition of sessions.
- Accept happens on thread 0; new sessions get round-robin assigned
  to thread `i = next_session_id % N`.
- A session never migrates across threads after assignment (session
  affinity is permanent for the session's lifetime).
- Cross-session messages always go through `session_handle` →
  `service::send_to(handle, bytes)`. Under v1 this resolves to a
  direct send on the same reactor; under v2 the service looks up
  the recipient's partition and either dispatches inline (same
  partition as caller) or hops through the library's `job_queue`
  (different partition). The handler API does not change between
  v1 and v2.
- **The v1 API already enforces the cross-session reference
  invariant.** Handlers receive `session&` only for their own
  session; peers are `session_handle` only (see
  [`../wiki/server/handlers.md`](../wiki/server/handlers.md) §
  "Cross-session messaging"). Capturing a peer reference across
  `co_await` is a v1 style violation **and** a v2 race (because the
  peer may be owned by another reactor, or may have closed and had
  its slot reused); the rule is enforced now so the v2 routing
  change is mechanical, not a handler-API rewrite.
- Concrete handlers must not assume the v1 single-thread invariant
  in any other way: use `thread_context` TLS rather than file-scope
  statics; do not rely on serial ordering of sends across sessions.

---

## Where state lives

| State                                  | Owner                       | Notes                                                       |
|----------------------------------------|-----------------------------|-------------------------------------------------------------|
| Per-connection recv buffer             | `iouring_net::session`      | Library-managed, fixed buffer registered with kernel        |
| Per-connection send buffer / queue     | `iouring_net::session`      | Library-managed                                             |
| Per-connection user state (game-side)  | (deferred to v2)            | v1 packets are stateless echo/broadcast; nothing to store   |
| Dispatcher table                       | `packet_dispatcher`         | Process-global, populated once at startup                   |
| Schema constants (IDs, field offsets)  | `generated/packet_ids.h`    | Compile-time                                                |
| Configuration                          | `server_lifecycle::cfg_`    | Parsed once at startup, immutable afterwards                |

No global mutable state outside the dispatcher table (which is
itself written exactly once, before the reactor starts). The library
owns all I/O state.

---

## Failure model

Three concentric rings:

1. **I/O failure** (`session.recv` returns `unexpected(error)`):
   close the session, log at info, continue. The accept loop is
   unaffected.
2. **Protocol failure** (deframer rejects bytes, e.g. payload smaller
   than the schema requires): close the session, log at warn, do not
   propagate. The library's `packet_framing` is the authority on what
   "valid framing" means.
3. **Programming error** (handler hits an assert or throws):
   `std::terminate`. We never catch unknown exceptions — they
   indicate a bug, and silencing them buries the diagnostic.

The integration tests assert ring 1 and ring 2 are recoverable; ring 3
crashes the test, which is the desired behavior.

---

## What the architecture explicitly does **not** do

- **No connection multiplexing.** One TCP connection per session, one
  session per coroutine. No shared sockets.
- **No request/response IDs.** All packets are fire-and-forget; the
  client correlates `CS_*` and `SC_*` by content. Adding an opaque
  correlation ID is a v2 schema change.
- **No back-pressure signalling at the protocol level.** When a
  session's send queue fills, the library applies its own
  backpressure (the proxy's `co_await session.send(...)` suspends).
  There is no protocol-level "slow down" packet.
- **No middleware layer.** Authentication, rate limiting, logging
  interceptors — none of that in v1. A future v2 can wrap the
  dispatcher with a chain-of-responsibility pattern; deferred.

---

## Cross-references

- [`04-protocol.md`](04-protocol.md) — the bytes on the wire
- [`05-codegen.md`](05-codegen.md) — how `generated/*` is produced
- [`wiki/server/dispatch.md`](../wiki/server/dispatch.md) — dispatcher
  spec in implementation detail
- [`wiki/server/lifecycle.md`](../wiki/server/lifecycle.md) — lifecycle
  spec in implementation detail
- [`wiki/runtime/integration.md`](../wiki/runtime/integration.md) —
  how the product wires into the library at the CMake / link level
- [`iouring-net-lib/docs/00-overview.md`](../../iouring-net-lib/docs/00-overview.md)
  — the library architecture this sits on
