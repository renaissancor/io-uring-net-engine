cmake_minimum_required(VERSION 3.25)
project(iouring_net
  VERSION   0.0.1
  LANGUAGES CXX
  DESCRIPTION "Linux-native C++20 coroutine network library on io_uring")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Default to Debug if no build type set (CMakePresets.json overrides this).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Compile-commands DB so clangd / clang-tidy / IDEs work out of the box.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(deps)
include(compiler-warnings)

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(iouring_net STATIC
  src/_placeholder.cpp)
add_library(iouring_net::iouring_net ALIAS iouring_net)

target_compile_features(iouring_net PUBLIC cxx_std_20)
target_include_directories(iouring_net PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

target_link_libraries(iouring_net
  PUBLIC
    Threads::Threads
    fmt::fmt
    liburing::uring
    tl::expected)

iouring_net_apply_warnings(iouring_net)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(IOURING_NET_BUILD_TESTS "Build the iouring_net test suite" ON)
if(IOURING_NET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
