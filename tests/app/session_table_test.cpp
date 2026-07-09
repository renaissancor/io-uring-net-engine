// tests/app/session_table_test.cpp
//
// SessionManager authority-table behavior: id minting, generation guard,
// state/owner transitions, removal + slot reuse, and full-table refusal. This
// is the acceptor-side truth map; it holds no fd or ring state.

#include "app/config.h"
#include "app/session_table.h"

#include <catch2/catch_test_macros.hpp>

using app::session_state;
using app::session_table;

TEST_CASE("session_table: allocate mints unique ids and starts accepted",
          "[app][session_table]") {
    session_table t;
    REQUIRE(t.count() == 0);

    app::session_record* a = t.allocate(/*account=*/1);
    app::session_record* b = t.allocate(/*account=*/2);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->id != b->id);
    REQUIRE(a->id != app::k_invalid_session);
    REQUIRE(a->state == session_state::accepted);
    REQUIRE(a->owner_worker == -1);
    REQUIRE(t.count() == 2);
}

TEST_CASE("session_table: find locates live records, misses absent ids",
          "[app][session_table]") {
    session_table t;
    app::session_record* a = t.allocate(1);
    const app::session_id id = a->id;

    REQUIRE(t.find(id) == a);
    REQUIRE(t.find(app::k_invalid_session) == nullptr);
    REQUIRE(t.find(id + 999) == nullptr);
}

TEST_CASE("session_table: generation guard rejects stale references",
          "[app][session_table]") {
    session_table t;
    app::session_record* a = t.allocate(1);
    const app::session_id id  = a->id;
    const u32             gen = a->generation;

    REQUIRE(t.validate(id, gen));
    REQUIRE_FALSE(t.validate(id, gen + 1));       // wrong generation
    REQUIRE_FALSE(t.validate(app::k_invalid_session, 0));

    // Remove then reallocate: the freed slot comes back with a bumped
    // generation, so a message carrying the OLD generation is rejected.
    REQUIRE(t.remove(id));
    REQUIRE_FALSE(t.validate(id, gen));           // id gone entirely
    app::session_record* b = t.allocate(2);
    REQUIRE(b == a);                              // same slot reused
    REQUIRE(b->generation == gen + 1);            // generation advanced
    REQUIRE_FALSE(t.validate(b->id, gen));        // stale gen refused
    REQUIRE(t.validate(b->id, gen + 1));
}

TEST_CASE("session_table: state + owner transitions", "[app][session_table]") {
    session_table t;
    app::session_record* a = t.allocate(1);
    const app::session_id id = a->id;

    REQUIRE(t.mark_assigning(id, /*worker=*/0));
    REQUIRE(a->state == session_state::assigning);
    REQUIRE(a->owner_worker == 0);

    REQUIRE(t.mark_in_world(id));
    REQUIRE(a->state == session_state::in_world);

    REQUIRE_FALSE(t.mark_assigning(id + 123, 0));  // absent id
}

TEST_CASE("session_table: remove frees the slot and decrements count",
          "[app][session_table]") {
    session_table t;
    app::session_record* a = t.allocate(1);
    const app::session_id id = a->id;
    REQUIRE(t.count() == 1);

    REQUIRE(t.remove(id));
    REQUIRE(t.count() == 0);
    REQUIRE(t.find(id) == nullptr);
    REQUIRE_FALSE(t.remove(id));   // double-remove is a clean false
}

TEST_CASE("session_table: allocate returns nullptr when full", "[app][session_table]") {
    session_table t;
    u32 n = 0;
    while (t.allocate(1) != nullptr) ++n;
    REQUIRE(n == app::config::k_max_sessions);
    REQUIRE(t.count() == app::config::k_max_sessions);
    REQUIRE(t.allocate(1) == nullptr);   // stays refused
}
