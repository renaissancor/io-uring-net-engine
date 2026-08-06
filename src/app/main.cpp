// app/main.cpp
//
// The supervisor — sole spawner and root of the supervision tree. main()
// IS the supervisor thread; it never runs an engine. It brings up the
// worker pool and the acceptor, blocks on a termination signal, then drives
// an ordered shutdown.
//
// v1 boot (db deferred — see project memory "db thread deferred"):
//   role token + name + signal mask
//   -> LANDLORD: allocate the worker table up front
//   -> spawn workers -> BARRIER (all running)
//   -> spawn acceptor (producer comes online after its consumers are ready)
//   -> sigwait(SIGINT/SIGTERM)
//   -> shutdown: acceptor first, then workers
//
// Skeleton: no listen socket / accept / handoff yet — this proves the
// 3-role roster and boot ordering. The data path lands next.

#include "acceptor_ctl.h"
#include "config.h"
#include "detail/thread_role.h"
#include "mesh.h"
#include "roster.h"
#include "worker_ctl.h"

#include "../check.h"
#include "../runtime/thread.h"
#include "../sds/static_vector.h"
#include "../types.h"

#include <csignal>
#include <cstdio>
#include <pthread.h>

namespace {

// Block SIGINT/SIGTERM process-wide BEFORE any thread is spawned, so every
// worker/acceptor inherits the blocked mask and ONLY the supervisor reaps
// them via sigwait. Masking first, spawning second, is the ordering that
// makes this race-free.
sigset_t install_signal_mask() noexcept {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    const int rc = pthread_sigmask(SIG_BLOCK, &set, nullptr);
    LNX_CHECK(rc == 0);
    return set;
}

}  // namespace

int main() {
    using namespace app;

    // (1) This thread IS the supervisor. Install the role token first so any
    //     accidental engine_*::instance() call traps instead of constructing
    //     an engine body on the root thread.
    detail::tls_role = detail::thread_role::supervisor;
    pthread_setname_np(pthread_self(), "main");

    // (2) Mask termination signals before spawning (children inherit it).
    const sigset_t term_set = install_signal_mask();

    // (3) Config. The thread roster is fixed at compile time (roster.h), so
    //     there is no worker-count field to validate and no reachable state in
    //     which the roster is wrong.
    config cfg;

    // (4) LANDLORD: own ALL cross-thread storage up front — constructed before
    //     any thread that can see it exists, destroyed after every one is
    //     joined. Exact-sized by the roster: one admission pipe and one
    //     close-notify pipe per worker, no slack slots.
    //
    //     The pipes embed byte rings and are non-movable, so they are named
    //     locals in this frame, which outlives every thread. The worker table
    //     holds address-pinned, non-default-constructible control blocks, which
    //     is what sds::static_vector exists for.
    static_assert(roster::k_worker_count * (sizeof(acceptor_to_worker_pipe)
                                            + sizeof(worker_to_acceptor_pipe))
                      <= 1024 * 1024,
                  "mesh edges live in main's stack frame — keep the roster's "
                  "total under 1 MiB or move them to static storage");

    acceptor_to_worker_pipe to_worker[roster::k_worker_count];
    worker_to_acceptor_pipe from_worker[roster::k_worker_count];

    sds::static_vector<worker_ctl, roster::k_worker_count> workers;
    for (i32 i = 0; i < roster::k_worker_count; ++i) {
        workers.emplace_back(i, cfg);
        workers[i].install_pipes(&to_worker[i], &from_worker[i]);
    }

    // (5) Start workers, then BARRIER until every one publishes running.
    for (auto& w : workers) {
        w.start();
    }
    for (auto& w : workers) {
        while (!w.is_running()) {
            lnx::this_thread::yield();
        }
    }
    std::printf("[main] %d worker(s) running\n", roster::k_worker_count);

    // (6) Workers are live -> start the acceptor. The handoff producer only
    //     comes online after its consumers (workers) are ready. The acceptor is
    //     the mesh hub, so it takes the whole edge array, not a single pair.
    acceptor_ctl acceptor{cfg};
    acceptor.install_pipes(to_worker, from_worker);
    acceptor.start();
    while (!acceptor.is_running()) {
        lnx::this_thread::yield();
    }
    std::printf("[main] acceptor running\n");

    // (7) Supervise: block until a termination signal arrives.
    std::printf("[main] up — Ctrl-C (SIGINT) or SIGTERM to stop\n");
    int sig = 0;
    const int rc = sigwait(&term_set, &sig);
    LNX_CHECK(rc == 0);
    std::printf("\n[main] signal %d received — shutting down\n", sig);

    // (8) Ordered shutdown: acceptor first (stop new admissions), then
    //     workers. Reverse of boot order.
    acceptor.request_stop();
    acceptor.join();
    for (auto& w : workers) {
        w.request_stop();
    }
    for (auto& w : workers) {
        w.join();
    }
    std::printf("[main] stopped\n");
    return 0;
}
