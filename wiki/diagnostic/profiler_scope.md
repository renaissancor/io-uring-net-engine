# Scope profiler — RAII-scoped interval timing with per-name aggregation

## Purpose

Measure the elapsed wall-clock time of named code regions ("scopes"),
aggregate per scope across many invocations, and report min/avg/max/total
plus call counts. Scopes are demarcated by an RAII guard: construction
records the entry tick, destruction (or explicit `stop()`) records the
leave tick and pushes a `Record{enter, leave}` into the manager's
per-scope vector.

Use cases: instrumenting hot paths in the io_uring reactor (SQE submit,
CQE drain), packet framing/dispatch, session lifecycle hooks, and any
ad-hoc "where is the time going?" investigation during development.

Sibling tool: `profiler_deadlock` (lock-order cycle detector).
Different concern, same `diagnostic/` umbrella.

## Reference origin

- `WindowsLibrary/Library/Include/Profiler.h:6` — header
  (`Win::Profiler::Manager`, `Win::Profiler::Enter`).
- `WindowsLibrary/Library/Sources/Profiler.cpp:7` — `PrintConsoleTick`,
  `PrintConsoleTime`, `GetFunctionSummary`, `SaveDataTXT/CSV`,
  `SaveFuncCSV`.

The Linux port:
- Drops the WinAPI clock plumbing (`QueryPerformanceCounter`,
  `QueryPerformanceFrequency`, `LARGE_INTEGER`).
- Renames `Profiler::Enter` (verb) → `Profiler::Scope` (noun) to match
  scope-guard idiom (`std::scoped_lock`, `std::lock_guard`).
- Renames `sectionName` parameter → `scope_name` throughout.
- Drops `_frequency` field — `clock_gettime(CLOCK_MONOTONIC)` returns
  nanoseconds directly; no calibration step needed.
- Uses `gettid()` instead of `GetCurrentThreadId()`.
- Uses `std::ofstream` / `fmt::format` instead of `fopen_s` + `printf`.

## Public API sketch

```cpp
namespace iouring_net::diagnostic {

enum class time_unit : uint8_t {
    nanosec  = 0,
    microsec = 1,
    millisec = 2,
    sec      = 3,
};

struct record {
    uint64_t enter_ns;   // CLOCK_MONOTONIC reading at scope construction
    uint64_t leave_ns;   // CLOCK_MONOTONIC reading at scope destruction
};

struct summary {
    uint64_t total_ns  = 0;
    uint64_t min_ns    = UINT64_MAX;
    uint64_t max_ns    = 0;
    size_t   call_count = 0;
};

class profiler {
public:
    // thread_local — one manager per thread, no contention
    static profiler& instance() noexcept;

    profiler(const profiler&)            = delete;
    profiler& operator=(const profiler&) = delete;

    // Called from Scope::Scope and Scope::~Scope (or stop()).
    void add(const char* scope_name, uint64_t enter_ns, uint64_t leave_ns) noexcept;

    // Aggregation
    summary summarize(const std::vector<record>& records) const noexcept;

    // Reporting
    void print_ticks() const;                                    // raw ns
    void print_times(time_unit unit = time_unit::microsec) const;
    void save_txt(std::string_view path, time_unit unit = time_unit::microsec) const;
    void save_csv(std::string_view path, time_unit unit = time_unit::microsec) const;
    void save_func_csv(std::string_view path) const;             // raw ns CSV

    void clear() noexcept;
    pid_t thread_id() const noexcept { return tid_; }

private:
    profiler() noexcept;
    ~profiler() = default;

    container::cstr_hash_map<std::vector<record>> records_;
    pid_t                                          tid_ = 0;
};

// RAII scope guard — the only API most call sites touch.
class scope {
public:
    explicit scope(const char* scope_name) noexcept;
    ~scope() noexcept;

    scope(const scope&)            = delete;
    scope& operator=(const scope&) = delete;

    void stop() noexcept;            // early-stop; double-stop is a no-op

private:
    const char* scope_name_;
    uint64_t    enter_ns_;
    bool        stopped_ = false;
};

} // namespace iouring_net::diagnostic
```

Typical usage at a call site:

```cpp
void session::handle_packet(packet& p) {
    iouring_net::diagnostic::scope guard{"session::handle_packet"};
    // ... work ...
}  // guard destructor records leave time, pushes record into profiler
```

Or with manual early-stop:

```cpp
{
    iouring_net::diagnostic::scope guard{"setup"};
    do_expensive_setup();
    guard.stop();                  // record the setup time NOW
    do_unrelated_followup();       // not counted in "setup"
}
```

## Linux design

**Clock — `clock_gettime(CLOCK_MONOTONIC)`.** vDSO-accelerated on every
mainstream Linux distro (no syscall in the common case, ~20-30 ns/call),
monotonic, immune to NTP slewing. Returns `struct timespec { tv_sec,
tv_nsec }` — combine to `uint64_t ns = tv_sec * 1e9 + tv_nsec` and store
that as the canonical tick. No frequency calibration step (unlike
QueryPerformanceCounter), so the `_frequency` field disappears from the
manager.

Alternatives considered:

- `clock_gettime(CLOCK_MONOTONIC_RAW)` — not vDSO on kernels <4.18;
  effectively slower. No real benefit for an internal profiler.
- `__rdtsc()` — ~10 ns/call, but requires invariant TSC check, per-core
  skew handling, and CPU frequency awareness. Worth revisiting only if
  `clock_gettime` overhead shows up in profiler benchmarks (it almost
  certainly won't for this project's scope cadence).
- `std::chrono::steady_clock` — portable but on libstdc++ wraps
  `clock_gettime(CLOCK_MONOTONIC)` anyway, adding `duration<>` type
  machinery we don't need.

**Per-thread storage — `thread_local profiler instance`.** Each thread
gets its own manager. Eliminates lock contention on the hot path
(scope construction/destruction is allocation-free and lock-free in v1).
Cross-thread aggregation happens off the hot path at report time —
the reporter thread walks each thread's profiler and merges.

**Key storage — `cstr_hash_map<vector<record>>`.** The scope name is
required to be a string literal (`.rodata`) — typically
`"namespace::function"` written verbatim or via a macro. See
`wiki/data_structure/cstr_hash_map.md` for the contract. This buys
us zero key-allocation on insert and pointer-equality fast-path on
lookup for repeated scope names (the common case — same name hit
millions of times in one run).

**Tick representation — `uint64_t` nanoseconds.** Holds ~584 years of
monotonic time. No risk of overflow within a single run. Signed
arithmetic risk avoided by keeping enter/leave as `uint64_t`.

**Reporting.** All five report methods (`print_ticks`, `print_times`,
`save_txt`, `save_csv`, `save_func_csv`) iterate `records_` via the
cstr_hash_map iterator and call `summarize()` per scope. They're meant
for end-of-run analysis — not on the hot path.

**File I/O — `std::ofstream` + `fmt::format`.** Replaces `fopen_s` +
`fprintf` from the Windows source. `fmt::format` is already in the
project (per `feedback_pragmatic_versioning`). Use `std::format` directly
once minimum compiler bumps past gcc-13.

**Thread identity — `gettid()`.** Direct Linux kernel TID, suitable for
correlating with `perf`, `top -H`, `htop` thread view. `std::this_thread::
get_id()` is portable but opaque — useless for external tooling.
Include `<sys/types.h>` and `<unistd.h>`; `gettid()` was wrapped by
glibc in 2.30 (2019). For older glibc, fall back to
`syscall(SYS_gettid)`.

## Concurrency & ownership

**Multithreaded by design via TLS isolation.** This is the load-bearing
design choice — the profiler is *built* for multithreaded targets, not
patched into supporting them.

- Each thread holds its own `thread_local profiler` instance. N threads
  entering scopes concurrently touch **N different objects**, so there
  is **zero contention** on the hot path. No atomics, no locks, no
  cache-line ping-pong. A scope entry on thread A cannot stall a scope
  entry on thread B, even if they share the same scope name — because
  each thread has its own `cstr_hash_map<vector<record>>`.
- The per-thread instance is constructed lazily on first scope entry
  on that thread, destroyed at thread exit. No global init order issue,
  no need to enumerate threads up front.
- Cross-thread aggregation happens **off the hot path** at report time.
  The reporter thread walks the set of registered thread profilers and
  merges their `records_` maps. The registration list itself is
  protected by a mutex, but registration happens **once per thread**
  (on first scope entry), never per scope.
- The merge is straightforward because scope names are
  pointer-comparable literals (see `cstr_hash_map`): merging two threads'
  maps for the same scope `"X"` is just `std::vector::insert` of one
  thread's records into the other's vector.

**Other ownership rules:**

- The `scope` RAII type is stack-only by design (no copy, no move).
  Heap-allocating a scope would defeat the RAII contract — there'd be
  no guaranteed destruction at end-of-region.
- The profiler's `cstr_hash_map` insertion may allocate (`new Node`)
  on first occurrence of a scope name. Amortized: by the second
  invocation, the node already exists and `push_back` into the existing
  vector is the only cost. If first-call allocation latency ever
  shows up in a real workload, pre-warm with a startup pass that
  references every expected scope name once.

## Test plan

- **Unit — single scope.** Time a `usleep(10'000)`; assert one record
  with `leave_ns - enter_ns` in the range [10ms, 11ms].
- **Unit — repeated scope.** 1000 entries with `usleep(100)`; assert
  call count = 1000 and summary min/max/avg are sane (avg > 100µs).
- **Unit — nested scopes.** Outer + inner scope with the same and
  different names; assert outer covers inner's interval and both
  records exist independently.
- **Unit — early stop.** Construct scope, call `stop()`, then destruct;
  assert only ONE record was added (the destructor must observe
  `stopped_` and no-op).
- **Unit — thread isolation.** Two threads each register the scope
  `"X"`; assert each thread's profiler has its own vector and counts
  don't bleed across threads.
- **Unit — report format.** Save CSV to a temp file, parse it back,
  assert column headers and at least one data row match.
- **Microbench.** Measure overhead of an empty scope (`{ scope g{"x"}; }`)
  — target: <100 ns per entry/exit pair on modern x86-64.

## Open questions

1. **What identifies a scope?** Currently `const char*` literal. If a
   call site needs runtime-composed names (e.g., `"handle_packet_" +
   packet_type_name`), the literal contract breaks. Options: (a) reject
   the use case ("don't"), (b) add a separate `dynamic_scope` API that
   uses `std::string` keys at higher cost, (c) require the caller to
   intern the string. **Initial decision: (a) — literals only. Revisit
   if a real use case appears.**
2. **Auto-instrumentation via macros?** A `PROFILE_SCOPE()` macro using
   `__func__` saves typing but couples to the compiler's `__func__`
   semantics (which may or may not be a `.rodata` literal). Verify with
   `readelf -p .rodata` before relying on `__func__` as a literal.
3. **Histogram vs. min/max/avg.** Min/avg/max hides bimodality (e.g., a
   handler that's fast on cache hit and slow on cache miss looks the
   same average as a uniformly mediocre handler). A 10-bucket
   logarithmic histogram per scope would be more honest. Cost: O(N)
   buckets per scope instead of 1 SummaryData. Defer until v1 results
   show ambiguous distributions.
4. **Thread-local profiler shutdown order.** `thread_local` destruction
   order across threads at process exit is well-defined (each thread
   destructs its own `thread_local` at thread exit), but the *order of
   thread exits* relative to the reporter thread is not. If the reporter
   runs after some worker threads have already exited, their records
   are gone. **Resolution:** workers must call `profiler::instance()
   .move_records_to_global_sink()` before exit, OR the reporter must
   run while all workers are alive. Document the choice in `Service`'s
   shutdown sequence (see `wiki/network/listener_and_service.md`).
5. **`__rdtsc()` upgrade path.** Document where the clock-read happens
   so a future swap from `clock_gettime` to `__rdtsc()` is a single-
   function-rewrite, not a scattered grep. Helper: `inline uint64_t
   now_ns() noexcept`.
