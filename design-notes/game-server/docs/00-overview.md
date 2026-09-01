# 00 — Overview

> **⚠ Partially superseded (2026-07-04).** Written 2026-05-14, before
> the library's pivot (no coroutines; supervisor/acceptor/worker
> thread model; chat server moved into the lib repo). See
> [`08-architecture-pivot.md`](08-architecture-pivot.md) for what
> survives; the codegen pipeline, wire format, and repo-split tenets
> here remain valid.

Entry point for the `iouring-net-server` design. Defines mission,
scope, the application-layer architecture map, design tenets, and a
glossary. Read alongside
[`iouring-net-lib/docs/00-overview.md`](../../../engine-uring/doc/00-overview.md);
that document defines the layered map below the Application line, this
one defines what sits on top.

---

## Goal in one sentence

A reference game-server-style product that demonstrates the
`iouring-net-lib` library end-to-end — **header-normalized
payload-byte parity** with the Windows reference repos (see tenet 3
below for the full scope), a codegen-driven packet pipeline, and an
honest two-process integration test — built strictly through the
library's public `find_package` surface.

---

## Mission and portfolio role

The library proves that Linux io_uring + C++20 coroutines can host the
same engine primitives as the Windows IOCP reference. The server
proves the library is **actually consumable**: it exists outside the
library's source tree, builds from a clean install, and runs a real
protocol against a real client.

A reviewer can:
1. Clone `iouring-net-lib`, `cmake --install` it to a local prefix.
2. Clone `iouring-net-server`, `find_package` against that prefix.
3. Run the server binary, connect with the client binary, watch
   packets round-trip through the library's reactor.

That round-trip is the proof. Everything in this repo serves it.

---

## Application-layer architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  server/main.cpp           client/main.cpp                       │
│  - parse args              - parse args                          │
│  - construct service       - construct client_session            │
│  - bind to handlers        - bind to proxies                     │
│  - run reactor             - run reactor                         │
├──────────────────────────────────────────────────────────────────┤
│  Generated stub layer            Generated proxy layer           │
│  - stub_X(session&, id, body)    - send_Y(session_handle h,      │
│  - register_all(dispatcher)         fields...)                   │
│                                  - serializes to wire framing   │
│                                  - co_await h.send(buf)          │
├──────────────────────────────────────────────────────────────────┤
│  packet_dispatcher               Concrete client logic           │
│  (server/dispatch.{h,cpp})       (move, attack, sync test)       │
│  - dense 65536-slot table        - drives the protocol           │
│  - reject_unknown default                                        │
│                                                                  │
│  Concrete handle_X handlers      (free functions)                │
│  (server/handlers/*.cpp)                                         │
├──────────────────────────────────────────────────────────────────┤
│  iouring_net::            (library surface, find_package'd)      │
│  - service / listener / session / session_handle                 │
│  - packet_framing (deframer + frame_view) — NO dispatcher        │
│  - reactor / task<T> / job_queue                                 │
│  - sds primitives / memory pool / diagnostics                    │
└──────────────────────────────────────────────────────────────────┘
```

The dotted line between "Generated stub layer" and "Concrete
`handle_X` handlers" is the **only** code path the developer
hand-writes per packet. Adding a new packet is a single edit to
`proto/packets.json` plus implementing one free-function handler.
The dispatcher (`packet_dispatcher`) is **product-side**, not in
the library — see [`../../iouring-net-lib/wiki/network/packet_handler.md`](../../../engine-uring/doc/network/)
for the rationale.

---

## Component inventory

| Component                              | Status | Reference origin                                                                              |
|----------------------------------------|--------|-----------------------------------------------------------------------------------------------|
| `server/main.cpp`                      | New    | Lecture-derived; `IOCP_Rookiss/Server/Server.cpp:1` is a stub                                  |
| `server/handlers/*.{h,cpp}`            | New    | Each concrete handler mirrors `SelectServer/TestSerialize/Logic.cpp:On*` methods               |
| `server/dispatch.{h,cpp}`              | New    | `SelectServer/TestSerialize/Stub.cpp` (generated dispatch — we hand-write the dispatcher base) |
| `client/main.cpp`                      | New    | `SelectServer/.../Express.cpp` analog (test driver, not user-facing client)                    |
| `proto/packets.json`                   | Port   | `SelectServer/TestSerialize/packets.json` — schema format preserved, magic byte dropped         |
| `codegen/rpc_gen.py`                   | Port   | `SelectServer/TestSerialize/rpc_gen.py` (14 lines — orchestrator)                              |
| `codegen/stub_gen.py`                  | Port   | `SelectServer/TestSerialize/stub_gen.py` (76 lines)                                            |
| `codegen/proxy_gen.py`                 | Port   | `SelectServer/TestSerialize/proxy_gen.py` (94 lines)                                           |
| `generated/server_stub.{h,cpp}`        | Build  | output of `stub_gen.py`                                                                        |
| `generated/client_proxy.{h,cpp}`       | Build  | output of `proxy_gen.py`                                                                       |
| `generated/packet_ids.h`               | Build  | numeric ID enum, sourced from `packets.json`                                                   |
| `tests/integration/*`                  | New    | No reference — replaces the manual Visual Studio "run F5 in both projects" workflow            |

**Status legend** matches the library's `00-overview.md`:
- **Port** — design lifted from a reference with the platform layer swapped.
- **New** — designed from first principles for this repo.
- **Build** — generated artifact, gitignored.

---

## Design tenets

1. **Library is the only library.** No third-party network code, no
   second wire codec, no parallel framing implementation. If the
   library is missing something the product needs, fix the library
   first.

2. **Codegen-driven packet handling.** Every packet has an ID, a
   schema, and a generated stub. Hand-written packet parsing is a
   code-review red flag. Adding a new packet is a JSON edit plus a
   handler method, never a parser change.

3. **Wire-format parity with the Windows reference — payload-level
   parity under header normalization.** The framing header is
   **deliberately upgraded** from the reference's 3-byte
   `[0x89][u8 size][u8 type]` to the Linux 4-byte
   `[u16 size][u16 id]` (giving us 65 535 IDs vs 256, and 65 531
   byte payloads vs 255). The parity claim is therefore at the
   **payload-byte level**, not the frame-byte level: a packet with
   the same fields serializes the same payload bytes on both sides,
   given identical schema, little-endian hosts, schema IDs ≤ 255,
   schema payloads ≤ 255, and verified field offsets — see
   [`04-protocol.md`](04-protocol.md) § "Parity with the Windows
   reference" for the full precondition list.

   The cross-platform proof is **header-normalized trace replay**:
   recorded Windows traces are run through a small normalization step
   that re-emits the 3-byte header as a Linux 4-byte header before
   replay. Stripping the 0x89 magic byte alone is **not** sufficient,
   and v1 does **not** support an unmodified Windows client speaking
   to the Linux server end-to-end.

4. **`expected<T, std::error_code>` everywhere I/O can fail.** Same
   error model as the library — `tl::expected` aliased to `expected`
   via the library's `error/expected.h`. No exceptions across the
   library boundary.

5. **No `#ifdef _WIN32`.** Linux only. Cross-platform is a non-goal.
   If a piece of Windows reference code needs porting, port it cleanly
   to POSIX; do not preserve dual-compile paths.

6. **Integration tests are mandatory; two-process testing is layered.**
   In-process integration (Layer 2 in
   [`06-test-strategy.md`](06-test-strategy.md): server + client in
   the same binary on a loopback socket) gates every PR. **One
   minimal** two-process smoke test (Layer 3, server binary + client
   binary as separate processes) also gates every PR so the
   `find_package` + main-binary contract is exercised. Full E2E
   replay against recorded packet traces is nightly-only — slow
   process spawn + trace harness, not worth gating every PR on.

7. **One server, one client, one schema.** v1 ships exactly one server
   binary, one client binary, one `packets.json`. Multi-tenant
   configuration, hot reload, and runtime schema swaps are non-goals.

---

## Non-goals

- **MMO/game logic.** No rooms, no players, no GameSession, no
  DBSynchronizer. Packets like `CS_MOVE_START` exist to drive the
  protocol; their server-side behavior is whatever minimum is needed
  to broadcast a response. This is a network demo, not a game.
- **TLS / WebSocket / HTTP.** Raw TCP with the project's custom framing.
- **Multi-server fleet.** Single process, single listening socket. No
  cluster, no shard, no peering.
- **Hot reload of `packets.json`.** Codegen is a build-time step.
  Restart the server.
- **A retail-quality CLI.** `server --help` will exist; ergonomics are
  not a focus.
- **Windows compatibility.** None. Source code is Linux-only, and
  full Windows-frame-on-the-wire compatibility is **not** a goal —
  the library's framing header is intentionally wider than the
  Windows reference's (4-byte vs 3-byte). Cross-platform parity is
  at the **payload-byte level** under header normalization (see
  tenet 3 above and [`04-protocol.md`](04-protocol.md) § "Parity
  with the Windows reference").
- **Stable schema or migration story.** Bumping a packet field
  rebuilds both server and client. No version negotiation in v1.

---

## How this repo relates to the library

- **Direction of dependency.** Strictly one-way:
  `iouring-net-server` → `iouring-net-lib` → `liburing`/`fmt`/`tl::expected`.
- **How the dependency is expressed.** `find_package(iouring_net X.Y
  REQUIRED)` against a library install prefix. Never via git
  submodule, never via relative `#include` into the library's source.
- **What the library owns.** Layers 1–3 from
  [`iouring-net-lib/docs/00-overview.md`](../../../engine-uring/doc/00-overview.md):
  Primitive (memory pool, ring buffer, sync, diagnostics), Runtime
  (reactor, task<T>, job_queue), Network (service, listener,
  session, session_handle, packet_framing). **The library does NOT
  own a packet dispatcher or a codec template** — those were
  initially placed library-side but pulled into the product. See
  [`../../iouring-net-lib/wiki/network/packet_handler.md`](../../../engine-uring/doc/network/)
  for the deferral rationale.
- **What this repo owns.** Layer 4 (Application): the entire
  dispatcher (`packet_dispatcher`), generated stubs, concrete
  free-function handlers, server/client main, packet schema,
  codegen scripts.
- **Gray-zone resolutions** (documented in the library's
  `09-project-split.md`):
  - `packet_framing` deframer + `frame_view` → library
  - `session_handle` + `service::send_to` / `for_each_session` → library
  - `packet_dispatcher` (table, register, reject_unknown) → **product**
  - Generated stubs + codec layout (`packet_layout.h`) → product
  - Concrete `handle_X` handlers → product
  - Wire schema (`packets.json`) → product

---

## Build / kernel requirements (summary)

- Same toolchain as the library — see
  [`02-build-and-toolchain.md`](02-build-and-toolchain.md).
- One extra requirement: **Python 3.10+** for the codegen scripts.
  Python is build-time only; the produced server has no Python
  dependency at runtime.
- The library must be installed (or its install tree accessible via
  `CMAKE_PREFIX_PATH`) before this repo can configure.

---

## Glossary

- **Stub** — server-side generated code that reads a packet body off
  the wire, hands it to a concrete `on_X(...)` method, and writes any
  response. See `SelectServer/TestSerialize/Stub.{h,cpp}`.
- **Proxy** — client-side generated code that serializes a structured
  call (`send_CS_MOVE_START(session, dir, x, y)`) to wire bytes.
  See `SelectServer/TestSerialize/Proxy.{h,cpp}`.
- **C2S / S2C** — client-to-server / server-to-client. Packet
  direction; encoded in the schema as separate arrays in
  `packets.json`.
- **Dispatcher** — runtime component that reads the packet ID, looks
  up the registered stub, and invokes it. Owned by the server.
- **Express** — the SelectServer reference name for a script-style
  test driver that fires a sequence of packets at a server. Our
  client serves this role.
