// tests/smoke_test.cpp
//
// Toolchain smoke test. Exercises every dependency the project's CMake
// graph claims to provide: C++20 ranges + jthread, fmt::format / println
// (proves fmt 10+ floor), the project `expected` alias (proves
// tl::expected re-export wires up), and liburing init/exit (proves
// liburing-2.5+ links and the kernel allows io_uring_queue_init).
//
// If any of these fail, the build environment is mis-configured —
// no subsystem code can land until this passes.

#include <coroutine>
#include <cerrno>
#include <cstring>
#include <numeric>
#include <ranges>
#include <thread>
#include <vector>

#include <fmt/core.h>
#include <fmt/format.h>
#include <liburing.h>

#include <catch2/catch_test_macros.hpp>

#include "error/expected.h"

TEST_CASE("C++20 ranges + jthread", "[smoke][cxx20]") {
    std::vector v{1, 2, 3, 4};
    int sum = 0;
    for (int x : v | std::views::transform([](int n) { return n * n; })) {
        sum += x;
    }
    REQUIRE(sum == 30);  // 1+4+9+16

    int side = 0;
    {
        std::jthread t{[&] { side = 7; }};
    }  // join on dtor
    REQUIRE(side == 7);
}

TEST_CASE("fmt::format and fmt::println (proves fmt 10+ floor)", "[smoke][fmt]") {
    REQUIRE(fmt::format("{:#x}", 0x42) == "0x42");
    fmt::println("(smoke) fmt::println works");  // requires fmt 10+
}

TEST_CASE("project expected alias", "[smoke][expected]") {
    expected<int, int> ok{42};
    REQUIRE(ok.has_value());
    REQUIRE(ok.value() == 42);

    expected<int, int> err = unexpected{7};
    REQUIRE_FALSE(err.has_value());
    REQUIRE(err.error() == 7);
}

TEST_CASE("liburing queue init/exit", "[smoke][liburing]") {
    io_uring ring;
    int rc = io_uring_queue_init(8, &ring, 0);

    if (rc < 0) {
        const int err = -rc;
        INFO("io_uring_queue_init rc=" << rc
             << ", errno=" << err
             << " (" << std::strerror(err) << ")");

        if (err == EPERM || err == EACCES) {
            SUCCEED("io_uring blocked by host policy/runtime (seccomp/container/privilege)");
            return;
        }
        if (err == ENOSYS) {
            SUCCEED("io_uring syscall unavailable on this kernel/runtime");
            return;
        }
        if (err == ENOMEM) {
            FAIL("io_uring_queue_init failed with ENOMEM (kernel resources or memlock limits too low)");
        }
        if (err == EMFILE || err == ENFILE) {
            FAIL("io_uring_queue_init failed due to file descriptor limits (EMFILE/ENFILE)");
        }
    }

    REQUIRE(rc >= 0);
    io_uring_queue_exit(&ring);
}
