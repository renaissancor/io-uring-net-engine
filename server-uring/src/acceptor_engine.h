#pragma once
// app/acceptor_engine.h
//
// TLS-singleton body for the acceptor role. Constructed on first
// instance() call FROM the owning acceptor thread (gated by the role-
// token guard installed by acceptor_ctl::entry). Reachable only from
// the owning thread; supervisor / workers cannot touch it.
//
// Skeleton: no listen socket, no io_uring, no accept loop. Just the
// lifecycle plumbing — an empty run_loop that honors the three-state
// protocol. The listen-fd, accept loop, auth, and the acceptor->worker
// handoff land in the next phase.

namespace app {

struct acceptor_ctl;  // fwd-decl: engine knows ctl, not the other way

class acceptor_engine {
public:
    // TLS-Meyers singleton with role-token guard. Calling from any
    // non-acceptor thread (role-token != acceptor) traps LNX_CHECK BEFORE
    // the static thread_local body is constructed — same protocol as
    // worker_engine.
    static acceptor_engine& instance() noexcept;

    // Post-construction wiring: engine learns its ctl.
    // LNX_CHECKs: no-double-attach + no-null-attach.
    void attach(acceptor_ctl* h) noexcept;

    // Tick loop. Honors the three-state lifecycle:
    //   running -> draining -> stopped.
    // The acceptor thread (and ONLY the acceptor thread) calls this; on
    // exit, it self-publishes state==stopped via release-store.
    // LNX_CHECKs: attach() must have happened first.
    void run_loop() noexcept;

    acceptor_engine(const acceptor_engine&)            = delete;
    acceptor_engine& operator=(const acceptor_engine&) = delete;
    acceptor_engine(acceptor_engine&&)                 = delete;
    acceptor_engine& operator=(acceptor_engine&&)      = delete;

private:
    acceptor_engine() noexcept;
    ~acceptor_engine() noexcept;

    acceptor_ctl* _ctl = nullptr;   // plain ptr — single-owner per Lock 7
};

}  // namespace app
