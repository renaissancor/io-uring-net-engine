#pragma once
// app/handle_acceptor.h
//
// Acceptor-role active-object handle. Mirrors handle_worker: composes
// handle_thread for the generic per-thread metadata + lifecycle, spawns
// with the acceptor trampoline. The TLS engine body lives in
// engine_acceptor; this handle is the cross-thread API surface.
//
// Singleton role — exactly one acceptor per process, so it is referenced
// by name (not indexed like workers). Skeleton: lifecycle plumbing only.
// The listen-fd, accept loop, and acceptor->worker handoff queue land when
// the data path is first wired.

#include "config.h"
#include "handle_thread.h"

namespace app {

struct handle_acceptor {
    explicit handle_acceptor(const config& cfg) noexcept;
    ~handle_acceptor() noexcept = default;

    handle_acceptor(const handle_acceptor&)            = delete;
    handle_acceptor& operator=(const handle_acceptor&) = delete;
    handle_acceptor(handle_acceptor&&)                 = delete;
    handle_acceptor& operator=(handle_acceptor&&)      = delete;

    // Role-specific lifecycle — start() spawns with the acceptor trampoline.
    void start() noexcept;

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

    // listen_fd + acceptor->worker handoff inbox pointers land with the
    // data-path phase.

private:
    static void* entry(void* self) noexcept;   // per-class pthread trampoline
};

}  // namespace app
