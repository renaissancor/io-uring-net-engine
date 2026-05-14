# `iouring-net-server` — reference product on top of `iouring-net-lib`

A game-server-style product built on the `iouring-net-lib` Linux io_uring
network library. This is the **product** half of the two-repo portfolio
split documented in
[`iouring-net-lib/docs/09-project-split.md`](../iouring-net-lib/docs/09-project-split.md);
the library is consumed via `find_package(iouring_net)`, never via
relative `#include` into its source tree.

## Status

**Doc-first planning stage as of 2026-05-14.** No code, no
`CMakeLists.txt`, no executables yet. The library's network layer
(`session`, `listener`, `packet_framing`) lands first; this repo
becomes buildable in the milestone after that.

What exists now: architectural specs in `docs/` and per-component
designs in `wiki/`. Same two-tier doc layout as the library.

## Quick map

| Path                      | Purpose                                                              |
|---------------------------|----------------------------------------------------------------------|
| `docs/`                   | Cross-cutting architecture, build, codegen, test strategy            |
| `wiki/`                   | Per-source-file design specs (mirrors planned `src/` layout)         |
| (planned) `proto/`        | `packets.json` wire schema                                           |
| (planned) `codegen/`      | `rpc_gen.py`, `stub_gen.py`, `proxy_gen.py` — Linux ports            |
| (planned) `generated/`    | Codegen build outputs (gitignored)                                   |
| (planned) `server/`       | Server entry point + concrete packet handlers                        |
| (planned) `client/`       | Echo / test client                                                   |
| (planned) `tests/`        | Integration + E2E + replay tests                                     |

## Reading order

- **First-time reader:** [`docs/00-overview.md`](docs/00-overview.md) →
  [`docs/01-architecture.md`](docs/01-architecture.md) →
  [`docs/04-protocol.md`](docs/04-protocol.md).
- **CMake / install integrator:** [`docs/03-cmake.md`](docs/03-cmake.md).
- **Codegen contributor:** [`docs/05-codegen.md`](docs/05-codegen.md) →
  [`wiki/codegen/pipeline.md`](wiki/codegen/pipeline.md).
- **Test author:** [`docs/06-test-strategy.md`](docs/06-test-strategy.md).

## Toolchain

Identical to the library — C++20 locked, gcc 12+ / clang 14+, Ubuntu
22.04+ / Debian 12+ / RHEL 9+, kernel 5.19+ recommended. WSL2 is fine
for development. See
[`iouring-net-lib/docs/02-build-and-toolchain.md`](../iouring-net-lib/docs/02-build-and-toolchain.md)
for the authoritative version matrix; this repo's
[`docs/02-build-and-toolchain.md`](docs/02-build-and-toolchain.md)
records the product-specific additions (Python 3.10+ for codegen).

## License

TBD; will match the library.
