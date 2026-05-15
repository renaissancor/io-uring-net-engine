// tests/runtime/thread_test.cpp
//
// Unit tests for lnx::thread and lnx::this_thread::*.

#include "runtime/thread.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <utility>

namespace {

void* set_bool(void* arg) {
    auto* flag = static_cast<std::atomic<bool>*>(arg);
    flag->store(true);
    return nullptr;
}

void* delayed_set_int(void* arg) {
    auto* value = static_cast<std::atomic<int>*>(arg);
    lnx::this_thread::sleep_for_ns(10000000);
    value->store(1);
    return nullptr;
}

void* noop(void*) {
    return nullptr;
}

void* store_pthread_self(void* arg) {
    auto* id = static_cast<pthread_t*>(arg);
    *id = pthread_self();
    return nullptr;
}

struct identity_values {
    pthread_t main_id;
    int main_kernel_tid;
    pthread_t worker_id;
    int worker_kernel_tid;
};

void* store_identity_values(void* arg) {
    auto* values = static_cast<identity_values*>(arg);
    values->worker_id         = lnx::this_thread::id();
    values->worker_kernel_tid = lnx::this_thread::kernel_tid();
    return nullptr;
}

} // namespace

TEST_CASE("thread: default constructed is not joinable", "[runtime][thread]") {
    lnx::thread t;

    REQUIRE_FALSE(t.joinable());
}

TEST_CASE("thread: created thread runs the function and is joinable",
          "[runtime][thread]") {
    std::atomic<bool> flag{false};
    lnx::thread t{set_bool, &flag};

    REQUIRE(t.joinable());

    t.join();

    REQUIRE(flag.load());
    REQUIRE_FALSE(t.joinable());
}

TEST_CASE("thread: detached thread runs and the wrapper is no longer joinable",
          "[runtime][thread]") {
    std::atomic<int> value{0};
    lnx::thread t{delayed_set_int, &value};

    t.detach();

    REQUIRE_FALSE(t.joinable());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (value.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        lnx::this_thread::yield();
    }

    REQUIRE(value.load() == 1);
}

TEST_CASE("thread: move ctor transfers ownership", "[runtime][thread]") {
    lnx::thread source{noop, nullptr};
    lnx::thread dest{std::move(source)};

    REQUIRE_FALSE(source.joinable());
    REQUIRE(dest.joinable());

    dest.join();
}

TEST_CASE("thread: move assign transfers ownership", "[runtime][thread]") {
    lnx::thread source{noop, nullptr};
    lnx::thread dest;

    dest = std::move(source);

    REQUIRE_FALSE(source.joinable());
    REQUIRE(dest.joinable());

    dest.join();
}

TEST_CASE("thread: native_handle returns pthread_t", "[runtime][thread]") {
    pthread_t observed{};
    lnx::thread t{store_pthread_self, &observed};
    pthread_t handle = t.native_handle();

    t.join();

    REQUIRE(pthread_equal(handle, observed) != 0);
}

TEST_CASE("this_thread::id and kernel_tid return current thread identities",
          "[runtime][thread]") {
    identity_values values{
        lnx::this_thread::id(),
        lnx::this_thread::kernel_tid(),
        pthread_t{},
        0,
    };

    REQUIRE(values.main_kernel_tid != 0);

    lnx::thread t{store_identity_values, &values};
    t.join();

    REQUIRE(values.worker_kernel_tid != 0);
    REQUIRE(pthread_equal(values.main_id, values.worker_id) == 0);
    REQUIRE(values.worker_kernel_tid != values.main_kernel_tid);
}

TEST_CASE("this_thread::yield is callable and returns", "[runtime][thread]") {
    lnx::this_thread::yield();

    SUCCEED();
}

TEST_CASE("this_thread::sleep_for_ns sleeps for approximately the requested duration",
          "[runtime][thread]") {
    auto before = std::chrono::steady_clock::now();

    lnx::this_thread::sleep_for_ns(10000000);

    auto after   = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(after - before);

    REQUIRE(elapsed >= std::chrono::milliseconds(8));
}
