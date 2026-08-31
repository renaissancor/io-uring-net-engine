// tests/unit/seam_check_test.cpp
//
// Exercises the installed library through server_core — the in-process
// counterpart of the server_seam_smoke ctest entry. If this fails while
// options_test passes, the break is in the library install, not in
// product code.

#include <catch2/catch_test_macros.hpp>

#include "seam_check.h"

TEST_CASE("installed library round-trips bytes", "[unit][seam]") {
    CHECK(srv::seam_self_check());
}
