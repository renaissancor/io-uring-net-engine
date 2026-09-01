# `server/main.cpp`

## Purpose

Process entry point. Parses CLI flags, builds the dispatcher, registers
generated stubs, constructs the `server_lifecycle`, runs it, and
returns the appropriate exit code.

Does **not** implement any packet handling — that's
[`handlers.md`](handlers.md). Does **not** speak the protocol — that's
the library's `packet_framing`.

## Interface

CLI:
```
server [--port N]            Default: 7777
       [--bind ADDR]         Default: 0.0.0.0
       [--max-sessions N]    Default: 1024
       [--force-shutdown]    SIGINT closes connections immediately
       [--pid-file PATH]     For test orchestration
       [--log-level LEVEL]   trace|debug|info|warn|error (default: info)
```

Exit codes:
- `0` — clean shutdown via SIGINT/SIGTERM
- `1` — argument parsing error
- `2` — bind/listen failure
- `3` — reactor failure (one of the `iouring_net::service` invariants tripped)
- `>=128` — terminated by signal `N - 128` (POSIX convention)

## Implementation skeleton

```cpp
#include <iouring_net/service.h>
#include "lifecycle.h"
#include "dispatch.h"
#include "../generated/server_stub.h"

int main(int argc, char** argv) {
  auto cfg = parse_args(argc, argv);
  if (!cfg) { fmt::println(stderr, "{}", cfg.error()); return 1; }

  install_signal_handler();    // SIGINT/SIGTERM → stop_token

  packet_dispatcher dispatcher;
  iouring_server::generated::register_all(dispatcher);

  server_lifecycle lc(*cfg, dispatcher);
  if (auto r = lc.bind();    !r) { fmt::println(stderr, "bind: {}",   r.error().message()); return 2; }
  if (auto r = lc.listen();  !r) { fmt::println(stderr, "listen: {}", r.error().message()); return 2; }

  // Print the KERNEL-ASSIGNED port via lifecycle.bound_port(),
  // not cfg->port. With --port 0 the config field is literally 0;
  // the actual bound port only exists after bind(). E2E scrapes
  // this line for the port to connect to, so it has to be real.
  fmt::println("listening on {}:{}", cfg->bind_addr, lc.bound_port());

  iouring_net::run_until_complete(lc.run());
  return 0;
}
```

## Invariants

- `register_all` runs **before** `bind` so the dispatcher table is
  fully populated by the time the first packet arrives.
- The "listening on …" line is on stdout, single line, line-buffered.
  The E2E harness greps for it as a readiness signal — keep the
  format stable.
- No coroutine is spawned from `main()` directly except via
  `lc.run()`. No background threads.

## Reference origin

- `~/CLionProjects/IOCP_Rookiss/Server/Server.cpp:1` is a stub (no
  implementation); this main is fresh.
- `~/CLionProjects/SelectServer/TestSerialize/main.cpp` is a Windows
  Winsock setup that this main replaces wholesale.

## Cross-references

- [`lifecycle.md`](lifecycle.md) — what `server_lifecycle` does
- [`dispatch.md`](dispatch.md) — the dispatcher being built
- [`../../docs/01-architecture.md`](../../docs/01-architecture.md) §
  "Lifecycle" — sequence diagram
