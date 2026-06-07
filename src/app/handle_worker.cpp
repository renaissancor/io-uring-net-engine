#include "handle_worker.h"

#include "detail/thread_role.h"
#include "engine_worker.h"

#include "../check.h"
#include "../memory/packet_pool.h"
#include "../runtime/thread.h"

#include <cstdio>
#include <pthread.h>

namespace app {

handle_worker::handle_worker(i32 id, const config& cfg) noexcept
    : base(id, ""),   // base name set below
      _cfg(cfg)
{
    // Single decimal digit (0 .. k_worker_max-1) keeps every name 8 chars
    // ("worker_0".."worker_7"), matching "acceptor"/"database". The guard
    // turns an over-large id into a loud trap rather than a silently 9-char
    // name that breaks column alignment in ps/htop.
    LNX_CHECK(id >= 0 && id < config::k_worker_max);
    std::snprintf(base._name, sizeof(base._name), "worker_%d", id);
}

void handle_worker::start() noexcept {
    // Move-assigns a freshly-constructed lnx::thread into base._thread.
    // lnx::thread's move-assign LNX_CHECKs that the destination is not
    // already joinable — gives us "no double-start" for free.
    base._thread = lnx::thread{&handle_worker::entry, this};
}

void* handle_worker::entry(void* self) noexcept {
    auto* h = static_cast<handle_worker*>(self);

    // (1) Install role token FIRST — gates every TLS singleton that
    //     follows. Without this, engine_worker::instance()'s LNX_CHECK
    //     traps.
    detail::tls_role = detail::thread_role::worker;

    // (2) Identity for kernel-visible logs (ps -L, /proc/PID/task/TID/comm).
    pthread_setname_np(pthread_self(), h->base._name);

    // (3) Publish kernel tid BEFORE state transitions to running. Release
    //     ordering pairs with the acquire-load in handle_thread::kernel_tid().
    h->base._kernel_tid->store_release(lnx::this_thread::kernel_tid());

    // (4) TLS singletons construct on first call.
    mem::packet_pool::instance().prewarm();
    engine_worker& eng = engine_worker::instance();
    eng.attach(h);

    // (5) CAS-promote starting→running. Unconditional store_release(running)
    //     would clobber a concurrent request_stop()'s draining write —
    //     the race window is between start() returning and this line
    //     executing. compare_exchange returns the observed value:
    //       observed == starting → CAS swapped, state is now running
    //       observed == draining → request_stop() got here first; we
    //                              short-circuit straight to stopped
    //     Any other observed value violates the lifecycle (starting can
    //     only legally go to running OR draining; we own the worker thread
    //     so nobody else writes stopped).
    const i32 starting = static_cast<i32>(state::starting);
    const i32 running  = static_cast<i32>(state::running);
    const i32 draining = static_cast<i32>(state::draining);
    const i32 stopped  = static_cast<i32>(state::stopped);

    const i32 observed = h->base._state->compare_exchange(running, starting);
    if (observed == starting) {
        // (6) Drive the tick loop. Engine self-publishes stopped on exit.
        eng.run_loop();
    } else {
        // Supervisor requested stop during preamble. Skip the tick loop
        // and publish stopped directly — engine has nothing in flight.
        LNX_CHECK(observed == draining);
        h->base._state->store_release(stopped);
    }
    return nullptr;
}

}  // namespace app
