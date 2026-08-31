#pragma once
// app/config.h
//
// Runtime configuration. v1 placeholder — the worker_ctl ctor takes a
// `const config&` so the signature is stable as fields land in Phase 2+
// (listen address, db connect string, pre-warm overrides, ...).
//
// The thread roster is NOT here: it is fixed before runtime and lives in
// roster.h. Anything that decides how many threads exist, or how much storage
// they own, is a compile-time constant by design — see roster.h for why.

#include "types.h"

namespace app {

struct config {
    // SessionManager authority-table size (compile-time: it backs a fixed
    // inline session_record array, no heap). Fixed CAPACITY, not a ceiling on a
    // runtime value — the table is always exactly this many slots, which is why
    // it is `_capacity` and not `_max`. This is the correctness-milestone size,
    // NOT a final MMO scale number; raise it once worker-side SoA/mmap session
    // storage lands (see doc/10 §"Preserve: SoA").
    static constexpr usize k_session_capacity = 256;
};

}  // namespace app
