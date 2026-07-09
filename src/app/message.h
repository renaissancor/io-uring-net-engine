#pragma once
// app/message.h
//
// Thread-mesh message vocabulary. Every cross-role message is a POD,
// trivially-copyable struct so it can be blitted through a byte SPSC ring
// (sds::ring_buffer<N, ring_sync::spsc>) with a plain memcpy — no
// serialization, no heap, no vtables. See spsc_mailbox.h for the framing
// wrapper and wiki inter-thread-comms-spsc-mesh-pattern for the mesh rule.
//
// Wire frame in the ring:  [ app_msg_header | body bytes ]
//   header.size = number of BODY bytes that follow the header (NOT counting
//                 the header itself).
//   header.type = app_msg_type discriminant for the body struct.
// The mailbox length-prefixes with `size` so the consumer can peek the header,
// confirm the whole body is present, and only then dequeue the frame.

#include "session_id.h"
#include "../types.h"

namespace app {

enum class app_msg_type : u16 {
    adopt_session  = 1,   // acceptor -> worker: take ownership of an accepted fd
    session_closed = 2,   // worker -> acceptor: fd owner released the session
    stop           = 3,   // supervisor/peer -> role: cooperative drain request
};

// 4-byte length-prefix header. `size` is the body length so the ring transport
// stays type-agnostic; `type` selects the body struct at the consumer.
struct app_msg_header {
    u16 size;   // body bytes following this header
    u16 type;   // app_msg_type
};

// acceptor -> worker. The acceptor accepts the fd, mints id+generation+fake
// account, then immediately hands the fd to a worker which becomes its SOLE
// owner (invariant: one fd, one owner thread). initial_room is k_no_room on the
// v1 immediate-adopt path — room selection happens worker-side via C_SELECT_ROOM.
struct adopt_session_msg {
    int        fd;
    session_id id;
    u32        generation;
    account_id account;
    room_id    initial_room;
};

// worker -> acceptor. The worker released the fd (client quit, error, or
// drain). Carries generation so the acceptor can discard a close that refers to
// an id/slot that has already been recycled.
struct session_closed_msg {
    session_id id;
    u32        generation;
    worker_id  worker;
    u32        reason;
};

}  // namespace app
