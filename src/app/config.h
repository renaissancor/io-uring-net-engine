#pragma once
// app/config.h
//
// v1 placeholder. The handle_worker ctor takes a `const config&` so the
// signature is stable as configuration fields land in Phase 2+ (db
// connect string, pre-warm overrides, listen address, ...).

#include "../types.h"

namespace app {

struct config {
    // Worker pool ceiling. Worker ids are a single decimal digit
    // (worker_0 .. worker_7) so every name is 8 chars, matching "acceptor"
    // and "database". Beyond this, scale horizontally (more processes), not
    // more threads — see project memory "thread-comm-naming" / "db deferred".
    static constexpr i32 k_worker_max = 8;

    // v1 boots a single worker; raise toward k_worker_max to scale the pool.
    i32 worker_count = 1;
};

}  // namespace app
