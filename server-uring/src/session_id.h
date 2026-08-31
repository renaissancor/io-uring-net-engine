#pragma once
// app/session_id.h
//
// Flat identity aliases shared by the SessionManager (acceptor) authority
// table and the worker-side session cache. v1 keeps these as plain integer
// aliases — strong wrapper structs (phantom-typed handles) can come later if
// mixing a room_id where a session_id belongs ever bites. See handoff
// "SessionManager Data" and project memory "chat-server-v1 data layout".
//
//   session_id  monotonic, process-unique. 0 is the reserved invalid id.
//   account_id  fake guest identity for v1 (== session_id until DB/auth lands).
//   room_id     interaction-space id; 0 is the reserved "no room" sentinel.
//   worker_id   owner-thread index into the worker pool (0 .. k_worker_count-1).

#include "types.h"

namespace app {

using session_id = u64;
using account_id = u64;
using room_id    = u32;
using worker_id  = u32;

inline constexpr session_id k_invalid_session = 0;
inline constexpr room_id    k_no_room         = 0;

}  // namespace app
