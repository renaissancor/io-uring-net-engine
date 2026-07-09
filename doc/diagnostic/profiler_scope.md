# profiler_scope — RAII interval timing with per-thread, per-name aggregation

> **Status:** landed
> **Source:** `src/diagnostic/profiler_scope.{h,cpp}`
> **Namespace:** `profiler`
> **Depends:** `sds/cstr_hash_map`, `sds/malloc_vector`

## Purpose

Lightweight scope timing: a `profiler::scope` RAII guard records the wall-clock
interval between construction and destruction under a named key; a per-thread
`profiler::manager` singleton aggregates those intervals and reports/exports
them. TLS isolation is the multithreading strategy — each thread has its own
`manager`, so recording is contention-free with no atomics.

## API

```cpp
namespace profiler {

enum class time_unit : uint8_t { nanosec, microsec, millisec, sec };

struct record       { uint64_t enter_ns = 0, leave_ns = 0; };
struct summary_data { uint64_t total_ns, min_ns, max_ns; size_t call_count; };
using  record_list  = sds::malloc_vector<record>;

uint64_t now_ns() noexcept;    // CLOCK_MONOTONIC in nanoseconds

class manager {                 // per-thread singleton; non-copyable, non-movable
public:
    static manager& instance() noexcept;         // one per thread (thread_local)

    void add(const char* scope_name, uint64_t enter_ns, uint64_t leave_ns) noexcept;
    summary_data summarize(const record_list&) const noexcept;

    void print_ticks() const;
    void print_times(time_unit = time_unit::microsec) const;
    void save_txt (std::string_view path, time_unit = time_unit::microsec) const;
    void save_csv (std::string_view path, time_unit = time_unit::microsec) const;
    void save_func_csv(std::string_view path) const;

    void clear() noexcept;
    pid_t       thread_id()   const noexcept;
    size_t      scope_count() const noexcept;
    const record_list* records_for(const char* scope_name) const noexcept;
};

class scope {                   // RAII; non-copyable, non-movable
public:
    explicit scope(const char* scope_name) noexcept;   // captures enter_ns
    ~scope() noexcept;                                  // records unless stopped
    void stop() noexcept;                               // record now, early
};

}  // namespace profiler
```

## Invariants

- **One `manager` per thread** (`thread_local` via `instance()`); a worker's and
  the main thread's managers are different objects. No cross-thread merge in v1;
  reports are per-thread.
- Storage is `cstr_hash_map<record_list>` keyed by scope name → the name is a
  **borrowed `const char*`** (must outlive the manager; use `.rodata` literals —
  see [[cstr_hash_map]]). Each key maps to a `malloc_vector<record>` of intervals.
- `scope` records exactly once: at destruction, or earlier at `stop()` (after
  which the destructor does nothing). Non-movable — it is a stack guard.
- `manager` and `scope` are non-copyable and non-movable.

## Errors & edge cases

- `records_for` returns `nullptr` for an unseen scope name.
- `summarize` on an empty `record_list` yields `call_count == 0`,
  `min_ns == UINT64_MAX`, `max_ns == 0` (identity values — caller should treat
  `call_count == 0` as "no data").
- `save_*` perform file I/O; failure handling is I/O-path (not on the timing hot
  path). `now_ns()`/`add()` never fail.

## Notes

- `now_ns()` uses `CLOCK_MONOTONIC`; the Linux port of the original Windows
  profiler swaps `QueryPerformanceCounter` for `clock_gettime`.
- The `manager` depends on both `sds::` containers precisely because they avoid
  re-entering global `new` on the recording path ([[malloc_vector]]) and key by
  literal names ([[cstr_hash_map]]).

## Test plan

`tests/diagnostic/profiler_scope_test.cpp`: scope records an interval on
destruction; `stop()` records early and suppresses the destructor; `summarize`
min/max/total/count; per-thread isolation (two threads' managers are distinct);
`records_for`/`scope_count`; export smoke for `save_csv`/`save_txt`.

## Done when

- [x] Builds on `default` and `floor` presets
- [x] Tests pass under ASan+UBSan
- [x] This spec matches the built API

## Rationale

- Per-thread TLS design is the multithreading strategy, not a single-thread
  limitation — see `design/` and the profiler TLS decision.
- Depends on [[cstr_hash_map]] (borrowed-key) and [[malloc_vector]] (no-`new` growth).
