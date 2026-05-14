# `client/main.cpp`

## Purpose

A scripted test driver — not a user-facing client. Connects to a
running server, fires a sequence of `c2s` packets via the generated
proxies, and asserts that expected `s2c` packets arrive within a
deadline.

Doubles as the test harness for [`../../docs/06-test-strategy.md`](../../docs/06-test-strategy.md)
Layer 3 (E2E replay).

## Interface

CLI:
```
client [--host ADDR]         Default: 127.0.0.1
       [--port N]            Default: 7777
       [--replay PATH]       Replay a .pkts file; assert against .expected sibling
       [--record PATH]       Record sends + receives to PATH
       [--script NAME]       Run a built-in scripted scenario by name
       [--timeout-ms N]      Per-packet timeout for replay/expectation
       [--verbose]
```

Modes are mutually exclusive: `--replay`, `--record`, `--script` —
pick one. If none given, the client connects, sleeps until SIGINT,
prints any pushed packets. (Useful for manual server poking.)

Exit codes:
- `0` — script/replay completed and matched expectations
- `1` — argument error
- `2` — connect failure
- `3` — protocol error (malformed frame received)
- `4` — assertion failure (expected packet not received within timeout)
- `5` — receive a packet the schema doesn't define for s2c

## Built-in scripts

`--script smoke` (opt-in; NOT the default — no-mode is interactive
sleep per the description above):
- Connect.
- Send `CS_MOVE_START`, expect `SC_MOVE_START` echoed back **only if**
  another client is connected. Single-client smoke just verifies
  send-doesn't-error.
- Send `CS_ATTACK1`, same logic.
- Disconnect cleanly.

`--script two-client-move`:
- Spawns a second connection internally.
- Has client 1 send `CS_MOVE_START`, asserts client 2 receives
  `SC_MOVE_START` with the broadcast fields.

These scripts live in `client/scripts/*.cpp` — one TU per script.
They share the `client/connection.{h,cpp}` helper that owns one
`iouring_net::session` + a handler registry for `s2c` proxies.

## Replay format (`.pkts`)

Line-protocol, one event per line:

```
# Comment
> CS_MOVE_START dir=1 x=100 y=200          # > = send
< SC_MOVE_START id=42 dir=1 x=100 y=200    # < = expect receive
~ 50ms                                      # ~ = sleep
```

Field values use named tagging (`name=value`) so the parser doesn't
depend on schema field order — adding/reordering fields in
`packets.json` does not invalidate old replay files (as long as the
field names still exist).

Expected receives have an implicit timeout of `--timeout-ms` (default
1000). To override per-line:
```
< SC_DAMAGE attackerID=1 targetID=2 hp=80   timeout=500ms
```

The companion `.expected` file (optional) is the canonical client
log: every `<` line's actual receive is logged there, and replay mode
diffs against it. Lets test authors record the run once and check it
into the repo.

## Implementation skeleton

```cpp
int main(int argc, char** argv) {
  auto cfg = parse_args(argc, argv);
  if (!cfg) return 1;

  iouring_net::service svc;
  packet_dispatcher dispatcher;
  iouring_server::generated::register_all(dispatcher);  // registers s2c stubs

  auto run = [&]() -> iouring_net::task<int> {
    auto s_or = co_await svc.connect(cfg->host, cfg->port);
    if (!s_or) co_return 2;
    auto& s = *s_or;

    if (cfg->script)  co_return co_await run_script(s, dispatcher, *cfg->script);
    if (cfg->replay)  co_return co_await run_replay(s, dispatcher, *cfg->replay,
                                                     cfg->record_to);
    co_return co_await run_interactive(s, dispatcher);
  };

  return iouring_net::run_until_complete(run());
}
```

## Invariants

- The client uses the same dispatcher type as the server. The `s2c`
  packet handlers it registers (via generated `register_all`) are
  hand-written *expectation matchers*, not game logic. Each matcher
  pushes the received packet onto a `co_await`-able channel that
  scripts/replay consume.
- The client cannot construct a `c2s` body except via a `send_CS_*`
  proxy call. Hand-crafting bytes is forbidden — defeats the codegen
  guarantee.
- Proxy calls take a `session_handle`, not a `session&`. The client
  obtains its own handle once via `my_session.handle()` and passes
  that to every `send_CS_*` invocation. The handle is the same shape
  the server uses for peer routing (see
  [`../server/handlers.md`](../server/handlers.md) § "Cross-session
  messaging"); the client just happens to only ever target one
  handle (its own).

## Reference origin

- `~/CLionProjects/SelectServer/TestSerialize/Express.cpp:1` — script-
  style test driver, conceptual source.
- `~/CLionProjects/IOCP_Rookiss/Client/` — Windows client, structurally
  not reused.

## Cross-references

- [`../proto/packets.md`](../proto/packets.md) — what the client can send/expect
- [`../codegen/pipeline.md`](../codegen/pipeline.md) — what
  `register_all` looks like client-side
- [`../../docs/06-test-strategy.md`](../../docs/06-test-strategy.md) §
  "Layer 3 — end-to-end replay" — how this binary is invoked by CI
