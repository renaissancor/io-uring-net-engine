// tests/sds/malloc_vector_test.cpp
//
// Unit tests for sds::malloc_vector.

#include "sds/malloc_vector.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

using sds::malloc_vector;

TEST_CASE("malloc_vector: construct empty with initial capacity",
          "[sds][malloc_vector]") {
    malloc_vector<int> values{8};

    REQUIRE(values.empty());
    REQUIRE(values.size() == 0);
    REQUIRE(values.capacity() == 8);
}

TEST_CASE("malloc_vector: push_back grows and preserves values",
          "[sds][malloc_vector]") {
    malloc_vector<int> values{2};

    values.push_back(1);
    values.push_back(2);
    values.push_back(3);

    REQUIRE(values.size() == 3);
    REQUIRE(values.capacity() >= 3);
    REQUIRE(values[0] == 1);
    REQUIRE(values[1] == 2);
    REQUIRE(values[2] == 3);
    REQUIRE(values.front() == 1);
    REQUIRE(values.back() == 3);
}

TEST_CASE("malloc_vector: reserve and resize adjust storage",
          "[sds][malloc_vector]") {
    malloc_vector<int> values{1};

    values.reserve(10);
    REQUIRE(values.capacity() >= 10);
    REQUIRE(values.size() == 0);

    values.resize(4);
    REQUIRE(values.size() == 4);
    REQUIRE(values.capacity() >= 10);

    values.resize(2);
    REQUIRE(values.size() == 2);
}

TEST_CASE("malloc_vector: insert and erase at iterator positions",
          "[sds][malloc_vector]") {
    malloc_vector<int> values{2};
    values.push_back(1);
    values.push_back(3);

    auto pos = values.begin();
    ++pos;
    auto inserted = values.insert(pos, 2);

    REQUIRE(*inserted == 2);
    REQUIRE(values.size() == 3);
    REQUIRE(values[0] == 1);
    REQUIRE(values[1] == 2);
    REQUIRE(values[2] == 3);

    auto next = values.erase(values.find(2));

    REQUIRE(values.size() == 2);
    REQUIRE(*next == 3);
    REQUIRE(values[0] == 1);
    REQUIRE(values[1] == 3);
}

TEST_CASE("malloc_vector: find returns end for missing values",
          "[sds][malloc_vector]") {
    malloc_vector<int> values{4};
    values.push_back(4);
    values.push_back(5);

    REQUIRE(values.find(4) != values.end());
    REQUIRE(values.find(99) == values.end());
}

TEST_CASE("malloc_vector: pop_back and clear only adjust size",
          "[sds][malloc_vector]") {
    malloc_vector<int> values{4};
    values.push_back(1);
    values.push_back(2);
    const std::size_t capacity = values.capacity();

    values.pop_back();
    REQUIRE(values.size() == 1);
    REQUIRE(values.capacity() == capacity);

    values.clear();
    REQUIRE(values.empty());
    REQUIRE(values.capacity() == capacity);
}

TEST_CASE("malloc_vector: const iteration reads values",
          "[sds][malloc_vector]") {
    malloc_vector<int> values{4};
    values.push_back(10);
    values.push_back(20);

    const malloc_vector<int>& const_values = values;
    int sum = 0;
    for (const int value : const_values) {
        sum += value;
    }

    REQUIRE(sum == 30);
}

TEST_CASE("malloc_vector: move transfers malloc storage",
          "[sds][malloc_vector]") {
    malloc_vector<int> values{2};
    values.push_back(1);
    values.push_back(2);

    malloc_vector<int> moved{std::move(values)};

    REQUIRE(moved.size() == 2);
    REQUIRE(moved[0] == 1);
    REQUIRE(moved[1] == 2);
    REQUIRE(values.empty());
    REQUIRE(values.capacity() == 0);

    malloc_vector<int> assigned{1};
    assigned.push_back(99);
    assigned = std::move(moved);

    REQUIRE(assigned.size() == 2);
    REQUIRE(assigned[0] == 1);
    REQUIRE(assigned[1] == 2);
}

TEST_CASE("malloc_vector: stores trivial aggregate records",
          "[sds][malloc_vector]") {
    struct sample {
        int first;
        int second;

        bool operator==(const sample&) const = default;
    };

    static_assert(std::is_trivially_copyable_v<sample>);

    malloc_vector<sample> values{1};
    values.push_back(sample{1, 2});
    values.push_back(sample{3, 4});

    REQUIRE(values.size() == 2);
    REQUIRE(values[0] == sample{1, 2});
    REQUIRE(values[1] == sample{3, 4});
}
