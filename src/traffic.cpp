#include "traffic.h"

#include "corpus.h"
#include "config.h"
#include "netutil.h"
#include "wire.h"

#include <sys/epoll.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

// Consumes complete frames from c.in and records a latency sample for every
// broadcast-chat frame that carries one of our blobs.
//
// The server broadcasts "nick: " + blob, so the blob starts after the first
// ": ". Finding it by scan rather than by assuming a fixed nick width keeps
// this working when the client count changes the nick length.
void consume_frames(conn& c, int64_t recv_ts, histogram& lat,
                           uint64_t& frames_in, uint64_t& samples_bad,
                           uint32_t node, uint64_t& foreign)
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

        // Whose message is this? A room may hold clients from another loadgen
        // process, and subtracting a foreign timestamp means subtracting a
        // foreign clock. Count them so a misconfigured fleet is visible rather
        // than silently folded into the histogram, but never sample them.
        uint32_t origin = 0;
        std::memcpy(&origin, blob + 12, sizeof(origin));
        if (origin != node) { ++foreign; continue; }

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

bool dump_stats(const config& cfg, const traffic_stats& st)
{
    std::FILE* f = std::fopen(cfg.dump.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "node %u\n", cfg.node);
    std::fprintf(f, "conns %d\nper_room %d\nrate %.6f\nduration %d\n",
                 cfg.conns, cfg.per_room, cfg.rate, cfg.duration);
    std::fprintf(f, "sent %llu\nframes_in %llu\nbytes_in %llu\n"
                    "unparsed %llu\nforeign %llu\nbackpressed %llu\nlost_conns %d\n",
                 static_cast<unsigned long long>(st.sent),
                 static_cast<unsigned long long>(st.frames_in),
                 static_cast<unsigned long long>(st.bytes_in),
                 static_cast<unsigned long long>(st.samples_bad),
                 static_cast<unsigned long long>(st.foreign),
                 static_cast<unsigned long long>(st.backpressed),
                 st.lost_conns);
    dump_histogram(f, "latency",  st.latency);
    dump_histogram(f, "self_lag", st.self_lag);
    return std::fclose(f) == 0;
}

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
void run_traffic(int ep, std::vector<conn>& conns, std::vector<int>& live,
                        const config& cfg, traffic_stats& st)
{
    if (cfg.rate <= 0 || cfg.duration <= 0 || live.empty())
        return;

    // Payload bodies, built once. Nothing is generated per message: RNG in the
    // hot loop is client CPU, and client CPU spent here shows up as server
    // latency in the results.
    //
    // Two sources, and which one is in use changes what the run is for.
    // The filler is a fixed byte count, so frame size is a controlled
    // variable and two runs are comparable. The corpus is realistic chat,
    // with the length distribution real conversations have, so the run
    // exercises every frame-size path at once but its size is no longer a
    // knob. Fixed length stays the default precisely because every recorded
    // baseline used it.
    std::string filler(static_cast<size_t>(std::max(cfg.size, k_size_classes[3])), 'x');
    for (size_t i = 0; i < filler.size(); ++i)
        filler[i] = static_cast<char>('a' + (i % 26));

    corpus body;
    if (cfg.use_corpus) {
        body.build(cfg.corpus_seed, 4096, k_max_blob - k_blob_header);
        std::printf("[cfg ] corpus: %zu lines, %zu..%zu bytes, mean %.1f "
                    "(seed %u)\n",
                    body.size(), body.min_bytes(), body.max_bytes(),
                    body.mean_bytes(), cfg.corpus_seed);
    }

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

            blob.assign(k_blob_header, '\0');
            std::memcpy(blob.data(),      &due,      sizeof(due));
            std::memcpy(blob.data() + 8,  &c.seq,    sizeof(c.seq));
            std::memcpy(blob.data() + 12, &cfg.node, sizeof(cfg.node));
            std::memcpy(blob.data() + 16, &c.index,  sizeof(c.index));
            if (cfg.use_corpus) {
                blob.append(body.next());
            } else {
                const size_t fill = cfg.size_mix
                    ? static_cast<size_t>(k_size_classes[class_cursor++ % 4])
                    : static_cast<size_t>(cfg.size);
                blob.append(filler, 0, std::min(fill, k_max_blob - k_blob_header));
            }
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
                // Stamped per socket, not once per epoll batch. Stamping the
                // batch is receive-side coordinated omission — the exact
                // mistake this tool was built to avoid on the send side,
                // wearing the other hat. A saturated client takes longer to
                // walk its ready list, so the frames at the end of the walk
                // are the late ones, and dating them from when the walk began
                // deletes precisely the delay that saturation caused. It is
                // invisible to the self-lag guard, which only watches sends.
                //
                // Measured: at 3M deliveries/s the batch stamp reported p50
                // 0.109ms from one process and 18.868ms from three carrying
                // the same connections and the same load, with the server at
                // 100% CPU and the same user/kernel split in both. The server
                // was doing identical work; only the number of file
                // descriptors per ready list differed, and with it the length
                // of the interval being erased.
                consume_frames(c, now_ns(), st.latency, st.frames_in,
                               st.samples_bad, cfg.node, st.foreign);
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
                // Per socket, for the reason given in the main loop. The drain
                // phase exists to catch the slowest samples, so batch-stamping
                // here would truncate exactly what it was added to preserve.
                consume_frames(c, now_ns(), st.latency, st.frames_in,
                               st.samples_bad, cfg.node, st.foreign);
            }
        }
    }
}

// The end-of-run report. Kept beside run_traffic rather than in main because
// every number it prints is a field of traffic_stats, and the verdict rules are
// part of what the traffic phase means, not part of process startup.
void report(const config& cfg, const traffic_stats& st)
{
    if (cfg.rate <= 0 || cfg.duration <= 0) return;

    std::printf("\n[stat] node=%u sent=%llu frames_in=%llu bytes_in=%llu "
                "unparsed=%llu foreign=%llu backpressed=%llu lost_conns=%d\n",
                cfg.node,
                static_cast<unsigned long long>(st.sent),
                static_cast<unsigned long long>(st.frames_in),
                static_cast<unsigned long long>(st.bytes_in),
                static_cast<unsigned long long>(st.samples_bad),
                static_cast<unsigned long long>(st.foreign),
                static_cast<unsigned long long>(st.backpressed),
                st.lost_conns);
    print_histogram("delivery latency", st.latency);
    print_histogram("client self-lag",  st.self_lag);

    // Two independent fleet checks, because they catch different mistakes
    // and neither one subsumes the other.
    //
    // Measured fan-out is the one that matters. Every chat sent should come
    // back once per room member, so frames_in/sent recovers the room size
    // the server actually used. Run two processes with the same --node and
    // their rooms merge: 2000 clients at --per-room 10 measured 2.01, not
    // 10, and the offered load was double what the command line asked for.
    // This catches the duplicate-node case that the ownership stamp cannot,
    // since two processes claiming node 0 are indistinguishable by
    // construction.
    if (st.sent) {
        const double fanout = static_cast<double>(st.frames_in) /
                              static_cast<double>(st.sent);
        const double want   = static_cast<double>(cfg.per_room);
        std::printf("[fleet] measured fan-out %.2f (--per-room %d)\n",
                    fanout, cfg.per_room);
        if (fanout > want * 1.05 || fanout < want * 0.95)
            std::printf("[fleet] fan-out is not what was requested — rooms "
                        "hold more or fewer clients than --per-room. Two "
                        "processes sharing a --node is the usual cause; "
                        "the offered load is then not the one you asked "
                        "for.\n");
    }

    // The ownership stamp catches the other direction: distinct node ids
    // whose clients nevertheless ended up in one room. Rooms are namespaced
    // by node, so under the current design this cannot happen and the
    // counter is an assertion on that invariant — it will start earning its
    // keep the moment a cross-node fan-out knob exists.
    if (st.foreign)
        std::printf("[fleet] %llu chat frames were stamped by a different "
                    "node and were excluded from the histogram. Rooms are "
                    "supposed to be node-private; they are not.\n",
                    static_cast<unsigned long long>(st.foreign));

    if (!cfg.dump.empty() && !dump_stats(cfg, st))
        std::fprintf(stderr, "could not write --dump %s\n", cfg.dump.c_str());

    // The verdict. Latency numbers taken while this process was itself
    // falling behind describe this process, not the server, and reporting
    // them without saying so is the classic way to publish a wrong result.
    //
    // The ratio alone is not enough. Self-lag inflates measured latency
    // roughly additively, so what decides whether a run is usable is
    // whether subtracting it would change the conclusion. Near the knee
    // both numbers shrink to the same scale and the bare ratio starts
    // flipping on scheduler noise: three back-to-back runs at the same
    // rate reported self-lag 0.133/0.143/0.131ms against latency
    // 0.726/0.658/0.604ms — indistinguishable measurements, and the
    // 5x ratio called the first OK and the other two VOID. Hence the
    // absolute floor: below a millisecond of client jitter, a server
    // whose p99 is sub-millisecond is sub-millisecond either way, and
    // the honest report is the corrected lower bound, not a thrown-away
    // run. Above the floor the correction is load-bearing and the run
    // really does describe the client.
    bool a = false, b = false;
    const int64_t lat99 = st.latency.pct(0.99, a);
    const int64_t lag99 = st.self_lag.pct(0.99, b);
    constexpr int64_t k_lag_floor_ns = 1'000'000;   // 1ms
    if (st.latency.total == 0) {
        std::printf("[VOID] no latency samples\n");
    } else if (lag99 * 5 > lat99 && lag99 >= k_lag_floor_ns) {
        std::printf("[VOID] self-lag p99 (%.3fms) is not small against "
                    "latency p99 (%.3fms) — this run measured the client, "
                    "not the server. Lower --rate or --conns, or add "
                    "processes and machines.\n",
                    static_cast<double>(lag99) / 1e6,
                    static_cast<double>(lat99) / 1e6);
    } else if (lag99 * 5 > lat99) {
        std::printf("[WARN] self-lag p99 (%.3fms) is a large fraction of "
                    "latency p99 (%.3fms), but is under 1ms in absolute "
                    "terms. Read the server p99 as >= %.3fms; the run is "
                    "usable, the headroom is not.\n",
                    static_cast<double>(lag99) / 1e6,
                    static_cast<double>(lat99) / 1e6,
                    static_cast<double>(lat99 - lag99) / 1e6);
    } else {
        std::printf("[ OK ] self-lag p99 (%.3fms) is small against latency "
                    "p99 (%.3fms); the client was not the bottleneck\n",
                    static_cast<double>(lag99) / 1e6,
                    static_cast<double>(lat99) / 1e6);
    }
}
