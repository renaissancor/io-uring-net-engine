#pragma once
// memory/packet_pool.h
//
// Per-thread packet pool: three fixed buckets (64 / 256 / 1024 B) backed
// by one mmap region owned by the thread. Free lists are intrusive
// (no atomics — TLS isolation eliminates contention) and must be
// pre-warmed at thread start; lazy fill on the hot path is a bug.
//
// Exhaustion is fatal (LNX_CHECK abort) by design — it means the
// pre-warm constant was sized wrong for the actual workload, not a soft
// condition the server should hide. See
// .omc/wiki/memory-pool-tls-singleton-mmap-design-decision.md.
//
// Alloc-thread == free-thread invariant: a block acquired on thread A
// MUST be released on thread A. Cross-thread free silently corrupts the
// free list.

#include "types.h"

namespace mem {

class packet_pool {
public:
    static constexpr usize k_bucket_size_64   = 64;
    static constexpr usize k_bucket_size_256  = 256;
    static constexpr usize k_bucket_size_1024 = 1024;

    static constexpr usize k_prewarm_64   = 256;
    static constexpr usize k_prewarm_256  = 256;
    static constexpr usize k_prewarm_1024 = 512;

    static constexpr usize k_bucket_count = 3;

    // Meyers thread_local singleton. First call on a thread mmaps the
    // region; thread exit munmaps it via the dtor.
    static packet_pool& instance() noexcept;

    // Populate intrusive free lists for all three buckets from the
    // arena. Idempotent — safe to call once per worker_entry.
    void prewarm() noexcept;

    // Hot-path acquire. `size` selects the smallest bucket that fits
    // (<=64 → 64, <=256 → 256, <=1024 → 1024). > 1024 is fatal.
    // Empty free list is fatal.
    void* acquire(usize size) noexcept;

    // Hot-path release. Must be called on the acquiring thread, with
    // the same `size` class that was acquired.
    void release(void* p, usize size) noexcept;

    // Diagnostic accessor: blocks currently outstanding in the bucket
    // that holds `bucket_size`-byte allocations.
    usize in_use(usize bucket_size) const noexcept;

    packet_pool(const packet_pool&)            = delete;
    packet_pool& operator=(const packet_pool&) = delete;
    packet_pool(packet_pool&&)                 = delete;
    packet_pool& operator=(packet_pool&&)      = delete;

private:
    struct free_node {
        free_node* next;
    };

    struct bucket {
        usize      block_size   = 0;
        usize      capacity     = 0;
        usize      in_use       = 0;
        free_node* free_list    = nullptr;
        byte*      region_begin = nullptr;
        byte*      region_end   = nullptr;
    };

    packet_pool() noexcept;
    ~packet_pool() noexcept;

    void          init_buckets() noexcept;
    bucket&       bucket_for(usize size) noexcept;
    const bucket& bucket_for(usize size) const noexcept;

    byte*  _region      = nullptr;
    usize  _region_size = 0;
    bucket _buckets[k_bucket_count]{};
    bool   _prewarmed   = false;
};

}  // namespace mem
