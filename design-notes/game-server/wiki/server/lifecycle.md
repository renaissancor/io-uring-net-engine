# `server/lifecycle.{h,cpp}` — `server_lifecycle`

## Purpose

The runtime owner of one server process. Holds the
`iouring_net::service`, the `iouring_net::listener`, and a non-owning
reference to the `packet_dispatcher`. Sequences bind → listen → accept
loop → shutdown.

Stateless about packets. Stateful only about sockets and the stop
token.

## Interface

```cpp
// server/lifecycle.h
#pragma once
#include <iouring_net/service.h>
#include <iouring_net/listener.h>
#include <iouring_net/task.h>
#include <iouring_net/error/expected.h>
#include <stop_token>
#include <string>

class packet_dispatcher;  // fwd; see dispatch.h

struct server_config {
  std::string bind_addr   = "0.0.0.0";
  uint16_t    port        = 7777;
  uint32_t    max_sessions = 1024;
  bool        force_shutdown = false;
};

class server_lifecycle {
public:
  server_lifecycle(server_config cfg, packet_dispatcher& dispatcher);

  iouring_net::expected<void, std::error_code> bind();
  iouring_net::expected<void, std::error_code> listen();
  iouring_net::task<void> run();
  void request_stop() noexcept;

  // For tests: returns the kernel-assigned port when cfg_.port == 0.
  uint16_t bound_port() const noexcept;

private:
  iouring_net::task<void> handle_session(iouring_net::session s);

  server_config       cfg_;
  packet_dispatcher&  dispatcher_;
  iouring_net::service  service_;
  iouring_net::listener listener_;
  std::stop_source    stop_;
  uint16_t            bound_port_{0};
};
```

## Invariants

1. **`bind` and `listen` are called exactly once each, in that order,
   before `run`.** Calling them out of order returns
   `errc::operation_not_permitted`. The class does not enforce
   ordering for callers other than `main`; tests construct via a
   helper that asserts the sequence.
2. **`run()` is a single coroutine, not a thread.** It executes on
   the caller's thread. The library's reactor pumps the
   `io_uring` SQ/CQ; we do not spin up our own.
3. **The accept loop is multishot.** One `IORING_OP_ACCEPT_MULTISHOT`
   SQE is submitted at `run()` start; CQEs flow until the stop token
   fires.
4. **A per-session coroutine is detached, not joined.** When
   `handle_session` returns, the session destructor closes the fd
   and frees the recv buffer back to the library's pool. The
   lifecycle does not track outstanding sessions individually —
   that's the service's job via `max_sessions`.
5. **`request_stop()` is safe to call from any thread.** It is
   **not** async-signal-safe in itself — `std::stop_source` /
   `std::stop_token` are not on the C++ async-signal-safe whitelist.
   The signal handler installed in `main()` therefore does **not**
   call `request_stop()` directly. Instead it uses one of:
   - `signalfd(2)` integrated with the reactor's `io_uring`-driven
     event loop (preferred — keeps the SIGINT path on the same code
     path as I/O), OR
   - a self-pipe `write(2)` from the handler (POSIX-portable
     fallback if `signalfd` becomes problematic), OR
   - the bare-minimum-correct path: the handler sets a
     `sig_atomic_t volatile g_stop = 1;` and the main loop polls it
     between accept iterations.
   `request_stop()` is invoked from the post-handler path (after
   the reactor notices the wake-up), not from the handler itself.

## Implementation notes

### Accept loop

```cpp
iouring_net::task<void> server_lifecycle::run() {
  while (!stop_.get_token().stop_requested()) {
    auto fd_or = co_await listener_.accept(stop_.get_token());
    if (!fd_or) {
      if (stop_.get_token().stop_requested()) break;
      log_warn("accept failed: {}", fd_or.error().message());
      continue;
    }
    auto s = service_.adopt_session(*fd_or);
    co_spawn(handle_session(std::move(s)));   // fire-and-forget
  }
  co_await service_.drain();
}
```

`co_spawn` (library helper) launches the per-session coroutine into
the same reactor without making the parent wait. `drain()` blocks
until every spawned coroutine finishes its current await point.

### Per-session loop

```cpp
iouring_net::task<void>
server_lifecycle::handle_session(iouring_net::session s) {
  for (;;) {
    auto frame = co_await s.recv_packet(stop_.get_token());
    if (!frame) break;   // peer closed or stop requested
    co_await dispatcher_.dispatch(s, frame->id, frame->body);
  }
}
```

The dispatcher returns a `task<void>`; awaiting it serializes one
packet at a time per session. Out-of-order handling within a session
is not supported in v1 (and the protocol doesn't require it).

**No `is_registered()` precheck here.** The dispatcher's table has
no null slots (the constructor fills every slot with a
`reject_unknown` handler that closes the session). The per-session
loop calls `dispatch()` unconditionally and lets the table own the
unknown-ID policy. Adding a precheck would split authority between
this file and `dispatch.cpp` — see
[`dispatch.md`](dispatch.md) § "Why default-rejector over null +
caller-precheck."

### Force-shutdown vs cooperative

`request_stop()` flips the token. The accept loop notices on its
next iteration. In-flight per-session coroutines notice on their
next `recv_packet`. With `--force-shutdown`, the service also
shutdowns sockets explicitly, which causes pending `recv` to fail
fast with `ECANCELED`.

## Load-test posture (deferred)

For v1 the lifecycle has no built-in load-test mode. Tools to use
when the time comes:
- `tcpkali` — high-rate connection generator
- `wrk` — sustained HTTP-style load (less applicable; binary protocol)
- A custom replay client that fires `proto/` packets at configurable rates

Aspiration, not v1 scope.

## Reference origin

- Conceptual mirror of `~/CLionProjects/IOCP_Rookiss/Server/Server.cpp`
  (which is a stub — the real shape was lecture material).
- The `request_stop` / token-driven shutdown is fresh; the Windows
  reference used a global flag and `WaitForSingleObject`.

## Cross-references

- [`dispatch.md`](dispatch.md) — the dispatcher this lifecycle owns
- [`main.md`](main.md) — how `server_lifecycle` is constructed
- [`../../docs/01-architecture.md`](../../docs/01-architecture.md) §
  "Threading model" and § "Lifecycle"
- `iouring_net::service`, `iouring_net::listener` — library types
  this consumes (library wiki, when implemented)
