#include "connect.h"

#include "netutil.h"
#include "wire.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

int run_connect(int ep, const config& cfg, const sockaddr_in& dst,
                std::vector<conn>& conns, std::vector<int>& live)
{
    int started = 0, established = 0, failed = 0, inflight = 0;
    std::map<int, int> fail_reasons;
    const int64_t t0 = now_ns();
    // ---- establish -----------------------------------------------------
    //
    // Connects are paced. Firing 10k SYNs at once overruns the listen backlog,
    // and the kernel's response to a full accept queue is to drop SYNs
    // silently — which surfaces as mysterious timeouts rather than as an error.

    while (!g_stop && (established + failed) < cfg.conns) {
        while (inflight < cfg.inflight && started < cfg.conns) {
            int err = 0;
            const int fd = start_connect(cfg, dst, started, err);
            if (fd < 0) { ++failed; ++fail_reasons[err]; ++started; continue; }
            if (fd >= static_cast<int>(conns.size()))
                conns.resize(static_cast<size_t>(fd) + 1024);

            conns[fd].state = conn_state::connecting;
            conns[fd].index = started;

            epoll_event ev{};
            ev.events  = EPOLLOUT;   // writable == connect resolved
            ev.data.fd = fd;
            if (::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
                ++failed; ++fail_reasons[errno];
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
        if (n == 0 && inflight == 0 && started >= cfg.conns) break;

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
                // the room, so N clients in one room is O(N^2) frames — 40k in
                // a single room is 800M notices and the connect phase never
                // finishes. Fan-out is a separate experiment with its own knob.
                // Namespaced by node. Two processes left to their own devices
                // both produce c0..cN and r0..rM, so their rooms merge: ask for
                // --per-room 10 across two processes and the server actually
                // sees rooms of 20, doubling the fan-out you thought you were
                // measuring. Disjoint rooms also mean a node only ever receives
                // its own traffic, which is the structural half of the fix that
                // the node stamp in the blob then enforces.
                const std::string tag = "n" + std::to_string(cfg.node);
                put_frame(c.pending, g_proto.id_set_nick,
                          tag + "c" + std::to_string(c.index));
                put_frame(c.pending, g_proto.id_join,
                          tag + "r" + std::to_string(c.index / cfg.per_room));

                c.state = conn_state::ready;
                --inflight;
                ++established;
                live.push_back(fd);

                if (!flush_pending(fd, c)) {
                    ++failed; --established;
                    ++fail_reasons[errno];
                    live.pop_back();
                    ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                    c = conn{};
                    ::close(fd);
                    continue;
                }

                // EPOLLOUT must not stay armed once there is nothing to write.
                // A socket is writable almost always, so a permanent EPOLLOUT
                // spins epoll_wait at 100% CPU — server.cpp lesson 3, and on
                // the client it silently becomes the bottleneck that makes the
                // server look slow.
                epoll_event ev{};
                ev.events  = EPOLLIN | (c.pending.empty() ? 0u : static_cast<uint32_t>(EPOLLOUT));
                ev.data.fd = fd;
                ::epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);

                if ((established % 5000) == 0)
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
                if (alive && (evs[i].events & EPOLLIN)) {
                    alive = read_available(fd, c, junk);
                    c.rx_len = 0;   // join notices; nothing to measure yet
                }
                if (!alive || (evs[i].events & (EPOLLERR | EPOLLHUP))) {
                    --established;
                    ++failed;
                    ++fail_reasons[errno ? errno : ECONNRESET];
                    live.erase(std::find(live.begin(), live.end(), fd));
                    ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                    c = conn{};
                    ::close(fd);
                }
            }
        }
    }

    const double secs = static_cast<double>(now_ns() - t0) / 1e9;
    std::printf("\n[conn] established=%d failed=%d in %.2fs (%.0f conn/s)\n",
                established, failed, secs, secs > 0 ? established / secs : 0.0);
    for (const auto& [err, count] : fail_reasons)
        std::printf("       %6d x %s\n", count, std::strerror(err));

    // Discard whatever the join notices left buffered, so the traffic phase
    // starts from a clean parse position.
    for (int fd : live) conns[fd].rx_len = 0;
    return established;
}
