#include "thread_ctl.h"

#include "../check.h"

namespace app {

thread_ctl::thread_ctl(i32 id, const char* name) noexcept
    : _id(id)
{
    // Copy at most 15 bytes + NUL. No <cstring> needed; pthread_setname_np
    // cap is 16 bytes including terminator (Linux ABI).
    usize n = 0;
    while (n < sizeof(_name) - 1 && name != nullptr && name[n] != '\0') {
        _name[n] = name[n];
        ++n;
    }
    _name[n] = '\0';
}

i32 thread_ctl::kernel_tid() const noexcept {
    // Acquire-load on _state pairs with the release-store in the entry
    // trampoline that publishes running. Without this acquire, the read
    // of _kernel_tid is unsynchronized. See wiki §"Trampoline pattern."
    if (_state->load_acquire() != static_cast<i32>(state::running)) {
        return 0;
    }
    return _kernel_tid->load_relaxed();
}

void thread_ctl::request_stop() noexcept {
    // Transitions starting OR running → draining via CAS-loop. Both are
    // legitimate "pre-stop" states:
    //   starting → draining   (supervisor asks before worker thread published
    //                          running; the trampoline observes draining at
    //                          its CAS-promote step and short-circuits to
    //                          stopped without entering run_loop)
    //   running  → draining   (normal-path drain request after worker is
    //                          live; run_loop observes draining and exits)
    // Once draining or stopped, subsequent calls are no-ops. This makes
    // request_stop idempotent AND race-free against the trampoline's
    // own CAS-promote of starting→running.
    const i32 starting = static_cast<i32>(state::starting);
    const i32 running  = static_cast<i32>(state::running);
    const i32 draining = static_cast<i32>(state::draining);

    i32 observed = _state->load_acquire();
    while (observed == starting || observed == running) {
        const i32 actual = _state->compare_exchange(draining, observed);
        if (actual == observed) {
            return;   // CAS swapped, draining published
        }
        observed = actual;   // retry against the latest value
    }
    // observed is draining or stopped — already past the request_stop
    // window; no-op.
}

void thread_ctl::join() noexcept {
    if (_thread.joinable()) {
        _thread.join();
    }
    // Worker thread is responsible for publishing stopped before exit. If
    // join returned without it, the lifecycle protocol was violated.
    LNX_CHECK(_state->load_acquire() == static_cast<i32>(state::stopped));
}

}  // namespace app
