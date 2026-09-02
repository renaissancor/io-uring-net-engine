#include "netutil.h"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

// Added in Linux 4.2 for exactly this use case; guard so the file still builds
// on older headers.
#ifndef IP_BIND_ADDRESS_NO_PORT
#define IP_BIND_ADDRESS_NO_PORT 24
#endif

// ------------------------------------------------------------------ limits
//
// The first wall anyone hits. RLIMIT_NOFILE defaults to 1024, so without this
// the run dies at the 1024th connection with EMFILE and it looks like a
// network problem. Raising the soft limit up to the hard limit needs no
// privilege; the hard limit is typically 1048576 on modern systems.

bool raise_fd_limit(rlim_t want, rlim_t& got)
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

// Source IPs are round-robined across connections. A connection is identified
// by the 4-tuple (src IP, src port, dst IP, dst port); against a single
// server IP:port the last two are fixed, so one source IP yields only as many
// tuples as it has ephemeral ports (~28k by default,
// net.ipv4.ip_local_port_range). Adding source IPs opens another field in the
// tuple. Linux treats all of 127.0.0.0/8 as local, so 127.0.0.2+ are free
// extra budget on loopback.
static bool bind_source(int fd, const config& cfg, int index)
{
    if (cfg.src_ips <= 1)
        return true;   // no bind at all: connect() autobinds, 4-tuple aware

    // Without this, bind() has to pick the port immediately — and at bind time
    // the kernel does not know the destination yet, so it can only guarantee
    // uniqueness on (src IP, src port) rather than on the full 4-tuple. That
    // caps a bound socket at the ephemeral range per source IP even when the
    // destinations differ. IP_BIND_ADDRESS_NO_PORT says "fix the address, let
    // connect() choose the port", restoring 4-tuple-aware allocation. It
    // exists for load generators and proxies specifically.
    const int one = 1;
    ::setsockopt(fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof(one));

    // The base is explicit rather than derived from --node on purpose.
    // Ephemeral ports are a per-source-IP kernel resource, so two processes on
    // the SAME box must not share a 127.0.0.x or they divide one pool instead
    // of getting two. Two processes on DIFFERENT boxes should both leave the
    // base at 1, since their pools are already separate. Deriving this from
    // --node would silently break the second case, which is the case the whole
    // multi-process design exists for.
    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_port   = 0;
    const std::string ip = "127.0.0." +
        std::to_string(cfg.src_ip_base + (index % cfg.src_ips));
    if (::inet_pton(AF_INET, ip.c_str(), &src.sin_addr) != 1)
        return false;

    return ::bind(fd, reinterpret_cast<sockaddr*>(&src), sizeof(src)) == 0;
}

int start_connect(const config& cfg, const sockaddr_in& dst, int index, int& out_errno)
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

    // Nagle would coalesce small frames and hold them for up to 40ms, which
    // would be indistinguishable from server latency in the histogram. The
    // server sets this on its accepted sockets too; one side is not enough.
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // SO_LINGER{1,0} makes close() send RST instead of FIN. The active closer
    // is the side that eats TIME_WAIT, and that is us: 28k sockets sitting in
    // TIME_WAIT for 60s exhausts the ephemeral range and the *next* run fails
    // for no visible reason. A load harness wants the RST; a real client never
    // should, because it discards anything still in the send buffer.
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


static char g_scratch[65536];

// Sends as much of c.pending as the kernel will take. Returns false on a hard
// error. A partial send is normal: the tail stays in c.pending and the caller
// keeps EPOLLOUT armed.
bool flush_pending(int fd, conn& c)
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

// Pulls everything available into c.in. Returns false when the peer closed or
// the socket errored.
//
// Stops after a SHORT read. Under level-triggered epoll a recv() that returns
// fewer bytes than asked has emptied the socket at that instant; the classic
// "loop until EAGAIN" then costs a second syscall per readable socket that
// only ever returns EAGAIN, and epoll_wait re-reports the fd anyway if more
// arrives. Measured before this change: exactly two recv() per readable
// socket (200,022 recvs for 100,650 frames under callgrind), and recv() was
// ~90% of this process's syscalls -- the kernel half of a saturated client.
// Semantics are unchanged: the same bytes are read, the per-socket recv stamp
// is still taken after this returns, and bytes that land during the walk are
// read in the next batch with a later, more honest stamp instead of an
// earlier one.
bool read_available(int fd, conn& c, uint64_t& bytes_in)
{
    for (;;) {
        const ssize_t n = ::recv(fd, g_scratch, sizeof(g_scratch), 0);
        if (n > 0) {
            bytes_in += static_cast<uint64_t>(n);
            c.in.append(g_scratch, static_cast<size_t>(n));
            if (static_cast<size_t>(n) < sizeof(g_scratch)) return true;   // drained
            continue;   // exactly filled: there may be more
        }
        if (n == 0) return false;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        if (errno == EINTR) continue;
        return false;
    }
}
