#include "histogram.h"


// Percentiles from separate processes cannot be combined — averaging two p99s
// is not the fleet's p99 and is not any other statistic either. The only
// correct merge is over the buckets themselves, so each process writes its raw
// histograms and merge.py adds them.
//
// Sparse on purpose: a dense million-bucket dump is 1M lines per histogram per
// process, almost all of them zero. Occupied buckets at these rates number in
// the low thousands.
void dump_histogram(std::FILE* f, const char* label, const histogram& h)
{
    std::fprintf(f, "hist %s total=%llu overflow=%llu min=%lld max=%lld bucket_ns=%lld\n",
                 label,
                 static_cast<unsigned long long>(h.total),
                 static_cast<unsigned long long>(h.overflow),
                 static_cast<long long>(h.total ? h.min_ns : 0),
                 static_cast<long long>(h.max_ns),
                 static_cast<long long>(histogram::bucket_ns));
    for (size_t b = 0; b < histogram::buckets; ++b)
        if (h.counts[b])
            std::fprintf(f, "%zu %u\n", b, h.counts[b]);
    std::fprintf(f, "end\n");
}

void print_histogram(const char* label, const histogram& h)
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
