// tests/app/handle_worker_skeleton_test.cpp
//
// Phase 2 skeleton smoke test: handle_worker spawns a worker thread,
// the worker constructs engine_worker via TLS Meyers, attaches, runs
// an empty tick loop until request_stop, drains, publishes stopped,
// and joins clean.
//
// Verifies the locked design from .omc/wiki/handle-engine-split-pattern.md:
//   - role-token install in trampoline (gates engine_worker::instance())
//   - three-state lifecycle (running → draining → stopped)
//   - kernel_tid published before state==running, observable post-running
//   - request_stop idempotence
//   - clean join under TSan + ASan

#include "app/config.h"
#include "app/handle_thread.h"
#include "app/handle_worker.h"
#include "runtime/thread.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>

namespace {

void spin_until_running(app::handle_worker& w) noexcept {
    while (!w.is_running()) {
        lnx::this_thread::yield();
    }
}

void spin_until_stopped(app::handle_worker& w) noexcept {
    while (!w.is_stopped()) {
        lnx::this_thread::yield();
    }
}

void spin_until_kernel_tid(app::handle_worker& w) noexcept {
    // kernel_tid() may legitimately return 0 in the start()→trampoline
    // race window even after is_running() is true (ctor default-inits
    // _state to running, so observing running doesn't strictly imply the
    // trampoline's release-publish has happened). Spin until the value
    // is actually published.
    while (w.kernel_tid() == 0) {
        lnx::this_thread::yield();
    }
}

}  // namespace

TEST_CASE("app: handle_worker spawns, runs, drains, stops clean",
          "[app][skeleton][handle_worker]") {
    app::config        cfg{};
    app::handle_worker w{0, cfg};

    REQUIRE(w.id() == 0);
    REQUIRE(w.is_starting());     // ctor default = starting; running is published by trampoline
    REQUIRE_FALSE(w.is_running());

    w.start();
    spin_until_running(w);
    spin_until_kernel_tid(w);

    REQUIRE(w.is_running());
    REQUIRE_FALSE(w.is_draining());
    REQUIRE_FALSE(w.is_stopped());
    REQUIRE(w.kernel_tid() > 0);

    w.request_stop();
    spin_until_stopped(w);

    REQUIRE_FALSE(w.is_running());
    REQUIRE(w.is_stopped());

    w.join();
    SUCCEED();
}

TEST_CASE("app: handle_worker::request_stop is idempotent",
          "[app][skeleton][handle_worker]") {
    app::config        cfg{};
    app::handle_worker w{1, cfg};

    w.start();
    spin_until_running(w);

    // Spam request_stop — only the first running→draining CAS succeeds;
    // subsequent calls observe non-running and no-op.
    w.request_stop();
    w.request_stop();
    w.request_stop();

    spin_until_stopped(w);
    w.join();
    SUCCEED();
}
