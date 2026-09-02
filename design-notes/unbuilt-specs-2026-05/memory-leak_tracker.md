# Leak tracker — debug-build allocation tracing

## Purpose

Record every allocation with its call-site and a sequence number, and report
any unfreed allocations at process exit. Catches the "object held by a
forgotten reference" case that ASan can't see (because there's no
out-of-bounds write or use-after-free — just a logical leak).

Debug-build only. Zero overhead in release.

## Reference origin

- `WindowsLibrary/Library/Include/NewTracer.h:33` — `NewTracer::Manager`,
  the singleton that records and reports.

## Public API sketch

```cpp
namespace iouring_net::mem::debug {

struct allocation_record {
    void*           ptr;
    size_t          size;
    uint64_t        seq;
    const char*     file;
    int             line;
    const char*     function;
};

class leak_tracker {
public:
    static leak_tracker& instance();

    void record_alloc(void* p, size_t n, std::source_location loc);
    void record_free(void* p);

    std::vector<allocation_record> live_allocations() const;
    void report(std::ostream& out) const;          // pretty-print live set

    void set_break_on_seq(uint64_t seq);            // SIGTRAP at this allocation
};

// Macros that wrap mem::xnew / xdelete in debug builds
#if IOURING_NET_DEBUG_LEAK_TRACKER
  #define IOURING_NET_NEW(T, ...)    \
      (::iouring_net::mem::debug::xnew_tracked<T>(std::source_location::current(), ##__VA_ARGS__))
#else
  #define IOURING_NET_NEW(T, ...)    (::iouring_net::mem::xnew<T>(__VA_ARGS__))
#endif

} // namespace iouring_net::mem::debug
```

The macro is the typical entry point in instrumented builds. Direct
`mem::xnew` calls bypass the tracker — that's intentional for hot-path code
that we deliberately don't trace.

## Linux design

**Storage.** `std::unordered_map<void*, allocation_record>` guarded by a
`std::mutex`. Inserts on alloc, erases on free. Slow vs. uninstrumented
allocation; debug-only.

**Sequence number.** Monotonic atomic counter. Useful for "break on the
N-th allocation" (`set_break_on_seq`) — when leaks are reproducible by
sequence, this is by far the fastest way to root-cause.

**Source location.** `std::source_location::current()` is the C++20 way;
zero macro tricks. Reference repo uses `__FILE__`/`__LINE__` — we
upgrade.

**Report format.** Sorted by size descending, grouped by call site:

```
==== engine-uring leak report ====
Live allocations: 14
Total bytes: 12,288

  4096 B × 2 — Session::Session   (src/network/session.cpp:42)
  1024 B × 5 — RecvRingBuffer     (src/primitives/ring_buffer.cpp:18)
   ...
```

**Symbol resolution.** `std::source_location` already gives file/line/function;
no `backtrace_symbols` needed. If we want a full stack trace, layer in
`std::stacktrace` (C++23, libstdc++ 14+) — defer.

## Concurrency & ownership

- Singleton; `instance()` is Meyers static.
- All public methods take the internal mutex. Performance is poor under
  contention by design — instrument only suspect paths.
- Process exit: a `static` destructor calls `report(std::cerr)` if any
  live allocations remain. In release builds the tracker doesn't exist.

## Test plan

- Unit: alloc 3, free 2 — `live_allocations().size() == 1` with the
  expected record.
- Unit: `set_break_on_seq` triggers in debug build (use a signal handler
  to make it survivable in tests, or run in a child process).
- Integration: full echo-server run under instrumented build; assert
  zero leaks at shutdown.

## Open questions

1. **Per-thread vs. global storage.** Reference uses global. A per-thread
   ring with periodic merging would have lower contention but more
   complexity. Defer; instrument only when needed.
2. **Stacktrace per allocation.** Powerful for root-causing but slow even
   to capture. Make it a runtime knob (`--leak-tracker-stacktrace`) so a
   developer can enable per-run.
3. **Coexistence with ASan.** ASan's leak detector handles the C-runtime
   case. Our tracker covers project-pool allocations specifically. Run
   ASan and our tracker in separate CI jobs; the union catches more.
