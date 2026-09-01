# `docs/` — server-wide design and operational documentation

Cross-cutting layer. Per-source-file design specs live in `wiki/`.

The numbering parallels the library's `docs/` so the two repos are
read as a pair. Where a document overlaps a library doc, this repo's
version covers the **product-specific** additions and references the
library doc for shared content.

## What each doc covers

| File                                                | Audience               | Subject                                                                                                |
|-----------------------------------------------------|------------------------|--------------------------------------------------------------------------------------------------------|
| [`00-overview.md`](00-overview.md)                  | everyone, first        | Mission, application-layer architecture map, design tenets, non-goals, glossary.                       |
| [`01-architecture.md`](01-architecture.md)          | design reader          | Component-level architecture: session glue, dispatcher, handler hierarchy, threading, where state lives. |
| [`02-build-and-toolchain.md`](02-build-and-toolchain.md) | implementor       | Product-specific toolchain additions on top of the library floor (Python 3.10+, codegen deps).        |
| [`03-cmake.md`](03-cmake.md)                        | implementor            | `find_package(iouring_net)` consumption, pre-build codegen step, presets mirroring the library.       |
| [`04-protocol.md`](04-protocol.md)                  | protocol implementor   | Wire framing, packet ID ranges, endianness, max packet size, parity with library's framing.           |
| [`05-codegen.md`](05-codegen.md)                    | codegen contributor    | `packets.json` schema, `rpc_gen.py` / `stub_gen.py` / `proxy_gen.py` roles, CMake integration.        |
| [`06-test-strategy.md`](06-test-strategy.md)        | test author            | Integration + E2E + replay testing, sanitizer policy, fuzz harness for deframer.                       |
| [`07-ci.md`](07-ci.md)                              | release engineer       | CI matrix, library-tag pinning, reproducibility envelope.                                              |

## Suggested reading order

- **First-time reader:** `00` → `01` → `04`. After that any wiki page reads independently.
- **CMake / install integrator:** `03` → `02` (kernel + lib version).
- **Codegen contributor:** `05` → `wiki/codegen/pipeline.md` → `wiki/proto/packets.md`.
- **Writing a handler:** `wiki/server/dispatch.md` → `wiki/server/handlers.md`.
- **Writing a test:** `06` → relevant wiki spec.

## Cross-repo references

The library's authoritative docs that this repo depends on:
- [`iouring-net-lib/docs/00-overview.md`](../../iouring-net-lib/docs/00-overview.md) — layered subsystem map
- [`iouring-net-lib/docs/02-build-and-toolchain.md`](../../iouring-net-lib/docs/02-build-and-toolchain.md) — toolchain floor
- [`iouring-net-lib/docs/09-project-split.md`](../../iouring-net-lib/docs/09-project-split.md) — the boundary that defines what lives where
- [`iouring-net-lib/wiki/network/packet_framing.md`](../../iouring-net-lib/wiki/network/packet_framing.md) — the framing primitive this product consumes (when it lands)
