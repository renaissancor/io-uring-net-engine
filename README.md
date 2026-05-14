# iouring-net-lib

A Linux-native, C++20 network engine built on `io_uring` and coroutines. The
goal is a small, presentable library that demonstrates modern Linux systems
programming applied to the kind of high-throughput connection handling that
shows up in AI inference serving, distributed runtimes, and async backends —
the same lessons that the Windows IOCP family of reference projects teaches,
ported to a 2026-relevant stack.

The CMake scaffolding, dependency graph (`liburing`, `{fmt}`, `Catch2`,
`tl::expected`), CI floor job, and an `examples/hello/` smoke executable
have landed. Subsystem source files (memory pool, ring buffer, reactor,
session, …) have **design specs under `wiki/<category>/<name>.md`** and
are not yet implemented — the wiki specs are detailed enough to build
each one without re-deriving design.

---

## Quick start

Verified on Ubuntu 24.04 / WSL2 (kernel 6.6+) with `g++-12` or newer and
`liburing-dev >= 2.5`. See `docs/06-system-setup.md` for the full
distro-aware install runbook.

```bash
make hello                     # configure + build + run examples/hello
make test                      # configure + build + ctest (default preset)
make test-sds                  # configure + build + run only sds:: tests
make test  PRESET=floor        # same, but on the gcc-12 floor preset
make build PRESET=release      # release build, no sanitizers
make help                      # list every target
```

The `make` targets are thin wrappers around `cmake --preset …`,
`cmake --build --preset …`, and `ctest --preset …`; the raw CMake
commands work too. Build directories live under `build/<preset>/` and
are gitignored.

---

## Why this exists

Three predecessor repositories live at `~/CLionProjects/`:

| Repo               | What it is                                              |
|--------------------|---------------------------------------------------------|
| `IOCP_Rookiss`     | Engine primitives: 47-class memory pool, sync, deadlock |
| `SelectServer`     | `select()`-based game servers (FighterOOP, StarServer)  |
| `WindowsLibrary`   | Utility layer: atomics, mutexes, threads, smart ptrs    |

All three are Windows-bound. The systems-level lessons in them — memory
pools, lock-free stacks, per-entity job queues, packet framing — are
OS-agnostic at the conceptual layer. This project preserves those lessons
while replacing the Windows surface with `io_uring`, POSIX, and modern C++.

The Windows repos remain as **read-only architectural references**. Nothing
in this repo links against them.

---

## Scope

In scope for v1 (echo server over a single connection):

1. `io_uring` reactor (replaces `IocpCore` / `select()`)
2. C++20 coroutine `Session` with `co_await read()` / `co_await write()`
3. Memory pool (47 size classes, ported from `IOCP_Rookiss/Engine/MemoryPool.h`)
4. Object pool / allocator adapter (templated, OS-agnostic in design)
5. Ring buffer + serial buffer (ported from `WindowsLibrary` / `SelectServer`)
6. Sync primitives (`std::atomic`, `std::mutex`, `std::shared_mutex`,
   custom Treiber stack to replace Win32 `SLIST`)
7. Per-entity job queue (new design — note: not actually implemented in any
   reference repo, so this is a fresh design informed by the lecture material)
8. Packet framing: `[uint16 size | uint16 id][payload]` — a deliberately
   wider 4-byte header than the Windows reference's 3-byte
   `[0x89][u8 size][u8 type]`, lifting the limits from 256 IDs / 255-byte
   payloads to 65 535 / 65 531. Parity with the reference is at the
   **payload-byte level** (per-packet field serialization is byte-identical
   on matching schemas), enabling **header-normalized trace replay** for
   cross-platform verification. Not source-compatible; not a drop-in
   live-client interop claim.

Out of scope for v1: ODBC / database layer, MMO room logic, deadlock profiler
(may be ported later), Windows compatibility shims.

---

## Reading order

Start with `docs/00-overview.md`. Then `docs/01-windows-to-linux-mapping.md`
for the master API mapping table. After that, read whichever subsystem
interests you — wiki specs are designed to be readable independently.

Cross-cutting documentation lives in `docs/`; per-source-file design
specs live in `wiki/`. Each directory has its own `README.md` index.

```
docs/                  ← cross-cutting design + operations
├── README.md          ← index of every doc + suggested reading paths
├── 00-overview.md                    # scope, subsystem map, tenets, non-goals
├── 01-windows-to-linux-mapping.md    # master Win32 → Linux API mapping
├── 02-build-and-toolchain.md         # language, kernel, deps, repo layout
├── 04-coding-style.md                # naming, error model, namespaces, aliases
├── 05-cmake.md                       # CMake target, presets, deps.cmake
├── 06-system-setup.md                # distro install runbook + smoke tests
├── 07-ci-and-reproducibility.md      # CI matrix, Dockerfile, version-snapshot
└── 08-test-strategy.md               # test pyramid, coverage targets, sanitizers

wiki/                  ← per-source-file design specs (one per planned src/ file)
├── README.md          ← wiki ↔ src/ mapping table
├── sds/               ring_buffer, serial_buffer, cstr_hash_map  (generic data structures, sds:: namespace)
├── memory/            memory_pool, object_pool, leak_tracker
├── sync/              sync_primitives, lock_free_stack
├── diagnostic/        profiler_deadlock, profiler_scope
├── runtime/           coroutine_task, job_queue, thread_context
└── network/           io_uring_reactor, listener_and_service, session,
                       session_handle, packet_framing
                       (packet_handler deferred — product-side)
```

---

## Status

- Design docs: stable across `docs/` and `wiki/`.
- Build scaffolding: landed (CMake, presets, deps graph, CI floor job,
  devcontainer, smoke test, `examples/hello/`).
- First subsystem landed: `sds::ring_buffer` — direct port from
  WindowsLibrary, 10 Catch2 cases / 46 assertions (`make test-sds`).
- Next subsystems: `sds::cstr_hash_map`, `diagnostic::profiler_scope`,
  then `recv_ring_buffer` / `send_ring_buffer` specializations.
- First end-to-end milestone: minimal `io_uring` reactor that echoes a
  single TCP connection.
