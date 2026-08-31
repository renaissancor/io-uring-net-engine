// tests/sds/cstr_hash_map_test.cpp
//
// Unit tests for sds::cstr_hash_map.
// Ported from the Windows cstr_hash_map contract with Linux namespace/style.

#include "sds/cstr_hash_map.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using sds::cstr_hash_map;

namespace {

std::size_t djb2(std::string_view key) noexcept {
    std::size_t hash = 5381;
    for (const char ch : key) {
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(ch);
    }
    return hash;
}

}  // namespace

TEST_CASE("cstr_hash_map: construct empty", "[sds][cstr_hash_map]") {
    cstr_hash_map<int> map{4};

    REQUIRE(map.empty());
    REQUIRE(map.size() == 0);
    REQUIRE(map.capacity() == 4);
    REQUIRE(map.load_factor() == 0.0F);
    REQUIRE(map.begin() == map.end());
}

TEST_CASE("cstr_hash_map: insert and find literal keys", "[sds][cstr_hash_map]") {
    cstr_hash_map<int> map{8};

    map.insert("alpha", 1);
    map.insert("beta", 2);
    map.insert("gamma", 3);

    REQUIRE_FALSE(map.empty());
    REQUIRE(map.size() == 3);
    REQUIRE(map.contains("alpha"));
    REQUIRE(map.contains("beta"));
    REQUIRE_FALSE(map.contains("missing"));
    REQUIRE(map.find("alpha").value() == 1);
    REQUIRE(map.find("beta").value() == 2);
    REQUIRE(map.find("gamma").value() == 3);
}

TEST_CASE("cstr_hash_map: insert updates existing key", "[sds][cstr_hash_map]") {
    cstr_hash_map<int> map{8};

    map.insert("alpha", 1);
    map.insert("alpha", 42);

    REQUIRE(map.size() == 1);
    REQUIRE(map.find("alpha").value() == 42);
}

TEST_CASE("cstr_hash_map: operator[] inserts default value and returns reference",
          "[sds][cstr_hash_map]") {
    cstr_hash_map<std::string> map{8};

    REQUIRE(map["created"].empty());
    map["created"] = "value";

    REQUIRE(map.size() == 1);
    REQUIRE(map.find("created").value() == "value");
}

TEST_CASE("cstr_hash_map: erase removes only the selected key", "[sds][cstr_hash_map]") {
    cstr_hash_map<int> map{8};

    map.insert("alpha", 1);
    map.insert("beta", 2);
    map.insert("gamma", 3);

    map.erase("beta");
    map.erase("missing");

    REQUIRE(map.size() == 2);
    REQUIRE_FALSE(map.contains("beta"));
    REQUIRE(map.find("alpha").value() == 1);
    REQUIRE(map.find("gamma").value() == 3);
}

TEST_CASE("cstr_hash_map: clear removes all elements and keeps table reusable",
          "[sds][cstr_hash_map]") {
    cstr_hash_map<int> map{4};

    map.insert("alpha", 1);
    map.insert("beta", 2);
    map.clear();

    REQUIRE(map.empty());
    REQUIRE(map.begin() == map.end());

    map.insert("gamma", 3);
    REQUIRE(map.size() == 1);
    REQUIRE(map.find("gamma").value() == 3);
}

TEST_CASE("cstr_hash_map: rehash preserves existing values", "[sds][cstr_hash_map]") {
    cstr_hash_map<int> map{2};
    const std::array<const char*, 8> keys{
        "key_0", "key_1", "key_2", "key_3", "key_4", "key_5", "key_6", "key_7"};

    for (std::size_t i = 0; i < keys.size(); ++i) {
        map.insert(keys[i], static_cast<int>(i));
    }

    REQUIRE(map.capacity() > 2);
    REQUIRE(map.size() == keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        REQUIRE(map.find(keys[i]).value() == static_cast<int>(i));
    }
}

TEST_CASE("cstr_hash_map: collision chain stores both values", "[sds][cstr_hash_map]") {
    constexpr std::size_t capacity = 4;
    std::string first;
    std::string second;

    for (char a = 'a'; a <= 'z' && second.empty(); ++a) {
        for (char b = 'a'; b <= 'z' && second.empty(); ++b) {
            std::string candidate{a, b};
            if (first.empty()) {
                first = candidate;
                continue;
            }
            if ((djb2(candidate) & (capacity - 1)) == (djb2(first) & (capacity - 1))) {
                second = candidate;
            }
        }
    }

    REQUIRE_FALSE(first.empty());
    REQUIRE_FALSE(second.empty());

    cstr_hash_map<int> map{capacity};
    map.insert(first.c_str(), 1);
    map.insert(second.c_str(), 2);

    REQUIRE(map.size() == 2);
    REQUIRE(map.find(first.c_str()).value() == 1);
    REQUIRE(map.find(second.c_str()).value() == 2);
}

TEST_CASE("cstr_hash_map: content comparison finds equivalent nonliteral key",
          "[sds][cstr_hash_map]") {
    cstr_hash_map<int> map{8};
    char same_text[] = {'a', 'l', 'p', 'h', 'a', '\0'};

    map.insert("alpha", 7);

    REQUIRE(map.find(same_text).value() == 7);
}

TEST_CASE("cstr_hash_map: const lookup and iteration", "[sds][cstr_hash_map]") {
    cstr_hash_map<int> map{8};
    map.insert("alpha", 1);
    map.insert("beta", 2);

    const cstr_hash_map<int>& const_map = map;
    REQUIRE(const_map.find("alpha").value() == 1);
    REQUIRE(const_map.contains("beta"));

    std::vector<std::string_view> keys;
    for (auto it = const_map.begin(); it != const_map.end(); ++it) {
        keys.emplace_back(it.key());
    }

    std::ranges::sort(keys);
    REQUIRE(keys == std::vector<std::string_view>{"alpha", "beta"});
}

TEST_CASE("cstr_hash_map: iterators expose entry-like access",
          "[sds][cstr_hash_map]") {
    cstr_hash_map<int> map{8};
    map.insert("alpha", 1);

    auto it = map.find("alpha");
    REQUIRE(std::string_view((*it).first) == "alpha");
    REQUIRE((*it).second == 1);
    REQUIRE(std::string_view(it->first) == "alpha");
    REQUIRE(it->second == 1);
    REQUIRE(it->value() == 1);

    (*it).second = 11;
    REQUIRE(map.find("alpha").value() == 11);

    it->second = 12;
    REQUIRE(map.find("alpha").value() == 12);

    const cstr_hash_map<int>& const_map = map;
    auto const_it = const_map.find("alpha");
    REQUIRE(std::string_view((*const_it).first) == "alpha");
    REQUIRE((*const_it).second == 12);
    REQUIRE(std::string_view(const_it->first) == "alpha");
    REQUIRE(const_it->second == 12);
    REQUIRE(const_it->value() == 12);
}

TEST_CASE("cstr_hash_map: move transfers ownership", "[sds][cstr_hash_map]") {
    cstr_hash_map<int> map{4};
    map.insert("alpha", 1);
    map.insert("beta", 2);

    cstr_hash_map<int> moved{std::move(map)};

    REQUIRE(moved.size() == 2);
    REQUIRE(moved.find("alpha").value() == 1);
    REQUIRE(moved.find("beta").value() == 2);
    REQUIRE(map.empty());
    REQUIRE_FALSE(map.contains("alpha"));

    map.insert("fresh", 3);
    REQUIRE(map.find("fresh").value() == 3);

    cstr_hash_map<int> assigned{2};
    assigned.insert("old", 99);
    assigned = std::move(moved);

    REQUIRE(assigned.size() == 2);
    REQUIRE_FALSE(assigned.contains("old"));
    REQUIRE(assigned.find("alpha").value() == 1);
    REQUIRE(assigned.find("beta").value() == 2);
}

TEST_CASE("cstr_hash_map: randomized operations match unordered_map oracle",
          "[sds][cstr_hash_map]") {
    constexpr std::size_t key_count = 128;
    std::array<std::string, key_count> key_storage{};
    std::array<const char*, key_count> keys{};

    for (std::size_t i = 0; i < key_count; ++i) {
        key_storage[i] = "prop_key_" + std::to_string(i);
        keys[i] = key_storage[i].c_str();
    }

    cstr_hash_map<int> map{2};
    std::unordered_map<std::string_view, int> oracle;
    std::mt19937 rng{0xC057CAFE};
    std::uniform_int_distribution<std::size_t> key_dist{0, key_count - 1};
    std::uniform_int_distribution<int> op_dist{0, 99};
    std::uniform_int_distribution<int> value_dist{-10000, 10000};

    for (int step = 0; step < 5000; ++step) {
        const std::size_t key_index = key_dist(rng);
        const char* key = keys[key_index];
        const std::string_view key_view{key_storage[key_index]};
        const int op = op_dist(rng);

        if (op < 45) {
            const int value = value_dist(rng);
            map.insert(key, value);
            oracle[key_view] = value;
        } else if (op < 70) {
            const int delta = value_dist(rng) % 23;
            map[key] += delta;
            oracle[key_view] += delta;
        } else if (op < 85) {
            map.erase(key);
            oracle.erase(key_view);
        } else {
            REQUIRE(map.contains(key) == oracle.contains(key_view));
            auto it = map.find(key);
            if (auto expected = oracle.find(key_view); expected != oracle.end()) {
                REQUIRE(it != map.end());
                REQUIRE(it->second == expected->second);
            } else {
                REQUIRE(it == map.end());
            }
        }

        REQUIRE(map.size() == oracle.size());
    }

    std::size_t iterated = 0;
    for (auto it = map.begin(); it != map.end(); ++it) {
        const auto expected = oracle.find(it->first);
        REQUIRE(expected != oracle.end());
        REQUIRE(it->second == expected->second);
        ++iterated;
    }
    REQUIRE(iterated == oracle.size());
}
