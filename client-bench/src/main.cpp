// loadgen — connection-scale and latency load generator.
//
// Phase 1 (connect.cpp): establish N connections, shard them across rooms.
// Phase 2 (traffic.cpp): open-loop message traffic with a delivery-latency
//                        histogram and a client self-lag histogram beside it.
//
// This is a measuring instrument, not a product, which is why it lives in its
// own repo rather than inside either server it measures. An instrument that
// lives inside one of the things it compares inherits that project's
// constraints — engine-uring bans the STL, which a load generator has no
// reason to obey — and makes cross-server comparison harder to justify than
// it should be.
//
// Single-threaded on purpose. Scale out with processes and machines, not
// threads: same core scaling, no shared state, and it forces the multi-box
// design from day one. Extra processes on one box do NOT buy extra ephemeral
// ports — those are a per-source-IP resource — hence --src-ips and
// --src-ip-base. Use fleet.py to run more than one and merge the result.
//
// Running several processes is not only a way to offer more load. It is the
// only way to check this instrument against itself: a client that is quietly
// saturated reports numbers that look fine, and the way that was caught was
// carrying the same load with one process and with three and finding they
// disagreed by two orders of magnitude. Treat a single-process number at high
// rate as unverified until a fleet run agrees with it.
//
//   make
//   ./loadgen --conns 10000 --per-room 10 --rate 1 --duration 10
//   python3 fleet.py --nodes 3 --conns 3334 --rate 30

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <vector>

#include "config.h"
#include "conn.h"
#include "connect.h"
#include "netutil.h"
#include "traffic.h"

int main(int argc, char** argv)
{
    config cfg;
    if (!parse_args(argc, argv, cfg))
        return 2;

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    const rlim_t want = static_cast<rlim_t>(cfg.conns) + 256;
    rlim_t limit = 0;
    raise_fd_limit(want < 65536 ? 65536 : want, limit);
    std::printf("[cfg ] RLIMIT_NOFILE soft = %llu (need ~%llu)\n",
                static_cast<unsigned long long>(limit),
                static_cast<unsigned long long>(want));
    if (limit < want)
        std::printf("[warn] fd limit below target; expect EMFILE near %llu conns\n",
                    static_cast<unsigned long long>(limit));
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

    std::vector<conn> conns(limit + 16);
    std::vector<int>  live;
    live.reserve(static_cast<size_t>(cfg.conns));

    run_connect(ep, cfg, dst, conns, live);

    traffic_stats st;
    run_traffic(ep, conns, live, cfg, st);
    // Exit 3 means the verdict was VOID: the run produced numbers, and they
    // do not describe the server. A printed verdict is only a gate if
    // something can act on it without reading prose, so it leaves through
    // the exit status too. 0 covers both [ OK ] and [WARN] -- WARN is a
    // usable run with no headroom, not a failed one.
    const bool usable = report(cfg, st);

    for (size_t fd = 0; fd < conns.size(); ++fd)
        if (conns[fd].state != conn_state::none)
            ::close(static_cast<int>(fd));
    ::close(ep);
    return usable ? 0 : 3;
}
