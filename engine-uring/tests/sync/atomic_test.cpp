// tests/sync/atomic_test.cpp
//
// Unit tests for lnx::atomic32, lnx::atomic64, and lnx::atomic_ptr.

#include "sync/atomic.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <thread>
#include <vector>

TEST_CASE("atomic32: load store and arithmetic mirror WinAtomic shape",
          "[sync][atomic]") {
    lnx::atomic32 value{10};

    REQUIRE(value.load() == 10);
    value.store(20);
    REQUIRE(value.load_acquire() == 20);

    REQUIRE(value.increment() == 21);
    REQUIRE(value.decrement() == 20);
    REQUIRE(value++ == 20);
    REQUIRE(value.load() == 21);
    REQUIRE(++value == 22);
    REQUIRE(value-- == 22);
    REQUIRE(value.load() == 21);
    REQUIRE(--value == 20);

    REQUIRE((value += 5) == 25);
    REQUIRE((value -= 3) == 22);

    REQUIRE(value.exchange(7) == 22);
    REQUIRE(value.load() == 7);
}

TEST_CASE("atomic32: fetch and bitwise operations return expected values",
          "[sync][atomic]") {
    lnx::atomic32 value{0b1010};

    REQUIRE(value.fetch_add(5) == 0b1010);
    REQUIRE(value.load() == 15);

    REQUIRE(value.fetch_sub(3) == 15);
    REQUIRE(value.load() == 12);

    REQUIRE(value.fetch_and(0b0110) == 12);
    REQUIRE(value.load() == 0b0100);

    REQUIRE(value.fetch_or(0b0011) == 0b0100);
    REQUIRE(value.load() == 0b0111);

    REQUIRE(value.fetch_xor(0b0101) == 0b0111);
    REQUIRE(value.load() == 0b0010);

    REQUIRE((value |= 0b1000) == 0b1010);
    REQUIRE((value &= 0b1110) == 0b1010);
    REQUIRE((value ^= 0b0011) == 0b1001);
}

TEST_CASE("atomic32: compare_exchange returns observed old value",
          "[sync][atomic]") {
    lnx::atomic32 value{5};

    REQUIRE(value.compare_exchange(9, 5) == 5);
    REQUIRE(value.load() == 9);

    REQUIRE(value.compare_exchange(11, 5) == 9);
    REQUIRE(value.load() == 9);
}

TEST_CASE("atomic64: arithmetic works across threads", "[sync][atomic]") {
    lnx::atomic64 counter{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&counter] {
            for (int j = 0; j < 10000; ++j) {
                counter.increment();
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    REQUIRE(counter.load() == 40000);
}

TEST_CASE("atomic_ptr: exchange and compare_exchange use pointer values",
          "[sync][atomic]") {
    int first  = 1;
    int second = 2;
    int third  = 3;

    lnx::atomic_ptr ptr{&first};

    REQUIRE(ptr.load() == &first);
    REQUIRE(ptr.exchange(&second) == &first);
    REQUIRE(ptr.load_acquire() == &second);

    REQUIRE(ptr.compare_exchange(&third, &second) == &second);
    REQUIRE(ptr.load() == &third);

    REQUIRE(ptr.compare_exchange(&first, &second) == &third);
    REQUIRE(ptr.load() == &third);
}

TEST_CASE("atomic wrappers are naturally sized unless explicitly aligned",
          "[sync][atomic]") {
    REQUIRE(sizeof(lnx::atomic32) == sizeof(std::int32_t));
    REQUIRE(sizeof(lnx::atomic64) == sizeof(std::int64_t));
    REQUIRE(sizeof(lnx::atomic_ptr) == sizeof(void*));

    REQUIRE(alignof(lnx::cache_aligned<lnx::atomic32>) == lnx::CACHE_LINE_SIZE);
}
