#pragma once
// app/detail/thread_role.h
//
// Per-thread role token used to gate engine_*::instance() against
// accidental construction on non-owning threads. Default value is
// `none`; each role's entry trampoline installs the appropriate
// value as its FIRST line before any TLS singleton is touched.
//
// See .omc/wiki/ctl-engine-split-pattern.md §"Engine construction
// protocol" for the rationale (the role-token guard prevents Meyers
// `static thread_local` from silently constructing a fresh body on
// the wrong thread — e.g., 573 MiB session SoA on the supervisor).

#include "../../types.h"

namespace app::detail {

enum class thread_role : i32 {
    none       = 0,   // zero-init on every thread that did not go through a trampoline
    worker     = 1,
    db         = 2,   // deferred out of v1 — see project memory "db thread deferred"
    supervisor = 3,
    acceptor   = 4,
};

extern thread_local thread_role tls_role;

}  // namespace app::detail
