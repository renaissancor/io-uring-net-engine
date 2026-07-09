# thread — pthread-backed thread wrapper

> **Status:** landed
> **Source:** `src/runtime/thread.h`
> **Namespace:** `lnx` (+ `lnx::this_thread`)
> **Depends:** `check`

## Purpose

A small RAII owner for a native pthread, plus current-thread helpers. The
Linux counterpart to `std::thread`/`std::jthread`, deliberately lower-level: it
makes `pthread_create/join/detach` and `gettid()` behavior explicit for a
systems library. C-style entry point only (`void*(*)(void*)`), which is what the
supervisor/worker/acceptor trampolines use.

## API

```cpp
namespace lnx {

class thread {                              // non-copyable; MOVABLE
public:
    thread() noexcept;                      // not joinable
    thread(void* (*fn)(void*), void* arg) noexcept;   // pthread_create; traps on failure
    ~thread() noexcept;                     // traps if still joinable (must join/detach first)
    thread(thread&&) noexcept;              thread& operator=(thread&&) noexcept;

    void      join()   noexcept;            // traps if not joinable / on pthread error
    void      detach() noexcept;            // traps if not joinable / on pthread error
    bool      joinable() const noexcept;
    pthread_t native_handle() const noexcept;
};

namespace this_thread {
    void      yield() noexcept;             // sched_yield
    void      sleep_for_ns(int64_t ns) noexcept;   // clock_nanosleep(CLOCK_MONOTONIC); tolerates EINTR
    pthread_t id() noexcept;                // pthread_self
    int       kernel_tid() noexcept;        // syscall(SYS_gettid)
}

}  // namespace lnx
```

## Invariants

- **Non-copyable, movable.** Move transfers ownership; the moved-from `thread`
  becomes non-joinable. Move-assigning over a still-joinable target traps.
- **Destroying a joinable thread traps** (`LNX_CHECK(!_joinable)`) — you must
  `join()` or `detach()` first. This diverges from `std::thread`
  (`std::terminate`) on purpose: the trap halts at the misuse site with the
  stack intact, and keeps the primitive layer free of `std::terminate`.
- No name, no cached kernel TID in v1 (the current thread's TID is available via
  `this_thread::kernel_tid()`). Thread naming belongs to the role trampolines
  (`pthread_setname_np`), not this wrapper.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `pthread_create` fails (OOM / thread limit) | `LNX_CHECK` trap (unrecoverable) |
| `join`/`detach` on a non-joinable thread | `LNX_CHECK` trap |
| any `pthread_*` non-zero rc | `LNX_CHECK` trap |
| `sleep_for_ns` interrupted | `EINTR` tolerated (returns; not retried) |

`LNX_CHECK` (always-on) is used throughout — thread ops are cold-path, so the
branch is free, and it closes the "release build silently leaks a thread handle"
hole. (There is no `LNX_DCHECK` in the project — see [[check]].)

## Notes

- POSIX cancellation is not used; cooperative stop flags live in the owning
  subsystem (see below).
- Callable/lambda construction, `pthread_attr_t` (affinity, stack size,
  scheduling), and a cached per-wrapper TID are deferred until a real use case
  needs them.

## Cooperative stop + blocking waits

A supervisor stop is a **cooperative** atomic flag
(`handle_thread::request_stop()` CAS-es `running → draining`); the engine loop
must reach the flag check to observe it. Today's loops spin on
`this_thread::yield()` + non-blocking `io_uring_peek_cqe`, so the flag is seen
every tick. **Once a loop blocks** in `io_uring_wait_cqe`/`submit_and_wait`, an
atomic flag alone cannot wake a thread parked in the kernel — pair
`request_stop()` with a wake: eventfd, a bounded-timeout wait, an io_uring
timeout SQE, or a message-ring wake. A bounded timeout is acceptable for the
first blocking pass if the latency/CPU tradeoff is documented; eventfd wake is
the cleaner end state. Referenced by `doc/10-realtime-server-architecture.md` §9.

## Test plan

`tests/runtime/thread_test.cpp`: default-constructed is not joinable; runs `fn`
and is joinable; detach; move ctor/assign transfer ownership; `native_handle`
(via `pthread_equal`); `this_thread::id`/`kernel_tid`/`yield`/`sleep_for_ns`.

## Done when

- [x] Builds on `default` and `floor` presets
- [x] Tests pass under ASan+UBSan (join/detach state machine clean under TSan)
- [x] This spec matches the built API

## Rationale

- `design/2026-05-25-handle-engine-split.md` — how the handle/engine split drives threads.
- Trap-not-terminate rationale shared with [[check]] and [[mutex]].
