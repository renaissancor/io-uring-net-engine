# 08 — Architecture pivot (2026-07) and testing architecture as-built

Reconciles this repo's May-2026 design docs with what `iouring-net-lib`
actually became, and records the scaffold + testing architecture that
landed on 2026-07-04. Where this document contradicts `00`–`07`, this
document wins.

---

## What changed in the library after these docs were written

Docs `00`–`07` were written 2026-05-14. Between 2026-05-16 and
2026-06-07 the library locked a different runtime model (see
`iouring-net-lib/.omc/wiki/` decision pages and
`docs/discussions/`):

| May-14 assumption (docs 00–07)                  | Library reality today                                                     |
|--------------------------------------------------|----------------------------------------------------------------------------|
| C++20 coroutines: `task<T>`, `co_await` send/recv | **No coroutines.** Removed from the design (`738d2bb`).                    |
| `service` / `listener` / `session` public API     | **Not exported.** Public surface is primitives only (see below).           |
| Single-thread v1 reactor                          | **Supervisor + acceptor + N workers (+db)**; per-worker io_uring, busy-poll |
| Cross-thread via `session_handle` / `job_queue`   | **SPSC copy-via-inbox mesh**; TLS pools with alloc-thread == free-thread   |
| Chat/game product lives here                      | **Chat server v1 lives inside the lib repo** (`src/app/`, `server` target)  |

The 2026-05-21 scope decision (`908a7aa`): the lib repo is
**chat-only**; game work (primitive MMO, renderer thread) moves to a
separate repo. **This repo is that game-server product repo.** Its
consumption contract is unchanged: `find_package(iouring_net)` against
an install prefix, never relative includes.

### What survives from docs 00–07 unchanged

- The two-repo split and the `find_package`-only seam (`03-cmake.md`).
- The codegen packet pipeline design (`05-codegen.md`) and wire format
  (`04-protocol.md`) — nothing in the pivot touches bytes-on-the-wire.
- The **test pyramid** of `06-test-strategy.md` (layers, parity replay,
  fuzz, sanitizer matrix). Only its code examples (coroutine fixtures,
  `iouring_net::service`) are stale; the layer model is authoritative
  and is what the scaffold below implements.
- Dispatcher is product-side. Under the thread model it becomes
  **one dispatcher instance per worker thread** (built once before
  workers start, or per-worker const tables) — never a shared mutable
  table across workers.

---

## The library's exported surface today (the seam inventory)

Installed to `<prefix>/include/iouring_net/` + `libiouring_net.a`
(static, `iouring_netConfig.cmake`, `SameMajorVersion`). Includes
resolve **without** an `iouring_net/` prefix (the install interface
points inside the directory): `<types.h>`, `<error/expected.h>`,
`<sds/ring_buffer.h>`, `<sds/cstr_hash_map.h>`, `<sds/malloc_vector.h>`,
`<sync/atomic.h>`, `<diagnostic/profiler_scope.h>`.

**Seam backlog** — headers the library must add to its public
`FILE_SET` before this repo can implement a real server runtime:

1. `check.h` (`LNX_CHECK`) — used by everything.
2. `sds/spsc_queue.h`, `sds/static_vector.h` — the mesh + supervisor
   storage primitives.
3. `runtime/thread.h`, `memory/packet_pool.h` — thread + TLS pool.
4. The `app/` layer (`handle_worker` / `engine_worker` /
   `handle_acceptor` / `engine_acceptor`) **or** a deliberate decision
   that the product re-implements its own app layer on lower-level
   exports. This is the single biggest open architecture question and
   should be settled in the library's `09-project-split.md` before
   game-server runtime code starts here.

Until at least items 1–3 land, this repo stays at v0 (seam proof +
codegen groundwork).

---

## Testing architecture (as-built, 2026-07-04)

The structural rule that makes everything testable:

> **`server_core` static library carries every translation unit with
> logic in it; `main.cpp` is a thin shell (parse → act → exit code).**
> Tests link `server_core` directly — the same pattern as the library's
> tests linking the `iouring_net` target.

### Layer map (docs/06 pyramid → concrete targets)

| Layer | Where | Target / harness | Status |
|-------|-------|------------------|--------|
| 1 unit | `tests/unit/*_test.cpp` | `iouring_server-test` (Catch2, `catch_discover_tests` with `DISCOVERY_MODE PRE_TEST` for the Ubuntu-24.04 TSan workaround) | **Live** — `options_test`, `seam_check_test` |
| 2 in-process integration | `tests/integration/` | Same Catch2 binary; pattern is the lib's `tests/net/echo_smoke_test.cpp`: server threads + loopback client thread in one process, port handshake via atomics (or bind port 0 + `getsockname`), stop via atomic flag | **Blocked on seam backlog** (needs thread/spsc/app exports) |
| 3 two-process smoke | `tests/CMakeLists.txt` | Framework-free ctest entry `server_seam_smoke` spawning the real binary — PR-gated per tenet 6 | **Live** |
| 3 nightly replay / cross-platform parity | `tests/e2e/` | Per `06-test-strategy.md`; arrives with the codegen pipeline | Planned |
| fuzz | `tests/fuzz/` | Deframer fuzz; arrives when the library exports `packet_framing` | Planned |

Catch2 is acquired product-side (`cmake/deps.cmake`: `find_package`
3.4.0 floor → FetchContent `v3.4.0`), mirroring the library's pins.
It is the only dependency this repo resolves itself; fmt/liburing/
tl::expected/Threads propagate through `iouring_net::iouring_net`.

Conventions carried from the library:

- Tags mirror directories: `[unit]`, later `[integration]`, plus a
  per-subsystem tag (`[options]`, `[seam]`, later `[dispatch]`,
  `[handlers]`) so `iouring_server-test "[dispatch]"` works like the
  lib's `make test-sds`.
- One test binary for layers 1–2; two-process orchestration stays in
  ctest/shell, never inside Catch2.
- Catch2 test names must not begin with `-` (ctest passes the name as
  argv; Catch2 parses a leading dash as a CLI flag).

### Known gap — sanitizer parity across the seam

The presets (`default` ASan+UBSan, `tsan`, `release`, `floor`) mirror
the library's, but the installed `libiouring_net.a` at `~/.local` is a
single Release build. ASan tolerates linking uninstrumented code (it
just doesn't check it); **TSan does not reliably** — races inside
library code would be invisible. Before Layer-2 concurrency tests
land, the workflow needs per-preset library installs (e.g. prefixes
`~/.local/iouring_net/<preset>` selected by `CMAKE_PREFIX_PATH` per
preset, or CI installing a sanitizer-matched lib build first). Tracked
here so the tsan preset isn't trusted prematurely.

---

## v0 scaffold inventory (landed 2026-07-04)

```
CMakeLists.txt            find_package(iouring_net 0.0.1 REQUIRED); server/ + tests/
CMakePresets.json         default | tsan | release | floor (mirror lib) + CMAKE_PREFIX_PATH=$HOME/.local
Makefile                  configure/build/test/run wrappers (mirror lib)
cmake/compiler-warnings.cmake   iouring_server_apply_warnings — same warning set as lib
cmake/deps.cmake          Catch2 only
server/                   server_core (options, seam_check) + thin main → iouring_net-server
tests/unit/               options_test, seam_check_test
tests/CMakeLists.txt      Catch2 binary + server_seam_smoke two-process test
.clang-format/.clang-tidy/.clangd/.editorconfig   copied from lib
```

Verified: library installed via
`cmake -S ../iouring-net-lib -B build/seam -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local`
+ `--install`; then `make test` here → configure resolves
`iouring_net` from `~/.local`, 8/8 tests pass under ASan+UBSan.

---

## Milestones from here

| Milestone | Gate |
|-----------|------|
| M0 seam proof (**done**) | `find_package` + link + boot + ctest green |
| M1 codegen pipeline | None — pure product-side. Port `packets.json` + generators per `05-codegen.md`; Layer-1 stub/proxy golden tests. Can start now. |
| M2 runtime skeleton | Seam backlog items 1–3 exported by the lib. Supervisor boot (steps 4–7 of the lib's boot sequence) + Layer-2 loopback echo test. |
| M3 packet round-trip | Lib exports framing (or M2 decision says product owns it). Dispatcher + handlers + per-packet Layer-2 tests. |
| M4 replay / parity | M3 + recorded traces. Nightly E2E + cross-platform parity per `06-test-strategy.md`. |
