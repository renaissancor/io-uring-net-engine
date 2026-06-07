#include "engine_acceptor.h"

#include "detail/thread_role.h"
#include "handle_acceptor.h"

#include "../check.h"
#include "../runtime/thread.h"

namespace app {

engine_acceptor& engine_acceptor::instance() noexcept {
    // Guard runs BEFORE the static thread_local body is referenced. If a
    // non-acceptor thread reaches this, we trap before construction — same
    // protocol as engine_worker's role-token guard.
    LNX_CHECK(detail::tls_role == detail::thread_role::acceptor);

    static thread_local engine_acceptor inst;   // ctor runs on first call per thread
    return inst;
}

engine_acceptor::engine_acceptor() noexcept {
    // Skeleton: no listen socket, no io_uring_queue_init, no recv/send
    // buffers, no pre-session pool. Those land here when accept + handoff
    // are first wired.
}

engine_acceptor::~engine_acceptor() noexcept = default;

void engine_acceptor::attach(handle_acceptor* h) noexcept {
    LNX_CHECK(_handle == nullptr);   // no double-attach
    LNX_CHECK(h       != nullptr);   // no null-attach
    _handle = h;
}

void engine_acceptor::run_loop() noexcept {
    LNX_CHECK(_handle != nullptr);   // attach() must have happened

    // Phase 1 — running: empty tick (skeleton has no listen socket yet).
    // Bumps heartbeat each iteration so a future watchdog can observe
    // liveness. Yields instead of busy-polling — the real body swaps this
    // for io_uring multishot accept + the handoff push per the locked design.
    while (_handle->base._state->load_acquire()
           == static_cast<i32>(state::running)) {
        _handle->base._heartbeat_seq->fetch_add(1);
        lnx::this_thread::yield();
    }

    // Phase 2 — draining: nothing in-flight at skeleton level.

    // Phase 3 — publish stopped. ONLY this thread writes stopped.
    _handle->base._state->store_release(static_cast<i32>(state::stopped));
}

}  // namespace app
