// tests/sds/ring_buffer_test.cpp
//
// Unit tests for sds::ring_buffer.
// Direct port from WindowsLibrary; mirrors enqueue/dequeue/peek semantics
// with snake_case API.

#include "sds/ring_buffer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

using sds::ring_buffer;

TEST_CASE("ring_buffer: construct empty", "[sds][ring_buffer]") {
    ring_buffer rb{16};
    REQUIRE(rb.is_empty());
    REQUIRE_FALSE(rb.is_full());
    REQUIRE(rb.used_size() == 0);
    REQUIRE(rb.capacity() == 16);
    REQUIRE(rb.free_size() == 15);  // capacity - 1 (separator)
}

TEST_CASE("ring_buffer: capacity floor of 4", "[sds][ring_buffer]") {
    ring_buffer rb{1};
    REQUIRE(rb.capacity() == 4);
}

TEST_CASE("ring_buffer: enqueue/dequeue round-trip", "[sds][ring_buffer]") {
    ring_buffer rb{16};
    const char* src = "hello";
    REQUIRE(rb.enqueue(src, 5) == 5);
    REQUIRE(rb.used_size() == 5);

    char out[6] = {};
    REQUIRE(rb.dequeue(out, 5) == 5);
    out[5] = '\0';
    REQUIRE(std::string(out) == "hello");
    REQUIRE(rb.is_empty());
}

TEST_CASE("ring_buffer: enqueue wraps around physical boundary",
          "[sds][ring_buffer]") {
    ring_buffer rb{8};  // capacity 8, usable 7

    // Push 5, drain 5 — head advances to position 5.
    const char* a = "AAAAA";
    REQUIRE(rb.enqueue(a, 5) == 5);
    char buf[8] = {};
    REQUIRE(rb.dequeue(buf, 5) == 5);
    REQUIRE(std::memcmp(buf, "AAAAA", 5) == 0);
    REQUIRE(rb.is_empty());

    // Push 5 more — last 2 wrap (positions 5,6,7,0,1).
    const char* b = "BBBBB";
    REQUIRE(rb.enqueue(b, 5) == 5);
    REQUIRE(rb.used_size() == 5);

    char buf2[8] = {};
    REQUIRE(rb.dequeue(buf2, 5) == 5);
    REQUIRE(std::memcmp(buf2, "BBBBB", 5) == 0);
}

TEST_CASE("ring_buffer: peek does not consume", "[sds][ring_buffer]") {
    ring_buffer rb{16};
    REQUIRE(rb.enqueue("XYZ", 3) == 3);

    char peeked[4] = {};
    REQUIRE(rb.peek(peeked, 3) == 3);
    peeked[3] = '\0';
    REQUIRE(std::string(peeked) == "XYZ");
    REQUIRE(rb.used_size() == 3);  // still buffered
}

TEST_CASE("ring_buffer: peek across wrap boundary returns reassembled bytes",
          "[sds][ring_buffer]") {
    ring_buffer rb{8};
    REQUIRE(rb.enqueue("AAAAA", 5) == 5);
    char drop[5];
    REQUIRE(rb.dequeue(drop, 5) == 5);

    // Now write 5 bytes that wrap.
    REQUIRE(rb.enqueue("01234", 5) == 5);

    char peeked[6] = {};
    REQUIRE(rb.peek(peeked, 5) == 5);
    peeked[5] = '\0';
    REQUIRE(std::string(peeked) == "01234");
}

TEST_CASE("ring_buffer: auto-resize on enqueue overflow", "[sds][ring_buffer]") {
    ring_buffer rb{4};
    const char* big = "ABCDEFGHIJ";  // 10 bytes, exceeds capacity
    REQUIRE(rb.enqueue(big, 10) == 10);
    REQUIRE(rb.capacity() >= 11);  // must fit 10 + 1 separator
    REQUIRE(rb.used_size() == 10);

    char out[11] = {};
    REQUIRE(rb.dequeue(out, 10) == 10);
    out[10] = '\0';
    REQUIRE(std::string(out) == "ABCDEFGHIJ");
}

TEST_CASE("ring_buffer: dequeue caps at used_size", "[sds][ring_buffer]") {
    ring_buffer rb{16};
    REQUIRE(rb.enqueue("hi", 2) == 2);
    char out[10] = {};
    REQUIRE(rb.dequeue(out, 10) == 2);  // only 2 bytes available
    REQUIRE(rb.is_empty());
}

TEST_CASE("ring_buffer: zero-size operations are no-ops",
          "[sds][ring_buffer]") {
    ring_buffer rb{8};
    REQUIRE(rb.enqueue("abc", 3) == 3);

    const auto capacity = rb.capacity();
    const auto head     = rb.head_index();
    const auto tail     = rb.tail_index();
    const auto used     = rb.used_size();

    char out[4] = {};
    REQUIRE(rb.enqueue("", 0) == 0);
    REQUIRE(rb.dequeue(out, 0) == 0);
    REQUIRE(rb.peek(out, 0) == 0);

    REQUIRE(rb.capacity() == capacity);
    REQUIRE(rb.head_index() == head);
    REQUIRE(rb.tail_index() == tail);
    REQUIRE(rb.used_size() == used);

    REQUIRE(rb.dequeue(out, 3) == 3);
    REQUIRE(std::memcmp(out, "abc", 3) == 0);
}

TEST_CASE("ring_buffer: direct_enqueue_size respects wrap", "[sds][ring_buffer]") {
    ring_buffer rb{8};
    REQUIRE(rb.direct_enqueue_size() == 7);  // tail==head==0, head!=0 not triggered: 8-0-(0==0?1:0)=7

    REQUIRE(rb.enqueue("AAAA", 4) == 4);
    // tail=4, head=0 → direct_enqueue_size = (8-4) - 1 = 3
    REQUIRE(rb.direct_enqueue_size() == 3);
}

TEST_CASE("ring_buffer: move_head / move_tail advance cursors",
          "[sds][ring_buffer]") {
    ring_buffer rb{8};
    REQUIRE(rb.head_index() == 0);
    REQUIRE(rb.tail_index() == 0);
    REQUIRE(rb.move_tail(3) == 3);
    REQUIRE(rb.tail_index() == 3);
    REQUIRE(rb.move_head(2) == 2);
    REQUIRE(rb.head_index() == 2);
    REQUIRE(rb.used_size() == 1);
}

TEST_CASE("ring_buffer: randomized operations match deque reference",
          "[sds][ring_buffer]") {
    ring_buffer rb{8};
    std::deque<char> reference;
    std::mt19937 rng{0xC0FFEE};
    std::uniform_int_distribution<int> op_dist{0, 2};
    std::uniform_int_distribution<int> size_dist{0, 64};

    int next_byte = 0;

    for (int step = 0; step < 1000; ++step) {
        const int op = op_dist(rng);
        const auto request = static_cast<std::size_t>(size_dist(rng));

        if (op == 0) {
            std::vector<char> input(request);
            for (char& byte : input) {
                byte = static_cast<char>('A' + (next_byte++ % 26));
            }

            REQUIRE(rb.enqueue(input.data(), input.size()) == input.size());
            reference.insert(reference.end(), input.begin(), input.end());
        } else if (op == 1) {
            std::vector<char> output(std::max<std::size_t>(request, 1));
            const auto expected = std::min(request, reference.size());

            REQUIRE(rb.dequeue(output.data(), request) == expected);
            for (std::size_t i = 0; i < expected; ++i) {
                REQUIRE(output[i] == reference.front());
                reference.pop_front();
            }
        } else {
            std::vector<char> output(std::max<std::size_t>(request, 1));
            const auto expected = std::min(request, reference.size());

            REQUIRE(rb.peek(output.data(), request) == expected);
            for (std::size_t i = 0; i < expected; ++i) {
                REQUIRE(output[i] == reference[i]);
            }
        }

        REQUIRE(rb.used_size() == reference.size());
        REQUIRE(rb.free_size() == rb.capacity() - rb.used_size() - 1);
    }
}
