#pragma once
// app/engine_acceptor.h
//
// TLS-singleton body for the acceptor role. Constructed on first
// instance() call FROM the owning acceptor thread (gated by the role-
// token guard installed by handle_acceptor::entry). Reachable only from
// the owning thread; supervisor / workers cannot touch it.
//
// Skeleton: no listen socket, no io_uring, no accept loop. Just the
// lifecycle plumbing — an empty run_loop that honors the three-state
// protocol. The listen-fd, accept loop, auth, and the acceptor->worker
// handoff land in the next phase.

namespace app {

struct handle_acceptor;  // fwd-decl: engine knows handle, not the other way

class engine_acceptor {
public:
    // TLS-Meyers singleton with role-token guard. Calling from any
    // non-acceptor thread (role-token != acceptor) traps LNX_CHECK BEFORE
    // the static thread_local body is constructed — same protocol as
    // engine_worker.
    static engine_acceptor& instance() noexcept;

    // Post-construction wiring: engine learns its handle.
    // LNX_CHECKs: no-double-attach + no-null-attach.
    void attach(handle_acceptor* h) noexcept;

    // Tick loop. Honors the three-state lifecycle:
    //   running -> draining -> stopped.
    // The acceptor thread (and ONLY the acceptor thread) calls this; on
    // exit, it self-publishes state==stopped via release-store.
    // LNX_CHECKs: attach() must have happened first.
    void run_loop() noexcept;

    engine_acceptor(const engine_acceptor&)            = delete;
    engine_acceptor& operator=(const engine_acceptor&) = delete;
    engine_acceptor(engine_acceptor&&)                 = delete;
    engine_acceptor& operator=(engine_acceptor&&)      = delete;

private:
    engine_acceptor() noexcept;
    ~engine_acceptor() noexcept;

    handle_acceptor* _handle = nullptr;   // plain ptr — single-owner per Lock 7
};

}  // namespace app
