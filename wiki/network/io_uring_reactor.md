# io_uring reactor — submit/complete loop

## Purpose

The heart of the runtime. Owns one `io_uring` per thread, submits SQEs on
behalf of awaitables, and resumes the owning coroutines when CQEs arrive.
Replaces what `IocpCore` (declared in `IOCP_Rookiss/Engine/pch.h:12-14` but
never implemented) would have been on Windows, and replaces the `select()`
loop in `SelectServer/FighterOOP/Net.cpp:287-306`.

## Reference origin

- No reference implementation. `IOCP_Rookiss` declares `<winsock2.h>` and
  `<mswsock.h>` but contains no IOCP loop, no `Session`, no `IocpEvent`.
- Design is informed by `liburing` documentation, `cppcoro::io_service`,
  and `axboe/liburing` examples.

## Public API sketch

```cpp
namespace iouring_net::net {

class reactor {
public:
    struct config {
        uint32_t entries          = 4096;          // SQ + CQ ring size
        uint32_t flags            = 0;             // IORING_SETUP_*
        bool     register_buffers = true;          // pre-register recv buffers
        bool     enable_sqpoll    = false;         // kernel polls SQ; opt-in
    };

    explicit reactor(config c);
    ~reactor();

    // Drive the loop until shutdown is requested
    void run();
    void shutdown() noexcept;

    // Awaitable I/O ops — return objects whose `co_await` produces
    //   std::expected<T, std::error_code>
    auto async_accept(int listen_fd);
    auto async_connect(int fd, const sockaddr* addr, socklen_t len);
    auto async_recv(int fd, std::span<std::byte> buf, int flags = 0);
    auto async_send(int fd, std::span<const std::byte> buf, int flags = 0);
    auto async_close(int fd);

    // Cross-thread wakeup (used by job_queue v2)
    void post(std::function<void()> f);

private:
    struct io_uring                  ring_;
    bool                             stop_requested_{false};

    // Slab of `io_op` objects holding the awaiter state. Indexed by
    // sqe->user_data low bits.
    iouring_net::mem::slab<io_op>    op_slab_;
};

} // namespace iouring_net::net
```

## Linux design

**Setup.** `io_uring_queue_init(entries, &ring_, flags)`. Probe
capabilities at startup via `io_uring_get_probe`; cache support flags
(multishot accept, multishot recv, fixed buffers, `IOSQE_BUFFER_SELECT`).

**`user_data` convention.** Every SQE encodes a tagged pointer:

```
bits 0..2  — op kind (accept, connect, recv, send, close, msg_ring)
bits 3..63 — pointer to io_op control block (8-byte aligned, low 3 bits free)
```

The control block is allocated from a slab so the pointer is stable
across resubmissions.

**Awaiter shape.** Each `async_*` returns a stack-local awaitable:

```cpp
struct recv_awaiter {
    reactor*           rx;
    int                fd;
    std::span<std::byte> buf;
    io_op*             op;          // assigned in await_suspend

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        op = rx->op_slab_.alloc();
        op->coro = h;
        struct io_uring_sqe* sqe = io_uring_get_sqe(&rx->ring_);
        io_uring_prep_recv(sqe, fd, buf.data(), buf.size(), 0);
        io_uring_sqe_set_data(sqe, encode(op_kind::recv, op));
    }

    std::expected<size_t, std::error_code> await_resume() {
        return op->result;          // populated by reactor on CQE
    }
};
```

**Run loop.**

```cpp
void run() {
    while (!stop_requested_) {
        io_uring_submit_and_wait(&ring_, 1);
        struct io_uring_cqe* cqe;
        unsigned head;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            auto [kind, op] = decode(cqe->user_data);
            op->result = (cqe->res < 0)
                ? std::unexpected(make_error(-cqe->res))
                : std::expected<size_t, std::error_code>(cqe->res);
            op->coro.resume();           // back into the coroutine
            op_slab_.free(op);
        }
        io_uring_cq_advance(&ring_, head + 1);
    }
}
```

**Multishot accept.** When `IORING_FEAT_ACCEPT_MULTISHOT` is available
(kernel 5.19+), one accept SQE produces multiple CQEs (one per accepted
connection). The op_block stays alive until the listener tears down.

**Multishot recv with provided buffers.** Kernel 6.0+ feature. Requires
`io_uring_register_buf_ring(ring_, ...)` once and submitting recv with
`IOSQE_BUFFER_SELECT`. Each CQE carries a buffer ID in `cqe->flags`
(`IORING_CQE_F_BUFFER` bit set). Strictly faster for high-fan-out
scenarios; postponed for v1 in favor of one-recv-per-session for
simplicity.

**Cross-thread wakeup.** `io_uring_prep_msg_ring(other_ring, payload,
len, fd)` (kernel 5.18+). v1 has one reactor; this is wired but unused
until v2.

**Shutdown.** `shutdown()` sets `stop_requested_` and submits a no-op
SQE to wake the loop. The loop exits after the next CQE batch. Ops in
flight are canceled via `io_uring_prep_cancel_all` (kernel 6.0+) or
manually canceled per-op.

## Concurrency & ownership

- One reactor per thread. Reactors do not share rings.
- All public methods are *thread-affined* to the reactor thread except
  `post()` and `shutdown()`, which are explicitly cross-thread.
- The op slab is reactor-thread-only (no synchronization).
- `post()` enqueues to a `lock_free_queue` and submits an
  `IORING_OP_MSG_RING` SQE on the local ring (or a `nop` if same ring),
  triggering the loop to drain the post queue.

## Test plan

- Unit: synthetic listen-on-loopback + `async_accept` produces a CQE
  for every incoming connection.
- Unit: `async_recv` on a closed peer returns 0 bytes (EOF).
- Unit: `async_recv` on a peer that errors returns
  `std::errc::connection_reset` (or equivalent).
- Stress: 100 concurrent connections each doing 1000-byte echo round
  trips; assert correct payloads received and zero kernel SQE drops.
- TSan: cross-thread `post()` and `shutdown()` are clean.
- Capability probe: assert that the runtime refuses to start if
  required `IORING_FEAT_*` flags are missing on the host kernel.

## Open questions

1. **SQPOLL.** Kernel-side polling avoids syscalls per submission. Big
   win at high QPS; high CPU baseline. Make it a config knob (off by
   default for v1).
2. **Fixed FDs (`IORING_REGISTER_FILES`).** Lets the kernel skip the
   per-op fd-table lookup. Combined with multishot accept, the listener
   would register one fd. Worth it. v1: implement; v2: extend to
   per-session.
3. **One reactor or multiple.** v1 ships single. Adding more is mostly
   listener-distribution policy (round-robin? hash by source IP?
   `SO_REUSEPORT` + per-thread listener?). Document each option in v2.
4. **Awaitable stack vs. heap.** `recv_awaiter` is stack-local in the
   coroutine frame. The `io_op` control block is slab-allocated. This
   matches `cppcoro::io_service` and avoids per-op allocation.
5. **Error category.** Define an `iouring_net::error_category` so
   `std::error_code` from a CQE round-trips properly through code that
   doesn't know about `errno`.
