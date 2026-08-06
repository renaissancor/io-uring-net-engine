// tests/app/worker_ctl_skeleton_test.cpp
//
// Phase 2 skeleton smoke test: worker_ctl spawns a worker thread,
// the worker constructs worker_engine via TLS Meyers, attaches, runs
// an empty tick loop until request_stop, drains, publishes stopped,
// and joins clean.
//
// Verifies the locked design from .omc/wiki/ctl-engine-split-pattern.md:
//   - role-token install in trampoline (gates worker_engine::instance())
//   - three-state lifecycle (running → draining → stopped)
//   - kernel_tid published before state==running, observable post-running
//   - request_stop idempotence
//   - clean join under TSan + ASan
//
// Every case uses worker id 0. Ids are ROSTER INDICES (roster.h), not free
// labels — worker_ctl traps on id >= k_worker_count because such a worker does
// not exist in this binary's roster. Each TEST_CASE owns its own worker_ctl in
// its own scope, so they never need distinct ids.

#include "app/config.h"
#include "app/thread_ctl.h"
#include "app/worker_ctl.h"
#include "runtime/thread.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>

namespace {

void spin_until_running(app::worker_ctl& w) noexcept {
    while (!w.is_running()) {
        lnx::this_thread::yield();
    }
}

void spin_until_stopped(app::worker_ctl& w) noexcept {
    while (!w.is_stopped()) {
        lnx::this_thread::yield();
    }
}

void spin_until_kernel_tid(app::worker_ctl& w) noexcept {
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

TEST_CASE("app: worker_ctl spawns, runs, drains, stops clean",
          "[app][skeleton][worker_ctl]") {
    app::config        cfg{};
    app::worker_ctl w{0, cfg};

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

TEST_CASE("app: worker_ctl::request_stop is idempotent",
          "[app][skeleton][worker_ctl]") {
    app::config        cfg{};
    app::worker_ctl w{0, cfg};

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

TEST_CASE("app: request_stop on starting ctl transitions to draining; worker short-circuits to stopped",
          "[app][skeleton][worker_ctl]") {
    // Verifies the latent race surfaced by the wiki review: request_stop()
    // arriving BEFORE the trampoline CAS-promotes starting→running must
    // still take effect. The simple CAS(draining, running) silently drops
    // the request when observed == starting; the worker then never sees
    // a stop and spins forever. The fix handles starting→draining as a
    // valid transition.
    app::config        cfg{};
    app::worker_ctl w{0, cfg};

    REQUIRE(w.is_starting());

    // Before start() — no worker thread exists yet; state is starting.
    // request_stop() must drive it to draining synchronously.
    w.request_stop();
    REQUIRE(w.is_draining());

    // Now start the worker. The trampoline's CAS(running, starting) fails
    // because state is draining; trampoline observes draining and short-
    // circuits to stopped without entering run_loop.
    w.start();
    spin_until_stopped(w);
    REQUIRE(w.is_stopped());

    w.join();
    SUCCEED();
}
