#pragma once
// app/config.h
//
// v1 placeholder. The handle_worker ctor takes a `const config&` so the
// signature is stable as configuration fields land in Phase 2+ (db
// connect string, pre-warm overrides, listen address, ...).

namespace app {

struct config {
    // empty — fields land as concrete needs appear
};

}  // namespace app
