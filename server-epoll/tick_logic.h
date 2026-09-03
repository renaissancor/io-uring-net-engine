// tick_logic.h — the synthetic logic term of the tick-budget experiment, and
// the instrument that measures the tick it runs in.
//
// This header is included unchanged by every server that joins the
// experiment (server.cpp today; server_sds.cpp and server-uring when their
// data paths take a tick). That is a rule, not a convenience: the comparison
// is epoll against io_uring with the logic term held exactly equal, and a few
// percent of drift in the logic term voids it. Hence no STL, no engine
// headers, nothing a server could specialise — plain arrays over an mmap.
// design-notes/2026-09-02-where-io-uring-becomes-meaningful.md § 7 and
// design-notes/2026-09-03-working-set-knob-for-the-tick-budget-experiment.md
// are the design; this file is the implementation of § 7.2 as amended.
//
// Three knobs, all from the environment, all zero-cost when CHAT_TICK_HZ is
// unset (every entry point returns before touching anything):
//
//   CHAT_TICK_HZ              tick rate; 0 / unset = no tick, loop unchanged
//   LOGIC_NS_PER_ENTITY_TICK  spun per live session per tick   (c_player)
//   LOGIC_NS_PER_MSG          spun per inbound chat frame       (c_msg)
//   LOGIC_BYTES_PER_ENTITY    size W of each session's state block, default 64
//   CHAT_TICK_DUMP            path; the raw phase histograms are written here
//                             at shutdown in client-bench's `hist` line format
//
// The memory term. Each session owns a W-byte block in one MAP_POPULATEd
// arena, indexed by fd so the block is found without a lookup that would
// itself be a cache event under study. The tick walks every live block in
// slot order one cache line at a time, load - fold - store: a read-modify-
// write, because a read-only sweep lets the compiler and the prefetcher do
// what no game system does. The handler touches the sender's own block the
// same way, which is the random-access half. The fold lands in the block's
// first line so the work is observable and cannot be eliminated.
//
// The cycle term. Spin, never sleep: sleeping yields the core and the kernel
// does I/O work in the gap, the opposite of what heavy logic does to a tick.
// Calibrated once at startup to iterations per nanosecond and burned by
// count; a clock read inside the burn loop is a vDSO call per check and skews
// small values.
//
// The instrument. Per tick period: nanoseconds spent draining I/O (from every
// wake to the end of its event processing, summed), in the tick function, and
// in the post-batch flush passes (summed); the number of wakes; and an
// overrun count. 1 us buckets to 1 s, the client's histogram shape, so the
// two can be read with the same eye. A tick that ends past deadline + period
// is an overrun and the deadline is reset from now rather than caught up:
// back-to-back catch-up ticks would starve the drain and measure nothing.
#pragma once

#include <sys/mman.h>
#include <time.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tick {

inline int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// ---------------------------------------------------------------- histogram
//
// Same shape as client-bench's: 1 us buckets to 1 s, overflow counted apart,
// a percentile inside the overflow reported as beyond range. The counts
// array is mmapped so its 4 MB is resident only where touched.

struct hist {
    static constexpr int64_t bucket_ns = 1000;
    static constexpr size_t  buckets   = 1000000;
    uint32_t* counts   = nullptr;
    uint64_t  total    = 0;
    uint64_t  overflow = 0;
    int64_t   max_ns   = 0;
    int64_t   min_ns   = INT64_MAX;

    bool init() {
        void* p = ::mmap(nullptr, buckets * sizeof(uint32_t), PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return false;
        counts = static_cast<uint32_t*>(p);
        return true;
    }
    void add(int64_t ns) {
        if (ns < 0) ns = 0;
        ++total;
        if (ns > max_ns) max_ns = ns;
        if (ns < min_ns) min_ns = ns;
        const size_t b = static_cast<size_t>(ns / bucket_ns);
        if (b >= buckets) { ++overflow; return; }
        ++counts[b];
    }
    int64_t pct(double p, bool& beyond) const {
        beyond = false;
        if (total == 0) return 0;
        const uint64_t want = static_cast<uint64_t>(p * static_cast<double>(total));
        uint64_t seen = 0;
        for (size_t b = 0; b < buckets; ++b) {
            seen += counts[b];
            if (seen >= want) return static_cast<int64_t>(b) * bucket_ns;
        }
        beyond = true;
        return static_cast<int64_t>(buckets) * bucket_ns;
    }
    void print(const char* label) const {
        if (total == 0) { std::printf("  %-12s (no samples)\n", label); return; }
        bool b50, b90, b99, b999;
        const int64_t p50 = pct(0.50, b50), p90 = pct(0.90, b90),
                      p99 = pct(0.99, b99), p999 = pct(0.999, b999);
        std::printf("  %-12s n=%llu  p50=%.3f%s  p90=%.3f%s  p99=%.3f%s  "
                    "p99.9=%.3f%s  max=%.3fms\n",
                    label, static_cast<unsigned long long>(total),
                    p50 / 1e6, b50 ? "+" : "", p90 / 1e6, b90 ? "+" : "",
                    p99 / 1e6, b99 ? "+" : "", p999 / 1e6, b999 ? "+" : "",
                    max_ns / 1e6);
    }
    void dump(std::FILE* f, const char* label) const {
        std::fprintf(f, "hist %s total=%llu overflow=%llu min=%lld max=%lld bucket_ns=%lld\n",
                     label, static_cast<unsigned long long>(total),
                     static_cast<unsigned long long>(overflow),
                     static_cast<long long>(total ? min_ns : 0),
                     static_cast<long long>(max_ns),
                     static_cast<long long>(bucket_ns));
        for (size_t b = 0; b < buckets; ++b)
            if (counts[b]) std::fprintf(f, "%zu %u\n", b, counts[b]);
        std::fprintf(f, "end\n");
    }
};

// -------------------------------------------------------------------- state

struct state {
    bool     enabled  = false;
    uint32_t hz       = 0;
    int64_t  period   = 0;      // ns
    uint64_t ns_msg   = 0;
    uint64_t ns_ent   = 0;
    size_t   w        = 64;     // bytes per entity block
    size_t   slots    = 0;
    uint8_t* arena    = nullptr;
    uint64_t* live    = nullptr;   // bitmap, one bit per slot
    size_t   live_n   = 0;
    double   iters_per_ns = 0;
    const char* dump_path = nullptr;

    // per-period accumulators
    int64_t  deadline    = 0;
    int64_t  drain_acc   = 0;
    int64_t  flush_acc   = 0;
    uint32_t wakes       = 0;
    // totals
    uint64_t ticks       = 0;
    uint64_t overruns    = 0;
    uint64_t msgs        = 0;
    hist     h_drain, h_tick, h_flush, h_wakes;
};

inline state g;

// The burn. One dependent multiply-xorshift per iteration, a volatile sink
// so the loop survives -O2, and no clock inside it.
inline uint64_t burn(uint64_t iters) {
    uint64_t x = 0x9E3779B97F4A7C15ULL ^ iters;
    for (uint64_t i = 0; i < iters; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        x *= 0x2545F4914F6CDD1DULL;
    }
    return x;
}
inline volatile uint64_t sink;

inline void spin_ns(uint64_t ns) {
    if (ns == 0) return;
    sink = burn(static_cast<uint64_t>(static_cast<double>(ns) * g.iters_per_ns) + 1);
}

// Iterations per nanosecond: one warm-up burn, then seven timed 20 ms burns,
// median. A single 20 ms sample moved 0.43-0.57 iters/ns between runs on
// WSL2 (frequency state, a vCPU migration); the median of seven is what a
// row can be read against, and the banner and the dump both carry it so
// drift between cells is visible rather than silent.
inline void calibrate() {
    uint64_t iters = 1u << 20;
    for (;;) {                                   // size the burn to ~20 ms
        const int64_t t0 = now_ns();
        sink = burn(iters);
        if (now_ns() - t0 >= 20'000'000) break;
        iters *= 2;
    }
    double r[7];
    for (int k = 0; k < 7; ++k) {
        const int64_t t0 = now_ns();
        sink = burn(iters);
        r[k] = static_cast<double>(iters) / static_cast<double>(now_ns() - t0);
    }
    for (int i = 1; i < 7; ++i)                  // insertion sort, seven values
        for (int j = i; j > 0 && r[j - 1] > r[j]; --j) { double t = r[j]; r[j] = r[j - 1]; r[j - 1] = t; }
    g.iters_per_ns = r[3];
}

// The memory term: one RMW per cache line of the block; the fold is stored
// into the first line so the walk is observable.
inline void touch_block(size_t slot) {
    uint8_t* b = g.arena + slot * g.w;
    uint64_t acc, v;
    std::memcpy(&acc, b, 8);
    for (size_t off = 0; off < g.w; off += 64) {
        std::memcpy(&v, b + off, 8);
        acc += v;
        v = acc ^ 0xA5A5A5A5A5A5A5A5ULL;
        std::memcpy(b + off, &v, 8);
    }
    std::memcpy(b, &acc, 8);
}

inline bool init(size_t max_conns) {
    if (const char* v = std::getenv("CHAT_TICK_HZ")) g.hz = static_cast<uint32_t>(std::atoi(v));
    if (g.hz == 0) return true;   // disabled: nothing below runs, ever
    g.enabled = true;
    g.period  = 1000000000LL / g.hz;
    if (const char* v = std::getenv("LOGIC_NS_PER_MSG"))         g.ns_msg = std::strtoull(v, nullptr, 10);
    if (const char* v = std::getenv("LOGIC_NS_PER_ENTITY_TICK")) g.ns_ent = std::strtoull(v, nullptr, 10);
    if (const char* v = std::getenv("LOGIC_BYTES_PER_ENTITY"))   g.w      = std::strtoull(v, nullptr, 10);
    g.dump_path = std::getenv("CHAT_TICK_DUMP");
    if (g.w < 64 || (g.w % 64) != 0) {
        std::fprintf(stderr, "LOGIC_BYTES_PER_ENTITY must be a positive multiple of 64\n");
        return false;
    }
    // fd-indexed, and a few fds (listener, epoll, signalfd, reserve) sit
    // below or among the connections, so give the arena headroom.
    g.slots = max_conns + 64;
    const size_t bytes = g.slots * g.w;
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (p == MAP_FAILED) { std::perror("mmap arena"); return false; }
    g.arena = static_cast<uint8_t*>(p);
    for (size_t off = 0; off < bytes; off += 4096) g.arena[off] = 1;   // belt and braces
    const size_t words = (g.slots + 63) / 64;
    g.live = static_cast<uint64_t*>(std::calloc(words, sizeof(uint64_t)));
    if (!g.live) return false;
    if (!g.h_drain.init() || !g.h_tick.init() || !g.h_flush.init() || !g.h_wakes.init())
        return false;
    calibrate();
    std::printf("[cfg ] tick = %u Hz (period %.3f ms); logic %llu ns/entity/tick, "
                "%llu ns/msg, %zu B/entity (arena %zu slots, %.1f MB); "
                "burn calibrated at %.2f iters/ns\n",
                g.hz, g.period / 1e6,
                static_cast<unsigned long long>(g.ns_ent),
                static_cast<unsigned long long>(g.ns_msg),
                g.w, g.slots, bytes / 1e6, g.iters_per_ns);
    g.deadline = now_ns() + g.period;
    return true;
}

inline void session_add(int fd) {
    if (!g.enabled || fd < 0 || static_cast<size_t>(fd) >= g.slots) return;
    const size_t s = static_cast<size_t>(fd);
    if (!(g.live[s / 64] & (1ULL << (s % 64)))) { g.live[s / 64] |= 1ULL << (s % 64); ++g.live_n; }
}
inline void session_remove(int fd) {
    if (!g.enabled || fd < 0 || static_cast<size_t>(fd) >= g.slots) return;
    const size_t s = static_cast<size_t>(fd);
    if (g.live[s / 64] & (1ULL << (s % 64))) { g.live[s / 64] &= ~(1ULL << (s % 64)); --g.live_n; }
}

// Handler-side work per inbound chat frame: the sender's block, then c_msg.
inline void on_msg(int fd) {
    if (!g.enabled || fd < 0 || static_cast<size_t>(fd) >= g.slots) return;
    ++g.msgs;
    touch_block(static_cast<size_t>(fd));
    spin_ns(g.ns_msg);
}

// The tick function: every live block in slot order, c_player each.
inline void run_tick() {
    const size_t words = (g.slots + 63) / 64;
    for (size_t wi = 0; wi < words; ++wi) {
        uint64_t bits = g.live[wi];
        while (bits) {
            const int b = __builtin_ctzll(bits);
            bits &= bits - 1;
            touch_block(wi * 64 + static_cast<size_t>(b));
            spin_ns(g.ns_ent);
        }
    }
}

// ------------------------------------------------------------ loop hooks
//
// The loop calls these in order each iteration:
//   timeout_ns()                   before the wait
//   drain_begin() / drain_end()    around event processing
//   maybe_tick()                   after processing, before the flush pass
//   flush_begin() / flush_end()    around the flush/reap tail
// All return immediately when disabled.

inline int64_t timeout_ns() {           // -1 = block
    if (!g.enabled) return -1;
    const int64_t left = g.deadline - now_ns();
    return left > 0 ? left : 0;
}

inline int64_t t_mark;
inline void drain_begin() { if (g.enabled) { t_mark = now_ns(); ++g.wakes; } }
inline void drain_end()   { if (g.enabled) g.drain_acc += now_ns() - t_mark; }
inline void flush_begin() { if (g.enabled) t_mark = now_ns(); }
inline void flush_end()   { if (g.enabled) g.flush_acc += now_ns() - t_mark; }

inline void maybe_tick() {
    if (!g.enabled) return;
    const int64_t t0 = now_ns();
    if (t0 < g.deadline) return;
    run_tick();
    const int64_t t1 = now_ns();
    ++g.ticks;
    g.h_drain.add(g.drain_acc);
    g.h_flush.add(g.flush_acc);
    g.h_tick.add(t1 - t0);
    g.h_wakes.add(static_cast<int64_t>(g.wakes) * hist::bucket_ns);   // count, in "us" buckets
    g.drain_acc = g.flush_acc = 0;
    g.wakes = 0;
    if (t1 > g.deadline + g.period) {
        ++g.overruns;
        g.deadline = t1 + g.period;      // reset, do not catch up
    } else {
        g.deadline += g.period;
    }
}

inline void report() {
    if (!g.enabled) return;
    std::printf("[tick] %llu ticks, %llu overruns (%.2f%%), %llu handler calls, "
                "%zu sessions live at exit; per-period ms:\n",
                static_cast<unsigned long long>(g.ticks),
                static_cast<unsigned long long>(g.overruns),
                g.ticks ? 100.0 * g.overruns / g.ticks : 0.0,
                static_cast<unsigned long long>(g.msgs), g.live_n);
    g.h_drain.print("io drain");
    g.h_tick.print("tick");
    g.h_flush.print("flush");
    // wakes were stored as count*1000 ns so the 1us histogram counts them
    bool b;
    std::printf("  %-12s p50=%lld  p90=%lld  p99=%lld  max=%lld  (epoll wakes per period)\n",
                "wakes", static_cast<long long>(g.h_wakes.pct(0.50, b) / hist::bucket_ns),
                static_cast<long long>(g.h_wakes.pct(0.90, b) / hist::bucket_ns),
                static_cast<long long>(g.h_wakes.pct(0.99, b) / hist::bucket_ns),
                static_cast<long long>(g.h_wakes.max_ns / hist::bucket_ns));
    if (g.dump_path) {
        std::FILE* f = std::fopen(g.dump_path, "w");
        if (!f) { std::perror("CHAT_TICK_DUMP"); return; }
        std::fprintf(f, "tick-dump v1 hz=%u ns_per_entity=%llu ns_per_msg=%llu bytes_per_entity=%zu "
                        "iters_per_ns=%.4f\nticks %llu\noverruns %llu\nmsgs %llu\n",
                     g.hz, static_cast<unsigned long long>(g.ns_ent),
                     static_cast<unsigned long long>(g.ns_msg), g.w, g.iters_per_ns,
                     static_cast<unsigned long long>(g.ticks),
                     static_cast<unsigned long long>(g.overruns),
                     static_cast<unsigned long long>(g.msgs));
        g.h_drain.dump(f, "io_drain");
        g.h_tick.dump(f, "tick");
        g.h_flush.dump(f, "flush");
        g.h_wakes.dump(f, "wakes");
        std::fclose(f);
    }
}

} // namespace tick
