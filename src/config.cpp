#include "config.h"

#include "wire.h"

#include <cstdio>
#include <cstdlib>

const int k_size_classes[4] = {16, 32, 256, 1000};


void usage()
{
    std::printf(
        "usage: loadgen [options]\n"
        "  --host <ip>        server address           (default 127.0.0.1)\n"
        "  --port <n>         server port              (default 9000)\n"
        "  --conns <n>        connections to open      (default 10000)\n"
        "  --per-room <n>     clients per room         (default 10)\n"
        "  --src-ips <n>      bind across n source IPs  (default 1)\n"
        "  --src-ip-base <n>  first 127.0.0.x to bind   (default 1)\n"
        "  --node <id>        this process in a fleet   (default 0)\n"
        "  --dump <path>      write raw histograms for merge.py\n"
        "  --server-pid <pid> sample the server's CPU over the traffic\n"
        "                       window; answers whether the SERVER was at\n"
        "                       its limit, which self-lag cannot\n"
        "  --inflight <n>     concurrent connects      (default 256)\n"
        "  --rcvbuf <bytes>   SO_RCVBUF, 0=default     (default 8192)\n"
        "  --sndbuf <bytes>   SO_SNDBUF, 0=default     (default 8192)\n"
        "  --rate <msg/s>     per connection, 0=none   (default 1)\n"
        "  --duration <secs>  traffic duration         (default 10)\n"
        "  --size <bytes>     filler per message       (default 64)\n"
        "  --size-mix         rotate 16/32/256/1000 instead of --size\n"
        "  --corpus           realistic chat text instead of fixed filler;\n"
        "                       length then follows a real chat distribution\n"
        "                       and --size / --size-mix no longer apply\n"
        "  --corpus-seed <n>  corpus seed, fixed by default (default 1)\n"
        "  --proto <name>     study | iouring          (default study)\n"
        "                       study:   length field counts payload only\n"
        "                       iouring: length field includes the header\n");
}

bool parse_args(int argc, char** argv, config& cfg)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next_int = [&](int& dst) {
            if (i + 1 >= argc) return false;
            dst = std::atoi(argv[++i]);
            return true;
        };
        if      (a == "--host" && i + 1 < argc) cfg.host = argv[++i];
        else if (a == "--port" && i + 1 < argc) cfg.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--rate" && i + 1 < argc) cfg.rate = std::atof(argv[++i]);
        else if (a == "--size-mix")  cfg.size_mix = true;
        else if (a == "--corpus")    cfg.use_corpus = true;
        else if (a == "--corpus-seed") {
            int n = 0;
            if (!next_int(n) || n < 0) return false;
            cfg.corpus_seed = static_cast<uint32_t>(n);
        }
        else if (a == "--proto" && i + 1 < argc) {
            const std::string p = argv[++i];
            if      (p == "study")   g_proto.len_includes_header = false;
            else if (p == "iouring") g_proto.len_includes_header = true;
            else { std::fprintf(stderr, "unknown --proto %s\n", p.c_str()); return false; }
        }
        else if (a == "--conns")     { if (!next_int(cfg.conns))    return false; }
        else if (a == "--per-room")  { if (!next_int(cfg.per_room)) return false; }
        else if (a == "--src-ips")   { if (!next_int(cfg.src_ips))  return false; }
        else if (a == "--src-ip-base") { if (!next_int(cfg.src_ip_base)) return false; }
        else if (a == "--dump" && i + 1 < argc) cfg.dump = argv[++i];
        else if (a == "--node") {
            int n = 0;
            if (!next_int(n) || n < 0) return false;
            cfg.node = static_cast<uint32_t>(n);
        }
        else if (a == "--server-pid") { if (!next_int(cfg.server_pid)) return false; }
        else if (a == "--inflight")  { if (!next_int(cfg.inflight)) return false; }
        else if (a == "--rcvbuf")    { if (!next_int(cfg.rcvbuf))   return false; }
        else if (a == "--sndbuf")    { if (!next_int(cfg.sndbuf))   return false; }
        else if (a == "--duration")  { if (!next_int(cfg.duration)) return false; }
        else if (a == "--size")      { if (!next_int(cfg.size))     return false; }
        else { usage(); return false; }
    }
    if (cfg.conns <= 0 || cfg.per_room <= 0 || cfg.src_ips <= 0 || cfg.inflight <= 0) {
        std::fprintf(stderr, "counts must be positive\n");
        return false;
    }
    if (cfg.src_ip_base < 1 || cfg.src_ip_base + cfg.src_ips - 1 > 255) {
        std::fprintf(stderr,
                     "--src-ip-base %d with --src-ips %d runs past 127.0.0.255\n",
                     cfg.src_ip_base, cfg.src_ips);
        return false;
    }
    if (cfg.server_pid < 0) {
        std::fprintf(stderr, "--server-pid must not be negative\n");
        return false;
    }
    if (cfg.rate < 0 || cfg.duration < 0) {
        std::fprintf(stderr, "rate and duration must not be negative\n");
        return false;
    }
    if (cfg.size < 0) cfg.size = 0;
    if (static_cast<size_t>(cfg.size) + k_blob_header > k_max_blob) {
        cfg.size = static_cast<int>(k_max_blob - k_blob_header);
        std::fprintf(stderr, "[warn] --size clamped to %d (server payload cap)\n", cfg.size);
    }
    return true;
}
