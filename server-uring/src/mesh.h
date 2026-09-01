#pragma once
// app/mesh.h
//
// Length-prefixed framing over a thread-mesh byte stream (sds::pipe).
//
// The transport is a raw byte pipe with no notion of messages — exactly like
// TCP. This header is the framing layer on top, and it is deliberately a set of
// free functions rather than a wrapper class: the pipe IS the mesh edge, and
// wrapping it would hide the byte API that the socket path also parses against.
//
// Frame layout matches the wire packet shape so ONE parse loop serves both:
//
//   [ app_msg_header | body bytes ]
//     header.size = body bytes following the header (NOT counting the header)
//     header.type = app_msg_type discriminant selecting the body struct
//
// Reader rule: peek the header -> confirm the WHOLE body is present -> only
// then consume. mesh_post() publishes header+body via pipe::enqueue2(), which
// advances the producer cursor once, after both regions land — so a partial
// frame is never observable. try_recv still verifies full availability before
// consuming a byte, so the reader never depends on writer behavior it cannot
// see.
//
// Backpressure, not growth: mesh_post() returns false and writes NOTHING when
// the pipe lacks room for the whole frame. That is a signal for the caller
// (drop-and-close), never a resize trigger.
//
// Size limits: the TRANSPORT has no message cap — a frame is bounded only by
// the pipe's capacity, same as a pipe(2). k_mesh_body_max below is a PROTOCOL
// bound: the largest body in the mesh vocabulary, used to size receive
// buffers. Raise it when a new message body approaches it.

#include "config.h"
#include "message.h"
#include "check.h"
#include "sds/pipe.h"
#include "types.h"

namespace app {

// Largest body in the mesh vocabulary. Sized well above the current structs
// (adopt_session_msg / session_closed_msg are ~32 B) so receive scratch stays a
// fixed, small cost. This bounds the PROTOCOL, not the pipe.
inline constexpr usize k_mesh_body_max = 1024;

// Bytes a frame occupies in the pipe for a body of `body_len`.
constexpr usize mesh_frame_size(usize body_len) noexcept {
    return sizeof(app_msg_header) + body_len;
}

// ---- producer side ------------------------------------------------------

// Post a raw body under `type` as one atomic [header|body] frame. Returns false
// — writing nothing — when the pipe lacks room for the whole frame.
// `body` may be null iff body_len == 0.
template <typename Pipe>
bool mesh_post(Pipe& p, app_msg_type type, const void* body, u16 body_len) noexcept {
    const app_msg_header hdr{body_len, static_cast<u16>(type)};
    const usize          want = mesh_frame_size(body_len);
    return p.enqueue2(reinterpret_cast<const byte*>(&hdr), sizeof hdr,
                      static_cast<const byte*>(body), body_len) == want;
}

// Typed convenience: post a POD message struct from message.h.
template <typename Pipe, typename T>
bool mesh_post_msg(Pipe& p, app_msg_type type, const T& msg) noexcept {
    static_assert(sizeof(T) <= k_mesh_body_max, "message body exceeds mesh protocol bound");
    return mesh_post(p, type, &msg, static_cast<u16>(sizeof(T)));
}

// ---- consumer side ------------------------------------------------------

// Read exactly one whole frame. On success writes the header into `hdr_out`,
// copies the body into `body_out` (must hold >= header.size bytes), and returns
// true. Returns false — consuming NOTHING — when no complete frame is ready.
template <typename Pipe>
bool mesh_try_recv(Pipe& p, app_msg_header& hdr_out, byte* body_out, usize body_cap) noexcept {
    constexpr usize k_hdr = sizeof(app_msg_header);

    // (1) A whole header must be present before `size` can be trusted.
    if (p.used_size() < k_hdr) return false;

    // (2) Peek — do NOT consume — to learn the body length.
    app_msg_header hdr{};
    const usize    got = p.peek(reinterpret_cast<byte*>(&hdr), k_hdr);
    LNX_CHECK(got == k_hdr);

    // (3) Verify the ENTIRE frame is present before consuming a byte.
    const usize frame_len = mesh_frame_size(hdr.size);
    if (p.used_size() < frame_len) return false;

    // A frame whose body outruns the caller's buffer is a producer/consumer
    // sizing bug — trap rather than silently truncate.
    LNX_CHECK(hdr.size <= body_cap);

    // (4) Whole frame confirmed — consume header, then body.
    byte scratch[k_hdr];
    p.dequeue(scratch, k_hdr);                    // drop the peeked header
    if (hdr.size) p.dequeue(body_out, hdr.size);

    hdr_out = hdr;
    return true;
}

// ---- concrete mesh edges ------------------------------------------------
// The acceptor->worker edge is the hot admission path (larger); the
// worker->acceptor close-notify edge is lighter.
using acceptor_to_worker_pipe = sds::pipe<64 * 1024>;
using worker_to_acceptor_pipe = sds::pipe<16 * 1024>;

// The admission edge may drop: mesh_post() returning false means the acceptor
// closes the fd and never mints the session. The close-notify edge may NOT.
// A dropped session_closed leaks its authority-map entry for the life of the
// process, and a worker cannot block waiting for room.
//
// What makes it undroppable is a protocol rule, not a bigger pipe: a worker
// posts EXACTLY ONE session_closed per adopted session, and the slot cannot be
// reused until the acceptor has consumed that close (the generation field in
// session_closed_msg is what lets the acceptor discard a stale one). So the
// worst case in flight on one edge is every session closing at once, and that
// worst case has to fit.
//
// It fits today — 256 x 28 B = 7 KiB into 16 KiB — but until this assert it
// fit by coincidence. Shrinking the pipe or raising k_session_capacity would
// have compiled fine and leaked under load.
static_assert(config::k_session_capacity
                      * mesh_frame_size(sizeof(session_closed_msg))
                  <= worker_to_acceptor_pipe::capacity(),
              "close-notify must never drop: one in-flight close per session "
              "must fit the worker->acceptor pipe. Raise the pipe, or lower "
              "config::k_session_capacity.");

}  // namespace app
