// tests/sds/ring_buffer_test.cpp
//
// Unit + stress tests for the unified sds::ring_buffer<N, Sync>. The single
// policy is the recv/send (one-owner) ring; the spsc policy is the cross-thread
// mesh edge. Deterministic cases run on `single`; the multi-threaded FIFO
// stress runs on `spsc` (ported from the retired spsc_queue suite) and is
// TSan-checked via the tsan preset (see doc/08-test-strategy.md).

#include "sds/ring_buffer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <random>
#include <thread>
#include <vector>

using sds::ring_buffer;
using sds::ring_sync;

namespace {
// Enqueue a C-string's bytes; returns the enqueue() result.
template <typename Ring>
usize put(Ring& rb, const char* s) {
    return rb.enqueue(reinterpret_cast<const byte*>(s), std::strlen(s));
}
}  // namespace

TEST_CASE("ring_buffer: starts empty", "[sds][ring_buffer]") {
    ring_buffer<16, ring_sync::single> rb;
    REQUIRE(rb.is_empty());
    REQUIRE_FALSE(rb.is_full());
    REQUIRE(rb.used_size() == 0);
    REQUIRE(rb.capacity() == 16);
    REQUIRE(rb.free_size() == 16);  // all N usable — no wasted separator slot
}

TEST_CASE("ring_buffer: enqueue/dequeue round-trip", "[sds][ring_buffer]") {
    ring_buffer<16, ring_sync::single> rb;
    REQUIRE(put(rb, "hello") == 5);
    REQUIRE(rb.used_size() == 5);

    byte out[6] = {};
    REQUIRE(rb.dequeue(out, 5) == 5);
    out[5] = '\0';
    REQUIRE(std::string(reinterpret_cast<char*>(out)) == "hello");
    REQUIRE(rb.is_empty());
}

TEST_CASE("ring_buffer: whole N usable, then all-or-nothing refusal (no resize)",
          "[sds][ring_buffer]") {
    ring_buffer<8, ring_sync::single> rb;
    const byte eight[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(rb.enqueue(eight, 8) == 8);   // fills all 8 bytes
    REQUIRE(rb.is_full());
    REQUIRE(rb.free_size() == 0);

    const byte one[1] = {9};
    REQUIRE(rb.enqueue(one, 1) == 0);     // refused — bounded, never grows
    REQUIRE(rb.capacity() == 8);          // capacity fixed at compile time

    byte out[8] = {};
    REQUIRE(rb.dequeue(out, 8) == 8);
    REQUIRE(std::memcmp(out, eight, 8) == 0);
}

TEST_CASE("ring_buffer: a frame larger than N is refused whole",
          "[sds][ring_buffer]") {
    ring_buffer<8, ring_sync::single> rb;
    const byte big[10] = {};
    REQUIRE(rb.enqueue(big, 10) == 0);    // > capacity → nothing enqueued
    REQUIRE(rb.is_empty());
}

TEST_CASE("ring_buffer: enqueue wraps around physical boundary",
          "[sds][ring_buffer]") {
    ring_buffer<8, ring_sync::single> rb;
    REQUIRE(put(rb, "AAAAA") == 5);
    byte buf[8] = {};
    REQUIRE(rb.dequeue(buf, 5) == 5);     // cursor advances to offset 5
    REQUIRE(rb.is_empty());

    REQUIRE(put(rb, "BBBBB") == 5);       // last 2 wrap past the end
    REQUIRE(rb.used_size() == 5);
    byte buf2[8] = {};
    REQUIRE(rb.dequeue(buf2, 5) == 5);
    REQUIRE(std::memcmp(buf2, "BBBBB", 5) == 0);
}

TEST_CASE("ring_buffer: peek does not consume, reassembles across wrap",
          "[sds][ring_buffer]") {
    ring_buffer<8, ring_sync::single> rb;
    REQUIRE(put(rb, "AAAAA") == 5);
    byte drop[5];
    REQUIRE(rb.dequeue(drop, 5) == 5);
    REQUIRE(put(rb, "01234") == 5);       // wraps

    byte peeked[6] = {};
    REQUIRE(rb.peek(peeked, 5) == 5);
    peeked[5] = '\0';
    REQUIRE(std::string(reinterpret_cast<char*>(peeked)) == "01234");
    REQUIRE(rb.used_size() == 5);         // still buffered
}

TEST_CASE("ring_buffer: dequeue caps at used_size", "[sds][ring_buffer]") {
    ring_buffer<16, ring_sync::single> rb;
    REQUIRE(put(rb, "hi") == 2);
    byte out[10] = {};
    REQUIRE(rb.dequeue(out, 10) == 2);    // only 2 available
    REQUIRE(rb.is_empty());
}

TEST_CASE("ring_buffer: zero-size operations are no-ops", "[sds][ring_buffer]") {
    ring_buffer<8, ring_sync::single> rb;
    REQUIRE(put(rb, "abc") == 3);
    const auto used = rb.used_size();

    byte out[4] = {};
    REQUIRE(rb.enqueue(out, 0) == 0);
    REQUIRE(rb.dequeue(out, 0) == 0);
    REQUIRE(rb.peek(out, 0) == 0);
    REQUIRE(rb.used_size() == used);

    REQUIRE(rb.dequeue(out, 3) == 3);
    REQUIRE(std::memcmp(out, "abc", 3) == 0);
}

TEST_CASE("ring_buffer: enqueue2 joins two regions into one frame",
          "[sds][ring_buffer]") {
    ring_buffer<16, ring_sync::single> rb;
    const byte hdr[2]  = {0xAA, 0xBB};
    const byte body[3] = {1, 2, 3};

    REQUIRE(rb.enqueue2(hdr, 2, body, 3) == 5);
    REQUIRE(rb.used_size() == 5);

    byte out[5] = {};
    REQUIRE(rb.dequeue(out, 5) == 5);
    const byte want[5] = {0xAA, 0xBB, 1, 2, 3};   // b lands directly after a
    REQUIRE(std::memcmp(out, want, 5) == 0);
    REQUIRE(rb.is_empty());
}

TEST_CASE("ring_buffer: enqueue2 is all-or-nothing on the PAIR",
          "[sds][ring_buffer]") {
    ring_buffer<8, ring_sync::single> rb;
    const byte hdr[4]  = {1, 2, 3, 4};
    const byte body[6] = {5, 6, 7, 8, 9, 10};

    // Either region alone would fit; together they exceed capacity. Nothing
    // may land — a partial header would strand a frame the reader can't parse.
    REQUIRE(rb.enqueue2(hdr, 4, body, 6) == 0);
    REQUIRE(rb.is_empty());

    // Exactly-capacity pair is accepted.
    REQUIRE(rb.enqueue2(hdr, 4, body, 4) == 8);
    REQUIRE(rb.is_full());
}

TEST_CASE("ring_buffer: enqueue2 empty second region equals plain enqueue",
          "[sds][ring_buffer]") {
    ring_buffer<16, ring_sync::single> rb;
    const byte hdr[3] = {7, 8, 9};

    REQUIRE(rb.enqueue2(hdr, 3, nullptr, 0) == 3);   // null body legal at len 0
    byte out[3] = {};
    REQUIRE(rb.dequeue(out, 3) == 3);
    REQUIRE(std::memcmp(out, hdr, 3) == 0);
}

TEST_CASE("ring_buffer: enqueue2 splits correctly when the wrap falls inside "
          "the FIRST region", "[sds][ring_buffer]") {
    ring_buffer<8, ring_sync::single> rb;

    // March the cursor to offset 7 so a 2-byte header straddles the end.
    const byte filler[7] = {};
    byte       drop[7]   = {};
    REQUIRE(rb.enqueue(filler, 7) == 7);
    REQUIRE(rb.dequeue(drop, 7) == 7);

    const byte hdr[2]  = {0xDE, 0xAD};
    const byte body[4] = {1, 2, 3, 4};
    REQUIRE(rb.enqueue2(hdr, 2, body, 4) == 6);      // hdr spans offsets 7 -> 0

    byte out[6] = {};
    REQUIRE(rb.dequeue(out, 6) == 6);
    const byte want[6] = {0xDE, 0xAD, 1, 2, 3, 4};
    REQUIRE(std::memcmp(out, want, 6) == 0);
    REQUIRE(rb.is_empty());
}

TEST_CASE("ring_buffer: enqueue2 splits correctly when the wrap falls inside "
          "the SECOND region", "[sds][ring_buffer]") {
    ring_buffer<8, ring_sync::single> rb;

    // Cursor to offset 4: the 2-byte header fits flat, the 4-byte body wraps.
    const byte filler[4] = {};
    byte       drop[4]   = {};
    REQUIRE(rb.enqueue(filler, 4) == 4);
    REQUIRE(rb.dequeue(drop, 4) == 4);

    const byte hdr[2]  = {0xBE, 0xEF};
    const byte body[4] = {9, 8, 7, 6};
    REQUIRE(rb.enqueue2(hdr, 2, body, 4) == 6);      // body spans offsets 6,7 -> 0,1

    byte out[6] = {};
    REQUIRE(rb.dequeue(out, 6) == 6);
    const byte want[6] = {0xBE, 0xEF, 9, 8, 7, 6};
    REQUIRE(std::memcmp(out, want, 6) == 0);
    REQUIRE(rb.is_empty());
}

TEST_CASE("ring_buffer: enqueue2 matches a pre-assembled enqueue at every "
          "wrap offset", "[sds][ring_buffer]") {
    // Sweep the producer cursor through every offset so the wrap lands inside
    // the header, inside the body, and exactly between them.
    constexpr usize k_hdr  = 3;
    constexpr usize k_body = 5;

    for (usize start = 0; start < 8; ++start) {
        ring_buffer<8, ring_sync::single> rb;
        const byte filler[8] = {};
        byte       drop[8]   = {};
        if (start) {
            REQUIRE(rb.enqueue(filler, start) == start);
            REQUIRE(rb.dequeue(drop, start) == start);
        }

        byte hdr[k_hdr];
        byte body[k_body];
        for (usize i = 0; i < k_hdr; ++i)  hdr[i]  = static_cast<byte>(0xF0 + i);
        for (usize i = 0; i < k_body; ++i) body[i] = static_cast<byte>(i + 1);

        REQUIRE(rb.enqueue2(hdr, k_hdr, body, k_body) == k_hdr + k_body);

        byte out[k_hdr + k_body] = {};
        REQUIRE(rb.dequeue(out, k_hdr + k_body) == k_hdr + k_body);
        REQUIRE(std::memcmp(out, hdr, k_hdr) == 0);
        REQUIRE(std::memcmp(out + k_hdr, body, k_body) == 0);
        REQUIRE(rb.is_empty());
    }
}

TEST_CASE("ring_buffer: zero-copy enqueue hook (io_uring recv shape)",
          "[sds][ring_buffer]") {
    ring_buffer<8, ring_sync::single> rb;
    REQUIRE(rb.direct_enqueue_size() == 8);          // empty → whole buffer flat

    // Kernel writes 4 bytes into the flat region, we commit them.
    std::memcpy(rb.direct_enqueue_ptr(), "WXYZ", 4);
    rb.commit_enqueue(4);
    REQUIRE(rb.used_size() == 4);
    REQUIRE(rb.direct_enqueue_size() == 4);          // 8-4 free, contiguous to end

    byte out[4] = {};
    REQUIRE(rb.dequeue(out, 4) == 4);
    REQUIRE(std::memcmp(out, "WXYZ", 4) == 0);
}

TEST_CASE("ring_buffer: zero-copy dequeue hook (io_uring send shape)",
          "[sds][ring_buffer]") {
    ring_buffer<8, ring_sync::single> rb;
    REQUIRE(put(rb, "PQRS") == 4);
    REQUIRE(rb.direct_dequeue_size() == 4);
    REQUIRE(std::memcmp(rb.direct_dequeue_ptr(), "PQRS", 4) == 0);
    rb.commit_dequeue(4);                            // kernel sent it
    REQUIRE(rb.is_empty());
}

TEST_CASE("ring_buffer: spsc policy round-trips single-threaded too",
          "[sds][ring_buffer]") {
    ring_buffer<16, ring_sync::spsc> edge;
    REQUIRE(put(edge, "frame") == 5);
    byte out[5] = {};
    REQUIRE(edge.dequeue(out, 5) == 5);
    REQUIRE(std::memcmp(out, "frame", 5) == 0);
}

TEST_CASE("ring_buffer: randomized single-owner ops match deque reference",
          "[sds][ring_buffer]") {
    ring_buffer<8, ring_sync::single> rb;
    std::deque<byte> reference;
    std::mt19937 rng{0xC0FFEE};
    std::uniform_int_distribution<int> op_dist{0, 2};
    std::uniform_int_distribution<int> size_dist{0, 12};

    int next_byte = 0;
    for (int step = 0; step < 2000; ++step) {
        const int op = op_dist(rng);
        const auto request = static_cast<usize>(size_dist(rng));

        if (op == 0) {  // enqueue — all-or-nothing, only when it fits
            std::vector<byte> in(request);
            for (byte& b : in) b = static_cast<byte>('A' + (next_byte++ % 26));
            const usize got = rb.enqueue(in.data(), in.size());
            if (request <= rb.capacity() - reference.size()) {
                REQUIRE(got == request);
                reference.insert(reference.end(), in.begin(), in.end());
            } else {
                REQUIRE(got == 0);  // refused, reference unchanged
            }
        } else if (op == 1) {  // dequeue
            std::vector<byte> out(std::max<usize>(request, 1));
            const auto expected = std::min(request, reference.size());
            REQUIRE(rb.dequeue(out.data(), request) == expected);
            for (usize i = 0; i < expected; ++i) {
                REQUIRE(out[i] == reference.front());
                reference.pop_front();
            }
        } else {  // peek
            std::vector<byte> out(std::max<usize>(request, 1));
            const auto expected = std::min(request, reference.size());
            REQUIRE(rb.peek(out.data(), request) == expected);
            for (usize i = 0; i < expected; ++i) REQUIRE(out[i] == reference[i]);
        }

        REQUIRE(rb.used_size() == reference.size());
        REQUIRE(rb.free_size() == rb.capacity() - rb.used_size());
    }
}

TEST_CASE("ring_buffer: 1M-frame SPSC stress preserves FIFO order",
          "[sds][ring_buffer][stress]") {
    constexpr u32 kTotal = 1'000'000;
    ring_buffer<1024, ring_sync::spsc> edge;  // 256 four-byte frames of headroom

    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        for (u32 i = 0; i < kTotal; ++i) {
            byte f[4];
            std::memcpy(f, &i, 4);
            while (edge.enqueue(f, 4) == 0) { /* spin until consumer drains */ }
        }
        producer_done.store(true, std::memory_order_release);
    });

    u32 expected = 0;
    while (expected < kTotal) {
        byte f[4];
        if (edge.dequeue(f, 4) == 4) {
            u32 value = 0;
            std::memcpy(&value, f, 4);
            REQUIRE(value == expected);
            ++expected;
        }
    }

    producer.join();
    REQUIRE(producer_done.load(std::memory_order_acquire));
    REQUIRE(edge.is_empty());
}
