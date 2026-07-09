#pragma once
// app/handle_worker.h
//
// Worker-role active-object handle. Composes handle_thread for the
// generic per-thread metadata + lifecycle, adds worker-specific cross-
// thread observables. The TLS engine body lives in engine_worker; this
// handle is the cross-thread API surface (atomic state, future SPSC
// inbox pointers).
//
// Phase 2 skeleton: only the lifecycle plumbing. Stats atomics and
// peer/aux inbox queue pointers land when peers/db_thread/supervisor
// inboxes are first wired up.

#include "config.h"
#include "handle_thread.h"
#include "spsc_mailbox.h"

namespace app {

struct handle_worker {
    handle_worker(i32 id, const config& cfg) noexcept;
    ~handle_worker() noexcept = default;

    handle_worker(const handle_worker&)            = delete;
    handle_worker& operator=(const handle_worker&) = delete;
    handle_worker(handle_worker&&)                 = delete;
    handle_worker& operator=(handle_worker&&)      = delete;

    // Role-specific lifecycle — start() spawns with worker trampoline.
    void start() noexcept;

    // Install the mesh edges the supervisor (LANDLORD) owns. `in` is the
    // acceptor->worker admission inbox this worker drains; `out` is the
    // worker->acceptor close-notify outbox it posts to. Must be called BEFORE
    // start() so the engine sees non-null edges on its first tick.
    void install_mailboxes(acceptor_to_worker_mailbox* in,
                           worker_to_acceptor_mailbox* out) noexcept {
        _from_acceptor = in;
        _to_acceptor   = out;
    }

    // Forward generic lifecycle to base.
    void request_stop() noexcept { base.request_stop(); }
    void join()         noexcept { base.join(); }

    // Forward observers.
    i32         id()          const noexcept { return base.id(); }
    const char* name()        const noexcept { return base.name(); }
    state       get_state()   const noexcept { return base.get_state(); }
    bool        is_starting() const noexcept { return base.is_starting(); }
    bool        is_running()  const noexcept { return base.is_running(); }
    bool        is_draining() const noexcept { return base.is_draining(); }
    bool        is_stopped()  const noexcept { return base.is_stopped(); }
    i32         kernel_tid()  const noexcept { return base.kernel_tid(); }

    // Data — public for composition.
    handle_thread base;
    config        _cfg;

    // Mesh edges (supervisor-owned; this handle only borrows). The engine
    // reads them via the handle. Null until install_mailboxes(); the adopt/
    // drain logic that consumes them lands with worker-side session storage.
    acceptor_to_worker_mailbox* _from_acceptor = nullptr;
    worker_to_acceptor_mailbox* _to_acceptor   = nullptr;

private:
    static void* entry(void* self) noexcept;   // per-class pthread trampoline
};

}  // namespace app
