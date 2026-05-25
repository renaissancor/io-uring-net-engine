#include "handle_thread.h"

#include "../check.h"

namespace app {

handle_thread::handle_thread(i32 id, const char* name) noexcept
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

i32 handle_thread::kernel_tid() const noexcept {
    // Acquire-load on _state pairs with the release-store in the entry
    // trampoline that publishes running. Without this acquire, the read
    // of _kernel_tid is unsynchronized. See wiki §"Trampoline pattern."
    if (_state->load_acquire() != static_cast<i32>(state::running)) {
        return 0;
    }
    return _kernel_tid->load_relaxed();
}

void handle_thread::request_stop() noexcept {
    // CAS running→draining is idempotent: a second call (or one after the
    // worker already self-transitioned to stopped) fails the CAS and is a
    // no-op. We deliberately ignore the observed-value return.
    (void)_state->compare_exchange(
        static_cast<i32>(state::draining),
        static_cast<i32>(state::running));
}

void handle_thread::join() noexcept {
    if (_thread.joinable()) {
        _thread.join();
    }
    // Worker thread is responsible for publishing stopped before exit. If
    // join returned without it, the lifecycle protocol was violated.
    LNX_CHECK(_state->load_acquire() == static_cast<i32>(state::stopped));
}

}  // namespace app
