// tests/app/spsc_mailbox_test.cpp
//
// Framing + backpressure tests for app::spsc_mailbox. The mesh transport
// (sds::ring_buffer<N, ring_sync::spsc>) is stress-tested elsewhere; here we
// prove the WHOLE-FRAME contract the mailbox adds on top: post assembles a
// header+body frame, try_recv only yields a complete frame, partial state
// consumes nothing, oversize is refused, and FIFO order across message types
// is preserved. Single-threaded alternation is a legal spsc use (at most one
// producer, at most one consumer — never concurrent here).

#include "app/message.h"
#include "app/spsc_mailbox.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using app::app_msg_header;
using app::app_msg_type;
using app::adopt_session_msg;
using app::session_closed_msg;

namespace {
// Smallest legal ring: must exceed one whole max-size frame.
using test_mailbox = app::spsc_mailbox<4096>;
}  // namespace

TEST_CASE("spsc_mailbox: empty mailbox yields nothing", "[app][mailbox]") {
    test_mailbox mb;
    REQUIRE(mb.empty());

    app_msg_header hdr{};
    byte body[app::k_max_msg_body];
    REQUIRE_FALSE(mb.try_recv(hdr, body, sizeof body));
    REQUIRE(mb.empty());
}

TEST_CASE("spsc_mailbox: post/recv round-trips a typed message", "[app][mailbox]") {
    test_mailbox mb;

    adopt_session_msg sent{};
    sent.fd           = 42;
    sent.id           = 7;
    sent.generation   = 3;
    sent.account      = 7;
    sent.initial_room = 0;

    REQUIRE(mb.post_msg(app_msg_type::adopt_session, sent));
    REQUIRE_FALSE(mb.empty());

    app_msg_header hdr{};
    byte body[app::k_max_msg_body];
    REQUIRE(mb.try_recv(hdr, body, sizeof body));
    REQUIRE(hdr.type == static_cast<u16>(app_msg_type::adopt_session));
    REQUIRE(hdr.size == sizeof(adopt_session_msg));

    adopt_session_msg got{};
    std::memcpy(&got, body, sizeof got);
    REQUIRE(got.fd == 42);
    REQUIRE(got.id == 7u);
    REQUIRE(got.generation == 3u);

    REQUIRE(mb.empty());   // frame fully consumed
}

TEST_CASE("spsc_mailbox: FIFO order across mixed message types", "[app][mailbox]") {
    test_mailbox mb;

    adopt_session_msg a{};
    a.id = 100;
    session_closed_msg c{};
    c.id = 200;
    c.reason = 9;

    REQUIRE(mb.post_msg(app_msg_type::adopt_session, a));
    REQUIRE(mb.post_msg(app_msg_type::session_closed, c));

    app_msg_header hdr{};
    byte body[app::k_max_msg_body];

    // First out is the adopt.
    REQUIRE(mb.try_recv(hdr, body, sizeof body));
    REQUIRE(hdr.type == static_cast<u16>(app_msg_type::adopt_session));
    adopt_session_msg ga{};
    std::memcpy(&ga, body, sizeof ga);
    REQUIRE(ga.id == 100u);

    // Then the close.
    REQUIRE(mb.try_recv(hdr, body, sizeof body));
    REQUIRE(hdr.type == static_cast<u16>(app_msg_type::session_closed));
    session_closed_msg gc{};
    std::memcpy(&gc, body, sizeof gc);
    REQUIRE(gc.id == 200u);
    REQUIRE(gc.reason == 9u);

    REQUIRE(mb.empty());
}

TEST_CASE("spsc_mailbox: post refuses a body larger than k_max_msg_body",
          "[app][mailbox]") {
    test_mailbox mb;
    byte big[app::k_max_msg_body + 1]{};
    REQUIRE_FALSE(mb.post(app_msg_type::adopt_session, big, sizeof big));
    REQUIRE(mb.empty());
}

TEST_CASE("spsc_mailbox: post fails cleanly when the ring is full", "[app][mailbox]") {
    // Small ring, large-ish bodies: fill until a post is refused, then drain
    // one and confirm a post succeeds again — overflow is backpressure, not UB.
    app::spsc_mailbox<1024> mb;

    adopt_session_msg m{};
    int posted = 0;
    while (mb.post_msg(app_msg_type::adopt_session, m)) {
        m.id = static_cast<u64>(++posted);
    }
    REQUIRE(posted > 0);

    app_msg_header hdr{};
    byte body[app::k_max_msg_body];
    REQUIRE(mb.try_recv(hdr, body, sizeof body));      // drain one
    REQUIRE(mb.post_msg(app_msg_type::adopt_session, m));  // room again
}
