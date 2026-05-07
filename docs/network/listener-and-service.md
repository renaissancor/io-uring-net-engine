# Listener and Service

## Purpose

**Listener** — owns the listening socket, accepts incoming connections,
and hands each one to a freshly constructed `Session`. Replaces the
`accept` loop in `SelectServer/FighterOOP/Net.cpp:181`.

**Service** — the top-level container. Owns the reactor, the listener,
and the set of active sessions. Equivalent to a "server" entry point —
the user instantiates one `service`, calls `run()`, and the rest of the
machinery follows.

## Reference origin

- Listener concept: `SelectServer/FighterOOP/Net.cpp:181` (`accept` +
  `FD_SET` on new socket).
- Service: no reference implementation. The reference design has
  `Net::Manager` as a roughly equivalent god-object holding the listening
  socket and the session set; we split that responsibility.

## Public API sketch

```cpp
namespace iouring_net::net {

struct listener_config {
    uint16_t        port;
    int             backlog       = 4096;
    bool            reuse_port    = true;
    sockaddr_in     bind_addr     = {};         // 0.0.0.0 if zeroed
};

class listener {
public:
    listener(reactor& rx, listener_config cfg);
    ~listener();

    // Each accepted connection is constructed via the factory and started.
    iouring_net::rt::task<void> run(
        std::function<session::ptr(int /*fd*/, sockaddr_in /*peer*/)> factory);

    void stop();

private:
    reactor&        reactor_;
    listener_config cfg_;
    int             listen_fd_{-1};
    bool            stopping_{false};
};

class service {
public:
    struct config {
        listener_config listen;
        reactor::config reactor;
    };

    explicit service(config c);
    ~service();

    // User registers a per-session factory + handler. The factory is the
    // hook for application-specific session subclasses.
    void set_session_factory(
        std::function<session::ptr(int, sockaddr_in)> factory);

    void run();                                  // blocks until stop()
    void stop() noexcept;

private:
    reactor    reactor_;
    listener   listener_;
    iouring_net::sync::shared_mutex sessions_mutex_;
    std::unordered_set<session::ptr>           sessions_;

    std::function<session::ptr(int, sockaddr_in)> factory_;
};

} // namespace iouring_net::net
```

## Linux design

**Listener — socket setup.**

```cpp
listener::listener(reactor& rx, listener_config cfg)
    : reactor_(rx), cfg_(std::move(cfg))
{
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (cfg_.reuse_port)
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);

    sockaddr_in addr = cfg_.bind_addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg_.port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0)
        throw std::system_error(errno, std::system_category(), "bind");
    if (::listen(listen_fd_, cfg_.backlog) < 0)
        throw std::system_error(errno, std::system_category(), "listen");
}
```

**Listener — accept loop.**

```cpp
task<void> listener::run(factory_fn factory) {
    while (!stopping_) {
        sockaddr_in peer{};
        socklen_t   peer_len = sizeof peer;
        auto r = co_await reactor_.async_accept(listen_fd_, peer, peer_len);
        if (!r) {
            if (r.error() == std::errc::operation_canceled) break;
            // Transient errors (EMFILE, etc.) — log + back off briefly
            continue;
        }
        int conn_fd = *r;
        auto sess = factory(conn_fd, peer);
        // Start the session's run loop as a fire-and-forget coroutine.
        // The coroutine extends the session's lifetime via shared_from_this.
        sess->run();
    }
}
```

When `IORING_FEAT_ACCEPT_MULTISHOT` is present, the awaiter shape
changes — one SQE produces a stream of CQEs — but the surface here is
identical (the reactor hides multishot behind the awaiter).

**Service — composition.**

```cpp
void service::run() {
    listener_.run([this](int fd, sockaddr_in peer) -> session::ptr {
        auto sess = factory_(fd, peer);
        {
            iouring_net::sync::exclusive_lock lk(sessions_mutex_);
            sessions_.insert(sess);
        }
        sess->on_disconnect = [this](session& s) {
            iouring_net::sync::exclusive_lock lk(sessions_mutex_);
            sessions_.erase(s.shared_from_this());
        };
        return sess;
    });

    reactor_.run();                               // blocks
}
```

**Shutdown.** `service::stop()` calls `listener_.stop()` (which closes
the listening fd, causing in-flight accept SQEs to error out) and
`reactor_.shutdown()`. Active sessions complete their current frames and
exit naturally as their socket reads return 0.

## Concurrency & ownership

- v1: Listener and service share the reactor's thread. The accept
  coroutine runs on the reactor thread; session handoff is in-thread.
- The session set is protected by a `shared_mutex` — read-heavy access
  pattern (broadcast iteration), occasional writes (insert / erase). v1
  doesn't broadcast, so the shared_mutex is mostly write-write; switch
  to a plain mutex if profiling shows it. Kept as `shared_mutex` to
  signal future intent.
- Lifetime: `service` owns reactor, listener, and the session set.
  Sessions are `shared_ptr`-managed; the set holds one ref per active
  connection.

## Test plan

- Unit: instantiate `service` with a noop session factory; client
  connects 10 times; assert 10 entries in the session set; client
  disconnects; assert 0.
- Unit: bind to port 0 (kernel picks); read back the chosen port via
  `getsockname` for tests that need it.
- Integration: 1000 concurrent connect/disconnect cycles; assert no
  fd leak (`ls /proc/self/fd | wc -l` before/after).
- Failure: `bind` to a port already in use → service constructor
  throws `std::system_error`.

## Open questions

1. **Multiple listeners per service.** Future-friendly: one service
   could own multiple listeners (e.g., admin port + game port). v1: one.
2. **`SO_REUSEPORT` + multi-reactor scaling.** With per-thread reactors
   and `SO_REUSEPORT` listeners, the kernel hashes incoming SYNs across
   listeners. The v1 single-reactor design defers this to v2.
3. **Connection limit.** No hard cap today. Add `listener_config::max_connections`
   and enforce in the accept loop (close immediately if over cap, or
   block accept SQE re-arming).
4. **Session factory ergonomics.** A user wanting to subclass `session`
   pays a `std::function` indirection per accept. Acceptable until
   profiled.
