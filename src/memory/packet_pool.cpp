#include "memory/packet_pool.h"

#include "check.h"

#include <fmt/core.h>

#include <sys/mman.h>

namespace mem {

namespace {

constexpr usize k_region_align = 16;
constexpr usize k_region_slack = 4096;

constexpr usize align_up(usize n, usize align) noexcept {
    return (n + align - 1) & ~(align - 1);
}

}  // namespace

packet_pool& packet_pool::instance() noexcept {
    thread_local packet_pool inst;
    return inst;
}

packet_pool::packet_pool() noexcept {
    const usize bytes_64   = align_up(k_bucket_size_64   * k_prewarm_64,   k_region_align);
    const usize bytes_256  = align_up(k_bucket_size_256  * k_prewarm_256,  k_region_align);
    const usize bytes_1024 = align_up(k_bucket_size_1024 * k_prewarm_1024, k_region_align);

    _region_size = bytes_64 + bytes_256 + bytes_1024 + k_region_slack;

    void* p = ::mmap(nullptr, _region_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    LNX_CHECK(p != MAP_FAILED);
    _region = static_cast<byte*>(p);

    init_buckets();
}

packet_pool::~packet_pool() noexcept {
    if (_region != nullptr) {
        int rc = ::munmap(_region, _region_size);
        LNX_CHECK(rc == 0);
        _region = nullptr;
    }
}

void packet_pool::init_buckets() noexcept {
    byte* cursor = _region;

    _buckets[0].block_size   = k_bucket_size_64;
    _buckets[0].capacity     = k_prewarm_64;
    _buckets[0].region_begin = cursor;
    cursor += align_up(k_bucket_size_64 * k_prewarm_64, k_region_align);
    _buckets[0].region_end   = cursor;

    _buckets[1].block_size   = k_bucket_size_256;
    _buckets[1].capacity     = k_prewarm_256;
    _buckets[1].region_begin = cursor;
    cursor += align_up(k_bucket_size_256 * k_prewarm_256, k_region_align);
    _buckets[1].region_end   = cursor;

    _buckets[2].block_size   = k_bucket_size_1024;
    _buckets[2].capacity     = k_prewarm_1024;
    _buckets[2].region_begin = cursor;
    cursor += align_up(k_bucket_size_1024 * k_prewarm_1024, k_region_align);
    _buckets[2].region_end   = cursor;
}

void packet_pool::prewarm() noexcept {
    if (_prewarmed) return;

    // Push blocks onto each bucket's free list in reverse so the first
    // acquire returns the lowest address — predictable for debug dumps.
    for (usize i = 0; i < k_bucket_count; ++i) {
        bucket& b = _buckets[i];
        for (usize idx = b.capacity; idx > 0; --idx) {
            byte* block = b.region_begin + (idx - 1) * b.block_size;
            auto* node  = reinterpret_cast<free_node*>(block);
            node->next  = b.free_list;
            b.free_list = node;
        }
    }

    _prewarmed = true;
}

packet_pool::bucket& packet_pool::bucket_for(usize size) noexcept {
    if (size <= k_bucket_size_64)  return _buckets[0];
    if (size <= k_bucket_size_256) return _buckets[1];
    LNX_CHECK(size <= k_bucket_size_1024);
    return _buckets[2];
}

const packet_pool::bucket& packet_pool::bucket_for(usize size) const noexcept {
    if (size <= k_bucket_size_64)  return _buckets[0];
    if (size <= k_bucket_size_256) return _buckets[1];
    LNX_CHECK(size <= k_bucket_size_1024);
    return _buckets[2];
}

void* packet_pool::acquire(usize size) noexcept {
    bucket& b = bucket_for(size);

    if (b.free_list == nullptr) {
        fmt::print(stderr,
                   "fatal: packet_pool bucket exhausted\n"
                   "  bucket_size    = {} B\n"
                   "  prewarm_count  = {}\n"
                   "  current_in_use = {}\n",
                   b.block_size, b.capacity, b.in_use);
        LNX_CHECK(b.free_list != nullptr);
    }

    free_node* node = b.free_list;
    b.free_list = node->next;
    ++b.in_use;
    return node;
}

void packet_pool::release(void* p, usize size) noexcept {
    LNX_CHECK(p != nullptr);
    bucket& b = bucket_for(size);

    auto* block = static_cast<byte*>(p);
    LNX_CHECK(block >= b.region_begin && block < b.region_end);
    LNX_CHECK(b.in_use > 0);

    auto* node  = static_cast<free_node*>(p);
    node->next  = b.free_list;
    b.free_list = node;
    --b.in_use;
}

usize packet_pool::in_use(usize bucket_size) const noexcept {
    return bucket_for(bucket_size).in_use;
}

}  // namespace mem
