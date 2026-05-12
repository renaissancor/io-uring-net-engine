# Scope profiler — RAII-scoped interval timing with per-name aggregation

## Purpose

Measure the elapsed wall-clock time of named code regions ("scopes"),
aggregate per scope across many invocations, and report min/avg/max/total
plus call counts. Scopes are demarcated by an RAII guard: construction
records the entry tick, destruction (or explicit `stop()`) records the
leave tick and pushes a `record{enter, leave}` into the manager's
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

## Windows comparison

The Linux port preserves the core Windows design: one `thread_local`
manager per thread, a lightweight RAII guard, `cstr_hash_map` keyed by
raw literal names, malloc-backed per-scope record arrays, and reporting
methods for raw ticks plus converted time summaries.

Important differences from `WindowsLibrary/Library/Include/Profiler.h`
and `Sources/Profiler.cpp`:

| Windows `Win::Profiler` | Linux `profiler` | Reason |
|---|---|---|
| `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `LARGE_INTEGER` | `clock_gettime(CLOCK_MONOTONIC)` and `uint64_t` nanoseconds | Linux monotonic clock already returns time units; no frequency calibration field is needed. |
| `Manager::GetInstance()` | `manager::instance()` | Project naming convention. |
| `Enter` / `Leave()` | `scope` / `stop()` | Matches C++ scope-guard vocabulary; `stop()` remains idempotent like Windows `Stop()`. |
| `sectionName`, `functionName` labels | `scope_name`, `Scope ...` labels | The profiler measures arbitrary named regions, not only functions. |
| `DWORD GetThreadId()` | `pid_t thread_id()` | Linux TID is useful with `perf`, `top -H`, and `htop`. |
| `Frequency()`, `GetUnitStr()`, `GetUnitMultiplier()` public helpers | Not public | No frequency exists in the Linux representation; unit helpers are implementation details. |
| `long long` tick fields | `uint64_t enter_ns`, `leave_ns` | Monotonic nanoseconds are non-negative and have enough range for process lifetime. |
| `std::vector<Record>` | `sds::malloc_vector<record>` | Matches the low-level profiler/memory-tracking direction: contiguous growth with `malloc/free`, not `operator new`. |
| `printf`, `fprintf`, `fopen_s` | `{fmt}` and `std::ofstream` | Existing project dependency and portable C++ file I/O. |
| `Function Name,Total Ticks` CSV headers | `Scope Name,Total ns` CSV headers | Raw Linux values are nanoseconds, not opaque platform ticks. |

Implementation additions not present in the Windows code:

- `now_ns()` isolates clock reads so a future `rdtsc` experiment is a
  single helper rewrite.
- `records_for()` and `scope_count()` expose a small read-only query
  surface for tests and diagnostics.
- `scope` explicitly deletes move as well as copy. The Windows guard is
  implicitly non-copyable in practice but does not spell out move
  policy.
- Null scope names trip a debug `assert`; the Windows source assumes
  non-null and would fail later while hashing.

Missing or intentionally deferred relative to Windows:

- No `Leave()` compatibility alias. New Linux call sites should use
  `stop()`. Add an alias only if porting old call sites becomes noisy.
- Report methods currently return `void` and silently skip an unopened
  file. Windows prints an error to `std::cerr`. If callers need to act on
  report failures, change these to return `expected<void, error>`.
- No global cross-thread report merge yet. Like the Windows sample app,
  each thread can report its own manager; a shared global sink is future
  work.

## Public API sketch

```cpp
namespace profiler {

enum class time_unit : uint8_t {
    nanosec,
    microsec,
    millisec,
    sec,
};

struct record {
    uint64_t enter_ns;   // CLOCK_MONOTONIC reading at scope construction
    uint64_t leave_ns;   // CLOCK_MONOTONIC reading at scope destruction
};

struct summary_data {
    uint64_t total_ns  = 0;
    uint64_t min_ns    = UINT64_MAX;
    uint64_t max_ns    = 0;
    size_t   call_count = 0;
};

using record_list = sds::malloc_vector<record>;

uint64_t now_ns() noexcept;

class manager {
public:
    // thread_local — one manager per thread, no contention
    static manager& instance() noexcept;

    manager(const manager&)            = delete;
    manager& operator=(const manager&) = delete;
    manager(manager&&)                 = delete;
    manager& operator=(manager&&)      = delete;

    // Called from scope::scope and scope::~scope (or stop()).
    void add(const char* scope_name, uint64_t enter_ns, uint64_t leave_ns) noexcept;

    // Aggregation
    summary_data summarize(const record_list& records) const noexcept;

    // Reporting
    void print_ticks() const;                                    // raw ns
    void print_times(time_unit unit = time_unit::microsec) const;
    void save_txt(std::string_view path, time_unit unit = time_unit::microsec) const;
    void save_csv(std::string_view path, time_unit unit = time_unit::microsec) const;
    void save_func_csv(std::string_view path) const;             // raw ns CSV

    void clear() noexcept;
    pid_t thread_id() const noexcept;
    size_t scope_count() const noexcept;
    const record_list* records_for(const char* scope_name) const noexcept;

private:
    manager() noexcept;
    ~manager() = default;

    sds::cstr_hash_map<record_list> records_;
    pid_t                           tid_ = 0;
};

// RAII scope guard — the only API most call sites touch.
class scope {
public:
    explicit scope(const char* scope_name) noexcept;
    ~scope() noexcept;

    scope(const scope&)            = delete;
    scope& operator=(const scope&) = delete;
    scope(scope&&)                 = delete;
    scope& operator=(scope&&)      = delete;

    void stop() noexcept;            // early-stop; double-stop is a no-op

private:
    const char* scope_name_;
    uint64_t    enter_ns_;
    bool        stopped_ = false;
};

} // namespace profiler
```

Typical usage at a call site:

```cpp
void session::handle_packet(packet& p) {
    profiler::scope guard{"session::handle_packet"};
    // ... work ...
}  // guard destructor records leave time, pushes record into profiler
```

Or with manual early-stop:

```cpp
{
    profiler::scope guard{"setup"};
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

**Per-thread storage — `thread_local manager instance`.** Each thread
gets its own manager. Eliminates inter-thread lock contention on the hot
path. Scope construction is just a clock read; `stop()` is a clock read
plus append into the current thread's vector. First use of a scope name,
or vector growth, may allocate. Cross-thread aggregation is not
implemented in v1; each thread reports its own manager, matching the
Windows sample app's thread-local output pattern.

**Key/value storage — `cstr_hash_map<malloc_vector<record>>`.** The
scope name is required to be a string literal (`.rodata`) — typically
`"namespace::function"` written verbatim or via a macro. See
`wiki/sds/cstr_hash_map.md` for the key contract and
`wiki/sds/malloc_vector.md` for the record-list contract. This buys us
zero key-allocation on insert, pointer-equality fast-path on lookup for
repeated scope names, and malloc/free-backed contiguous record storage.

**Tick representation — `uint64_t` nanoseconds.** Holds ~584 years of
monotonic time. No risk of overflow within a single run. Signed
arithmetic risk avoided by keeping enter/leave as `uint64_t`.

**Reporting.** All five report methods (`print_ticks`, `print_times`,
`save_txt`, `save_csv`, `save_func_csv`) iterate `records_` via the
cstr_hash_map iterator and call `summarize()` per scope. They're meant
for end-of-run analysis — not on the hot path. The Linux implementation
uses `Scope` labels instead of Windows `Function` labels because named
regions can cover arbitrary code blocks.

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

- Each thread holds its own `thread_local manager` instance. N threads
  entering scopes concurrently touch **N different objects**, so there
  is **zero contention** on the hot path. No atomics, no locks, no
  cache-line ping-pong. A scope entry on thread A cannot stall a scope
  entry on thread B, even if they share the same scope name — because
  each thread has its own `cstr_hash_map<malloc_vector<record>>`.
- The per-thread instance is constructed lazily on first scope entry
  on that thread, destroyed at thread exit. No global init order issue,
  no need to enumerate threads up front.
- v1 has no registry of live thread profilers and no cross-thread merge.
  This is deliberate for the first port: the hot path stays simple, and
  each worker can save its own TXT/CSV reports during shutdown. A future
  global sink can merge per-thread maps once service shutdown semantics
  are in place.
- A future merge remains straightforward because scope names are
  pointer-comparable literals (see `cstr_hash_map`): merging two
  threads' maps for the same scope `"X"` is record-list append into the
  aggregate scope bucket.

**Other ownership rules:**

- The `scope` RAII type is stack-only by design (no copy, no move).
  Heap-allocating a scope would defeat the RAII contract — there'd be
  no guaranteed destruction at end-of-region.
- The profiler's `cstr_hash_map` insertion may allocate (`new Node`)
  on first occurrence of a scope name. The `malloc_vector` may allocate
  or grow via `malloc` on first record inserts. Amortized: by the second
  invocation, the map node already exists and record append is just a
  capacity check plus store unless the record list grows. If first-call
  allocation latency ever shows up in a real workload, pre-warm with a
  startup pass that references every expected scope name once.

## Test plan

- **Landed — summary.** Explicit records summarize total/min/max/count.
- **Landed — empty summary.** Empty record vectors report zero count,
  zero total, zero min, and zero max.
- **Landed — single scope.** A short RAII scope records one interval
  with `leave_ns >= enter_ns` and positive elapsed time.
- **Landed — nested scopes.** Outer and inner scopes record
  independently; the outer interval covers the inner interval.
- **Landed — early stop.** `stop()` is idempotent; destructor does not
  add a second record.
- **Landed — thread isolation.** Main and worker threads each record
  their own `"X"` scope without sharing vectors.
- **Landed — report format.** `save_csv()` and `save_func_csv()` write
  expected headers and rows to temporary files.
- **Missing — repeated stress.** Add a 1000-iteration repeated-scope test
  once test runtime budget is clearer.
- **Missing — TXT/console format.** `save_txt()`, `print_ticks()`, and
  `print_times()` are not captured in tests yet.
- **Missing — microbench.** Measure empty-scope overhead; target remains
  under ~100 ns per entry/exit pair on modern x86-64.

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
   runs after some worker threads have already exited, their records are
   gone. **v1 decision:** each worker reports its own manager before
   exit, like the Windows sample app. **Future option:** add a global
   sink and have workers transfer records during controlled service
   shutdown. Document the choice in `Service`'s shutdown sequence (see
   `wiki/network/listener_and_service.md`).
5. **`__rdtsc()` upgrade path.** Document where the clock-read happens
   so a future swap from `clock_gettime` to `__rdtsc()` is a single-
   function-rewrite, not a scattered grep. Helper: `inline uint64_t
   now_ns() noexcept`.
