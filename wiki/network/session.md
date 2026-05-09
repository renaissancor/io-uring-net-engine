# Session — one TCP connection driven by a coroutine

## Purpose

`Session` represents one accepted (or connected) TCP connection. It owns
the socket fd, a recv ring buffer, a send ring buffer, and the coroutine
that drives I/O on that connection. The coroutine is started by the
reactor when a connection is accepted (or connected); it runs until the
peer closes, the application closes, or the reactor shuts down.

Replaces the `Session` struct in `SelectServer/FighterOOP/Net.h:39`,
which was a passive struct fed by an external select loop. The new
session is the active driver.

## Reference origin

- `SelectServer/FighterOOP/Net.h:39` — passive `Session { socket,
  recvBuffer, sendBuffer, sockaddr }`.
- `IOCP_Rookiss` — no `Session` implementation exists (declared in
  intent only).

## Public API sketch

```cpp
namespace iouring_net::net {

class session : public std::enable_shared_from_this<session> {
public:
    using ptr = std::shared_ptr<session>;

    session(reactor& rx, int fd, sockaddr_in peer);
    ~session();

    // Driver — typically started by listener::on_accept
    iouring_net::rt::task<void> run();

    // Outbound write — appends to send ring, schedules a send if idle.
    // Awaits backpressure if the send ring is full.
    iouring_net::rt::task<void> send(std::span<const std::byte> bytes);

    // Application-level disconnect; sends FIN and tears down the coroutine.
    void disconnect();

    // Hooks (set by application before run() is awaited)
    std::function<iouring_net::rt::task<void>(session&, frame_view)>
        on_packet;
    std::function<void(session&)>
        on_disconnect;

    // Accessors
    int                fd()        const { return fd_; }
    const sockaddr_in& peer()      const { return peer_; }
    bool               connected() const { return connected_; }

private:
    reactor&                          reactor_;
    int                               fd_;
    sockaddr_in                       peer_;
    bool                              connected_{true};

    iouring_net::buf::recv_ring_buffer recv_buf_;
    iouring_net::buf::send_ring_buffer send_buf_;

    iouring_net::sync::mutex           send_lock_;        // only if cross-thread send
    bool                               send_in_flight_{false};
};

} // namespace iouring_net::net
```

## Linux design

**Run loop.**

```cpp
task<void> session::run() {
    while (connected_) {
        auto writable = recv_buf_.writable_contig();
        auto result = co_await reactor_.async_recv(fd_, writable);
        if (!result || *result == 0) {
            disconnect();
            break;
        }
        recv_buf_.commit_write(*result);

        while (auto frame = recv_buf_.peek_packet()) {
            co_await on_packet(*this, *frame);
            recv_buf_.commit_read(frame->total_size);
        }
    }
    if (on_disconnect) on_disconnect(*this);
    co_await reactor_.async_close(fd_);
}
```

The packet handler is itself a coroutine — `on_packet` returns
`task<void>`, so the session can `co_await` per-packet processing
without blocking the recv pipeline. The handler can hop to a job queue
or another reactor and resume on the original session thread.

**Send path.**

```cpp
task<void> session::send(std::span<const std::byte> bytes) {
    while (!send_buf_.try_append(bytes)) {
        co_await send_buf_.async_wait_writable();      // backpressure
    }
    if (!send_in_flight_) {
        send_in_flight_ = true;
        kick_send();                                    // chains the send
    }
}

void session::kick_send() {
    auto out = send_buf_.readable_contig();
    if (out.empty()) {
        send_in_flight_ = false;
        return;
    }
    // Fire-and-forget coroutine bound to this reactor:
    [&]() -> task<void> {
        auto r = co_await reactor_.async_send(fd_, out);
        if (r) {
            send_buf_.commit_read(*r);
            kick_send();                                // tail-call chain
        } else {
            disconnect();
        }
    }();                                                // started lazily
}
```

**Wire framing.** `recv_buf_.peek_packet` parses
`[uint16 size | uint16 id]` and returns a `frame_view{size, id,
payload_span}`. See `wiki/network/packet_framing.md`.

**Close semantics.** `disconnect()`:
1. Marks `connected_ = false`.
2. Cancels in-flight recv via `io_uring_prep_cancel`.
3. Lets the run-loop fall through to `async_close`.
The session lives as long as a `shared_ptr` is held. The reactor and the
listener typically hold one reference each; the run() coroutine extends
the lifetime via `shared_from_this()` inside the coroutine frame.

**Backpressure.** Send ring full → producer coroutine suspends on
`async_wait_writable`. The send-completion handler calls `notify` on the
condition. v1 implements the simple version with a per-session
`std::condition_variable_any` adapter for coroutines. (More efficient
implementations replace this with a signalfd-based event; defer.)

## Concurrency & ownership

- v1: session is thread-affined to its reactor. All `send` calls happen
  on the reactor thread. No `send_lock_` needed in v1; the field is
  declared so v2 cross-thread sends from worker threads have a slot.
- Lifetime: `shared_ptr<session>`. Owners: listener (during accept→
  application handoff), reactor (during in-flight ops via the awaiter
  control block — but those keep the coroutine handle alive, which keeps
  the session alive transitively).
- Coroutine pinning: `run()` uses `auto self = shared_from_this()` in the
  first line so the session lives at least until run() returns.

## Test plan

- Unit: connect a fake peer, send 100 packets of various sizes, assert
  all received in order.
- Unit: peer closes mid-recv — session emits `on_disconnect` exactly
  once, fd is closed.
- Unit: backpressure — fill send ring to capacity, assert producer
  suspends, drain on the receiver side, assert producer resumes.
- Stress: 1000 simultaneous sessions on one reactor, each echoing
  packets at 1 kHz; assert correctness for 60 seconds.
- TSan: same workload under thread sanitizer.

## Open questions

1. **`enable_shared_from_this`** vs. intrusive ref-count. The reference
   repo uses raw pointers and manual ref-counting in places. We use
   `shared_ptr` for clarity; revisit only if profiling demands it.
2. **Send batching.** Multiple `send` calls within one frame can be
   coalesced into one `async_send`. v1 does this implicitly via the
   send ring (one in-flight send per session). v2 may add scatter-gather
   via `writev`-shaped multi-iovec sends.
3. **Read coalescing under multishot recv.** When we move to multishot
   recv (kernel 6.0+), the run-loop changes shape — multiple CQEs
   per accepted connection without re-arming. The packet-parse loop
   stays identical.
4. **Half-close.** TCP supports independent half-closes; we currently
   collapse them. If application semantics ever need a clean `shutdown(WR)`
   while still reading, expose `session::shutdown_write()`.
