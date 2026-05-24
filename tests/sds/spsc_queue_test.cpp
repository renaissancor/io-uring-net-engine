// tests/sds/spsc_queue_test.cpp
//
// Unit + stress tests for sds::spsc_queue<T, N>. Multi-threaded test runs
// under TSan via the project's tsan preset (see docs/08-test-strategy.md).

#include "sds/spsc_queue.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

using sds::spsc_queue;

TEST_CASE("spsc_queue: starts empty", "[sds][spsc_queue]") {
    spsc_queue<int, 8> q;
    REQUIRE(q.empty());
    REQUIRE_FALSE(q.full());
    REQUIRE(q.size() == 0);
    REQUIRE(q.capacity() == 8);
}

TEST_CASE("spsc_queue: push/pop round-trip", "[sds][spsc_queue]") {
    spsc_queue<int, 4> q;
    REQUIRE(q.try_push(10));
    REQUIRE(q.try_push(20));
    REQUIRE(q.size() == 2);

    int out = 0;
    REQUIRE(q.try_pop(out));
    REQUIRE(out == 10);
    REQUIRE(q.try_pop(out));
    REQUIRE(out == 20);
    REQUIRE(q.empty());
    REQUIRE_FALSE(q.try_pop(out));
}

TEST_CASE("spsc_queue: full() and try_push refusal", "[sds][spsc_queue]") {
    spsc_queue<int, 4> q;
    REQUIRE(q.try_push(1));
    REQUIRE(q.try_push(2));
    REQUIRE(q.try_push(3));
    REQUIRE(q.try_push(4));
    REQUIRE(q.full());
    REQUIRE_FALSE(q.try_push(5));  // full → returned false, no trap

    int out = 0;
    REQUIRE(q.try_pop(out));
    REQUIRE(out == 1);
    REQUIRE(q.try_push(5));  // drained one slot, push succeeds
}

TEST_CASE("spsc_queue: indices wrap around N", "[sds][spsc_queue]") {
    spsc_queue<int, 4> q;

    // Cycle through the ring multiple times to exercise the mask wrap.
    for (int cycle = 0; cycle < 8; ++cycle) {
        for (int i = 0; i < 4; ++i) {
            REQUIRE(q.try_push(cycle * 100 + i));
        }
        for (int i = 0; i < 4; ++i) {
            int out = -1;
            REQUIRE(q.try_pop(out));
            REQUIRE(out == cycle * 100 + i);
        }
        REQUIRE(q.empty());
    }
}

TEST_CASE("spsc_queue: empty pop returns false without touching out",
          "[sds][spsc_queue]") {
    spsc_queue<int, 4> q;
    int out = 0xDEAD;
    REQUIRE_FALSE(q.try_pop(out));
    REQUIRE(out == 0xDEAD);
}

TEST_CASE("spsc_queue: 1M-item SPSC stress preserves FIFO order",
          "[sds][spsc_queue][stress]") {
    constexpr int kTotal = 1'000'000;
    spsc_queue<int, 1024> q;

    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (int i = 0; i < kTotal; ++i) {
            while (!q.try_push(i)) {
                // Spin until consumer drains a slot.
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    int expected = 0;
    while (expected < kTotal) {
        int value = 0;
        if (q.try_pop(value)) {
            REQUIRE(value == expected);
            ++expected;
        }
    }

    producer.join();
    REQUIRE(producer_done.load(std::memory_order_acquire));
    REQUIRE(q.empty());
}
