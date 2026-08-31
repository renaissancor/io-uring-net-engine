#pragma once
// app/session_record.h
//
// SessionManager (acceptor) authority record — the global truth for a session's
// identity, lifecycle state, and current owner worker. This is NOT the
// worker-side session object: it holds NO fd read/write ownership, NO recv/send
// rings, and NO room/entity state. Those live worker-side under the locked
// SoA/mmap session layout (see handoff "Preserve: SoA Session Storage").
//
// The acceptor stays authoritative for id -> state and id -> owner even after
// it hands the fd to a worker; "authority" here means the mapping, not the I/O.

#include "session_id.h"
#include "types.h"

namespace app {

// SessionManager-side lifecycle. Distinct from the thread `state` enum — this
// tracks a session's journey through the mesh, not a thread's runtime.
enum class session_state : u08 {
    accepted,       // fd accepted, id minted, not yet handed to a worker
    assigning,      // adopt_session posted to a worker; awaiting confirmation
    in_world,       // worker owns the fd and the session has entered a room
    disconnecting,  // close in flight
    closed,         // slot free for reuse
};

struct session_record {
    session_id    id;          // process-unique; k_invalid_session when free
    u32           generation;  // bumped per slot reuse; guards stale messages
    account_id    account;     // fake guest id for v1
    i32           owner_worker; // -1 before assignment
    session_state state;
};

}  // namespace app
