cmake_minimum_required(VERSION 3.25)
project(uring_server
  VERSION   0.0.1
  LANGUAGES CXX
  DESCRIPTION "Realtime chat server built on the iouring_net engine")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Default to Debug if no build type set.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(compiler-warnings)

# ---------------------------------------------------------------------------
# The engine — consumed ONLY through an install prefix (find_package), never
# a relative include into ../engine-uring/src. This is the product/library
# boundary from design-notes/09-project-split.md § "The contract surface":
# if a header is missing here, the fix is amending the engine's public
# FILE_SET, not reaching around it.
#
#   cmake -S engine-uring -B build/engine && cmake --build build/engine
#   cmake --install build/engine --prefix <prefix>
#   cmake -S server-uring -B build/server -DCMAKE_PREFIX_PATH=<prefix>
# ---------------------------------------------------------------------------
find_package(iouring_net 0.0.1 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Core library — everything except main(), so the test suite links the
# exact objects the server ships.
# ---------------------------------------------------------------------------
add_library(uring_server_core STATIC
  src/detail/thread_role.cpp
  src/acceptor_engine.cpp
  src/worker_engine.cpp
  src/acceptor_ctl.cpp
  src/thread_ctl.cpp
  src/worker_ctl.cpp
  src/session_table.cpp)

target_compile_features(uring_server_core PUBLIC cxx_std_20)
target_include_directories(uring_server_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(uring_server_core PUBLIC iouring_net::iouring_net)
iouring_net_apply_warnings(uring_server_core)

# Thread roster size — COMPILE-TIME, see src/roster.h. The roster is fixed
# before runtime by design; this is a build-time knob, not a config field.
# Configure with -DIOURING_NET_WORKER_COUNT=4 to produce a variant binary so
# a worker-count sweep stays cheap to benchmark. PUBLIC so the server binary
# and the test suite compile against the same roster.
set(IOURING_NET_WORKER_COUNT "1" CACHE STRING
  "Worker threads in the thread roster (1..8, fixed at compile time)")
target_compile_definitions(uring_server_core PUBLIC
  IOURING_NET_WORKER_COUNT=${IOURING_NET_WORKER_COUNT})

# ---------------------------------------------------------------------------
# Server executable — supervisor / acceptor / worker runtime entry point
# ---------------------------------------------------------------------------
add_executable(uring-server src/main.cpp)
target_link_libraries(uring-server PRIVATE uring_server_core)
iouring_net_apply_warnings(uring-server)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(URING_SERVER_BUILD_TESTS "Build the uring-server test suite" ON)

if(URING_SERVER_BUILD_TESTS)
  enable_testing()

  find_package(Catch2 3.4.0 CONFIG QUIET)
  if(Catch2_FOUND)
    message(STATUS "deps: Catch2 ${Catch2_VERSION} via find_package")
  else()
    message(STATUS "deps: Catch2 system below floor; vendoring v3.4.0")
    include(FetchContent)
    FetchContent_Declare(Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG        v3.4.0
      GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  endif()
  include(Catch)

  add_executable(uring_server-test
    tests/worker_ctl_skeleton_test.cpp
    tests/session_table_test.cpp
    tests/mesh_test.cpp)
  target_link_libraries(uring_server-test PRIVATE
    uring_server_core
    Catch2::Catch2WithMain)
  iouring_net_apply_warnings(uring_server-test)

  # PRE_TEST discovery: same TSan-on-noble rationale as the engine suite —
  # see engine-uring/doc/08-test-strategy.md § "TSan on Ubuntu 24.04".
  catch_discover_tests(uring_server-test
    DISCOVERY_MODE PRE_TEST
    EXTRA_ARGS --allow-running-no-tests)
endif()
