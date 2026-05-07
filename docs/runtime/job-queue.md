# JobQueue — per-entity serialized work queue

## Purpose

Serialize all mutations to one logical entity (Session, Room, Player,
arbitrary user object) without holding a mutex around the entity for the
duration of each operation. Each entity owns one `job_queue`. Pushing a
job appends a callable; the queue runs jobs FIFO on whatever thread
happens to dequeue, but at most one thread runs jobs from a given queue at
a time.

This is the model the Rookiss lecture calls "JobQueue + GGlobalQueue."
Neither reference repo actually implements it — that's noted in
`docs/00-overview.md`. The design here is fresh.

## Reference origin

- Conceptually from the Rookiss lecture material; no reference
  implementation in `IOCP_Rookiss`, `WindowsLibrary`, or `SelectServer`.
- The closest analog in any reference repo is the `select()` per-frame
  packet dispatch loop in `SelectServer/FighterOOP/Net.cpp:5-72` — an
  in-thread serializer of packet handlers, not multi-thread.

## Public API sketch

```cpp
namespace iouring_net::rt {

class job_queue : public std::enable_shared_from_this<job_queue> {
public:
    using job_fn = std::function<void()>;       // small functor; coroutines wrap

    void push(job_fn job);                       // append job; possibly dispatch

    template <class Awaitable>
    void co_push(Awaitable&& awaitable);         // accepts coroutine task<>

private:
    void execute();                              // drains queue, called by exactly one thread

    iouring_net::sync::lock_free_queue<job_node> queue_;
    std::atomic<int32_t>                         in_flight_{0};
};

class global_dispatch {
public:
    static global_dispatch& instance();

    void enqueue(std::shared_ptr<job_queue> q);
    bool try_drain_one();                         // worker thread loop body
};

} // namespace iouring_net::rt
```

User pattern:

```cpp
auto session_jq = std::make_shared<job_queue>();

session_jq->push([s = session]{ s->update_position(...); });
session_jq->push([s = session]{ s->broadcast_to_room(...); });
// Both run on whatever worker drains the queue, but in order.
```

## Linux design

**Drainer election.** First push grows `in_flight_` from 0 → 1; the
pushing thread is now the drainer. It calls `execute()`, which pops jobs
and runs them. After running each job, decrement `in_flight_`; if there
are still queued jobs, continue. If the count drops to zero, exit; the
*next* pusher elects a new drainer.

**Cross-thread push.** A non-drainer thread that increments `in_flight_`
from N>0 to N+1 simply pushes; the existing drainer picks up the work.
This is the "GGlobalQueue" knob — for now we don't actually need a global
queue because the drainer is whoever pushed first.

**v2: dedicated worker pool.** Replace "drainer is the pushing thread"
with "drainer is whichever worker pops the queue from `global_dispatch`."
Push then becomes:
1. Append job to queue.
2. If `in_flight_` was 0, hand the queue to `global_dispatch`.
A worker pool drains queues round-robin. v1 single-threaded ships
without this; the design slot is reserved.

**Coroutine integration.** `co_push(awaitable)` wraps an awaitable so it
runs on the queue's drainer thread and resumes the original coroutine
when finished. Useful for "modify session state from a request handler
that doesn't own the session." Same shape as Boost.Asio's `post()` to a
strand.

**Reentrance.** A job that calls `push` on its own queue does NOT
recursively run the new job (avoids stack growth). The new job is
appended; the drainer picks it up after the current job returns. This
matches the "GameJob queues itself" idiom common in MMO code.

## Concurrency & ownership

- Push is MPSC: any thread, single drainer at a time.
- The internal queue is a `lock_free_queue` — Michael & Scott MS queue
  or Vyukov MPMC queue (open question below).
- `in_flight_` is `std::atomic<int32_t>`. Push increments
  (`fetch_add(1, memory_order_acq_rel)`); drainer decrements after each
  job. Drainer election uses the previous value: if `fetch_add` returned
  0, you become the drainer.
- Lifetime: `job_queue` is `shared_ptr`-owned. The drainer holds a copy
  of `shared_from_this()` for the duration of `execute()`; the queue
  cannot self-destruct mid-drain.

## Test plan

- Unit: push 100 jobs, verify all run in order on a single drainer.
- Unit: push 100 jobs from 8 threads concurrently; verify all 800 run
  exactly once and any single-queue ordering is preserved (per-thread
  push ordering visible at the queue's output).
- Stress: 8 push threads × 100k jobs/queue × 16 queues; assert no
  drops, no double-runs. Run under TSan.
- Integration: a session whose state mutations all go through its
  `job_queue`, exercised by a recv loop on one thread and a timer on
  another.

## Open questions

1. **MS queue vs. Vyukov MPMC.** MS queue allocates per node; Vyukov uses
   a fixed-size ring. For unbounded job queues, MS queue is the clean
   fit. Decision: MS queue, allocated through the project memory pool.
2. **Coroutine vs. functor.** `std::function<void()>` is the simple shape.
   For coroutine tasks, wrap with a `task<>` adapter. Keep both APIs; the
   coroutine path is `co_push(awaitable)`.
3. **Backpressure.** Unbounded today. If a queue grows pathologically,
   that's a bug in the producer. v2 may add a soft-cap that blocks (or
   suspends a coroutine push) above a threshold.
4. **Drainer-throughput unfairness.** A queue with constant pushes pins
   one thread as drainer indefinitely. v2 mitigation: drainer
   periodically yields back to `global_dispatch`. v1: not an issue
   because we run single-threaded.
5. **Should `job_queue` be templated on the entity type?** No — keeping
   it untyped lets one queue serialize jobs across heterogeneous targets
   (e.g., a Room queue running player-update and broadcast jobs). Capture
   the entity in the lambda.
