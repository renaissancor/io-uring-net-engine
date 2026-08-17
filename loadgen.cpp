// loadgen.cpp — connection-scale and latency load generator.
//
// Phase 1: establish N connections, shard them across rooms, hold.
// Phase 2: open-loop message traffic with a delivery-latency histogram and a
//          client self-lag histogram beside it.
//
// This is a measuring instrument, not a product, which is why it lives in its
// own repo rather than inside either server it measures. An instrument that
// lives inside one of the things it compares inherits that project's
// constraints — iouring-net-lib bans the STL, which a load generator has no
// reason to obey — and makes cross-server comparison harder to justify than
// it should be.
//
// Single-threaded on purpose. Scale out with processes and machines, not
// threads: same core scaling, no shared state, and it forces the multi-box
// design from day one. Note that extra processes on one box do NOT buy extra
// ephemeral ports — those are a per-source-IP resource. Hence --src-ips.
//
//   make
//   ./loadgen --conns 10000 --per-room 10 --rate 1 --duration 10
//   ./loadgen --conns 40000 --src-ips 4 --rate 2 --duration 20

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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

// Added in Linux 4.2 for exactly this use case; guard so the file still builds
// on older headers.
#ifndef IP_BIND_ADDRESS_NO_PORT
#define IP_BIND_ADDRESS_NO_PORT 24
#endif

// ------------------------------------------------------------------- wire
//
// Both targets use the same 4-byte little-endian header with no byte
// swapping, and they differ in exactly one respect:
//
//   epoll-chat-study   [uint16 len ][uint16 type]   len  = payload bytes
//   iouring-net-*      [uint16 size][uint16 id  ]   size = payload + header
//
// The width is identical; what differs is whether the length field counts the
// header. That is the entire porting seam, so it is the one thing --proto
// switches. Getting it backwards does not fail loudly — it desynchronises the
// stream by four bytes per frame and every subsequent parse is garbage — so
// it is worth having as an explicit flag rather than an edit.

static constexpr size_t k_header_size = 4;   // uint16 + uint16, both targets

struct proto {
    bool     len_includes_header = false;  // false: study, true: iouring-net
    uint16_t id_set_nick         = 1;
    uint16_t id_join             = 2;
    uint16_t id_chat             = 3;
    uint16_t id_notice           = 100;
    uint16_t id_chat_out         = 101;
};

// The IDs above are epoll-chat-study's. The iouring-net product assigns its
// own from a schema (see iouring-net-server/docs/04-protocol.md § packet ID
// ranges) and that schema does not exist yet, so --proto currently switches
// framing only. Fill these in from the generated table when there is one
// rather than guessing: an unrecognised ID closes the session there, so a
// wrong guess presents as a connection failure and not as a protocol error.
static proto g_proto;

// The study server caps a payload at 1024 and silently drops anything larger.
// What it broadcasts is "nick: " + our blob, so our own budget is smaller
// than 1024 by the length of that prefix. Leave room for a 6-digit nick.
static constexpr size_t k_max_payload  = 1024;
static constexpr size_t k_prefix_slack = 16;
static constexpr size_t k_max_blob     = k_max_payload - k_prefix_slack;

// Our chat payload. The timestamp is the INTENDED send time, not the actual
// one — see the scheduling comment in run_traffic().
//
//   [8B intended_ts_ns][4B seq][4B client_id][filler ...]
static constexpr size_t k_blob_header = 16;

static void put_frame(std::string& out, uint16_t type, const char* data, size_t len)
{
    const auto n = static_cast<uint16_t>(
        g_proto.len_includes_header ? len + k_header_size : len);
    char hdr[k_header_size];
    std::memcpy(hdr,     &n,    sizeof(n));
    std::memcpy(hdr + 2, &type, sizeof(type));
    out.append(hdr, sizeof(hdr));
    out.append(data, len);
}

static void put_frame(std::string& out, uint16_t type, const std::string& payload)
{
    put_frame(out, type, payload.data(), payload.size());
}

// Payload byte count for a header whose length field read as `raw`. Returns
// false when the frame is malformed, which under the inclusive convention
// includes any size below the header itself.
static bool payload_len(uint16_t raw, size_t& out)
{
    if (!g_proto.len_includes_header) { out = raw; return true; }
    if (raw < k_header_size) return false;
    out = static_cast<size_t>(raw) - k_header_size;
    return true;
}

// ------------------------------------------------------------------ config

struct config {
    std::string host      = "127.0.0.1";
    uint16_t    port      = 9000;
    int         conns     = 10000;
    int         per_room  = 10;
    int         src_ips   = 1;
    int         inflight  = 256;
    int         rcvbuf    = 8192;
    int         sndbuf    = 8192;
    double      rate      = 1.0;   // messages/sec per connection; 0 = no traffic
    int         duration  = 10;    // seconds of traffic
    int         size      = 64;    // filler bytes per message
    bool        size_mix  = false; // rotate through the size classes instead
};

// Fixed strings per size class, not per-message RNG: generating randomness in
// the hot loop burns client CPU and that cost lands in the measurement.
// Content is irrelevant — TCP does not compress — but length class is not,
// since small frames are syscall-bound and frames over the MSS take the
// segmentation path.
static const int k_size_classes[] = {16, 32, 256, 1000};

// ------------------------------------------------------------------- state

enum class conn_state : uint8_t { none, connecting, ready, dead };

struct conn {
    conn_state  state = conn_state::none;
    int         index = -1;   // logical client id, for nick/room
    uint32_t    seq   = 0;
    std::string pending;      // unsent tail
    std::string in;           // accumulated recv bytes, may hold a partial frame
};

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static int64_t now_ns()
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// --------------------------------------------------------------- histogram
//
// Fixed 1us buckets out to 1s. Four megabytes and exact within its range,
// which beats an approximate log-bucket scheme at this scale for the trouble
// it saves. Anything past 1s lands in the overflow count and is reported
// separately rather than being quietly clamped into the top bucket.

struct histogram {
    static constexpr int64_t bucket_ns = 1000;      // 1us
    static constexpr size_t  buckets   = 1000000;   // -> 1s

    std::vector<uint32_t> counts = std::vector<uint32_t>(buckets, 0);
    uint64_t total    = 0;
    uint64_t overflow = 0;
    int64_t  max_ns   = 0;
    int64_t  min_ns   = INT64_MAX;

    void add(int64_t ns)
    {
        if (ns < 0) ns = 0;   // clock skew or a same-tick delivery
        ++total;
        if (ns > max_ns) max_ns = ns;
        if (ns < min_ns) min_ns = ns;
        const size_t b = static_cast<size_t>(ns / bucket_ns);
        if (b >= buckets) { ++overflow; return; }
        ++counts[b];
    }

    // Percentile in nanoseconds. Overflow entries are the largest samples, so
    // a percentile that falls inside them is reported as "beyond range"
    // rather than invented.
    int64_t pct(double p, bool& beyond) const
    {
        beyond = false;
        if (total == 0) return 0;
        const uint64_t want = static_cast<uint64_t>(p * static_cast<double>(total));
        uint64_t seen = 0;
        for (size_t b = 0; b < buckets; ++b) {
            seen += counts[b];
            if (seen >= want)
                return static_cast<int64_t>(b) * bucket_ns;
        }
        beyond = true;
        return static_cast<int64_t>(buckets) * bucket_ns;
    }
};

static void print_histogram(const char* label, const histogram& h)
{
    if (h.total == 0) {
        std::printf("  %-18s (no samples)\n", label);
        return;
    }
    auto ms = [](int64_t ns) { return static_cast<double>(ns) / 1e6; };
    bool b50 = false, b90 = false, b99 = false, b999 = false, b9999 = false;
    const int64_t p50   = h.pct(0.50,   b50);
    const int64_t p90   = h.pct(0.90,   b90);
    const int64_t p99   = h.pct(0.99,   b99);
    const int64_t p999  = h.pct(0.999,  b999);
    const int64_t p9999 = h.pct(0.9999, b9999);

    std::printf("  %-18s n=%llu  min=%.3fms  p50=%.3f%s  p90=%.3f%s  "
                "p99=%.3f%s  p99.9=%.3f%s  p99.99=%.3f%s  max=%.3fms\n",
                label,
                static_cast<unsigned long long>(h.total),
                ms(h.min_ns == INT64_MAX ? 0 : h.min_ns),
                ms(p50),   b50   ? "+" : "",
                ms(p90),   b90   ? "+" : "",
                ms(p99),   b99   ? "+" : "",
                ms(p999),  b999  ? "+" : "",
                ms(p9999), b9999 ? "+" : "",
                ms(h.max_ns));
    if (h.overflow)
        std::printf("  %-18s %llu samples over 1s (excluded from percentiles, "
                    "shown as '+')\n", "",
                    static_cast<unsigned long long>(h.overflow));
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

    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_port   = 0;
    const std::string ip = "127.0.0." + std::to_string(1 + (index % cfg.src_ips));
    if (::inet_pton(AF_INET, ip.c_str(), &src.sin_addr) != 1)
        return false;

    return ::bind(fd, reinterpret_cast<sockaddr*>(&src), sizeof(src)) == 0;
}

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

// -------------------------------------------------------------------- io

static char g_scratch[65536];

// Sends as much of c.pending as the kernel will take. Returns false on a hard
// error. A partial send is normal: the tail stays in c.pending and the caller
// keeps EPOLLOUT armed.
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

// Pulls everything available into c.in. Returns false when the peer closed or
// the socket errored.
static bool read_available(int fd, conn& c, uint64_t& bytes_in)
{
    for (;;) {
        const ssize_t n = ::recv(fd, g_scratch, sizeof(g_scratch), 0);
        if (n > 0) {
            bytes_in += static_cast<uint64_t>(n);
            c.in.append(g_scratch, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) return false;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

// Consumes complete frames from c.in and records a latency sample for every
// broadcast-chat frame that carries one of our blobs.
//
// The server broadcasts "nick: " + blob, so the blob starts after the first
// ": ". Finding it by scan rather than by assuming a fixed nick width keeps
// this working when the client count changes the nick length.
static void consume_frames(conn& c, int64_t recv_ts, histogram& lat,
                           uint64_t& frames_in, uint64_t& samples_bad)
{
    size_t off = 0;
    while (c.in.size() - off >= k_header_size) {
        uint16_t raw = 0, type = 0;
        std::memcpy(&raw,  c.in.data() + off, sizeof(raw));
        std::memcpy(&type, c.in.data() + off + 2, sizeof(type));

        size_t len = 0;
        if (!payload_len(raw, len)) {
            // Malformed under the inclusive convention. Almost always means
            // --proto is set the wrong way round: the stream is then off by
            // four bytes per frame and nothing after this point parses.
            ++samples_bad;
            c.in.clear();
            return;
        }

        if (c.in.size() - off < k_header_size + len)
            break;   // partial frame; wait for more bytes

        const char* payload = c.in.data() + off + k_header_size;
        off += k_header_size + len;
        ++frames_in;

        if (type != g_proto.id_chat_out || len < k_blob_header)
            continue;

        const char* sep = static_cast<const char*>(
            std::memchr(payload, ':', len));
        if (!sep || (sep + 2) > (payload + len)) { ++samples_bad; continue; }
        const char*  blob     = sep + 2;           // skip ": "
        const size_t blob_len = static_cast<size_t>((payload + len) - blob);
        if (blob_len < k_blob_header) { ++samples_bad; continue; }

        int64_t intended = 0;
        std::memcpy(&intended, blob, sizeof(intended));

        // The timestamp is the intended send time, so this subtraction already
        // includes any delay the client itself introduced. That is the point:
        // measuring from the actual send time would delete exactly the samples
        // where something went wrong. See run_traffic().
        lat.add(recv_ts - intended);
    }
    if (off) c.in.erase(0, off);
}

// -------------------------------------------------------------------- args

static void usage()
{
    std::printf(
        "usage: loadgen [options]\n"
        "  --host <ip>        server address           (default 127.0.0.1)\n"
        "  --port <n>         server port              (default 9000)\n"
        "  --conns <n>        connections to open      (default 10000)\n"
        "  --per-room <n>     clients per room         (default 10)\n"
        "  --src-ips <n>      bind across 127.0.0.1..n (default 1)\n"
        "  --inflight <n>     concurrent connects      (default 256)\n"
        "  --rcvbuf <bytes>   SO_RCVBUF, 0=default     (default 8192)\n"
        "  --sndbuf <bytes>   SO_SNDBUF, 0=default     (default 8192)\n"
        "  --rate <msg/s>     per connection, 0=none   (default 1)\n"
        "  --duration <secs>  traffic duration         (default 10)\n"
        "  --size <bytes>     filler per message       (default 64)\n"
        "  --size-mix         rotate 16/32/256/1000 instead of --size\n"
        "  --proto <name>     study | iouring          (default study)\n"
        "                       study:   length field counts payload only\n"
        "                       iouring: length field includes the header\n");
}

static bool parse_args(int argc, char** argv, config& cfg)
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
        else if (a == "--proto" && i + 1 < argc) {
            const std::string p = argv[++i];
            if      (p == "study")   g_proto.len_includes_header = false;
            else if (p == "iouring") g_proto.len_includes_header = true;
            else { std::fprintf(stderr, "unknown --proto %s\n", p.c_str()); return false; }
        }
        else if (a == "--conns")     { if (!next_int(cfg.conns))    return false; }
        else if (a == "--per-room")  { if (!next_int(cfg.per_room)) return false; }
        else if (a == "--src-ips")   { if (!next_int(cfg.src_ips))  return false; }
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

// ---------------------------------------------------------------- traffic

struct traffic_stats {
    histogram latency;    // delivery latency, intended-send to receive
    histogram self_lag;   // how late this process was issuing a send
    uint64_t  sent        = 0;
    uint64_t  frames_in   = 0;
    uint64_t  bytes_in    = 0;
    uint64_t  samples_bad = 0;
    uint64_t  backpressed = 0;   // sends skipped because pending was already full
    int       lost_conns  = 0;
};

// Open-loop send scheduling.
//
// Every connection sends at the same rate, so instead of a per-connection
// timer (40k timers is its own performance problem) the schedule is one
// global sequence: message m belongs to connection m % N and is DUE at
// start + m * slot, where slot = 1 / (N * rate). That spreads the load evenly
// across the period instead of firing every connection at once, and it makes
// "am I behind" a single comparison.
//
// The schedule does not depend on replies. Closed-loop sending — wait for the
// echo, then send again — cannot overload the server: when the server slows,
// the client slows with it, the queue never builds, and the latency graph
// comes out flattering and wrong.
static void run_traffic(int ep, std::vector<conn>& conns, std::vector<int>& live,
                        const config& cfg, traffic_stats& st)
{
    if (cfg.rate <= 0 || cfg.duration <= 0 || live.empty())
        return;

    // Fixed filler, generated once. Not random per message: RNG in the hot
    // loop is client CPU, and client CPU spent here shows up as server
    // latency in the results.
    std::string filler(static_cast<size_t>(std::max(cfg.size, k_size_classes[3])), 'x');
    for (size_t i = 0; i < filler.size(); ++i)
        filler[i] = static_cast<char>('a' + (i % 26));

    const size_t  n     = live.size();
    const int64_t slot  = static_cast<int64_t>(1e9 / (static_cast<double>(n) * cfg.rate));
    const int64_t start = now_ns();
    const int64_t end   = start + static_cast<int64_t>(cfg.duration) * 1000000000LL;

    // A single tick that falls far behind must not spin forever trying to
    // catch up while never servicing reads. Cap the burst; the resulting
    // lateness is recorded in self_lag rather than hidden.
    const size_t max_burst = std::max<size_t>(1024, n / 8);

    uint64_t m = 0;              // global message index
    size_t   class_cursor = 0;
    std::string blob;
    blob.reserve(k_max_blob);

    std::printf("[traf] %zu conns x %.2f msg/s for %ds (slot=%lldns, "
                "target %.0f msg/s)\n",
                n, cfg.rate, cfg.duration, static_cast<long long>(slot),
                static_cast<double>(n) * cfg.rate);

    int64_t next_report = start + 5000000000LL;

    while (!g_stop) {
        const int64_t loop_ts = now_ns();
        if (loop_ts >= end) break;

        // ---- issue everything that is due ----------------------------
        size_t burst = 0;
        for (;;) {
            const int64_t due = start + static_cast<int64_t>(m) * slot;
            if (due > loop_ts || burst >= max_burst) break;

            const int fd = live[m % n];
            conn& c = conns[fd];
            ++m;
            ++burst;
            if (c.state != conn_state::ready) continue;

            // How late this process is issuing the send. This is the number
            // that separates server queueing from client saturation, and
            // without it a saturated client reads as a slow server.
            st.self_lag.add(loop_ts - due);

            // A connection whose pending buffer has not drained is already
            // backpressured; piling on more would measure our own send queue.
            if (c.pending.size() > 64 * 1024) { ++st.backpressed; continue; }

            const size_t fill = cfg.size_mix
                ? static_cast<size_t>(k_size_classes[class_cursor++ % 4])
                : static_cast<size_t>(cfg.size);

            blob.assign(k_blob_header, '\0');
            std::memcpy(blob.data(),      &due,     sizeof(due));
            std::memcpy(blob.data() + 8,  &c.seq,   sizeof(c.seq));
            std::memcpy(blob.data() + 12, &c.index, sizeof(c.index));
            blob.append(filler, 0, std::min(fill, k_max_blob - k_blob_header));
            ++c.seq;

            put_frame(c.pending, g_proto.id_chat, blob);
            ++st.sent;

            if (!flush_pending(fd, c)) { c.state = conn_state::dead; continue; }
            if (!c.pending.empty()) {
                epoll_event ev{};
                ev.events  = EPOLLIN | static_cast<uint32_t>(EPOLLOUT);
                ev.data.fd = fd;
                ::epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
            }
        }

        // ---- service the sockets --------------------------------------
        //
        // The timeout is bounded by when the next message comes due, so the
        // loop neither spins nor oversleeps past a deadline.
        const int64_t next_due = start + static_cast<int64_t>(m) * slot;
        int wait_ms = static_cast<int>((next_due - now_ns()) / 1000000);
        if (wait_ms < 0) wait_ms = 0;
        if (wait_ms > 10) wait_ms = 10;

        epoll_event evs[4096];
        const int ready = ::epoll_wait(ep, evs, 4096, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait");
            break;
        }

        const int64_t recv_ts = now_ns();
        for (int i = 0; i < ready; ++i) {
            const int fd = evs[i].data.fd;
            conn& c = conns[fd];
            if (c.state != conn_state::ready) continue;

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
                alive = read_available(fd, c, st.bytes_in);
                consume_frames(c, recv_ts, st.latency, st.frames_in, st.samples_bad);
            }
            if (!alive || (evs[i].events & (EPOLLERR | EPOLLHUP))) {
                c.state = conn_state::dead;
                ++st.lost_conns;
                ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
            }
        }

        if (now_ns() >= next_report) {
            bool b = false;
            std::printf("[traf] sent=%llu recv=%llu lag_p99=%.3fms lost=%d\n",
                        static_cast<unsigned long long>(st.sent),
                        static_cast<unsigned long long>(st.frames_in),
                        static_cast<double>(st.self_lag.pct(0.99, b)) / 1e6,
                        st.lost_conns);
            next_report = now_ns() + 5000000000LL;
        }
    }

    // ---- drain tail ---------------------------------------------------
    //
    // Messages in flight when the clock ran out are still real deliveries.
    // Stopping dead would truncate the slowest samples, which is the same
    // mistake as coordinated omission wearing a different hat.
    const int64_t drain_until = now_ns() + 1000000000LL;
    while (!g_stop && now_ns() < drain_until) {
        epoll_event evs[4096];
        const int ready = ::epoll_wait(ep, evs, 4096, 100);
        if (ready <= 0) { if (ready < 0 && errno == EINTR) continue; else if (ready == 0) continue; else break; }
        const int64_t recv_ts = now_ns();
        for (int i = 0; i < ready; ++i) {
            const int fd = evs[i].data.fd;
            conn& c = conns[fd];
            if (c.state != conn_state::ready) continue;
            if (evs[i].events & EPOLLIN) {
                if (!read_available(fd, c, st.bytes_in)) {
                    c.state = conn_state::dead;
                    ++st.lost_conns;
                    ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                    ::close(fd);
                    continue;
                }
                consume_frames(c, recv_ts, st.latency, st.frames_in, st.samples_bad);
            }
        }
    }
}

// -------------------------------------------------------------------- main

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
                put_frame(c.pending, g_proto.id_set_nick, "c" + std::to_string(c.index));
                put_frame(c.pending, g_proto.id_join,     "r" + std::to_string(c.index / cfg.per_room));

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
                    c.in.clear();   // join notices; nothing to measure yet
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
    for (int fd : live) conns[fd].in.clear();

    // ---- traffic --------------------------------------------------------

    traffic_stats st;
    run_traffic(ep, conns, live, cfg, st);

    if (cfg.rate > 0 && cfg.duration > 0) {
        std::printf("\n[stat] sent=%llu frames_in=%llu bytes_in=%llu "
                    "unparsed=%llu backpressed=%llu lost_conns=%d\n",
                    static_cast<unsigned long long>(st.sent),
                    static_cast<unsigned long long>(st.frames_in),
                    static_cast<unsigned long long>(st.bytes_in),
                    static_cast<unsigned long long>(st.samples_bad),
                    static_cast<unsigned long long>(st.backpressed),
                    st.lost_conns);
        print_histogram("delivery latency", st.latency);
        print_histogram("client self-lag",  st.self_lag);

        // The verdict. Latency numbers taken while this process was itself
        // falling behind describe this process, not the server, and reporting
        // them without saying so is the classic way to publish a wrong result.
        bool a = false, b = false;
        const int64_t lat99 = st.latency.pct(0.99, a);
        const int64_t lag99 = st.self_lag.pct(0.99, b);
        if (st.latency.total == 0) {
            std::printf("[VOID] no latency samples\n");
        } else if (lag99 * 5 > lat99) {
            std::printf("[VOID] self-lag p99 (%.3fms) is not small against "
                        "latency p99 (%.3fms) — this run measured the client, "
                        "not the server. Lower --rate or --conns, or add "
                        "processes and machines.\n",
                        static_cast<double>(lag99) / 1e6,
                        static_cast<double>(lat99) / 1e6);
        } else {
            std::printf("[ OK ] self-lag p99 (%.3fms) is small against latency "
                        "p99 (%.3fms); the client was not the bottleneck\n",
                        static_cast<double>(lag99) / 1e6,
                        static_cast<double>(lat99) / 1e6);
        }
    }

    for (size_t fd = 0; fd < conns.size(); ++fd)
        if (conns[fd].state != conn_state::none)
            ::close(static_cast<int>(fd));
    ::close(ep);
    return 0;
}
