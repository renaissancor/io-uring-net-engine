#pragma once
// app/spsc_mailbox.h
//
// Thin framing wrapper over sds::ring_buffer<N, ring_sync::spsc> that turns a
// raw byte SPSC ring into a whole-message mailbox for the thread mesh. One
// producer thread posts; one consumer thread drains (the ring's spsc contract).
//
// Framing:  each frame is [ app_msg_header | body ].  post() assembles the
// header + body into ONE contiguous blob and enqueues it whole — the ring's
// enqueue is all-or-nothing, so a frame is either fully published or not at all.
//
// Parsing rule (handoff "Important parsing rule"):
//   peek header -> verify the WHOLE body is also present -> dequeue the frame.
// Because enqueue is all-or-nothing a partial frame can never be observed
// today, but try_recv() still verifies full availability before consuming a
// single byte, so the mailbox never depends on undocumented writer behavior and
// never strands half a frame in the ring.
//
// Bounded: post() refuses (returns false) when the ring lacks room for the
// whole frame — overflow is a backpressure signal for the caller, never a grow
// trigger. Oversize bodies are refused too (the ring is sized for the max
// frame; see the aliases at the bottom).

#include "message.h"
#include "../check.h"
#include "../sds/ring_buffer.h"
#include "../types.h"

namespace app {

// Largest body any mesh message may carry. Sized well above the current
// structs (adopt_session_msg / session_closed_msg are ~32 B) so the on-stack
// assembly/parse scratch stays a fixed, small cost. Raise only if a new message
// body approaches this — the ring must still exceed one whole frame.
inline constexpr usize k_max_msg_body = 240;

template <usize N>
class spsc_mailbox {
    static constexpr usize k_hdr = sizeof(app_msg_header);
    static_assert(N > k_hdr + k_max_msg_body,
                  "mailbox ring must exceed one whole max-size frame");

    sds::ring_buffer<N, sds::ring_sync::spsc> ring_;

public:
    spsc_mailbox() noexcept = default;

    spsc_mailbox(const spsc_mailbox&)            = delete;
    spsc_mailbox& operator=(const spsc_mailbox&) = delete;
    spsc_mailbox(spsc_mailbox&&)                 = delete;
    spsc_mailbox& operator=(spsc_mailbox&&)      = delete;

    // ---- producer side --------------------------------------------------
    // Post a raw body under `type`. Assembles [header|body] and enqueues it
    // whole. Returns false (enqueues nothing) if the body is oversize or the
    // ring lacks room for the whole frame. body may be null iff body_len == 0.
    bool post(app_msg_type type, const void* body, u16 body_len) noexcept {
        if (body_len > k_max_msg_body) return false;

        byte frame[k_hdr + k_max_msg_body];
        app_msg_header hdr{body_len, static_cast<u16>(type)};
        __builtin_memcpy(frame, &hdr, k_hdr);
        if (body_len) __builtin_memcpy(frame + k_hdr, body, body_len);

        const usize frame_len = k_hdr + body_len;
        return ring_.enqueue(frame, frame_len) == frame_len;
    }

    // Typed convenience: post a POD message struct. The struct must be one of
    // the message.h bodies; T's size must fit k_max_msg_body.
    template <typename T>
    bool post_msg(app_msg_type type, const T& msg) noexcept {
        static_assert(sizeof(T) <= k_max_msg_body, "message body too large");
        return post(type, &msg, static_cast<u16>(sizeof(T)));
    }

    // ---- consumer side --------------------------------------------------
    // Dequeue exactly one whole frame. On success writes the header into
    // `hdr_out`, copies the body into `body_out` (must hold >= header.size
    // bytes), and returns true. Returns false — consuming NOTHING — when no
    // complete frame is available. body_cap guards the caller's buffer.
    bool try_recv(app_msg_header& hdr_out, byte* body_out, usize body_cap) noexcept {
        // (1) A whole header must be present before we can trust `size`.
        if (ring_.used_size() < k_hdr) return false;

        // (2) Peek — do NOT consume — the header to learn the body length.
        app_msg_header hdr{};
        const usize got = ring_.peek(reinterpret_cast<byte*>(&hdr), k_hdr);
        LNX_CHECK(got == k_hdr);

        // (3) Verify the ENTIRE frame is present before consuming a byte.
        const usize frame_len = k_hdr + hdr.size;
        if (ring_.used_size() < frame_len) return false;

        // A frame claiming a body larger than the caller's buffer (or our own
        // max) is a framing/producer bug — trap rather than silently truncate.
        LNX_CHECK(hdr.size <= k_max_msg_body);
        LNX_CHECK(hdr.size <= body_cap);

        // (4) Whole frame confirmed present — now consume header then body.
        byte scratch[k_hdr];
        ring_.dequeue(scratch, k_hdr);            // drop the peeked header
        if (hdr.size) ring_.dequeue(body_out, hdr.size);

        hdr_out = hdr;
        return true;
    }

    // Observers (single-frame liveness checks; exact counts are advisory since
    // the peer may advance its cursor concurrently under spsc).
    bool  empty()     const noexcept { return ring_.is_empty(); }
    usize used_bytes() const noexcept { return ring_.used_size(); }
    usize free_bytes() const noexcept { return ring_.free_size(); }
    static constexpr usize capacity() noexcept { return N; }
};

// Concrete mesh edges (handoff aliases). The acceptor->worker edge is the hot
// admission path (larger); the worker->acceptor close-notify edge is lighter.
using acceptor_to_worker_mailbox = spsc_mailbox<64 * 1024>;
using worker_to_acceptor_mailbox = spsc_mailbox<16 * 1024>;

}  // namespace app
