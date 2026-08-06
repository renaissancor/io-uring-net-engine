#pragma once
// app/worker_engine.h
//
// TLS-singleton body for the worker role. Constructed on first
// instance() call FROM the owning worker thread (gated by the role-
// token guard installed by worker_ctl::entry). Reachable only from
// the owning thread; supervisor / peers / db_thread cannot touch it.
//
// Phase 2 skeleton: no SoA, no io_uring, no rooms. Just the lifecycle
// plumbing — empty run_loop that honors the three-state protocol.
// SoA + io_uring init move into the ctor once Phase 2 work lands.

namespace app {

struct worker_ctl;  // fwd-decl: engine knows ctl, not the other way

class worker_engine {
public:
    // TLS-Meyers singleton with role-token guard. Calling from any
    // non-worker thread (role-token != worker) traps LNX_CHECK BEFORE
    // the static thread_local body is constructed — see .omc/wiki
    // §"Engine construction protocol."
    static worker_engine& instance() noexcept;

    // Post-construction wiring: engine learns its ctl.
    // LNX_CHECKs: no-double-attach + no-null-attach.
    void attach(worker_ctl* h) noexcept;

    // Tick loop. Honors the three-state lifecycle:
    //   running → draining → stopped.
    // The worker thread (and ONLY the worker thread) calls this; on
    // exit, it self-publishes state==stopped via release-store.
    // LNX_CHECKs: attach() must have happened first.
    void run_loop() noexcept;

    worker_engine(const worker_engine&)            = delete;
    worker_engine& operator=(const worker_engine&) = delete;
    worker_engine(worker_engine&&)                 = delete;
    worker_engine& operator=(worker_engine&&)      = delete;

private:
    worker_engine() noexcept;
    ~worker_engine() noexcept;

    worker_ctl* _ctl = nullptr;   // plain ptr — single-owner per Lock 7
};

}  // namespace app
