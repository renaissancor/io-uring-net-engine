#include "acceptor_engine.h"

#include "detail/thread_role.h"
#include "acceptor_ctl.h"

#include "check.h"
#include "runtime/thread.h"

namespace app {

acceptor_engine& acceptor_engine::instance() noexcept {
    // Guard runs BEFORE the static thread_local body is referenced. If a
    // non-acceptor thread reaches this, we trap before construction — same
    // protocol as worker_engine's role-token guard.
    LNX_CHECK(detail::tls_role == detail::thread_role::acceptor);

    static thread_local acceptor_engine inst;   // ctor runs on first call per thread
    return inst;
}

acceptor_engine::acceptor_engine() noexcept {
    // Skeleton: no listen socket, no io_uring_queue_init, no recv/send
    // buffers, no pre-session pool. Those land here when accept + handoff
    // are first wired.
}

acceptor_engine::~acceptor_engine() noexcept = default;

void acceptor_engine::attach(acceptor_ctl* h) noexcept {
    LNX_CHECK(_ctl == nullptr);   // no double-attach
    LNX_CHECK(h       != nullptr);   // no null-attach
    _ctl = h;
}

void acceptor_engine::run_loop() noexcept {
    LNX_CHECK(_ctl != nullptr);   // attach() must have happened

    // Phase 1 — running: empty tick (skeleton has no listen socket yet).
    // Bumps heartbeat each iteration so a future watchdog can observe
    // liveness. Yields instead of busy-polling — the real body swaps this
    // for io_uring multishot accept + the handoff push per the locked design.
    while (_ctl->base._state->load_acquire()
           == static_cast<i32>(state::running)) {
        _ctl->base._heartbeat_seq->fetch_add(1);
        lnx::this_thread::yield();
    }

    // Phase 2 — draining: nothing in-flight at skeleton level.

    // Phase 3 — publish stopped. ONLY this thread writes stopped.
    _ctl->base._state->store_release(static_cast<i32>(state::stopped));
}

}  // namespace app
