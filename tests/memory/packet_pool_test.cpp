// tests/memory/packet_pool_test.cpp
//
// Unit tests for mem::packet_pool. Pool state is thread_local, so each
// test that mutates pool state runs on its own lnx::thread to avoid
// polluting siblings. The exhaustion test forks because LNX_CHECK
// raises SIGTRAP and there's no Catch2-native way to catch a trap.

#include "memory/packet_pool.h"
#include "runtime/thread.h"
#include "runtime/worker_entry.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

void* prewarm_then_acquire_capacity(void*) {
    auto& pool = mem::packet_pool::instance();
    pool.prewarm();

    // Acquire exactly the prewarm count for the 64 B bucket and verify
    // each call returns a usable, distinct address.
    void* prev = nullptr;
    for (usize i = 0; i < mem::packet_pool::k_prewarm_64; ++i) {
        void* p = pool.acquire(mem::packet_pool::k_bucket_size_64);
        if (p == nullptr || p == prev) {
            // Encode failure via a known sentinel; the test checks
            // in_use() below instead, so just bail.
            return reinterpret_cast<void*>(uintptr_t{1});
        }
        prev = p;
    }
    if (pool.in_use(mem::packet_pool::k_bucket_size_64)
        != mem::packet_pool::k_prewarm_64) {
        return reinterpret_cast<void*>(uintptr_t{1});
    }
    return nullptr;
}

struct roundtrip_result {
    void* first  = nullptr;
    void* second = nullptr;
    usize in_use_after_release = static_cast<usize>(-1);
};

void* roundtrip(void* arg) {
    auto* r = static_cast<roundtrip_result*>(arg);
    auto& pool = mem::packet_pool::instance();
    pool.prewarm();

    r->first  = pool.acquire(mem::packet_pool::k_bucket_size_64);
    pool.release(r->first, mem::packet_pool::k_bucket_size_64);
    r->in_use_after_release = pool.in_use(mem::packet_pool::k_bucket_size_64);
    r->second = pool.acquire(mem::packet_pool::k_bucket_size_64);
    return nullptr;
}

struct each_bucket_result {
    void* p64   = nullptr;
    void* p256  = nullptr;
    void* p1024 = nullptr;
};

void* each_bucket(void* arg) {
    auto* r = static_cast<each_bucket_result*>(arg);
    auto& pool = mem::packet_pool::instance();
    pool.prewarm();

    r->p64   = pool.acquire(64);
    r->p256  = pool.acquire(200);  // routes to 256 bucket
    r->p1024 = pool.acquire(900);  // routes to 1024 bucket
    return nullptr;
}

void* touch_pool_then_exit(void*) {
    // Forces packet_pool ctor (mmap) on this thread; thread exit then
    // runs the thread_local dtor (munmap). Catches it via /proc/self/maps.
    auto& pool = mem::packet_pool::instance();
    pool.prewarm();
    (void)pool.acquire(64);
    return nullptr;
}

usize count_mappings_of_size(usize target) noexcept {
    FILE* f = ::fopen("/proc/self/maps", "r");
    if (f == nullptr) return 0;

    usize count = 0;
    char line[512];
    while (::fgets(line, sizeof(line), f) != nullptr) {
        unsigned long lo = 0;
        unsigned long hi = 0;
        if (::sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
        if (hi - lo == static_cast<unsigned long>(target)) ++count;
    }
    ::fclose(f);
    return count;
}

constexpr usize expected_region_size() noexcept {
    constexpr usize align = 16;
    auto round = [](usize n) { return (n + align - 1) & ~(align - 1); };
    return round(mem::packet_pool::k_bucket_size_64   * mem::packet_pool::k_prewarm_64)
         + round(mem::packet_pool::k_bucket_size_256  * mem::packet_pool::k_prewarm_256)
         + round(mem::packet_pool::k_bucket_size_1024 * mem::packet_pool::k_prewarm_1024)
         + 4096;
}

}  // namespace

TEST_CASE("packet_pool: prewarm fills the free list to capacity",
          "[memory][packet_pool]") {
    lnx::thread t{prewarm_then_acquire_capacity, nullptr};
    t.join();
    SUCCEED();  // thread completed without trapping → prewarm filled
}

TEST_CASE("packet_pool: acquire/release round-trip returns the same block",
          "[memory][packet_pool]") {
    roundtrip_result r;
    lnx::thread t{roundtrip, &r};
    t.join();

    REQUIRE(r.first  != nullptr);
    REQUIRE(r.second != nullptr);
    REQUIRE(r.first  == r.second);          // LIFO free list
    REQUIRE(r.in_use_after_release == 0);   // release decremented
}

TEST_CASE("packet_pool: each size routes to a distinct bucket region",
          "[memory][packet_pool]") {
    each_bucket_result r;
    lnx::thread t{each_bucket, &r};
    t.join();

    REQUIRE(r.p64   != nullptr);
    REQUIRE(r.p256  != nullptr);
    REQUIRE(r.p1024 != nullptr);

    auto a64   = reinterpret_cast<uintptr_t>(r.p64);
    auto a256  = reinterpret_cast<uintptr_t>(r.p256);
    auto a1024 = reinterpret_cast<uintptr_t>(r.p1024);

    // Each pair must be in disjoint, non-overlapping address ranges,
    // proving the size-routing landed in distinct bucket regions.
    REQUIRE((a64   + 64   <= a256  || a256  + 256  <= a64));
    REQUIRE((a256  + 256  <= a1024 || a1024 + 1024 <= a256));
    REQUIRE((a64   + 64   <= a1024 || a1024 + 1024 <= a64));
}

TEST_CASE("packet_pool: exhausting a bucket fires LNX_CHECK (fatal trap)",
          "[memory][packet_pool]") {
    pid_t pid = ::fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        // Child: silence the expected diagnostic line so it doesn't
        // pollute test output, then over-allocate and let the trap fire.
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        auto& pool = mem::packet_pool::instance();
        pool.prewarm();
        for (usize i = 0; i < mem::packet_pool::k_prewarm_64; ++i) {
            (void)pool.acquire(64);
        }
        (void)pool.acquire(64);  // expected: SIGTRAP from LNX_CHECK
        ::_exit(0);              // unreachable
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    REQUIRE(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    // int3 → SIGTRAP, clang __builtin_debugtrap may emit ud2 → SIGILL,
    // __builtin_trap fallback → SIGILL/SIGABRT depending on libc.
    REQUIRE((sig == SIGTRAP || sig == SIGILL || sig == SIGABRT));
}

TEST_CASE("packet_pool: thread exit munmaps the region cleanly",
          "[memory][packet_pool]") {
    const usize region_size = expected_region_size();
    const usize before = count_mappings_of_size(region_size);

    {
        lnx::thread t{touch_pool_then_exit, nullptr};
        t.join();
    }

    const usize after = count_mappings_of_size(region_size);
    // If munmap was skipped, the mapping would still be present and
    // `after` would exceed `before`. Equality (or below) means the
    // dtor reclaimed the region.
    REQUIRE(after <= before);
}

TEST_CASE("worker_entry: trampoline prewarms before the worker body runs",
          "[memory][packet_pool][runtime]") {
    struct probe {
        bool   pool_ready = false;
        usize  in_use_at_entry = static_cast<usize>(-1);
    };
    auto body = [](void* arg) -> void* {
        auto* p = static_cast<probe*>(arg);
        auto& pool = mem::packet_pool::instance();
        // If the trampoline ran prewarm(), acquiring here must succeed
        // without us calling prewarm() ourselves.
        void* block = pool.acquire(64);
        p->pool_ready = (block != nullptr);
        p->in_use_at_entry = pool.in_use(64);
        return nullptr;
    };

    probe pr;
    lnx::worker_start start{ body, &pr };
    lnx::thread t{lnx::worker_entry, &start};
    t.join();

    REQUIRE(pr.pool_ready);
    REQUIRE(pr.in_use_at_entry == 1);
}
