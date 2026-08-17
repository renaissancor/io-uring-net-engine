// loadgen.cpp — connection-scale load generator for the epoll chat server.
//
// PHASE 1 ONLY. This establishes N connections, joins them to rooms, drains
// whatever the server sends, and holds. It does NOT yet send chat traffic or
// measure latency; that is phase 2 and it needs the coordinated-omission
// handling described at the bottom of this file.
//
// Unlike server.cpp, this file is NOT throwaway. It has to outlive
// epoll-chat-study and measure the io_uring server too, which is why the
// framing constants are parameterised rather than hardcoded (this project
// uses a 4-byte header, iouring-net-server uses 8).
//
// Single-threaded on purpose. Scale out with processes and machines, not
// threads: same core scaling, no shared state, and it forces the multi-box
// design from day one. Note that extra processes on one box do NOT buy extra
// ephemeral ports — those are per source IP, kernel-wide. Hence --src-ips.
//
//   make loadgen
//   ./loadgen --conns 10000 --per-room 10
//   ./loadgen --conns 30000 --src-ips 4      # needs 127.0.0.2..4 to be up

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

// ------------------------------------------------------------------- wire
//
// Mirrors server.cpp: 4-byte little-endian header, no byte swapping, payload
// length excludes the header. Kept as constants so the 8-byte header of the
// real project is a two-line change rather than a rewrite.

static constexpr size_t k_len_bytes  = 2;
static constexpr size_t k_type_bytes = 2;
static constexpr size_t k_header_size = k_len_bytes + k_type_bytes;

enum : uint16_t {
    c_set_nick = 1,
    c_join     = 2,
    c_chat     = 3,

    s_notice   = 100,
    s_chat     = 101,
};

static void put_frame(std::string& out, uint16_t type, const std::string& payload)
{
    const auto len = static_cast<uint16_t>(payload.size());
    char hdr[k_header_size];
    std::memcpy(hdr, &len, sizeof(len));
    std::memcpy(hdr + k_len_bytes, &type, sizeof(type));
    out.append(hdr, sizeof(hdr));
    out.append(payload);
}

// ------------------------------------------------------------------ config

struct config {
    std::string host        = "127.0.0.1";
    uint16_t    port        = 9000;
    int         conns       = 10000;
    int         per_room    = 10;
    int         src_ips     = 1;      // binds 127.0.0.1 .. 127.0.0.<src_ips>
    int         inflight    = 256;    // concurrent connects in flight
    int         rcvbuf      = 8192;   // 0 = leave kernel default
    int         sndbuf      = 8192;   // 0 = leave kernel default
    int         hold_secs   = 0;      // 0 = hold until SIGINT
};

// ------------------------------------------------------------------- state

enum class conn_state : uint8_t { none, connecting, ready, dead };

struct conn {
    conn_state  state = conn_state::none;
    int         index = -1;    // logical client id, for nick/room
    std::string pending;       // unsent tail of the join handshake
};

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static int64_t now_ns()
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// ------------------------------------------------------------------ limits
//
// The first wall anyone hits. RLIMIT_NOFILE defaults to 1024, so without this
// the run dies at the 1024th connection with EMFILE and it looks like a
// network problem. Raising the soft limit up to the hard limit needs no
// privilege; the hard limit is typically 1048576 on modern systems.

static bool raise_fd_limit(rlim_t want, rlim_t& got)
{
    rlimit rl{};
    if (::getrlimit(RLIMIT_NOFILE, &rl) < 0) {
        std::perror("getrlimit");
        return false;
    }
    const rlim_t target = (want > rl.rlim_max) ? rl.rlim_max : want;
    if (rl.rlim_cur < target) {
        rlimit next = rl;
        next.rlim_cur = target;
        if (::setrlimit(RLIMIT_NOFILE, &next) < 0) {
            std::perror("setrlimit");
            got = rl.rlim_cur;
            return false;
        }
    }
    got = target;
    return true;
}

// -------------------------------------------------------------- connecting

// Source IPs are round-robined across connections. Ephemeral ports are a
// per-source-IP resource (~28k by default, net.ipv4.ip_local_port_range), so
// one source IP caps a single-destination run at roughly that many sockets
// no matter how many processes you run. Linux treats all of 127.0.0.0/8 as
// local, so 127.0.0.2+ are free extra budget on loopback.
static bool bind_source(int fd, const config& cfg, int index)
{
    if (cfg.src_ips <= 1)
        return true;

    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_port   = 0;  // let the kernel pick the ephemeral port
    const std::string ip = "127.0.0." + std::to_string(1 + (index % cfg.src_ips));
    if (::inet_pton(AF_INET, ip.c_str(), &src.sin_addr) != 1)
        return false;

    return ::bind(fd, reinterpret_cast<sockaddr*>(&src), sizeof(src)) == 0;
}

// Returns the fd on success (connect in progress or complete), -1 on failure.
static int start_connect(const config& cfg, const sockaddr_in& dst, int index, int& out_errno)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { out_errno = errno; return -1; }

    // SO_RCVBUF must be set BEFORE connect(): the receive window is advertised
    // during the handshake, so setting it afterwards is cosmetic. server.cpp
    // lesson 7 is the same trap seen from the other side.
    if (cfg.rcvbuf > 0)
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cfg.rcvbuf, sizeof(cfg.rcvbuf));
    if (cfg.sndbuf > 0)
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &cfg.sndbuf, sizeof(cfg.sndbuf));

    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // SO_LINGER{1,0} makes close() send RST instead of FIN. The active closer
    // is the side that eats TIME_WAIT, and that is us: 10k sockets sitting in
    // TIME_WAIT for 60s exhausts the ephemeral range and the *next* run fails
    // for no visible reason. A load harness wants the RST; a real client
    // never should, because it discards anything still in the send buffer.
    linger lg{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));

    if (!bind_source(fd, cfg, index)) {
        out_errno = errno;
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) < 0
        && errno != EINPROGRESS) {
        out_errno = errno;
        ::close(fd);
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------- draining

// Phase 1 does not parse replies, but it must still read them. The server
// caps its per-connection pending buffer at 256 KB and drops whoever exceeds
// it, so a client that never reads gets disconnected by backpressure and the
// run looks like a server failure. Read and discard.
static char g_scratch[65536];

static bool drain(int fd, uint64_t& bytes_in)
{
    for (;;) {
        const ssize_t n = ::recv(fd, g_scratch, sizeof(g_scratch), 0);
        if (n > 0) { bytes_in += static_cast<uint64_t>(n); continue; }
        if (n == 0) return false;                       // peer closed
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

// Sends as much of c.pending as the kernel will take. Returns false on a hard
// error. A partial send is normal and not an error: the tail stays in
// c.pending and EPOLLOUT stays armed.
static bool flush_pending(int fd, conn& c)
{
    while (!c.pending.empty()) {
        const ssize_t n = ::send(fd, c.pending.data(), c.pending.size(), MSG_NOSIGNAL);
        if (n > 0) { c.pending.erase(0, static_cast<size_t>(n)); continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

// -------------------------------------------------------------------- args

static void usage()
{
    std::printf(
        "usage: loadgen [options]\n"
        "  --host <ip>        server address        (default 127.0.0.1)\n"
        "  --port <n>         server port           (default 9000)\n"
        "  --conns <n>        connections to open   (default 10000)\n"
        "  --per-room <n>     clients per room      (default 10)\n"
        "  --src-ips <n>      bind across 127.0.0.1..n (default 1)\n"
        "  --inflight <n>     concurrent connects   (default 256)\n"
        "  --rcvbuf <bytes>   SO_RCVBUF, 0=default  (default 8192)\n"
        "  --sndbuf <bytes>   SO_SNDBUF, 0=default  (default 8192)\n"
        "  --hold <secs>      hold then exit, 0=until SIGINT (default 0)\n");
}

static bool parse_args(int argc, char** argv, config& cfg)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](int& dst) {
            if (i + 1 >= argc) return false;
            dst = std::atoi(argv[++i]);
            return true;
        };
        if      (a == "--host" && i + 1 < argc) cfg.host = argv[++i];
        else if (a == "--port" && i + 1 < argc) cfg.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--conns")    { if (!next(cfg.conns))    return false; }
        else if (a == "--per-room") { if (!next(cfg.per_room)) return false; }
        else if (a == "--src-ips")  { if (!next(cfg.src_ips))  return false; }
        else if (a == "--inflight") { if (!next(cfg.inflight)) return false; }
        else if (a == "--rcvbuf")   { if (!next(cfg.rcvbuf))   return false; }
        else if (a == "--sndbuf")   { if (!next(cfg.sndbuf))   return false; }
        else if (a == "--hold")     { if (!next(cfg.hold_secs))return false; }
        else { usage(); return false; }
    }
    if (cfg.conns <= 0 || cfg.per_room <= 0 || cfg.src_ips <= 0 || cfg.inflight <= 0) {
        std::fprintf(stderr, "counts must be positive\n");
        return false;
    }
    return true;
}

// -------------------------------------------------------------------- main

int main(int argc, char** argv)
{
    config cfg;
    if (!parse_args(argc, argv, cfg))
        return 2;

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);   // belt and braces; sends use MSG_NOSIGNAL

    // Headroom over cfg.conns for the epoll fd, stdio, and slack.
    const rlim_t want = static_cast<rlim_t>(cfg.conns) + 256;
    rlim_t limit = 0;
    raise_fd_limit(want < 65536 ? 65536 : want, limit);
    std::printf("[cfg ] RLIMIT_NOFILE soft = %llu (need ~%llu)\n",
                static_cast<unsigned long long>(limit),
                static_cast<unsigned long long>(want));
    if (limit < want)
        std::printf("[warn] fd limit below target; expect EMFILE near %llu conns\n",
                    static_cast<unsigned long long>(limit));

    // One source IP covers ~28k ephemeral ports to a single destination.
    if (cfg.src_ips == 1 && cfg.conns > 25000)
        std::printf("[warn] %d conns from one source IP is near the ephemeral "
                    "port range; use --src-ips\n", cfg.conns);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(cfg.port);
    if (::inet_pton(AF_INET, cfg.host.c_str(), &dst.sin_addr) != 1) {
        std::fprintf(stderr, "bad --host %s\n", cfg.host.c_str());
        return 2;
    }

    const int ep = ::epoll_create1(0);
    if (ep < 0) { std::perror("epoll_create1"); return 1; }

    // Indexed by fd. Cheaper and more predictable than a hash map, and fd
    // numbers are bounded by the rlimit we just set.
    std::vector<conn> conns(limit + 16);

    int  started = 0, established = 0, failed = 0, inflight = 0;
    std::map<int, int> fail_reasons;   // errno -> count

    const int64_t t0 = now_ns();

    // ---- phase 1a: establish -------------------------------------------
    //
    // Connects are paced. Firing 10k SYNs at once overruns the listen backlog
    // (SOMAXCONN), and the kernel's response to an overflowing accept queue is
    // to drop SYNs silently — which surfaces as mysterious timeouts rather
    // than an error, so it is worth not provoking.

    while (!g_stop && (established + failed) < cfg.conns) {
        while (inflight < cfg.inflight && started < cfg.conns) {
            int err = 0;
            const int fd = start_connect(cfg, dst, started, err);
            if (fd < 0) {
                ++failed;
                ++fail_reasons[err];
                ++started;
                continue;
            }
            if (fd >= static_cast<int>(conns.size()))
                conns.resize(static_cast<size_t>(fd) + 1024);

            conns[fd].state = conn_state::connecting;
            conns[fd].index = started;

            epoll_event ev{};
            ev.events  = EPOLLOUT;   // writable == connect resolved
            ev.data.fd = fd;
            if (::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
                ++failed;
                ++fail_reasons[errno];
                conns[fd] = conn{};
                ::close(fd);
                ++started;
                continue;
            }
            ++inflight;
            ++started;
        }

        epoll_event evs[1024];
        const int n = ::epoll_wait(ep, evs, 1024, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait");
            break;
        }
        if (n == 0 && inflight == 0 && started >= cfg.conns)
            break;

        for (int i = 0; i < n; ++i) {
            const int fd = evs[i].data.fd;
            conn& c = conns[fd];

            if (c.state == conn_state::connecting) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
                if (soerr != 0 || (evs[i].events & (EPOLLERR | EPOLLHUP))) {
                    ++failed;
                    ++fail_reasons[soerr ? soerr : ECONNRESET];
                    --inflight;
                    ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                    c = conn{};
                    ::close(fd);
                    continue;
                }

                // Room sharding is not cosmetic. Join broadcasts a notice to
                // the room, so N clients in one room is O(N^2) frames — 10k in
                // a single room is 50M notices and the server never finishes
                // the connect phase. Rooms must be small here; fan-out is a
                // separate experiment with its own knob.
                put_frame(c.pending, c_set_nick, "c" + std::to_string(c.index));
                put_frame(c.pending, c_join,     "r" + std::to_string(c.index / cfg.per_room));

                c.state = conn_state::ready;
                --inflight;
                ++established;

                if (!flush_pending(fd, c)) {
                    ++failed; --established;
                    ++fail_reasons[errno];
                    ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                    c = conn{};
                    ::close(fd);
                    continue;
                }

                // EPOLLOUT must not stay armed once there is nothing to write.
                // A socket is writable almost always, so a permanent EPOLLOUT
                // spins epoll_wait at 100% CPU — the same trap as server.cpp
                // lesson 3, and on the client it silently becomes the
                // bottleneck that makes the server look slow.
                epoll_event ev{};
                ev.events  = EPOLLIN | (c.pending.empty() ? 0u : static_cast<uint32_t>(EPOLLOUT));
                ev.data.fd = fd;
                ::epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);

                if ((established % 1000) == 0)
                    std::printf("[conn] %d established (%d failed)\n", established, failed);
                continue;
            }

            if (c.state == conn_state::ready) {
                uint64_t junk = 0;
                bool alive = true;
                if (evs[i].events & EPOLLOUT) {
                    alive = flush_pending(fd, c);
                    if (alive && c.pending.empty()) {
                        epoll_event ev{};
                        ev.events  = EPOLLIN;
                        ev.data.fd = fd;
                        ::epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
                    }
                }
                if (alive && (evs[i].events & EPOLLIN))
                    alive = drain(fd, junk);
                if (!alive || (evs[i].events & (EPOLLERR | EPOLLHUP))) {
                    --established;
                    ++failed;
                    ++fail_reasons[errno ? errno : ECONNRESET];
                    ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                    c = conn{};
                    ::close(fd);
                }
            }
        }
    }

    const double secs = static_cast<double>(now_ns() - t0) / 1e9;
    std::printf("\n[done] established=%d failed=%d in %.2fs (%.0f conn/s)\n",
                established, failed, secs, secs > 0 ? established / secs : 0.0);
    for (const auto& [err, count] : fail_reasons)
        std::printf("       %6d x %s\n", count, std::strerror(err));

    // ---- phase 1b: hold -------------------------------------------------
    //
    // Holding matters: a connection that establishes and is dropped a second
    // later did not really scale. Keep draining so backpressure never fires,
    // and report attrition.

    if (established > 0 && !g_stop) {
        std::printf("[hold] draining; ^C to stop%s\n",
                    cfg.hold_secs > 0 ? "" : " (no timeout)");
        const int64_t deadline = cfg.hold_secs > 0
            ? now_ns() + static_cast<int64_t>(cfg.hold_secs) * 1000000000LL
            : 0;
        uint64_t bytes_in = 0;
        int64_t  next_report = now_ns() + 5000000000LL;

        while (!g_stop && established > 0) {
            if (deadline && now_ns() >= deadline) break;

            epoll_event evs[1024];
            const int n = ::epoll_wait(ep, evs, 1024, 500);
            if (n < 0) {
                if (errno == EINTR) continue;
                std::perror("epoll_wait");
                break;
            }
            for (int i = 0; i < n; ++i) {
                const int fd = evs[i].data.fd;
                conn& c = conns[fd];
                if (c.state != conn_state::ready)
                    continue;
                bool alive = true;
                if (evs[i].events & EPOLLIN)
                    alive = drain(fd, bytes_in);
                if (!alive || (evs[i].events & (EPOLLERR | EPOLLHUP))) {
                    --established;
                    ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                    c = conn{};
                    ::close(fd);
                }
            }
            if (now_ns() >= next_report) {
                std::printf("[hold] alive=%d bytes_in=%llu\n",
                            established, static_cast<unsigned long long>(bytes_in));
                next_report = now_ns() + 5000000000LL;
            }
        }
        std::printf("[exit] alive=%d bytes_in=%llu\n",
                    established, static_cast<unsigned long long>(bytes_in));
    }

    for (size_t fd = 0; fd < conns.size(); ++fd)
        if (conns[fd].state != conn_state::none)
            ::close(static_cast<int>(fd));
    ::close(ep);
    return 0;
}

// ---------------------------------------------------------------- phase 2
//
// Not implemented yet. Recorded here so the design decisions survive the gap.
//
// Send scheduling must be OPEN-LOOP: each connection gets a fixed next-send
// deadline (start + n * interval) and fires on schedule regardless of whether
// the previous reply arrived. Closed-loop send-after-reply cannot overload the
// server — when the server slows, the client slows with it, queueing never
// builds, and the latency graph is a flattering lie.
//
// Latency must be measured against the INTENDED send time, not the actual
// one:  latency = recv_time - next_send  (not - actual_send). Otherwise every
// delay the client itself caused is silently deleted from the histogram, which
// is exactly where the p99 lives. This is coordinated omission.
//
// The payload carries its own timestamp so the receiver can compute delivery
// latency without clock sync:
//
//     [4B len][2B type][8B send_ts_ns][4B seq][4B client_id][... filler ...]
//
// Because the chat server broadcasts, what this measures is delivery latency
// to other room members rather than sender RTT — the more meaningful number
// for a chat workload. Keeping every simulated client in one process means
// they share a clock, so the subtraction is valid.
//
// Filler comes from fixed strings per size class (16 / 32 / 256 / long), not
// per-message RNG: generating randomness in the hot loop burns client CPU and
// that cost lands in the measurement. Content does not matter — TCP does not
// compress — but length class does, since small frames are syscall-bound and
// frames over the MSS take the segmentation path.
//
// SELF-DIAGNOSIS IS MANDATORY. The loop must also histogram its own
// scheduling lag (actual wakeup - intended wakeup) and print it beside the
// latency numbers. Without it there is no way to tell server queueing from
// client saturation, and the classic failure is reporting "p99 200ms at 100k
// conns" when the client was the thing dying. If lag p99 is a meaningful
// fraction of latency p99, the run is void.
