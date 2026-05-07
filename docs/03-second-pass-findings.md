# 03 — Second-pass findings (gap analysis)

A second sweep of the three reference repos (`IOCP_Rookiss`,
`SelectServer`, `WindowsLibrary`) and an industry-standard review of the
existing design-doc set surfaced the items below. This doc is a
prioritized catalog so you can pick what to chase next without re-reading
the agent transcripts.

The findings fall into four buckets:

1. **Factual corrections to the existing 21 docs** — already applied
   in-place; logged here for audit.
2. **New ports / subsystems** the reference repos contain that the first
   pass missed.
3. **Operability and lifecycle gaps** an outside reviewer would flag —
   prioritized must / should / defer.
4. **Non-goals reaffirmed** — items that look missing but are
   intentionally out of scope.

---

## 1. Factual corrections (already applied)

| Where                                  | Original                                | Correction                                                                                                              |
|----------------------------------------|-----------------------------------------|------------------------------------------------------------------------------------------------------------------------|
| `primitives/memory-pool.md`            | "47 size classes"                       | **48** size classes: 32 entries of step 32 (32–1024) + 16 entries of step 64 (1088–2048). `MAX_ALLOC_SIZE = 2048`.       |
| `primitives/memory-pool.md`            | header sketch                           | `MemoryHeader { SLIST_ENTRY entry; i32 allocSize; i32 _padding; }` — `SLIST_ENTRY` **must be the first member** on Win32. |
| `primitives/deadlock-profiler.md`      | "port from reference"                   | Reference declares `DeadLockDebugger::CheckCycle()` at `IOCP_Rookiss/Engine/DeadLockDebugger.h:31` but **never implements it**. Our port actually implements cycle detection. |
| `network/packet-handler.md`            | "no codegen in any reference repo"      | **Wrong.** `SelectServer/TestSerialize/packets.json` + `rpc_gen.py` / `stub_gen.py` / `proxy_gen.py` is a real Python pre-build codegen pipeline. We may take design cues from it. |
| `00-overview.md`                       | subsystem inventory                     | Added `indexed_heap` and `cstr_hash_map` rows (out-of-scope for v1, listed for traceability).                           |

---

## 2. New facts worth knowing about the reference repos

These don't change the design doc set but add load-bearing context.

### Memory pool internals (IOCP_Rookiss)

- 48 buckets, hardcoded `constexpr` table at `Engine/Memory.h:17-29`.
- `MEMORY_ALLOCATION_ALIGNMENT = 16` is a Win32 SLIST requirement,
  not a free choice. The Linux replacement (Treiber stack) doesn't have
  this constraint, but the 16-byte allocation alignment is still useful
  for cache-line packing.
- `alignas(64)` is on every atomic type (`Atomic32`, `Atomic64`,
  `AtomicPtr`) — cache-line padding to prevent false sharing.
- `LoadAcquire()` is implemented via a dummy `_InterlockedCompareExchange(.., 0, 0)`. Win32 idiom; on Linux this is just a relaxed-load + acquire fence. Several call sites assume seq-cst — double-check before weakening.
- `GMemory` is a global `extern Memory*`; **its definition site is
  hidden** in the reference. Linux port should use a Meyers singleton.

### Selectserver discoveries

- Listen and accept sockets both set `TCP_NODELAY = 1` and `SO_LINGER =
  {1, 0}` (close with RST). The latter is unusual; document it.
- 60 FPS frame loop with `QueryPerformanceCounter` budget calculation;
  **no frame-skip logic** under overrun (only sleeps when ahead).
- Hardcoded constants worth porting: `SESSION_MAX = 64`, port 5000,
  recv buffer 4096, send buffer 4096.
- The header validation pipeline includes a **0x89 magic byte**, which
  we explicitly chose **not** to carry over (decision in
  `network/packet-framing.md`).
- Game state is mutated **directly** in packet handlers (no JobQueue
  in the actual code) — race-safe only because everything runs on one
  thread. Confirms that JobQueue is a fresh design, not a port.
- A `// TODO` at `FighterOOP/main.cpp:72` marks frame-overrun handling
  as the lecture's known-incomplete piece.

### WindowsLibrary discoveries

- `indexed_heap<T>` is a position-tracked min-heap (`_pos[]` array
  shadowing `_data[]`) — used by Dijkstra/A*/event scheduling. Out of
  scope for v1; nice-to-port for game-server use cases.
- `cstr_hash_map<V>` is a hand-rolled hash map keyed on `const char*`
  (assumes keys are `.rodata` literals). djb2 hash, 0.75 load factor,
  initial capacity 64. Author switched from FNV-1a to djb2 for speed
  (commented at `cstr_hash_map.h:317`). Out of scope for v1.
- `Profiler::Manager` is `thread_local` with multi-format output
  (txt/csv/console). Useful pattern for the v2 metrics doc.
- `NewTracer` records via a `placement-new` operator-new override
  capturing `__FILE__`/`__LINE__`. Our v1 design uses
  `std::source_location` instead — strict upgrade.
- `GuardOverflow` uses `VirtualProtect(PAGE_NOACCESS)` on the page
  *after* the allocation. Linux port maps to `mprotect(PROT_NONE)`.
- `SharedPtr<T>` is **intrusive** (`T : public RefCount`), unlike
  `std::shared_ptr`. Has a syntax error at `WinSharedPtr.h:51` (broken
  template copy ctor). Not blocking — we use `std::shared_ptr` anyway.
- `WindowsLibrary` actually has unit tests in `MainApp/Sources/`
  (`TestProfiler.cpp`, `test_cstr_hash_map.cpp`,
  `TestGuardOverflow.cpp`, `TestNewTracer.cpp`, `TestWinThread.cpp`,
  `test_indexed_heap.cpp`, `TestSerialBuffer.cpp`). Useful as
  reference test cases.

### SelectServer codegen pipeline (the big find)

`SelectServer/TestSerialize/` ships a Python preprocessor:

```
TestSerialize/
├── packets.json              ← schema: id, name, fields
├── rpc_gen.py                ← orchestrator
├── stub_gen.py               ← server-side handler stubs (ProcessPacket switch)
└── proxy_gen.py              ← client-side typed send wrappers
```

Run as a pre-build step. Output is checked-in C++ that defines per-id
codecs and a dispatch switch.

**Implication for our `network/packet-handler.md`:** the codegen path is
not theoretical — there's a working reference. v1 still hand-writes
codecs, but a v2 task is to port `rpc_gen.py` (or write a small CMake
function in its place) so user-defined `.json` schemas generate codec +
handler skeletons.

---

## 3. Operability / lifecycle gaps

Twenty industry-standard gaps surfaced from a third-party-reviewer
perspective. Prioritized below.

### Must-add before "presentable"

| # | Gap                                              | Suggested doc                                  |
|---|--------------------------------------------------|------------------------------------------------|
| 1 | Signal handling and graceful drain               | `docs/lifecycle-and-signals.md`                |
| 2 | Configuration loading (port, backlog, knobs)     | `docs/configuration.md`                        |
| 3 | Socket-option policy table (NODELAY, KEEPALIVE, IPV6_V6ONLY, REUSEPORT) | `docs/socket-options.md`            |
| 4 | EMFILE / accept-back-pressure response           | extend `network/io-uring-reactor.md`           |
| 5 | SQE submit failure & CQ overflow handling        | extend `network/io-uring-reactor.md`           |
| 6 | Cancellation model (`stop_token` across `co_await` chains, `IORING_OP_ASYNC_CANCEL`) | `docs/coroutine-cancellation.md`     |
| 7 | Slow-client / idle timeout / max-buffered cap    | `docs/timeouts-and-limits.md`                  |
| 8 | Threat model (untrusted-payload posture, allocation-amplification) | `docs/threat-model.md`             |
| 9 | Example surface: echo + request/response + broadcast | `examples/README.md` + 3 example dirs       |

### Should-add post-v1

| # | Gap                                              | Suggested doc                                  |
|---|--------------------------------------------------|------------------------------------------------|
| 10 | Structured logging design                        | `docs/logging.md`                              |
| 11 | Metrics (counters, histograms, scrape format)    | `docs/metrics.md`                              |
| 12 | Memory-pressure / pool-cap / soft watermark      | extend `primitives/memory-pool.md`             |
| 13 | Exception propagation contract across coroutines | extend `runtime/coroutine-task.md`             |
| 14 | CMake config-mode export (`iouringnetConfig.cmake`), pkg-config | extend `02-build-and-toolchain.md` |
| 15 | Sanitizer-build install separation               | extend `02-build-and-toolchain.md`             |
| 16 | API stability tiering (`iouring_net::` vs. `iouring_net::detail::`) | `docs/api-stability.md`              |
| 17 | Performance-baseline targets and harness         | `docs/performance-targets.md`                  |

### Defer (post-implementation, not pre)

| # | Gap                                              | Why deferred                                   |
|---|--------------------------------------------------|------------------------------------------------|
| 18 | Admin / introspection socket (UDS + SIGUSR1 dump) | Useful only when there are real ops scenarios |
| 19 | Debugging cookbook (gdb / perf / bpftrace recipes) | Write after first real debugging session    |
| 20 | Tutorial / cookbook tier of docs                  | Premature; the design docs are the readership for now |

---

## 4. Non-goals reaffirmed

These look missing from the doc set but are deliberately out of scope.
Documenting here so a future reviewer doesn't re-raise them.

- **TLS / HTTPS / TLS-on-TCP.** Out of scope. Handled by a layer above.
- **Async DNS, async file I/O, async timers as public APIs.** Internal
  uses go through `io_uring` directly; we do not export an
  async-everything surface.
- **MMO room logic, ODBC database layer, deadlock profiler in v1's
  critical path.** Engine focus only; port game-side later.
- **Cross-platform.** Linux only. No `#ifdef _WIN32` shims.
- **Stable ABI.** Static link or rebuild against your version. No
  versioned ABI guarantee in v1.
- **Wait-free anything.** Lock-free is the bar; wait-free is a separate
  research project.
- **Magic byte (0x89) in packet header.** SelectServer uses one;
  we explicitly do not. Application-level packet IDs do the same job.

---

## 5. Recommended next moves

If the goal is "ship a presentable v1," the practical next-doc order:

1. `docs/lifecycle-and-signals.md` (cheap; demonstrates operational
   awareness)
2. `docs/socket-options.md` (small table; high signal/density ratio)
3. `docs/configuration.md` (lightweight; unblocks all examples)
4. `docs/threat-model.md` (one page; sets the abuse boundary explicitly)
5. Extend `network/io-uring-reactor.md` with EMFILE / SQE-fail / CQ-overflow sections (in-place)
6. `docs/coroutine-cancellation.md` (load-bearing for any non-trivial use)
7. `docs/timeouts-and-limits.md` (covers slowloris and resource exhaustion)
8. `examples/echo/`, `examples/request-response/`, `examples/broadcast/` design specs

If the goal is "skip ahead to code," the design docs are sufficient to
start `examples/echo/` and the supporting CMake skeleton; the operability
gaps can be filled in as code reveals which are actually load-bearing.

---

## 6. Audit log

| Sweep date    | Sweep scope                                                  | Lead findings                                                                                       |
|---------------|--------------------------------------------------------------|-----------------------------------------------------------------------------------------------------|
| 2026-05-07    | Three-repo first-pass architecture report                    | Subsystem inventory, Win32-API surface, wire format                                                 |
| 2026-05-07    | Three-repo deep-dive + cross-repo TODO/build harvest + industry-gap review | This document; corrections to 4 docs; 17 net-new gaps catalogued; codegen pipeline discovered |
