// tests/app/mesh_test.cpp
//
// Framing + backpressure tests for the mesh layer (app/mesh.h) over a raw
// sds::pipe. The pipe itself is stress-tested in sds/ring_buffer_test.cpp;
// here we prove the WHOLE-FRAME contract framing adds on top: mesh_post
// publishes header+body atomically via enqueue2 (no staging buffer),
// mesh_try_recv only yields a complete frame, an incomplete frame consumes
// nothing, FIFO order holds across message types, and frames that straddle the
// pipe's wrap point round-trip intact.
//
// Single-threaded alternation is a legal spsc use (at most one writer, at most
// one reader — never concurrent here).

#include "mesh.h"
#include "message.h"

#include "runtime/thread.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using app::app_msg_header;
using app::app_msg_type;
using app::adopt_session_msg;
using app::session_closed_msg;

namespace {
using test_pipe = sds::pipe<4096>;
}  // namespace

TEST_CASE("mesh: empty pipe yields nothing", "[app][mesh]") {
    test_pipe p;
    REQUIRE(p.is_empty());

    app_msg_header hdr{};
    byte body[app::k_mesh_body_max];
    REQUIRE_FALSE(app::mesh_try_recv(p, hdr, body, sizeof body));
    REQUIRE(p.is_empty());
}

TEST_CASE("mesh: post/recv round-trips a typed message", "[app][mesh]") {
    test_pipe p;

    adopt_session_msg sent{};
    sent.fd           = 42;
    sent.id           = 7;
    sent.generation   = 3;
    sent.account      = 7;
    sent.initial_room = 0;

    REQUIRE(app::mesh_post_msg(p, app_msg_type::adopt_session, sent));
    REQUIRE_FALSE(p.is_empty());

    app_msg_header hdr{};
    byte body[app::k_mesh_body_max];
    REQUIRE(app::mesh_try_recv(p, hdr, body, sizeof body));
    REQUIRE(hdr.type == static_cast<u16>(app_msg_type::adopt_session));
    REQUIRE(hdr.size == sizeof(adopt_session_msg));

    adopt_session_msg got{};
    std::memcpy(&got, body, sizeof got);
    REQUIRE(got.fd == 42);
    REQUIRE(got.id == 7u);
    REQUIRE(got.generation == 3u);

    REQUIRE(p.is_empty());   // frame fully consumed
}

TEST_CASE("mesh: FIFO order across mixed message types", "[app][mesh]") {
    test_pipe p;

    adopt_session_msg a{};
    a.id = 100;
    session_closed_msg c{};
    c.id     = 200;
    c.reason = 9;

    REQUIRE(app::mesh_post_msg(p, app_msg_type::adopt_session, a));
    REQUIRE(app::mesh_post_msg(p, app_msg_type::session_closed, c));

    app_msg_header hdr{};
    byte body[app::k_mesh_body_max];

    // First out is the adopt.
    REQUIRE(app::mesh_try_recv(p, hdr, body, sizeof body));
    REQUIRE(hdr.type == static_cast<u16>(app_msg_type::adopt_session));
    adopt_session_msg ga{};
    std::memcpy(&ga, body, sizeof ga);
    REQUIRE(ga.id == 100u);

    // Then the close.
    REQUIRE(app::mesh_try_recv(p, hdr, body, sizeof body));
    REQUIRE(hdr.type == static_cast<u16>(app_msg_type::session_closed));
    session_closed_msg gc{};
    std::memcpy(&gc, body, sizeof gc);
    REQUIRE(gc.id == 200u);
    REQUIRE(gc.reason == 9u);

    REQUIRE(p.is_empty());
}

TEST_CASE("mesh: zero-length body round-trips", "[app][mesh]") {
    test_pipe p;
    REQUIRE(app::mesh_post(p, app_msg_type::stop, nullptr, 0));

    app_msg_header hdr{};
    byte body[app::k_mesh_body_max];
    REQUIRE(app::mesh_try_recv(p, hdr, body, sizeof body));
    REQUIRE(hdr.type == static_cast<u16>(app_msg_type::stop));
    REQUIRE(hdr.size == 0);
    REQUIRE(p.is_empty());
}

TEST_CASE("mesh: an incomplete frame consumes nothing", "[app][mesh]") {
    test_pipe p;

    // Write a bare header claiming a 64-byte body that never arrives. The
    // reader must refuse it AND leave every byte in place.
    const app_msg_header partial{64, static_cast<u16>(app_msg_type::adopt_session)};
    REQUIRE(p.enqueue(reinterpret_cast<const byte*>(&partial), sizeof partial)
            == sizeof partial);

    app_msg_header hdr{};
    byte body[app::k_mesh_body_max];
    REQUIRE_FALSE(app::mesh_try_recv(p, hdr, body, sizeof body));
    REQUIRE(p.used_size() == sizeof partial);   // nothing consumed

    // Once the body lands, the same frame reads cleanly.
    byte tail[64]{};
    tail[0] = static_cast<byte>(0xAB);
    REQUIRE(p.enqueue(tail, sizeof tail) == sizeof tail);
    REQUIRE(app::mesh_try_recv(p, hdr, body, sizeof body));
    REQUIRE(hdr.size == 64);
    REQUIRE(body[0] == static_cast<byte>(0xAB));
    REQUIRE(p.is_empty());
}

TEST_CASE("mesh: post fails cleanly when the pipe is full", "[app][mesh]") {
    // Small pipe, repeated posts: fill until one is refused, then drain a frame
    // and confirm a post succeeds again — overflow is backpressure, not UB.
    sds::pipe<1024> p;

    adopt_session_msg m{};
    int posted = 0;
    while (app::mesh_post_msg(p, app_msg_type::adopt_session, m)) {
        m.id = static_cast<u64>(++posted);
    }
    REQUIRE(posted > 0);

    const usize used_when_full = p.used_size();

    app_msg_header hdr{};
    byte body[app::k_mesh_body_max];
    REQUIRE(app::mesh_try_recv(p, hdr, body, sizeof body));          // drain one
    REQUIRE(p.used_size() < used_when_full);
    REQUIRE(app::mesh_post_msg(p, app_msg_type::adopt_session, m));  // room again
}

TEST_CASE("mesh: frames straddling the pipe wrap round-trip intact", "[app][mesh]") {
    // 14-byte frames (4-byte header + 10-byte body) against a 64-byte pipe do
    // not divide evenly, so repeated post/drain marches the cursor across the
    // wrap point and several frames land split across the buffer end. This is
    // the case enqueue2's two-region write has to get right.
    sds::pipe<64> p;

    constexpr u16 k_body = 10;
    for (int i = 0; i < 40; ++i) {
        byte out[k_body];
        for (u16 b = 0; b < k_body; ++b) {
            out[b] = static_cast<byte>(i * 16 + b);
        }
        REQUIRE(app::mesh_post(p, app_msg_type::adopt_session, out, k_body));

        app_msg_header hdr{};
        byte in[k_body];
        REQUIRE(app::mesh_try_recv(p, hdr, in, sizeof in));
        REQUIRE(hdr.size == k_body);
        REQUIRE(hdr.type == static_cast<u16>(app_msg_type::adopt_session));
        REQUIRE(std::memcmp(out, in, k_body) == 0);
        REQUIRE(p.is_empty());
    }
}

TEST_CASE("mesh: two frames in flight across the wrap keep FIFO order",
          "[app][mesh]") {
    // Keep two frames resident so the second is written while the first is
    // still unconsumed — the producer cursor is mid-buffer and the second
    // frame's header/body split across the wrap independently.
    sds::pipe<64> p;

    constexpr u16 k_body = 10;
    byte a[k_body];
    byte b[k_body];

    for (int i = 0; i < 40; ++i) {
        for (u16 k = 0; k < k_body; ++k) {
            a[k] = static_cast<byte>(i + k);
            b[k] = static_cast<byte>(i + k + 100);
        }
        REQUIRE(app::mesh_post(p, app_msg_type::adopt_session, a, k_body));
        REQUIRE(app::mesh_post(p, app_msg_type::session_closed, b, k_body));

        app_msg_header hdr{};
        byte in[k_body];

        REQUIRE(app::mesh_try_recv(p, hdr, in, sizeof in));
        REQUIRE(hdr.type == static_cast<u16>(app_msg_type::adopt_session));
        REQUIRE(std::memcmp(a, in, k_body) == 0);

        REQUIRE(app::mesh_try_recv(p, hdr, in, sizeof in));
        REQUIRE(hdr.type == static_cast<u16>(app_msg_type::session_closed));
        REQUIRE(std::memcmp(b, in, k_body) == 0);

        REQUIRE(p.is_empty());
    }
}

// ---------------------------------------------------------------------------
// Cross-thread torture
// ---------------------------------------------------------------------------
//
// Everything above alternates a single thread between the two roles, which is
// a legal spsc use but proves nothing about ORDERING. mesh frame atomicity
// rests entirely on ring_buffer::enqueue2 publishing the producer cursor once,
// after both the header and the body regions have landed. That claim is
// load-bearing for the whole mesh, and until this case it had no cross-thread
// test at all: sds/ring_buffer_test.cpp's 1M-frame stress exercises the
// single-region enqueue only.
//
// The pipe is deliberately tiny — a few frames of headroom — so the producer
// hits a full pipe constantly and every frame lands at a different offset.
// Bodies straddle the wrap in every possible position over a run this long.
//
// What a failure looks like: if the cursor were published before the body
// bytes were visible, a consumer would read a header whose body is still the
// previous frame's — caught here as a payload mismatch, and caught by TSan as
// the race it is. Run it under `make test PRESET=tsan`; CI does.

namespace {

// Bodies are self-describing: every byte is derived from the sequence number,
// so a torn frame is a content mismatch rather than a silent pass.
constexpr u32 k_torture_frames = 200'000;

u16 torture_body_len(u32 seq) noexcept {
    // 1..64 bytes, coprime stride so the length cycle does not line up with
    // the pipe size and the wrap point walks the whole frame.
    return static_cast<u16>(1 + (seq * 7) % 64);
}

byte torture_byte(u32 seq, u16 i) noexcept {
    return static_cast<byte>((seq * 31u + i * 17u) & 0xFF);
}

app_msg_type torture_type(u32 seq) noexcept {
    return (seq & 1) ? app_msg_type::session_closed : app_msg_type::adopt_session;
}

using torture_pipe = sds::pipe<256>;

struct torture_ctx {
    torture_pipe pipe;
};

void* torture_producer(void* arg) noexcept {
    auto* ctx = static_cast<torture_ctx*>(arg);
    for (u32 seq = 0; seq < k_torture_frames; ++seq) {
        const u16 len = torture_body_len(seq);
        byte      body[64];
        for (u16 i = 0; i < len; ++i) body[i] = torture_byte(seq, i);

        // Spin on backpressure — mesh_post writes nothing when the whole
        // frame does not fit, so a retry is not a partial re-send.
        while (!app::mesh_post(ctx->pipe, torture_type(seq), body, len)) {
            lnx::this_thread::yield();
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("mesh: enqueue2 frames survive a real producer thread intact",
          "[app][mesh][stress]") {
    // 256 B of pipe on the stack; the frames are at most 68 B, so at most
    // three are ever in flight and the producer blocks nearly every write.
    torture_ctx ctx{};

    lnx::thread producer{&torture_producer, &ctx};

    u32 seq = 0;
    while (seq < k_torture_frames) {
        app_msg_header hdr{};
        byte           body[app::k_mesh_body_max];
        if (!app::mesh_try_recv(ctx.pipe, hdr, body, sizeof body)) {
            lnx::this_thread::yield();
            continue;
        }

        const u16 want_len = torture_body_len(seq);

        // REQUIRE per frame would be 200k assertions through Catch2's
        // machinery; compare first and only report on mismatch so the run
        // stays a stress test rather than a benchmark of the test harness.
        bool ok = hdr.size == want_len
               && hdr.type == static_cast<u16>(torture_type(seq));
        for (u16 i = 0; ok && i < want_len; ++i) {
            ok = body[i] == torture_byte(seq, i);
        }
        if (!ok) {
            producer.join();
            INFO("frame " << seq << ": size=" << hdr.size << " want " << want_len
                          << ", type=" << hdr.type);
            REQUIRE(ok);
        }
        ++seq;
    }

    producer.join();
    REQUIRE(seq == k_torture_frames);
    REQUIRE(ctx.pipe.is_empty());
}
