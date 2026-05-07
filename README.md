# iouring-net-lib

A Linux-native, C++20 network engine built on `io_uring` and coroutines. The
goal is a small, presentable library that demonstrates modern Linux systems
programming applied to the kind of high-throughput connection handling that
shows up in AI inference serving, distributed runtimes, and async backends —
the same lessons that the Windows IOCP family of reference projects teaches,
ported to a 2026-relevant stack.

This repository contains **design documents only** at this stage. No code has
been written. Each subsystem has a short markdown spec under `docs/` that is
intended to be detailed enough to implement from end to end without
re-deriving design.

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
8. Packet framing: `[uint16 size | uint16 id][payload]` — same wire format as
   the Windows version, so the two implementations are directly comparable

Out of scope for v1: ODBC / database layer, MMO room logic, deadlock profiler
(may be ported later), Windows compatibility shims.

---

## Reading order

Start with `docs/00-overview.md`. Then `docs/01-windows-to-linux-mapping.md`
for the master API mapping table. After that, read whichever subsystem
interests you — they are designed to be readable independently.

```
docs/
├── 00-overview.md                    # scope, non-goals, subsystem map
├── 01-windows-to-linux-mapping.md    # master API mapping table
├── 02-build-and-toolchain.md         # CMake, liburing, kernel reqs
├── primitives/
│   ├── memory-pool.md
│   ├── object-pool.md
│   ├── ring-buffer.md
│   ├── serial-buffer.md
│   ├── sync-primitives.md
│   ├── lock-free-stack.md
│   ├── deadlock-profiler.md
│   └── leak-tracker.md
├── runtime/
│   ├── thread-context.md
│   ├── job-queue.md
│   └── coroutine-task.md
└── network/
    ├── io-uring-reactor.md
    ├── session.md
    ├── listener-and-service.md
    ├── packet-framing.md
    └── packet-handler.md
docs/testing/
└── test-strategy.md
```

---

## Status

- Design docs: in progress.
- First code: not yet written.
- First milestone: minimal `io_uring` reactor that echoes a single TCP
  connection. Gated on completion of the design doc set.
