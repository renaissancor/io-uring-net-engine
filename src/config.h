// config.h — command line surface. One struct, one parser, no globals.
#pragma once

#include <cstdint>
#include <string>

// ------------------------------------------------------------------ config

struct config {
    std::string host      = "127.0.0.1";
    uint16_t    port      = 9000;
    int         conns     = 10000;
    int         per_room  = 10;
    int         src_ips   = 1;
    int         src_ip_base = 1;   // first 127.0.0.x this process may bind
    int         inflight  = 256;
    int         rcvbuf    = 8192;
    int         sndbuf    = 8192;
    double      rate      = 1.0;   // messages/sec per connection; 0 = no traffic
    int         duration  = 10;    // seconds of traffic
    int         size      = 64;    // filler bytes per message
    bool        size_mix  = false; // rotate through the size classes instead
    uint32_t    node      = 0;     // this process's identity in a fleet
    bool        use_corpus  = false;  // realistic chat text instead of filler
    uint32_t    corpus_seed = 1;      // fixed so runs and nodes are identical
    std::string dump;              // path to write the raw histograms to
};

// Fixed strings per size class, not per-message RNG: generating randomness in
// the hot loop burns client CPU and that cost lands in the measurement.
// Content is irrelevant — TCP does not compress — but length class is not,
// since small frames are syscall-bound and frames over the MSS take the

// Payload size classes for --size-mix. Small frames are syscall-bound and
// frames over the MSS take the segmentation path, so a single size exercises
// exactly one of them.
extern const int k_size_classes[4];

void usage();
bool parse_args(int argc, char** argv, config& cfg);
