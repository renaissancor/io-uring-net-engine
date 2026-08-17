// traffic.h — the measuring phase: open-loop send schedule, latency and
// self-lag histograms, and the per-run dump that merge.py consumes.
#pragma once

#include <cstdint>
#include <vector>

#include "config.h"
#include "conn.h"
#include "histogram.h"

// ---------------------------------------------------------------- traffic

struct traffic_stats {
    histogram latency;    // delivery latency, intended-send to receive
    histogram self_lag;   // how late this process was issuing a send
    uint64_t  sent        = 0;
    uint64_t  frames_in   = 0;
    uint64_t  bytes_in    = 0;
    uint64_t  samples_bad = 0;
    uint64_t  foreign     = 0;   // chat frames stamped by another loadgen node
    uint64_t  backpressed = 0;   // sends skipped because pending was already full
    int       lost_conns  = 0;
};

void consume_frames(conn& c, int64_t recv_ts, histogram& lat,
                    uint64_t& frames_in, uint64_t& samples_bad,
                    uint32_t node, uint64_t& foreign);
bool dump_stats(const config& cfg, const traffic_stats& st);
void run_traffic(int ep, std::vector<conn>& conns, std::vector<int>& live,
                 const config& cfg, traffic_stats& st);
void report(const config& cfg, const traffic_stats& st);
