// traffic.h — the measuring phase: open-loop send schedule, latency and
// self-lag histograms, the verdict, and the per-run dump merge.py consumes.
//
// The file to read most carefully. Coordinated omission is possible in both
// directions here and both have been live defects:
//   - send side: the payload carries the INTENDED send time, not the actual
//     one. Measuring from the actual send deletes every delay the client
//     caused, which is where the tail lives. Handled from day one.
//   - receive side: recv_ts is stamped per socket, NOT once per epoll batch.
//     Batch-stamping dates the late frames from when the walk began and
//     deletes exactly the delay saturation caused. This was a real defect and
//     it under-reported p50 by 136x at 3M deliveries/s. See
//     design/2026-08-17-three-instrument-defects.md.
//
// The self-lag histogram guards the first and is blind to the second by
// construction. That is the argument for fleet mode.
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

    // Server CPU over the traffic window, from --server-pid. Negative means
    // not sampled. The only field describing the machine under test rather
    // than this process: a run where the client is idle proves the client was
    // not the limit and says nothing about whether the server was.
    //
    // 100% IS NOT SATURATION. This server held 100% of one core from 3M to
    // 10M deliveries/s, because a longer sweep batches more messages into
    // each syscall and cost per message falls as load rises. The high reading
    // carries almost no information; the LOW reading is the useful one, since
    // under ~95% the run is definitively below the ceiling. For the signal
    // that does work -- achieved-vs-offered paired with self-lag -- and the
    // cost model, see result-notes/2026-08-30-what-limits-the-server.md.
    double    server_cpu_pct  = -1.0;  // % of ONE core, summed user+kernel
    double    server_kern_pct = -1.0;  // kernel share of that, 0..100
};

void consume_frames(conn& c, int64_t recv_ts, histogram& lat,
                    uint64_t& frames_in, uint64_t& samples_bad,
                    uint32_t node, uint64_t& foreign);
bool dump_stats(const config& cfg, const traffic_stats& st);
void run_traffic(int ep, std::vector<conn>& conns, std::vector<int>& live,
                 const config& cfg, traffic_stats& st);
// Returns false when the run is VOID -- the numbers describe something other
// than the server and must not be quoted. main() turns that into exit code 3
// so a script can gate on it; a printed verdict nobody reads is not a gate.
bool report(const config& cfg, const traffic_stats& st);
