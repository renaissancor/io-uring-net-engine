# 2026-05-15 — Session log

Pick-up notes for the next session. Read top-to-bottom.

## What shipped today

Four commits, all pushed to `origin/main`, all green at 73/73 tests.

| Commit | Subject |
|---|---|
| `cb14a72` | `feat(sync): port WinAtomic to lnx::atomic32/64/ptr; add types.h` |
| `73098157` | `feat(sync): port WinMutex/WinSharedMutex to lnx:: over pthread direct` |
| `145b902` | `feat(sync): add LNX_DCHECK debug trap to lnx::mutex/shared_mutex` |
| `4823dfa` | `feat(runtime): land lnx::thread; introduce LNX_CHECK / LNX_DCHECK pair` |

State at session end: clean working tree, no uncommitted changes.

## Architectural ground rules locked in today

These are committed in trailers + wiki + memory; do not re-litigate without explicit cause:

- **Primitive layer is `std::`-free.** No `std::mutex`, `std::thread`, `std::system_error`, `std::abort`, `std::terminate`, `<cassert>`. pthread direct, futex direct, `__atomic_*` builtins. Reference: `feedback_no_std_primitives.md`.
- **Trap mechanism is `int 3` / SIGTRAP** (not `__builtin_trap` / SIGILL). Cross-compiler via `__builtin_debugtrap` on Clang, inline asm on GCC. Cross-arch via `int3` (x86) / `brk #0xf000` (aarch64). Reference: `doc/check.md`, `src/check.h`.
- **`LNX_CHECK` vs `LNX_DCHECK` split**: cold-path primitives (`lnx::thread`) use always-on `LNX_CHECK`; hot-path primitives (`lnx::mutex`) use debug-only `LNX_DCHECK`. Matches Folly/abseil/Chromium. Reference: `doc/check.md`.
- **Linux side snake_case, future Windows side PascalCase.** Reference: `feedback_naming_platform_split.md`.
- **Codex briefing pattern**: point codex at the WindowsLibrary reference path; do not pre-digest the API into the prompt. Reference: `feedback_codex_point_at_winapi_reference.md`.

## Next subsystem — revised build order

Yesterday's architectural discussion (TLS-first memory pool, SPSC on x86-64, many-core CAS scaling, object-pool vs memory-pool split) changed the order from the original plan:

1. **`lnx::lock_free_stack<T>`** — Treiber stack with tagged-head ABA protection. Design already in `doc/sync/lock_free_stack.md`. Small, well-spec'd. Uses primitives that just landed (`lnx::atomic*`, `lnx::cache_aligned`).
2. **`lnx::object_pool<T>`** — generic template on top of the lock-free stack. Pre-allocates N instances; push/pop via Treiber stack. ~50 lines. Replaces IOCP_Rookiss's `MemoryPool<T>` semantically — that was an object pool mis-labeled as a memory pool.
3. **SPSC + MPSC queue primitives** — needed for both the memory pool's cross-thread returns and the I/O message paths. SPSC on x86-64 is zero-sync (just memory_order_release/acquire, emits no fence instructions). MPSC is one `XCHG` per producer push (Vyukov pattern).
4. **`lnx::memory_pool`** — TLS-first byte allocator (mimalloc-style). Per-thread heap + per-thread MPSC return queue for cross-thread frees. Heterogeneous size classes. Uses the queue primitive from step 3.
5. *(Maybe)* central slab allocator behind the TLS heaps if going tcmalloc-style — could reuse the `lock_free_stack` here as a cold-path central. Or skip and go mimalloc-style per-thread `mmap`.

Steps 1-2 are small (a few hours). Step 3 is medium. Step 4 is the big one.

## Naming correction worth applying when (1) lands

The wiki page is currently named `doc/sync/lock_free_stack.md` and frames the structure as "memory pool free list." After yesterday's discussion, this is misleading — the structure is the object-pool primitive. When implementing, also:

- Rename `doc/sync/lock_free_stack.md` purpose section: "primitive behind object pools, not the byte-allocating memory pool."
- Write `doc/sync/object_pool.md` as the user-facing wrapper doc.
- Spec `doc/memory/memory_pool.md` separately for the TLS-first byte allocator (defer writing until step 4 is imminent).

## Open design decisions for next session

Nothing blocking. These are deferred-but-known:

- **Cached kernel TID in `lnx::thread`** (`lnx::thread::tid()` accessor). Needs trampoline + publish sync. Land when log/perf correlation actually requires it.
- **Ergonomic template ctor for `lnx::thread`** (`template<F, Args...>`). Win::Thread does this via `new std::function<void()>` — conflicts with the no-`std::` ethos. Would need a hand-rolled closure type. Defer.
- **`pthread_attr_t` exposure** for affinity / scheduling policy / stack size / thread name. Useful for the I/O reactor / worker pool. Land when those subsystems need it.
- **`lnx::condition_variable`**. Not yet wrapped. Defer until a use case appears.
- **`lnx::recursive_mutex`**. Listed in surface sketch but deliberately unshipped. Add only with a justified call site.
- **Adaptive `pthread_mutex_t` with `PTHREAD_MUTEX_ADAPTIVE_NP`**. Only with measurement showing contention.

## Memory updates from today's session

New entries in `~/.claude/projects/-home-stephen-CLionProjects-iouring-net-lib/memory/`:

- `feedback_use_codex_often.md` — delegate implementation/refactor to codex, keep Claude on design/review/commits.
- `feedback_codex_point_at_winapi_reference.md` — when delegating port work, give codex the WindowsLibrary path; don't pre-digest the API.
- `feedback_no_std_primitives.md` — primitive layer wraps pthread/futex/`__atomic_*` directly; no `std::abort` / `std::terminate` / `std::system_error`; use `__builtin_trap` (now superseded by `int 3` via `LNX_TRAP`).
- `feedback_naming_platform_split.md` — Linux side snake_case, Windows side PascalCase; apply to any future Win:: code.

## Future docs to write (don't write yet — these need the subsystems first)

- `doc/memory/memory_pool.md` — TLS-first byte allocator design. Lands with step 4.
- `doc/sync/object_pool.md` — generic object pool on top of `lock_free_stack`. Lands with step 2.
- `doc/diagnostic/ebpf_observability.md` — uprobes / tracepoints / bpftrace snippets for this project. Lands when the server is real enough to run under load.
- `doc/runtime/crash_handler.h` doc — sigaction + backtrace for SIGTRAP/SIGSEGV. Lands when there's a real reactor process to instrument.

## Quick-start for next session

1. `git status` → expect clean.
2. `make test` → expect 73/73.
3. Read `doc/sync/lock_free_stack.md` — the design is already there, just needs implementation.
4. Delegate to codex with the briefing pattern (point at `WindowsLibrary/Library/Include/` + `IOCP_Rookiss/Engine/MemoryPool.{h,cpp}`).
5. Review the diff carefully — ABA hazard is the spec'd subtle bug; verify the tagged-head implementation is correct.

Sleep well.
