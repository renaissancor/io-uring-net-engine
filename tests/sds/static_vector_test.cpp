// tests/sds/static_vector_test.cpp
//
// Unit tests for sds::static_vector<T, N>. The load-bearing case is a
// non-default-constructible, non-movable, lifetime-tracking element type
// (mirrors app::handle_worker): proves emplace_back constructs exactly the
// requested count and that clear/pop_back/dtor destroy each live element
// exactly once.

#include "sds/static_vector.h"

#include <catch2/catch_test_macros.hpp>

namespace {

// Tracks live instances via a shared counter. Non-default-constructible
// (ctor needs an id), non-copyable, non-movable — exactly the shape that
// rules out std::array and std::vector and forces static_vector.
struct tracked {
    int   id;
    int*  live;

    tracked(int id_, int* live_) noexcept : id(id_), live(live_) { ++*live; }
    ~tracked() noexcept { --*live; }

    tracked(const tracked&)            = delete;
    tracked& operator=(const tracked&) = delete;
    tracked(tracked&&)                 = delete;
    tracked& operator=(tracked&&)      = delete;
};

}  // namespace

TEST_CASE("static_vector: starts empty", "[sds][static_vector]") {
    sds::static_vector<int, 8> v;
    REQUIRE(v.empty());
    REQUIRE_FALSE(v.full());
    REQUIRE(v.size() == 0);
    REQUIRE(v.capacity() == 8);
}

TEST_CASE("static_vector: emplace_back grows size and returns the element",
          "[sds][static_vector]") {
    sds::static_vector<int, 4> v;
    int& a = v.emplace_back(10);
    int& b = v.emplace_back(20);

    REQUIRE(v.size() == 2);
    REQUIRE(a == 10);
    REQUIRE(b == 20);
    REQUIRE(v[0] == 10);
    REQUIRE(v[1] == 20);
    REQUIRE(v.front() == 10);
    REQUIRE(v.back() == 20);
}

TEST_CASE("static_vector: fills to capacity then reports full",
          "[sds][static_vector]") {
    sds::static_vector<int, 3> v;
    v.emplace_back(1);
    v.emplace_back(2);
    v.emplace_back(3);
    REQUIRE(v.full());
    REQUIRE(v.size() == 3);
}

TEST_CASE("static_vector: pop_back destroys only the last element",
          "[sds][static_vector]") {
    int live = 0;
    sds::static_vector<tracked, 4> v;
    v.emplace_back(0, &live);
    v.emplace_back(1, &live);
    v.emplace_back(2, &live);
    REQUIRE(live == 3);

    v.pop_back();
    REQUIRE(live == 2);
    REQUIRE(v.size() == 2);
    REQUIRE(v.back().id == 1);
}

TEST_CASE("static_vector: clear destroys every live element",
          "[sds][static_vector]") {
    int live = 0;
    sds::static_vector<tracked, 8> v;
    for (int i = 0; i < 5; ++i) v.emplace_back(i, &live);
    REQUIRE(live == 5);

    v.clear();
    REQUIRE(live == 0);
    REQUIRE(v.empty());
}

TEST_CASE("static_vector: destructor destroys the live prefix exactly once",
          "[sds][static_vector]") {
    int live = 0;
    {
        sds::static_vector<tracked, 16> v;
        for (int i = 0; i < 10; ++i) v.emplace_back(i, &live);
        REQUIRE(live == 10);
        // Leave the scope with elements still live — dtor must clean up.
    }
    REQUIRE(live == 0);
}

TEST_CASE("static_vector: carries non-movable elements without relocating them",
          "[sds][static_vector]") {
    int live = 0;
    sds::static_vector<tracked, 4> v;
    tracked& first = v.emplace_back(0, &live);
    const tracked* addr = &first;

    // Further emplaces must not relocate earlier elements (no growth/realloc).
    v.emplace_back(1, &live);
    v.emplace_back(2, &live);

    REQUIRE(&v[0] == addr);   // address stability — the reason for inline storage
    REQUIRE(v[0].id == 0);
}

TEST_CASE("static_vector: iterates the live prefix in insertion order",
          "[sds][static_vector]") {
    sds::static_vector<int, 8> v;
    for (int i = 0; i < 5; ++i) v.emplace_back(i * i);

    int expected = 0;
    for (int x : v) {
        REQUIRE(x == expected * expected);
        ++expected;
    }
    REQUIRE(expected == 5);
}
