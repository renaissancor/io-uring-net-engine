# 00 — Overview

This document is the entry point for the `engine-uring` design. It defines
scope, non-goals, the layered subsystem map, design tenets, and a glossary
that the rest of the docs assume.

---

## Goal in one sentence

A Linux-native C++20 realtime interaction network engine for MMO/RTS-style
servers that keeps session I/O, packet parsing, and authoritative world-state
mutation on the same owner thread so per-interaction latency is predictable —
carrying the engine-primitives lessons of the Windows IOCP reference repos onto
Linux `io_uring` while shedding Win32 entirely.

The primary motivation is **bounded, inspectable realtime interaction
latency**, not raw throughput: a packet arriving for an in-world session is
completed, parsed, validated, and applied to authoritative state by the same
worker thread that owns that state. The runtime shape, ownership invariants, and
first three-thread milestone live in
[`10-realtime-server-architecture.md`](../../server-uring/doc/10-realtime-server-architecture.md); this
overview covers the layered subsystem map and cross-cutting tenets.

**Ownership invariants** (full statement in doc 10): one fd → one owner thread;
one room/world state → one owner thread; the owning worker parses and executes
its own packets in-thread; cross-thread work is SPSC message passing, never
shared mutable state on the hot path; gameplay packets are valid only after
`S_ENTER_WORLD_OK`.

---

## Layered subsystem map

```
┌─────────────────────────────────────────────────────────────────────┐
│  Application                                                         │
│  (echo server, future user code)                                     │
├─────────────────────────────────────────────────────────────────────┤
│  Network layer                                                       │
│  Service · Listener · Session · SessionHandle · PacketFraming        │
│  (PacketHandler/dispatcher: deferred — product-side, see             │
│   doc/network/packet_handler.md)                                    │
├─────────────────────────────────────────────────────────────────────┤
│  Runtime layer                                                       │
│  Reactor (io_uring) · JobQueue · ThreadContext                       │
├─────────────────────────────────────────────────────────────────────┤
│  Primitive layer                                                     │
│  MemoryPool · ObjectPool · RingBuffer · SerialBuffer ·               │
│  Sync (atomic / mutex / shared_mutex / lock-free stack)              │
│  Debug (deadlock profiler · leak tracker)                            │
├─────────────────────────────────────────────────────────────────────┤
│  Platform                                                            │
│  liburing · POSIX (sockets, mmap, pthreads via libstdc++)            │
└─────────────────────────────────────────────────────────────────────┘
```

Lower layers know nothing about higher layers. The reactor depends on
primitives and `liburing`; the network layer depends on the reactor and
primitives; application code depends on the network layer.

The top layer (Application) does not live in this directory. It belongs
to `server-uring/`, a sibling directory in this monorepo that is
reserved and not yet written; it will consume this library through
`find_package(iouring_net)` against an install prefix, never through a
relative include. That boundary is the point and survives the two
projects sharing one repo. See
[`09-project-split.md`](../../design-notes/09-project-split.md) for the full
library/product architecture, boundary criteria, and the seam that
connects the two projects.

---

## Subsystem inventory

| Layer       | Subsystem                              | Status     | Reference origin                                        |
|-------------|----------------------------------------|------------|---------------------------------------------------------|
| Primitive   | `memory_pool`                          | Port       | `IOCP_Rookiss/Engine/MemoryPool.h:14`                   |
| Primitive   | `object_pool`                          | Port       | `IOCP_Rookiss/Engine/ObjectPool.h:8`                    |
| Primitive   | `stl_allocator`                        | Port       | `IOCP_Rookiss/Engine/Allocator.h:19`, `WindowsLibrary/Library/WinMemory.h:20` |
| Primitive   | `ring_buffer`                          | Port       | `WindowsLibrary/Library/Include/RingBuffer.h:5`, `SelectServer/.../RingBuffer.h` |
| Primitive   | `serial_buffer`                        | Port       | `WindowsLibrary/Library/Include/SerialBuffer.h:5`       |
| Primitive   | `lnx::mutex` / `lnx::shared_mutex`     | Rewrite    | `IOCP_Rookiss/Engine/Mutex.h:5`, `WindowsLibrary/Library/Include/WinMutex.h:7` |
| Primitive   | `lnx::atomic32` / `atomic64` / `atomic_ptr` | Rewrite | `IOCP_Rookiss/Engine/Atomic.h:5`, `WindowsLibrary/Library/Include/WinAtomic.h:6` |
| Primitive   | `lock_free_stack`                      | New        | Replaces Win32 `SLIST_HEADER`                           |
| Primitive   | `deadlock_profiler::manager`           | Port       | `IOCP_Rookiss/Engine/DeadLockDebugger.h:11` (reference declares `CheckCycle()` at `:31` but never implements it; our port does) |
| Primitive   | `leak_tracker::manager`                | Port       | `WindowsLibrary/Library/Include/NewTracer.h:33`         |
| Primitive   | `malloc_vector`                        | Port       | `WindowsLibrary/Library/Include/malloc_vector.h:5` — `std::vector`-shaped container backed by `malloc`/`free`; required by `leak_tracker` to avoid `new` recursion |
| Primitive   | `cstr_hash_map`                        | Port       | `WindowsLibrary/Library/Include/cstr_hash_map.h:5` — djb2-hashed `const char*`-key map; promoted from v2, implementation already exists |
| Primitive   | `indexed_heap`                         | Port       | `WindowsLibrary/Library/Include/indexed_heap.h:5` — position-tracked min-heap; promoted from v2, implementation already exists |
| Primitive   | `profiler::manager` / `profiler::scope` | Port      | `WindowsLibrary/Library/Include/Profiler.h:5` — Linux port swaps `QueryPerformanceCounter` for `clock_gettime(CLOCK_MONOTONIC)` |
| Primitive   | `guard_overflow::manager`              | Defer (v2) | `WindowsLibrary/Library/Include/GuardOverflow.h:7` — page-guard allocator; needs `mprotect`-based Linux rewrite |
| Primitive   | `log::logger`                          | New        | Per-thread queue + async file write; no counterpart in the reference repos checked here |
| Runtime     | `reactor` (io_uring)                   | New        | Replaces `IocpCore` (declared but not implemented in IOCP_Rookiss) |
| Runtime     | `job_queue`                            | New        | NextProject.md mentions it; no reference implementation in any repo |
| Runtime     | `thread_context` (TLS)                 | New        | Stubs only in `IOCP_Rookiss/Engine/ThreadManager.cpp:8` |
| Network     | `service`                              | New        | Concept only — no IOCP-side implementation              |
| Network     | `listener`                             | Port       | `SelectServer/.../Net.cpp:181` (accept loop)            |
| Network     | `session`                              | New        | Replaces select-loop `Session` struct (`SelectServer/.../Net.h:39`) |
| Network     | `packet_framing`                       | Port       | Concept from `SelectServer/.../Network.cpp:377` (magic header) |
| Network     | `packet_handler`                       | Deferred   | Pulled out of library v1 — the dispatcher is product-side. See `doc/network/packet_handler.md` (status note) and `server-uring/wiki/server/dispatch.md`. |

**Status legend:**
- **Port** — design is OS-agnostic; implementation is a clean transcription with the platform layer swapped.
- **Rewrite** — same intent, different primitives; implementation is rewritten against `std::atomic` / `std::mutex` / `std::shared_mutex`.
- **New** — no reference implementation exists; designed from first principles for this repo.
- **Deferred** — designed but pulled out of v1; either moved to the product repo or scheduled for a later library milestone. The original spec stays in `doc/` as a status note so the rationale is grep-able.

---

## Honest caveat — gap between `NextProject.md` and reference repos

The planning document lists "JobQueue + GGlobalQueue" and a "per-entity job
queue" as **ports** from the existing repo. They are not. Neither
`IOCP_Rookiss` nor `WindowsLibrary` contains a job queue, job serializer, or
global dispatcher. The design in this repo's runtime docs is therefore a
**fresh design** informed by the Rookiss lecture material rather than a port.

Same caveat for the IOCP event loop itself: `IOCP_Rookiss` declares the
network includes (`<winsock2.h>`, `<mswsock.h>`) but contains no `IocpCore`,
`Session`, or `IocpEvent` types. The Linux reactor design has no Windows
counterpart to mirror; it is informed by lecture knowledge and `liburing`
idioms.

This is a design-doc honesty marker, not a problem.

---

## Design tenets

1. **Single-thread data plane per worker (fused per-worker io_uring).** Each
   worker thread owns one io_uring ring and runs I/O submission, completion,
   framing, dispatch, and state mutation in a sequential tick loop. The
   worker never blocks except on `io_uring_submit_and_wait`. v1 ships
   `N_workers = 1`; the same shape scales to N>1 unchanged. See
   `.omc/wiki/threading-model-per-worker-io-uring-copy-via-inbox.md`,
   `.omc/wiki/worker-class-and-thread-roles.md`, and
   `../design-notes/2026-05-19-server-architecture.md` (historical, pre-pivot).

2. **One reactor per thread, optionally one thread.** v1 ships
   single-threaded. Multi-threaded support is a v2 concern, designed for but
   not implemented yet.

3. **`expected<T, std::error_code>` for I/O errors, exceptions for
   programming errors.** I/O failure is normal control flow; bugs are
   not. Project `expected` is `tl::expected` (see
   `02-build-and-toolchain.md` polyfill section); the call sites are
   API-identical to a future `std::expected`.

4. **No hidden allocations on the hot path.** All per-connection allocation
   goes through the memory pool.

5. **Reference-repo line-level traceability.** Every ported subsystem doc
   cites the original `path:line` so a reader can compare directly.

6. **Wire-format parity — payload-byte, under header normalization.**
   The framing header is a deliberately wider 4-byte
   `[uint16 size | uint16 id]` versus the Windows reference's 3-byte
   `[0x89][u8 size][u8 type]`. Parity is therefore not at the raw frame
   level; it is at the **payload-byte level**: for any packet name with
   the same field values, the payload bytes serialize identically on
   both sides (under preconditions: matching schema, IDs ≤ 255, payload
   ≤ 255, LE host, verified field offsets). A Windows-recorded trace
   replays against the Linux server after a header-normalization step
   converts the 3-byte Windows header to the 4-byte Linux header. This
   is the "directly comparable" property called out in `NextProject.md`,
   scoped honestly: it does **not** mean an unmodified Windows client
   binary speaks to the Linux server end-to-end (v1 ships no
   compat-deframer), and "strip 0x89" alone is **not** sufficient. See
   [`../doc/network/packet_framing.md`](../doc/network/packet_framing.md)
   § "Purpose" and the consumer-side detail in
   `server-uring/docs/04-protocol.md`
   § "Parity with the Windows reference."

7. **Namespace tiers.** `lnx::` is reserved for raw POSIX/Linux API
   wrappers (`mutex`, `atomic32`, future `file` / `socket` / `eventfd`) —
   the Linux equivalent of `std::` for primitives that directly touch
   kernel/libc. Pure data structures, compound primitives, and
   single-class subsystems live at global scope. Diagnostic subsystems
   that group a `manager` singleton with helper types get their own
   namespace (`leak_tracker::`, `profiler::`, `deadlock_profiler::`,
   `guard_overflow::`, `log::`), matching the WindowsLibrary `NewTracer::`
   / `GuardOverflow::` / `Profiler::` pattern. No umbrella namespace is
   created just to group files in the same `src/` folder. See
   `04-coding-style.md` for the full rules.

---

## Non-goals

- **Cross-platform.** Linux only. No `#ifdef _WIN32` shims. macOS is a maybe
  via `kqueue` if a contributor wants it; not blocking v1.
- **TLS / HTTP / WebSocket.** v1 is raw TCP with the custom framing.
  Higher-level protocols are a separate effort.
- **MMO/game logic.** Room, Player, GameSession, DBSynchronizer — out.
- **Async DNS, async file I/O, async timers as a public API.** Internal
  uses go through `io_uring` directly; we do not export an async-everything
  surface in v1.
- **Windows compatibility.** None.
- **Stable ABI.** Static link or rebuild against your version. No
  versioned ABI guarantee in v1.
- **Wait-free anything.** Lock-free is the bar; wait-free is a separate
  research project.
- **Magic byte (0x89) in packet header.** SelectServer's reference
  framer uses one; we explicitly do not. Application-level packet IDs
  do the same job. See `doc/network/packet_framing.md`.

---

## Build / kernel requirements (summary)

- Linux kernel 5.19+ recommended (multishot accept, fixed buffers,
  `IOSQE_BUFFER_SELECT`). 5.10+ minimum for basic `io_uring` correctness.
- WSL2 is fine for development; honest performance numbers belong on bare
  metal or a cloud VM.
- **C++20 only.** No C++23 in the public surface. The `expected` API is
  provided by `tl::expected` (vendored); `std::print` is provided by
  `{fmt}`. Both are API-compatible with the C++23 stdlib equivalents.
- See `02-build-and-toolchain.md` for the language standard, kernel
  matrix, and dependency floors. `06-system-setup.md` covers
  cross-distro install. `05-cmake.md` covers CMake conventions and
  `cmake/deps.cmake`. `07-ci-and-reproducibility.md` covers the CI
  matrix and the reproducibility envelope (Dockerfile, devcontainer,
  `version-snapshot.txt`).

---

## Glossary

- **Reactor** — the loop that submits SQEs (Submission Queue Entries) to
  `io_uring` and consumes CQEs (Completion Queue Entries).
- **SQE / CQE** — `io_uring` submission / completion queue entries.
- **Multishot op** — a single SQE that produces multiple CQEs over time
  (e.g., multishot accept, multishot recv).
- **Fixed buffer** — a buffer pre-registered with the kernel so the kernel
  references it by index instead of pinning per-op.
- **Session** — one logical TCP connection; owns recv/send buffers and the
  framing cursor on that connection.
- **Service** — a container that owns the reactor and a set of sessions.
- **Job queue** — a per-entity FIFO of work items that serializes
  modifications to that entity without taking a lock per operation.
