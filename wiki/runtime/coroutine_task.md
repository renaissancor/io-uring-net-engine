# task<T> — coroutine task type and awaiter contract

## Purpose

The single coroutine return type used everywhere in the project. `task<T>`
is a lazy, single-consumer coroutine handle. Calling a coroutine function
returns a `task<T>`; `co_await`ing it runs the coroutine to completion (or
its next suspension point) and yields the result.

Combined with the reactor's awaitable I/O ops (`async_read`, `async_write`,
`async_accept`, `async_connect`) this is what replaces every IOCP /
overlapped-I/O callback chain in the reference design.

## Reference origin

No reference; C++20 coroutines didn't exist when the reference repos were
written. Design informed by:
- `cppcoro::task<T>` (Lewis Baker)
- `folly::coro::Task<T>`
- the C++26 senders/receivers `std::execution::just` family

We copy the `cppcoro` shape — it is the most pedagogical and the most
widely-cited.

## Public API sketch

```cpp
namespace iouring_net::rt {

template <class T = void>
class [[nodiscard]] task {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    task(task&&) noexcept;
    task& operator=(task&&) noexcept;
    ~task();

    // Awaiter
    bool                 await_ready() const noexcept;
    handle_type          await_suspend(std::coroutine_handle<> caller) noexcept;
    T                    await_resume();

private:
    handle_type          handle_;
};

// Void specialization
template <> class task<void> { /* same shape, no return value */ };

// Synchronous wait — only used at the top of a thread (e.g., main, test)
template <class T>
T sync_wait(task<T>&& t);

} // namespace iouring_net::rt
```

A coroutine returning `task<int>`:

```cpp
task<int> read_packet_size(Session& s) {
    auto bytes = co_await s.async_read(2);
    if (!bytes) co_return -1;
    co_return (bytes[0] | (bytes[1] << 8));
}
```

## Linux design

**Lazy start.** `task<T>` is suspended-on-construction. The coroutine
body runs only when something `co_await`s the task or calls `sync_wait`.
This is the cppcoro convention; it gives the caller control over which
thread the coroutine starts on.

**`promise_type`.** Implements:
- `initial_suspend()` → `std::suspend_always` (lazy start)
- `final_suspend()` → custom awaiter that resumes the *continuation*
  (the `co_await`er) when the coroutine finishes
- `get_return_object()` → constructs the `task` from
  `coroutine_handle::from_promise(*this)`
- `return_value(T)` / `return_void()` — stores the result
- `unhandled_exception()` — captures the exception into the promise; rethrown by `await_resume`

**Continuation handoff.** Symmetric transfer: when a `co_await` chain
completes, `final_suspend`'s awaiter returns the continuation handle from
`await_suspend`, which the compiler tail-calls. No stack growth across
arbitrarily deep `co_await` chains.

**Cancellation.** `task<T>` supports cooperative cancellation via a
`std::stop_token` threaded through awaiters. The reactor's I/O awaiters
check the stop token before submitting an SQE; if set, they synthesize an
`std::errc::operation_canceled`. Mid-flight ops are canceled via
`io_uring_prep_cancel(.., user_data)`.

**Allocation.** Coroutine frames are heap-allocated by default. We
override `operator new`/`operator delete` on `promise_type` to route
through the project memory pool, so coroutine frame allocations land in
the appropriate size bucket. (Frame size is not statically known but is
typically 100–300 bytes per task.)

**Exceptions.** A coroutine that throws stores the exception in the
promise. `await_resume` rethrows. This is the C++ orthodoxy. We do *not*
short-circuit to `expected` at the language level — coroutines that
want to propagate errors as values do `co_return expected<...>`.

**`sync_wait`.** Used only at top of a thread (main, tests). Implements
a manual-reset event: starts the task, blocks the calling thread on the
event, the task's final-suspend signals the event. Never called from
inside the reactor.

## Concurrency & ownership

- A `task<T>` is **not** thread-safe. The continuation handoff assumes
  there's exactly one consumer. Multiple `co_await`s on the same `task<T>`
  are undefined behavior — the type is move-only and consumed once.
- The coroutine handle owns the frame; `~task()` destroys it. Moving a
  `task` transfers ownership.
- The thread on which a coroutine runs is determined by the awaiter that
  resumed it. Awaiters in this project are designed to resume on the
  reactor thread that owns the I/O — a coroutine started in the reactor
  stays in the reactor unless it explicitly hops to a worker.

## Test plan

- Unit: `sync_wait(produce_42())` returns 42.
- Unit: `co_await` chain across 5 levels; assert no stack growth (instrument
  with a max-stack-depth probe).
- Unit: exception thrown inside a coroutine surfaces at the outer
  `await_resume`.
- Unit: cancellation — pass a `stop_token`, request stop mid-flight,
  assert `operation_canceled` returned.
- Stress: 1000 simultaneous tasks running through a fake awaiter; verify
  no leaks (run under leak tracker), no use-after-free under ASan.

## Open questions

1. **Pool-allocate coroutine frames** — yes for v1, the operator-new
   override targets the memory pool. Frame sizes vary (compiler-controlled),
   so this lands in mixed buckets; that's fine.
2. **Eager vs. lazy start.** Lazy. cppcoro convention. The cost is one
   extra `coroutine_handle::resume()` per task; in exchange we get
   well-defined "what thread did this coroutine start on" semantics.
3. **`task<T>` vs. `shared_task<T>`.** `task<T>` is single-consumer. If we
   need a multi-consumer task (one producer, many `co_await`ers), define
   `shared_task<T>` separately. Out of scope for v1.
4. **Propagating `expected` through awaiters.** Do reactor I/O
   awaiters return `expected<bytes_view, std::error_code>` directly,
   or throw `std::system_error` on failure? **Decision: return
   `expected`.** Errors are normal control flow.
