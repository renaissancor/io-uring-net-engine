#include "engine_worker.h"

#include "detail/thread_role.h"
#include "handle_worker.h"

#include "../check.h"
#include "../runtime/thread.h"

namespace app {

engine_worker& engine_worker::instance() noexcept {
    // Guard runs BEFORE the static thread_local body is referenced. If a
    // non-worker thread (supervisor, test, db) reaches this, we trap
    // before construction — no 573 MiB body gets allocated on the wrong
    // thread. See wiki §"Engine construction protocol — role-token guard."
    LNX_CHECK(detail::tls_role == detail::thread_role::worker);

    static thread_local engine_worker inst;   // ctor runs on first call per thread
    return inst;
}

engine_worker::engine_worker() noexcept {
    // Phase 2 skeleton: no SoA mmap, no io_uring_queue_init. Those land
    // here when sessions/rooms are first wired.
}

engine_worker::~engine_worker() noexcept = default;

void engine_worker::attach(handle_worker* h) noexcept {
    LNX_CHECK(_handle == nullptr);   // no double-attach
    LNX_CHECK(h       != nullptr);   // no null-attach
    _handle = h;
}

void engine_worker::run_loop() noexcept {
    LNX_CHECK(_handle != nullptr);   // attach() must have happened

    // Phase 1 — running: empty tick (skeleton has no I/O / sessions yet).
    // Bumps heartbeat each iteration so a future watchdog can observe
    // liveness. Yields instead of busy-polling — Phase 2 swaps the body
    // for io_uring_submit + io_uring_peek_cqe per the locked design.
    while (_handle->base._state->load_acquire()
           == static_cast<i32>(state::running)) {
        _handle->base._heartbeat_seq->fetch_add(1);
        lnx::this_thread::yield();
    }

    // Phase 2 — draining: nothing in-flight at skeleton level (no SPSC
    // inboxes wired yet). Real drain logic lands with mesh.

    // Phase 3 — publish stopped. ONLY this thread writes stopped, per
    // worker-lifecycle protocol.
    _handle->base._state->store_release(static_cast<i32>(state::stopped));
}

}  // namespace app
