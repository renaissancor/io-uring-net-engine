# `server/handlers/*.{h,cpp}` — concrete packet handlers

## Purpose

The hand-written half of the codegen split. For each `c2s` packet
declared in `proto/packets.json`, the generator emits a forward
declaration `handle_X(session&, X_body)`; one file under
`server/handlers/` defines it.

## File-per-group convention

Group handlers by domain, one `.cpp` per group, not one per packet:

```
server/handlers/
├── move_handler.cpp     # handle_CS_MOVE_START, handle_CS_MOVE_STOP
├── attack_handler.cpp   # handle_CS_ATTACK1, _ATTACK2, _ATTACK3
└── sync_handler.cpp     # handle_CS_SYNC (when added)
```

Header files are not needed — the generated `server_stub.h` forward-
declares every `handle_X`, and the `.cpp` files just provide the
definitions. A handler that needs a helper invokes a free function in
the same file; cross-handler helpers live in `server/handlers/shared.h`.

## Signature

```cpp
iouring_net::task<void>
handle_CS_MOVE_START(iouring_net::session& s,
                      uint16_t id,
                      CS_MOVE_START_body body);
```

- **`session& s`** — the **originating** session (the one the
  packet was received from). The reference is stable for the
  lifetime of this coroutine because the per-session loop in
  `lifecycle.cpp` owns the `s` and outlives any handler invocation
  on it. Legal operations: `co_await s.send(...)`, `s.handle()`,
  `s.service()`, `s.id()`. **Illegal**: hold or pass any *other*
  session's reference; see § "Cross-session messaging" below.
- **`id`** — the packet ID the dispatcher routed to this handler.
  Always equal to the constant the generator emits for this packet
  (e.g., `packet_id::CS_MOVE_START`). Useful for diagnostics or
  for handler families that intentionally share an implementation
  across IDs.
- **`X_body`** — the parsed POD. By value (5–9 bytes typical; not
  worth a reference).
- **Returns `task<void>`** — handlers are coroutines so they can
  `co_await s.send(...)` or `co_await peer_handle.send(...)` on the
  broadcast/response path.

Handlers do not return errors. I/O failures bubble out of awaited
sends and close the session; protocol-level errors close the session
via the dispatcher. There is no "handler returns
`unexpected(reason)`" path because there's no recovery action to
take.

## Cross-session messaging: handles, not peer references

Handlers operate on **two distinct surfaces**:

- **Their own session, by reference (`session& s`).** The handler's
  receive coroutine runs on this session's reactor; the reference is
  stable for the lifetime of the coroutine. `co_await s.send(...)`,
  `s.handle()`, and `s.service()` are all legal.
- **Peer sessions, by `session_handle`.** A handle is an opaque,
  copyable, non-owning identifier (session-id + a generation counter
  in the implementation). Peers may live on a different reactor in
  v2; their lifetime is not pinned to any handler's execution.
  **Handlers must never hold or pass a peer's `session&` across a
  `co_await`.** The only legal way to talk to a peer is through its
  `session_handle`, routed via `service::send_to(...)` or the
  `session_handle::send(...)` convenience.

This rule makes v1→v2 mechanical: under v2 the service hops the
send onto the peer's owning reactor (sessions are *partitioned*, not
migrating — each session is assigned a reactor at construction and
stays there for its lifetime). The handler API does not change.
The rule applies in v1 too — even on a single thread — so handlers
cannot accidentally encode the single-thread invariant.

## Example: `move_handler.cpp`

```cpp
#include "../../generated/server_stub.h"
#include "../../generated/client_proxy.h"
#include <iouring_net/service.h>
#include <iouring_net/session.h>
#include <iouring_net/session_handle.h>
#include "shared.h"

using iouring_net::session_handle;
using iouring_net::task;
using namespace iouring_server::generated;

task<void>
handle_CS_MOVE_START(iouring_net::session& s, uint16_t /*id*/,
                      CS_MOVE_START_body body)
{
  const session_handle self = s.handle();
  // Broadcast SC_MOVE_START to every session except the sender.
  // `broadcast_except` iterates session_handles (NEVER session& peers);
  // each lambda invocation runs on the recipient's reactor in v2,
  // on the same thread as the caller in v1.
  co_await broadcast_except(s.service(), self,
    [self, body](session_handle peer) -> task<void> {
      co_await send_SC_MOVE_START(peer, self.id(),
                                   body.dir, body.x, body.y);
    });
}

task<void>
handle_CS_MOVE_STOP(iouring_net::session& s, uint16_t /*id*/,
                     CS_MOVE_STOP_body body)
{
  const session_handle self = s.handle();
  co_await broadcast_except(s.service(), self,
    [self, body](session_handle peer) -> task<void> {
      co_await send_SC_MOVE_STOP(peer, self.id(),
                                  body.dir, body.x, body.y);
    });
}
```

Key points:

- The generated proxy `send_SC_MOVE_START` takes a `session_handle`,
  not a `session&`. (See [`../../docs/05-codegen.md`](../../docs/05-codegen.md)
  § "Generated proxy" for the signature.)
- `broadcast_except(service, exclude_self, fn)` lives in `shared.h`.
  It walks the service's handle table, calls `fn(peer_handle)` for
  every handle that is not `exclude_self`, and awaits all the
  resulting `task<void>`s. Error in any one peer's send does not
  abort the broadcast.
- The lambda captures only `self` (the sender's handle) and `body`
  by value — never a `session&`. This is what keeps the v2 path
  open.

## Invariants

1. **A handler never blocks.** Synchronous `read`/`write`/`sleep` are
   forbidden. Use `co_await s.send(...)` or library timer primitives.
2. **A handler never spawns a detached thread.** All work happens on
   the reactor's thread, via coroutines.
3. **A handler never reads `body` past `sizeof(X_body)`.** The
   generator's stub already validated size; the handler operates on
   a fully-decoded POD.
4. **A handler never mutates `body`.** It's passed by value — but
   stylistically, treat it as `const` because most field updates
   want a corresponding `send_SC_*` for the canonical state.
5. **A handler never holds, captures, or passes a peer's `session&`
   across `co_await`.** Peers are identified by `session_handle`
   only; cross-session sends go through `peer_handle.send(...)` or
   `service.send_to(peer_handle, ...)`. The reason: a peer may be
   owned by another reactor (v2 partitioning), or may have closed
   and had its id slot reused (v1 or v2). Either case turns a
   captured peer `session&` resumed after `co_await` into a race
   condition or a use-after-free; `session_handle`'s generation
   counter makes the staleness detectable, and routing through
   `service::send_to` keeps every peer touch on the peer's owning
   reactor.
6. **A handler that broadcasts uses the library's
   handle-iterating helpers.** No hand-rolled "iterate all
   sessions" loop, and no helper that yields `session&` peers.
   The library's `service::for_each_session` yields
   `session_handle` only; same for `broadcast_except` in
   `shared.h`.

## Idempotence note

v1 packets are intentionally idempotent at the protocol level: a
duplicate `CS_MOVE_START` produces a duplicate `SC_MOVE_START`
broadcast. There's no de-duplication state, no sequence-number
tracking. This keeps handlers stateless and lets the test layer
exercise them with simple replay.

When v2 adds stateful handlers (game logic, login, etc.), the per-
session state lives in `iouring_net::session::user_data<T>()`
(library extension — to be added) and not in any handler-side global.

## Reference origin

- `~/CLionProjects/SelectServer/TestSerialize/Logic.cpp:1` — singleton
  `Logic::OnMoveStart(...)` is the reference's handler-class pattern.
  We use free functions instead; explanation in
  [`../../docs/05-codegen.md`](../../docs/05-codegen.md) § "Differences
  from the reference schema."

## Cross-references

- [`dispatch.md`](dispatch.md) — how a handler is reached at runtime
- [`../codegen/pipeline.md`](../codegen/pipeline.md) — what the
  generator emits that calls these handlers
- [`../../docs/05-codegen.md`](../../docs/05-codegen.md) — the broader
  codegen contract
