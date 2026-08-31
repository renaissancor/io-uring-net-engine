#include "acceptor_ctl.h"

#include "detail/thread_role.h"
#include "acceptor_engine.h"

#include "check.h"
#include "memory/packet_pool.h"
#include "runtime/thread.h"

#include <pthread.h>

namespace app {

acceptor_ctl::acceptor_ctl(const config& cfg) noexcept
    : base(0, "acceptor"),   // singleton: id is vestigial (not a worker index)
      _cfg(cfg)
{
}

void acceptor_ctl::start() noexcept {
    // Move-assigns a freshly-constructed lnx::thread into base._thread.
    // lnx::thread's move-assign LNX_CHECKs the destination is not already
    // joinable — "no double-start" for free.
    base._thread = lnx::thread{&acceptor_ctl::entry, this};
}

void* acceptor_ctl::entry(void* self) noexcept {
    auto* h = static_cast<acceptor_ctl*>(self);

    // (1) Install role token FIRST — gates acceptor_engine::instance().
    detail::tls_role = detail::thread_role::acceptor;

    // (2) Identity for kernel-visible logs (ps -L, /proc/PID/task/TID/comm).
    pthread_setname_np(pthread_self(), h->base._name);

    // (3) Publish kernel tid BEFORE state transitions to running. Release
    //     ordering pairs with the acquire-load in thread_ctl::kernel_tid().
    h->base._kernel_tid->store_release(lnx::this_thread::kernel_tid());

    // (4) TLS singletons construct on first call. The acceptor owns its own
    //     packet_pool (pre-session recv buffers in the data-path phase).
    mem::packet_pool::instance().prewarm();
    acceptor_engine& eng = acceptor_engine::instance();
    eng.attach(h);

    // (5) CAS-promote starting->running — same race-safe pattern as the
    //     worker trampoline: an unconditional store_release(running) would
    //     clobber a concurrent request_stop()'s draining write.
    const i32 starting = static_cast<i32>(state::starting);
    const i32 running  = static_cast<i32>(state::running);
    const i32 draining = static_cast<i32>(state::draining);

    const i32 observed = h->base._state->compare_exchange(running, starting);
    if (observed == starting) {
        // (6) Drive the tick loop. Engine self-publishes stopped on exit.
        eng.run_loop();
    } else {
        // Supervisor requested stop during preamble. Skip the loop and
        // publish stopped directly — engine has nothing in flight.
        LNX_CHECK(observed == draining);
        h->base._state->store_release(static_cast<i32>(state::stopped));
    }
    return nullptr;
}

}  // namespace app
