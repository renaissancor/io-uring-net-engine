# Deadlock profiler — lock-order graph cycle detection

## Purpose

Detect potential deadlocks at runtime by recording the order in which a
thread acquires locks and reporting any cycle in the resulting global
lock-order graph. Doesn't prevent deadlocks; it surfaces them in test/QA
runs before they happen in production.

Debug-build only. Zero runtime cost in release.

## Reference origin

- `IOCP_Rookiss/Engine/DeadLockDebugger.h:11`,
  `Engine/DeadLockDebugger.cpp:7` — class scaffolding (lock-tracking
  fields, `on_lock`/`on_unlock` mechanics) is implemented. **However,
  `CheckCycle()` is declared at `DeadLockDebugger.h:31` but has no
  implementation in the reference repo** — the cycle detection itself
  was never written. Our port actually implements DFS-based cycle
  detection. The design is OS-agnostic; only the host-mutex type
  underneath needs swapping.

## Public API sketch

```cpp
namespace iouring_net::sync::debug {

class deadlock_profiler {
public:
    static deadlock_profiler& instance();

    // Called by instrumented locks
    void on_lock(std::string_view name, void* obj);
    void on_unlock(std::string_view name, void* obj);

    // Called by tests / shutdown
    [[nodiscard]] std::vector<std::string> detect_cycles() const;

private:
    // Global edge set: (held_lock_name -> next_lock_name) for every
    // thread-observed acquisition order.
    std::unordered_map<std::string, std::set<std::string>> edges_;
    mutable std::mutex                                     edges_mutex_;

    static thread_local std::vector<std::string>           held_stack_;
};

// Instrumented lock wrapper used in debug builds
template <class Mutex>
class profiled_lock_guard {
public:
    explicit profiled_lock_guard(Mutex& m, std::string_view name);
    ~profiled_lock_guard();
private:
    Mutex&               mutex_;
    std::string_view     name_;
};

} // namespace iouring_net::sync::debug
```

In debug builds, `iouring_net::sync::lock_guard` aliases to
`profiled_lock_guard`. In release builds, it aliases to
`std::lock_guard`. The instrumentation is opt-in per-translation-unit
via a build flag — instrumenting *every* lock site adds nontrivial
overhead.

## Linux design

**Per-thread held-stack.** `thread_local std::vector<std::string>
held_stack_` records the names of currently-held locks on this thread.
On `on_lock(name)`:
1. For each `prior` in `held_stack_`, add edge `prior -> name` to the
   global graph.
2. Push `name` onto `held_stack_`.

On `on_unlock(name)`: pop from the stack. (Mismatched name → assertion;
that's a bug in the instrumented wrapper, not a deadlock.)

**Cycle detection.** DFS over the global edge graph. Run from the test
harness or from `Service::shutdown()`. Reports every simple cycle with the
sequence of lock names. Identical algorithm to the reference repo, just
with `std::unordered_map<string, set<string>>` instead of the Windows
custom containers.

**Lock identity.** Lock *names* (compile-time `std::string_view`), not
addresses. Names come from a project-wide convention: every lock is
declared with a `LOCK_NAME("category/role")` macro that registers the name
once. This avoids the "same lock appears at different addresses each run"
problem.

**Storage cost.** Edge set size is bounded by `O(N²)` where N is the number
of distinct lock names. For this project N is small (~10–20).

## Concurrency & ownership

- Singleton. Constructed on first use.
- Internal `edges_mutex_` protects the global graph. Acquired only on lock
  events, not during cycle detection (snapshot under the mutex, run DFS
  off-snapshot).
- Per-thread held stack is `thread_local`; no synchronization needed.
- The profiler's own lock cannot itself be instrumented — `edges_mutex_`
  is taken with `std::lock_guard<std::mutex>` directly, never the
  project's `sync::lock_guard`.

## Test plan

- Unit: take A then B on thread 1; take B then A on thread 2; call
  `detect_cycles()`; assert `["A -> B -> A"]` (or equivalent) in result.
- Unit: take A only; assert no cycles.
- Unit: chain of 5 locks A -> B -> C -> D -> E with no back-edge; assert
  no cycles.
- Unit: 8 threads taking various subsets of {A, B, C, D, E} with two
  threads constructing a cycle — assert cycle detected.
- Performance: instrumented build vs. uninstrumented build on the
  memory-pool stress test; report overhead.

## Open questions

1. **Per-instance vs. per-name granularity.** Reference uses names; we
   follow. Per-instance edges would catch real deadlocks that don't show
   up in the named-class graph (two instances of the same class taken
   in opposite order). Out of scope for v1; document as a known false-
   negative class.
2. **Reporting in CI.** Detect cycles at the end of every integration
   test run. Failing tests should print the cycle. **Implement as a
   Catch2 reporter.**
3. **Should we ship at all?** Code is small and self-contained; the
   discipline it enforces (lock naming, ordered acquisition) is valuable
   even if the detector is never used. Keep it.
4. **Heisenbug risk.** Instrumented mutexes have different timing than
   plain ones; bugs that reproduce in release may not reproduce under
   instrumentation. This is intrinsic to all lock-instrumentation tools.
   Run instrumented builds *and* TSan separately.
