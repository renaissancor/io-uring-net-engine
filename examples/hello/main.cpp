// examples/hello/main.cpp
//
// Smallest possible consumer of iouring_net::iouring_net. Proves the
// install/export surface compiles and links from a target outside the
// library, and exercises the project `expected<T, E>` alias plus the
// {fmt} println used throughout the codebase.

#include <fmt/core.h>

#include "error/expected.h"

namespace {

expected<int, const char*> doubled(int x) {
    if (x < 0) {
        return unexpected<const char*>{"input was negative"};
    }
    return x * 2;
}

}  // namespace

int main() {
    fmt::println("hello from iouring-net-lib");

    if (auto r = doubled(21); r.has_value()) {
        fmt::println("expected<int, const char*>: ok({})", r.value());
    } else {
        fmt::println("expected<int, const char*>: err({})", r.error());
    }

    return 0;
}
