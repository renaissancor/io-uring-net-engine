#pragma once
// app/handle_thread.h
//
// Universal per-thread metadata: identity, atomic lifecycle state,
// kernel tid cache, heartbeat counter, OS thread handle. Composed by
// every role-specific handle (handle_worker, handle_db,
// handle_supervisor) — never inherited.
//
// See .omc/wiki/handle-engine-split-pattern.md for the locked design.
// State transitions follow .omc/wiki/worker-lifecycle-three-state-protocol.md.

#include "../runtime/thread.h"
#include "../sync/atomic.h"
#include "../types.h"

namespace app {

// Four-state lifecycle. `starting` is the ctor default and exists to
// prevent a race between the trampoline's "publish running" store and a
// concurrent request_stop(): if the supervisor called request_stop()
// before the trampoline got there, an unconditional store_release(running)
// would clobber the draining write. The trampoline therefore CAS-promotes
// starting→running; if the CAS loses, the worker exits via the rare
// "stopped during startup" path. The runtime three-state contract
// (running → draining → stopped) per
// .omc/wiki/worker-lifecycle-three-state-protocol.md is unchanged;
// `starting` is purely a pre-running sentinel.
enum class state : i32 {
    starting = 0,
    running  = 1,
    draining = 2,
    stopped  = 3,
};

struct handle_thread {
    handle_thread(i32 id, const char* name) noexcept;
    ~handle_thread() noexcept = default;

    handle_thread(const handle_thread&)            = delete;
    handle_thread& operator=(const handle_thread&) = delete;
    handle_thread(handle_thread&&)                 = delete;
    handle_thread& operator=(handle_thread&&)      = delete;

    // Observers — safe from any thread (atomic snapshots).
    i32         id()          const noexcept { return _id; }
    const char* name()        const noexcept { return _name; }
    state       get_state()   const noexcept { return static_cast<state>(_state->load_acquire()); }
    bool        is_starting() const noexcept { return get_state() == state::starting; }
    bool        is_running()  const noexcept { return get_state() == state::running; }
    bool        is_draining() const noexcept { return get_state() == state::draining; }
    bool        is_stopped()  const noexcept { return get_state() == state::stopped; }

    // kernel_tid() returns 0 until the worker thread has published its
    // gettid() AND we've observed state==running via acquire-load. The
    // release/acquire pair on _state is what makes the _kernel_tid read
    // synchronized. Calling kernel_tid() during the start()→trampoline
    // race window can legitimately see 0; callers should retry or treat
    // 0 as "not yet known."
    i32         kernel_tid()  const noexcept;

    // Mutators — generic across roles.
    void        request_stop() noexcept;   // CAS running→draining; idempotent
    void        join()         noexcept;   // waits for thread exit + post-asserts stopped

    // Data — public for composition; the role-specific handle's entry()
    // trampoline writes _name, _kernel_tid, _state directly.
    i32                                _id;
    char                               _name[16];        // pthread_setname_np cap
    lnx::cache_aligned<lnx::atomic32>  _state;           // default = starting (= 0)
    lnx::cache_aligned<lnx::atomic32>  _kernel_tid;      // default = 0
    lnx::cache_aligned<lnx::atomic64>  _heartbeat_seq;   // default = 0
    lnx::thread                        _thread;          // OS handle; supervisor joins
};

}  // namespace app
