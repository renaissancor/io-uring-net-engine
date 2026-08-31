#pragma once
// server/options.h
//
// Process argument parsing, split out of main.cpp so it is unit-testable
// (tests/unit/options_test.cpp). Grows with the runtime: --port,
// --workers, --force-shutdown land here alongside the features that
// need them.

#include <string_view>

#include <error/expected.h>

namespace srv {

struct options {
    bool dry_run = false;
    bool help    = false;
};

extern const std::string_view k_usage;

// On failure the error is the offending argument. It views into argv,
// which outlives parsing in every caller.
expected<options, std::string_view> parse_options(int argc,
                                                  const char* const* argv);

}  // namespace srv
