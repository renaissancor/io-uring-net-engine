// tests/sync/mutex_test.cpp
//
// Unit tests for lnx::mutex, lnx::shared_mutex, and their RAII guards.

#include "sync/mutex.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

TEST_CASE("mutex: lock and unlock acquire and release", "[sync][mutex]") {
    lnx::mutex m;
    std::atomic<bool> try_result{true};

    {
        lnx::lock_guard guard{m};

        std::thread thread{[&m, &try_result] {
            try_result.store(m.try_lock());
        }};
        thread.join();
    }

    REQUIRE_FALSE(try_result.load());
    REQUIRE(m.try_lock());
    m.unlock();
}

TEST_CASE("mutex: multi-thread counter via lock_guard reaches expected total",
          "[sync][mutex]") {
    lnx::mutex m;
    int counter = 0;
    std::vector<std::thread> threads;

    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&m, &counter] {
            for (int j = 0; j < 10000; ++j) {
                lnx::lock_guard guard{m};
                ++counter;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    REQUIRE(counter == 40000);
}

TEST_CASE("unique_lock: default constructed is empty", "[sync][mutex]") {
    lnx::unique_lock u;

    REQUIRE_FALSE(u.owns_lock());
}

TEST_CASE("unique_lock: construction locks and destruction unlocks",
          "[sync][mutex]") {
    lnx::mutex m;

    {
        lnx::unique_lock u{m};
        REQUIRE(u.owns_lock());
    }

    REQUIRE(m.try_lock());
    m.unlock();
}

TEST_CASE("unique_lock: try_lock returns false when contended",
          "[sync][mutex]") {
    lnx::mutex m;
    std::atomic<bool> try_result{true};

    {
        lnx::lock_guard guard{m};

        std::thread thread{[&m, &try_result] {
            try_result.store(m.try_lock());
        }};
        thread.join();
    }

    REQUIRE_FALSE(try_result.load());
    REQUIRE(m.try_lock());
    m.unlock();

    lnx::unique_lock u{m};
    u.unlock();
    REQUIRE(u.try_lock());
}

TEST_CASE("unique_lock: move transfers ownership", "[sync][mutex]") {
    lnx::mutex m;

    {
        lnx::unique_lock a{m};
        lnx::unique_lock b{std::move(a)};

        REQUIRE_FALSE(a.owns_lock());
        REQUIRE(b.owns_lock());
    }

    REQUIRE(m.try_lock());
    m.unlock();
}

TEST_CASE("unique_lock: release relinquishes pointer without unlocking",
          "[sync][mutex]") {
    lnx::mutex m;
    lnx::unique_lock u{m};

    auto* p = u.release();

    REQUIRE(p == &m);
    REQUIRE_FALSE(u.owns_lock());
    REQUIRE_FALSE(m.try_lock());

    m.unlock();
}

TEST_CASE("unique_lock: unlock then try_lock re-acquires", "[sync][mutex]") {
    lnx::mutex m;
    lnx::unique_lock u{m};

    u.unlock();

    REQUIRE_FALSE(u.owns_lock());
    REQUIRE(u.try_lock());
    REQUIRE(u.owns_lock());
}

TEST_CASE("shared_mutex: exclusive blocks shared via guard scopes",
          "[sync][shared_mutex]") {
    lnx::shared_mutex sm;

    {
        lnx::exclusive_lock_guard guard{sm};
        REQUIRE_FALSE(sm.try_lock_shared());
    }

    REQUIRE(sm.try_lock_shared());
    sm.unlock_shared();
}

TEST_CASE("shared_mutex: multiple shared holders coexist",
          "[sync][shared_mutex]") {
    lnx::shared_mutex sm;

    REQUIRE(sm.try_lock_shared());
    REQUIRE(sm.try_lock_shared());

    sm.unlock_shared();
    sm.unlock_shared();

    REQUIRE(sm.try_lock_exclusive());
    sm.unlock_exclusive();
}

TEST_CASE("shared_mutex: read-heavy stress with single writer",
          "[sync][shared_mutex]") {
    lnx::shared_mutex sm;
    int counter = 0;
    std::vector<std::thread> readers;

    std::thread writer{[&sm, &counter] {
        for (int i = 0; i < 1000; ++i) {
            lnx::exclusive_lock_guard guard{sm};
            ++counter;
        }
    }};

    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&sm, &counter] {
            for (int j = 0; j < 10000; ++j) {
                lnx::shared_lock_guard guard{sm};
                int observed = counter;
                (void)observed;
            }
        });
    }

    writer.join();

    for (auto& reader : readers) {
        reader.join();
    }

    REQUIRE(counter == 1000);
}
