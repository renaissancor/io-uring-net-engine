cmake_minimum_required(VERSION 3.25)
project(iouring_net_server
  VERSION   0.0.1
  LANGUAGES CXX
  DESCRIPTION "Game-server product on top of iouring_net")

include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Default to Debug if no build type set (CMakePresets.json overrides this).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Consume the library — the ONLY way library code enters this repo.
# Resolved via CMAKE_PREFIX_PATH (presets point at $HOME/.local; see
# iouring-net-lib/docs/09-project-split.md § "The seam").
find_package(iouring_net 0.0.1 REQUIRED)

include(compiler-warnings)

# Codegen pipeline (proto/packets.json → generated stubs) is NOT wired
# yet — it lands with the packet-framing milestone. See
# docs/08-architecture-pivot.md § "Milestones". When it lands it is
# include(codegen) here, per docs/03-cmake.md.

add_subdirectory(server)

option(IOURING_SERVER_BUILD_TESTS "Build the test suite" ON)
if(IOURING_SERVER_BUILD_TESTS)
  include(deps)   # Catch2 only — runtime deps propagate via iouring_net
  enable_testing()
  add_subdirectory(tests)
endif()
