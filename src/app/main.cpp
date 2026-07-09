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

#include "config.h"
#include "detail/thread_role.h"
#include "handle_acceptor.h"
#include "handle_worker.h"
#include "spsc_mailbox.h"

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

    // (3) Config. v1 starts at one worker; raise toward k_worker_max to scale.
    config cfg;
    cfg.worker_count = 1;
    LNX_CHECK(cfg.worker_count >= 1 && cfg.worker_count <= config::k_worker_max);

    // (4) LANDLORD: own all cross-thread storage up front. The worker table
    //     holds address-pinned, non-movable handles -> sds::static_vector.
    //     (acceptor->worker handoff inboxes land with the handoff phase.)
    sds::static_vector<handle_worker, config::k_worker_max> workers;
    for (i32 i = 0; i < cfg.worker_count; ++i) {
        workers.emplace_back(i, cfg);
    }

    // (4a) LANDLORD: allocate the acceptor<->worker mesh edges up front and
    //      install them before any thread starts, so each engine sees non-null
    //      edges on its first tick. The mailboxes are non-movable (they embed
    //      byte rings), so they are named locals owned by this stack frame,
    //      which outlives every thread (all are joined before main returns).
    //      v1 wires the single worker 0; a per-worker edge array replaces this
    //      when worker_count > 1.
    LNX_CHECK(cfg.worker_count == 1);   // mesh fan-out is v1-single-worker only
    acceptor_to_worker_mailbox to_worker;
    worker_to_acceptor_mailbox from_worker;
    workers[0].install_mailboxes(&to_worker, &from_worker);

    // (5) Start workers, then BARRIER until every one publishes running.
    for (auto& w : workers) {
        w.start();
    }
    for (auto& w : workers) {
        while (!w.is_running()) {
            lnx::this_thread::yield();
        }
    }
    std::printf("[main] %d worker(s) running\n", cfg.worker_count);

    // (6) Workers are live -> start the acceptor. The handoff producer only
    //     comes online after its consumers (workers) are ready.
    handle_acceptor acceptor{cfg};
    acceptor.install_mailboxes(&to_worker, &from_worker);
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
