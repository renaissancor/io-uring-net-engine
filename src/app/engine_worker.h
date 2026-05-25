#pragma once
// app/engine_worker.h
//
// TLS-singleton body for the worker role. Constructed on first
// instance() call FROM the owning worker thread (gated by the role-
// token guard installed by handle_worker::entry). Reachable only from
// the owning thread; supervisor / peers / db_thread cannot touch it.
//
// Phase 2 skeleton: no SoA, no io_uring, no rooms. Just the lifecycle
// plumbing — empty run_loop that honors the three-state protocol.
// SoA + io_uring init move into the ctor once Phase 2 work lands.

namespace app {

struct handle_worker;  // fwd-decl: engine knows handle, not the other way

class engine_worker {
public:
    // TLS-Meyers singleton with role-token guard. Calling from any
    // non-worker thread (role-token != worker) traps LNX_CHECK BEFORE
    // the static thread_local body is constructed — see .omc/wiki
    // §"Engine construction protocol."
    static engine_worker& instance() noexcept;

    // Post-construction wiring: engine learns its handle.
    // LNX_CHECKs: no-double-attach + no-null-attach.
    void attach(handle_worker* h) noexcept;

    // Tick loop. Honors the three-state lifecycle:
    //   running → draining → stopped.
    // The worker thread (and ONLY the worker thread) calls this; on
    // exit, it self-publishes state==stopped via release-store.
    // LNX_CHECKs: attach() must have happened first.
    void run_loop() noexcept;

    engine_worker(const engine_worker&)            = delete;
    engine_worker& operator=(const engine_worker&) = delete;
    engine_worker(engine_worker&&)                 = delete;
    engine_worker& operator=(engine_worker&&)      = delete;

private:
    engine_worker() noexcept;
    ~engine_worker() noexcept;

    handle_worker* _handle = nullptr;   // plain ptr — single-owner per Lock 7
};

}  // namespace app
