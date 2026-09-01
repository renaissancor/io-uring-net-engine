// server/main.cpp
//
// Thin shell by design: parse → act → exit code. Everything with logic
// in it lives in server_core (options.cpp, seam_check.cpp, later
// lifecycle/dispatch/handlers) so the test binary links it directly —
// same pattern as the library's tests linking the iouring_net target.
//
// v0 is a find_package seam proof, not a server yet. The real entry
// point (the supervisor boot sequence) lands once the library exports
// its runtime/app layer; see docs/08-architecture-pivot.md.

#include <cstdio>

#include <fmt/core.h>

#include "options.h"
#include "seam_check.h"

int main(int argc, char** argv) {
    const auto parsed = srv::parse_options(argc, argv);
    if (!parsed) {
        fmt::print(stderr, "unknown argument: {}\n{}", parsed.error(),
                   srv::k_usage);
        return 2;
    }
    if (parsed->help) {
        fmt::print("{}", srv::k_usage);
        return 0;
    }

    if (!srv::seam_self_check()) {
        fmt::print(stderr, "seam self-check FAILED\n");
        return 1;
    }
    fmt::print("iouring_net-server v0: library seam OK "
               "(find_package(iouring_net) -> sds::ring_buffer round-trip)\n");

    if (!parsed->dry_run) {
        fmt::print("no runtime yet — supervisor boot lands after the library "
                   "exports its app layer (docs/08-architecture-pivot.md)\n");
    }
    return 0;
}
