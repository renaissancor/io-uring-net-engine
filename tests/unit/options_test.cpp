// tests/unit/options_test.cpp
//
// Layer 1: no I/O, no process spawn — the pattern every future unit
// test (dispatch table, stub decode, proxy encode) follows.

#include <catch2/catch_test_macros.hpp>

#include "options.h"

namespace {

expected<srv::options, std::string_view> parse(
    std::initializer_list<const char*> args) {
    // argv[0] is the program name, skipped by the parser.
    const char* argv[8] = {"iouring_net-server"};
    int         argc    = 1;
    for (const char* a : args) argv[argc++] = a;
    return srv::parse_options(argc, argv);
}

}  // namespace

TEST_CASE("no arguments yields defaults", "[unit][options]") {
    const auto r = parse({});
    REQUIRE(r.has_value());
    CHECK_FALSE(r->dry_run);
    CHECK_FALSE(r->help);
}

// Catch2 test names must not begin with '-': catch_discover_tests
// passes the name as argv and Catch2 would parse it as a CLI flag.
TEST_CASE("flag dry-run sets dry_run only", "[unit][options]") {
    const auto r = parse({"--dry-run"});
    REQUIRE(r.has_value());
    CHECK(r->dry_run);
    CHECK_FALSE(r->help);
}

TEST_CASE("flag help sets help only", "[unit][options]") {
    const auto r = parse({"--help"});
    REQUIRE(r.has_value());
    CHECK(r->help);
    CHECK_FALSE(r->dry_run);
}

TEST_CASE("flags combine", "[unit][options]") {
    const auto r = parse({"--dry-run", "--help"});
    REQUIRE(r.has_value());
    CHECK(r->dry_run);
    CHECK(r->help);
}

TEST_CASE("unknown argument is reported verbatim", "[unit][options]") {
    const auto r = parse({"--dry-run", "--bogus"});
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == "--bogus");
}

TEST_CASE("usage text names every accepted flag", "[unit][options]") {
    CHECK(srv::k_usage.find("--dry-run") != std::string_view::npos);
    CHECK(srv::k_usage.find("--help") != std::string_view::npos);
}
