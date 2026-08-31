#pragma once
// app/acceptor_ctl.h
//
// Acceptor-role active-object control block. Mirrors worker_ctl: composes
// thread_ctl for the generic per-thread metadata + lifecycle, spawns
// with the acceptor trampoline. The TLS engine body lives in
// acceptor_engine; this ctl is the cross-thread API surface.
//
// Singleton role — exactly one acceptor per process, so it is referenced
// by name (not indexed like workers). Skeleton: lifecycle plumbing only.
// The listen-fd, accept loop, and acceptor->worker handoff queue land when
// the data path is first wired.

#include "config.h"
#include "mesh.h"
#include "roster.h"
#include "thread_ctl.h"

namespace app {

struct acceptor_ctl {
    explicit acceptor_ctl(const config& cfg) noexcept;
    ~acceptor_ctl() noexcept = default;

    acceptor_ctl(const acceptor_ctl&)            = delete;
    acceptor_ctl& operator=(const acceptor_ctl&) = delete;
    acceptor_ctl(acceptor_ctl&&)                 = delete;
    acceptor_ctl& operator=(acceptor_ctl&&)      = delete;

    // Role-specific lifecycle — start() spawns with the acceptor trampoline.
    void start() noexcept;

    // Install the mesh edges the supervisor (LANDLORD) owns — the acceptor is
    // the hub, so it holds one edge pair PER WORKER: `out[i]` is the admission
    // pipe it writes toward worker i, `in[i]` the close-notify pipe it reads
    // back from worker i. Array-reference parameters so the roster size is
    // checked at the call site; must be called BEFORE start(), so the engine
    // sees non-null edges on its first tick.
    void install_pipes(acceptor_to_worker_pipe (&out)[roster::k_worker_count],
                       worker_to_acceptor_pipe (&in)[roster::k_worker_count]) noexcept {
        for (i32 i = 0; i < roster::k_worker_count; ++i) {
            _to_worker[i]   = &out[i];
            _from_worker[i] = &in[i];
        }
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
    config     _cfg;

    // Mesh edges (supervisor-owned; this ctl only borrows). Exact-sized by the
    // roster — no slack slots, and the fan-out loop has a constexpr bound.
    // listen_fd + accept SQEs land with the data-path phase.
    acceptor_to_worker_pipe* _to_worker[roster::k_worker_count]   = {};
    worker_to_acceptor_pipe* _from_worker[roster::k_worker_count] = {};

private:
    static void* entry(void* self) noexcept;   // per-class pthread trampoline
};

}  // namespace app
