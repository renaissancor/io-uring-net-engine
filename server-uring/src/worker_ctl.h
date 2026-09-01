#pragma once
// app/worker_ctl.h
//
// Worker-role active-object control block. Composes thread_ctl for the
// generic per-thread metadata + lifecycle, adds worker-specific cross-
// thread observables. The TLS engine body lives in worker_engine; this
// ctl is the cross-thread API surface (atomic state, future SPSC
// inbox pointers).
//
// Phase 2 skeleton: only the lifecycle plumbing. Stats atomics and
// peer/aux inbox queue pointers land when peers/db_thread/supervisor
// inboxes are first wired up.

#include "config.h"
#include "thread_ctl.h"
#include "mesh.h"

namespace app {

struct worker_ctl {
    worker_ctl(i32 id, const config& cfg) noexcept;
    ~worker_ctl() noexcept = default;

    worker_ctl(const worker_ctl&)            = delete;
    worker_ctl& operator=(const worker_ctl&) = delete;
    worker_ctl(worker_ctl&&)                 = delete;
    worker_ctl& operator=(worker_ctl&&)      = delete;

    // Role-specific lifecycle — start() spawns with worker trampoline.
    void start() noexcept;

    // Install the mesh edges the supervisor (LANDLORD) owns. `in` is the
    // acceptor->worker admission pipe this worker reads; `out` is the
    // worker->acceptor close-notify pipe it writes. Must be called BEFORE
    // start() so the engine sees non-null edges on its first tick — checked
    // there, because skipping it is a silent race rather than a crash.
    void install_pipes(acceptor_to_worker_pipe* in,
                       worker_to_acceptor_pipe* out) noexcept {
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
    thread_ctl base;
    config        _cfg;

    // Mesh edges (supervisor-owned; this ctl only borrows). The engine
    // reads them via the ctl. Null until install_pipes(); the adopt/
    // drain logic that consumes them lands with worker-side session storage.
    acceptor_to_worker_pipe* _from_acceptor = nullptr;
    worker_to_acceptor_pipe* _to_acceptor   = nullptr;

private:
    static void* entry(void* self) noexcept;   // per-class pthread trampoline
};

}  // namespace app
