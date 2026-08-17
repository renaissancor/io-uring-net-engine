// histogram.h — fixed-bucket latency histogram, and the two ways it leaves
// this process: printed for a human, dumped for merge.py.
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

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

void print_histogram(const char* label, const histogram& h);
void dump_histogram(std::FILE* f, const char* label, const histogram& h);
