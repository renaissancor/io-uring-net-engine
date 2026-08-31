cmake_minimum_required(VERSION 3.25)
project(iouring_net
  VERSION   0.0.1
  LANGUAGES CXX
  DESCRIPTION "Linux-native C++20 realtime interaction network engine on io_uring")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
  src/_placeholder.cpp
  src/app/detail/thread_role.cpp
  src/app/acceptor_engine.cpp
  src/app/worker_engine.cpp
  src/app/acceptor_ctl.cpp
  src/app/thread_ctl.cpp
  src/app/worker_ctl.cpp
  src/app/session_table.cpp
  src/diagnostic/profiler_scope.cpp
  src/memory/packet_pool.cpp)
add_library(iouring_net::iouring_net ALIAS iouring_net)

target_compile_features(iouring_net PUBLIC cxx_std_20)

# Thread roster size — COMPILE-TIME, see src/app/roster.h. The roster is fixed
# before runtime by design; this is a build-time knob, not a config field.
# Configure with -DIOURING_NET_WORKER_COUNT=4 to produce a variant binary so a
# worker-count sweep stays cheap to benchmark. PUBLIC so `server` and the test
# suite compile against the same roster as the library.
set(IOURING_NET_WORKER_COUNT "1" CACHE STRING
  "Worker threads in the thread roster (1..8, fixed at compile time)")
target_compile_definitions(iouring_net PUBLIC
  IOURING_NET_WORKER_COUNT=${IOURING_NET_WORKER_COUNT})

# Public API surface — explicit allowlist via FILE_SET HEADERS.
# Every file listed here is installed and reachable from consumers
# via `#include <iouring_net/...>`. Anything NOT listed here is
# private to the library, even if it happens to live in src/.
# This is the v1.1 boundary mechanism (CMake ≥ 3.23 required;
# our floor is 3.25, see CMakeLists.txt:1).
target_sources(iouring_net
  PUBLIC
    FILE_SET iouring_net_public_headers
    TYPE HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
    FILES
      src/types.h
      src/error/expected.h
      src/diagnostic/profiler_scope.h
      src/sds/cstr_hash_map.h
      src/sds/malloc_vector.h
      src/sds/ring_buffer.h
      src/sync/atomic.h)

target_include_directories(iouring_net PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/iouring_net>)

target_link_libraries(iouring_net
  PUBLIC
    Threads::Threads
    fmt::fmt
    liburing::uring
    tl::expected)

iouring_net_apply_warnings(iouring_net)

# ---------------------------------------------------------------------------
# Server executable — supervisor / acceptor / worker runtime entry point
# ---------------------------------------------------------------------------
add_executable(server src/app/main.cpp)
target_link_libraries(server PRIVATE iouring_net::iouring_net)
iouring_net_apply_warnings(server)

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
# install(TARGETS) drives BOTH the archive install and the FILE_SET
# header install. Only headers enumerated in the iouring_net_public_headers
# file-set above ship to the install prefix; any other header in src/ is
# automatically private. New public surface is added by amending the
# FILE_SET FILES list (one line per header), making boundary changes
# reviewable line-by-line. Referenced by doc/09-project-split.md §
# "The contract surface" and iouring-net-server/docs/07-ci.md §
# "Boundary enforcement".
install(TARGETS iouring_net
  EXPORT iouring_netTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  FILE_SET iouring_net_public_headers
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iouring_net)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/iouring_netConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/iouring_netConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iouring_net)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/iouring_netConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(EXPORT iouring_netTargets
  FILE iouring_netTargets.cmake
  NAMESPACE iouring_net::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iouring_net)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/iouring_netConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/iouring_netConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iouring_net)

# ---------------------------------------------------------------------------
# Tests + examples
# ---------------------------------------------------------------------------
option(IOURING_NET_BUILD_TESTS    "Build the iouring_net test suite"       ON)
option(IOURING_NET_BUILD_EXAMPLES "Build the iouring_net example programs" ON)

if(IOURING_NET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(IOURING_NET_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
