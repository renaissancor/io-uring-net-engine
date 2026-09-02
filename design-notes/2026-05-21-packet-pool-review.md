---
status: accepted
amended_by:
  - 2026-05-25-handle-engine-split.md
note: the app::worker entry-point shape in this note was replaced by the handle/engine split
---
# 2026-05-21 — packet_pool review todo

Findings from the Claude + Codex pass on the freshly-landed `packet_pool`
(Phase 0 first step). Recording each item without acting; discuss
individually in a later session.

**Scope of code under review:**
- `src/memory/packet_pool.h`
- `src/memory/packet_pool.cpp`
- ~~`src/runtime/worker_entry.h`~~ (deleted 2026-05-23 — see item 1)
- `tests/memory/packet_pool_test.cpp`

**Status at review:** ASan+UBSan build green; full suite 79 cases / 29,139 assertions; all 6 new packet_pool cases pass.

## 1. `worker_start` dangling-pointer window — RESOLVED 2026-05-23 (by deletion)

**Resolution:** `worker_start` POD and `worker_entry` trampoline were deleted entirely. The generic-trampoline pattern was YAGNI scaffolding ahead of `app::worker`; it added a lifetime contract (the dangling-pointer footgun) and baked in an incorrect "always prewarm packet_pool" assumption (would be wrong for `db_thread`, which doesn't allocate packets). When `app::worker` lands, it will own a private `static void* entry_point(void*)` that takes `this` as the arg — no transient struct, no lifetime question. Per-class entry points let each class declare its own setup needs.

The packet_pool test that previously exercised the trampoline was rewritten to call `prewarm()` inline from a captureless lambda passed directly to `lnx::thread` — covers the same TLS-singleton-on-a-spawned-thread surface area without the trampoline machinery.

---

**Original finding (for context):**

`lnx::thread` passes the caller's raw `arg` straight to `pthread_create`. `worker_entry()` dereferences `worker_start*` after running `prewarm()`. Caller had to keep `worker_start` alive until the trampoline read it. The test passed only because `start` outlived `t.join()`. Future caller writing `lnx::thread t{worker_entry, &local};` and returning from the enclosing scope before join → UB.

## 2. `fflush(stderr)` before `LNX_CHECK` on exhaustion (SHOULD-FIX)

`acquire()` writes the fatal diagnostic with `fmt::print(stderr, ...)` and immediately traps. `LNX_CHECK` does not flush. Block-buffered stderr (no tty, e.g. systemd capture) can lose the line.

**Open questions to discuss:**
- One-line fix here, or generalize as `LNX_FATAL(fmt_string, args...)` macro that prints + fflush + trap?
- Worth applying retroactively to other call sites where a diagnostic precedes `LNX_CHECK`?

References: `src/memory/packet_pool.cpp:108-114`, `src/check.h:32`.

## 3. acquire-before-prewarm reports as exhaustion (SHOULD-FIX)

`_prewarmed` is tracked but `acquire()` doesn't check it. Calling `acquire()` before `prewarm()` produces `fatal: packet_pool bucket exhausted` with `current_in_use = 0` — wrong cause, misleading debug.

**Open questions to discuss:**
- Add `LNX_CHECK(_prewarmed)` with a distinct "used before prewarm" diagnostic.
- Cost is one branch on the *exhaustion* path (already off hot path), so free.
- Could the trampoline pattern make this impossible by construction (mandatory prewarm at thread start)? If so, the runtime check is belt-and-braces.

References: `src/memory/packet_pool.cpp:72`, `src/memory/packet_pool.cpp:87`, `src/memory/packet_pool.cpp:107`, `.omc/wiki/memory-pool-tls-singleton-mmap-design-decision.md:61`.

## 4. Missing `thread_id` in exhaustion diagnostic (SHOULD-FIX)

The wiki's example crash log includes `thread_id = worker_0`. Current implementation prints only `bucket_size`, `prewarm_count`, `current_in_use`.

**Open questions to discuss:**
- Use `::syscall(SYS_gettid)` directly in `packet_pool.cpp` (Codex's recommendation — keeps layers clean).
- Or wait for a thread-naming convention (`worker_0`, `db_thread`, etc.) before stamping a string?
- Or both: kernel tid now, named identity when the naming layer arrives.

References: `src/memory/packet_pool.cpp:108-113`, `.omc/wiki/memory-pool-tls-singleton-mmap-design-decision.md:80`.

## 5. `/proc/self/maps` munmap proof is fragile (SHOULD-FIX → arguably nice-to-have)

`count_mappings_of_size()` matches any anon mapping with the exact byte size; could collide with unrelated mappings of the same size disappearing or appearing concurrently. Current risk is low (delta-based, our region size of ~610304 B is uncommon), but it's not a tight test.

**Open questions to discuss:**
- Add `friend`-based test accessor for `_region` / `_region_size`, then use `mincore()` post-join to assert the exact address range is gone.
- Or keep the rough check and live with the slight flakiness risk.
- Does this matter at v1, given Phase 0 exit criteria says "ASan clean, valgrind clean"?

References: `tests/memory/packet_pool_test.cpp:94-104`, `tests/memory/packet_pool_test.cpp:194-206`.

## 6. Double-release / use-after-release undetected (CONTENTIOUS)

`release()` checks the pointer is within bucket range and `in_use > 0`, but doesn't prove the block isn't already on the free list. Double-release of the same pointer corrupts the free list silently.

The wiki explicitly accepts this: *"Cross-thread free silently corrupts both pools' bookkeeping."* So this is *spec-compliant*. But Codex flagged it as Should-fix.

**Open questions to discuss:**
- Add a debug-only (e.g. `#ifndef NDEBUG`) bitmap or sorted set tracking in-use blocks?
- Or leave as-is — hot path stays branch-free, and the discipline is "the caller is responsible"?
- If we add detection, what's the abort trigger? `LNX_CHECK(!already_in_free_list)`?

References: `src/memory/packet_pool.cpp:127-132`, wiki: *"alloc-thread == free-thread invariant"*.

## 7. Worker-entry doc/code mismatch — SUPERSEDED 2026-05-23 (by deletion)

**Resolution:** `worker_entry.h` was deleted alongside `worker_start` (see item 1). The doc/code mismatch is moot because the file no longer exists. When `app::worker::entry_point` lands it will be its own pass with its own doc — not a carryover from the trampoline scaffolding.

---

**Original finding (for context):**


`worker_entry.h` doc-comment lists two steps:
```
[0] anchor mem::packet_pool::instance() — mmaps the region
[1] packet_pool::prewarm()              — populate free lists
```
…but the code does both in one chained call: `mem::packet_pool::instance().prewarm();`.

**Open questions to discuss:**
- Split into the two-line form so the code mirrors the doc. (Cosmetic, but a portfolio reader's confusion = lost signal.)

References: `src/runtime/worker_entry.h:14-29`.

## 8. Release-side alignment check (NICE-TO-HAVE)

`release()` validates `region_begin <= block < region_end` but not `(block - region_begin) % block_size == 0`. A pointer inside the bucket region but off a block boundary passes the range check and corrupts the free list.

**Open questions to discuss:**
- Adds one modulo per release. Cheap. Worth it?
- Same "alloc-thread == free-thread invariant" discipline tradeoff as item 6.

References: `src/memory/packet_pool.cpp:127-131`.

## Items explicitly NOT-on-this-list (deliberately omitted)

- **Strict-aliasing of `free_node` cast.** Both Claude and Codex agree this is OK: mmap storage is implicit-lifetime, the block is alive as `free_node` while on the free list and as raw bytes while acquired, no concurrent typed alias. Bears watching when the pool starts handing out non-trivial C++ objects (placement-new + explicit dtor required).
- **Header hygiene.** `packet_pool.h` includes only `types.h`; cpp pulls `<fmt/core.h>` and `<sys/mman.h>`. Correct as-is.
- **CMake / FILE_SET exposure.** `packet_pool.h` is intentionally *not* in the public FILE_SET — internal to the library, consumed by future in-repo chat-server.
- **Prewarm reverse-iteration ordering.** Internal detail. Cpp has a comment; header makes no promise (correctly).

## Suggested discussion order

1. (1) — only Must-fix; closes a real footgun before the worker code lands.
2. (2), (3), (4) — small, cheap, fix together as a "diagnostic polish" pass.
3. (6) vs. (8) — both touch the alloc/free-thread discipline; decide together.
4. (5) — when the test framework gets more sophisticated (after `mincore`-based assertion helper exists).
5. (7) — fold into whatever next touches `worker_entry.h`.

## Pointers to the two reviewers' raw output

Both reviews live in this session's transcript (2026-05-21). If transcript is rotated, the canonical findings are this file.
