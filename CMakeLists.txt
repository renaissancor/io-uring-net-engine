cmake_minimum_required(VERSION 3.25)
project(iouring_net
  VERSION   0.0.1
  LANGUAGES CXX
  DESCRIPTION "Linux-native C++20 coroutine network library on io_uring")

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
  src/_placeholder.cpp)
add_library(iouring_net::iouring_net ALIAS iouring_net)

target_compile_features(iouring_net PUBLIC cxx_std_20)
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
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS iouring_net
  EXPORT iouring_netTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iouring_net)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iouring_net
  FILES_MATCHING
  PATTERN "*.h"
  PATTERN "*.hpp")

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
