# cmake/deps.cmake
#
# Product-side dependency acquisition. Runtime deps (fmt, liburing,
# tl::expected, Threads) arrive transitively through
# iouring_net::iouring_net — the product must NOT find_package them
# itself (docs/03-cmake.md § Anti-patterns). The only dependency this
# file owns is Catch2, which is test-only and deliberately not part of
# the library's install contract.
#
# Pins mirror iouring-net-lib/cmake/deps.cmake so both repos test on
# the same framework version.

include(FetchContent)

set(CATCH2_FLOOR "3.4.0")
set(CATCH2_TAG   "v3.4.0")

find_package(Catch2 ${CATCH2_FLOOR} CONFIG QUIET)
if(Catch2_FOUND)
  message(STATUS "deps: Catch2 ${Catch2_VERSION} via find_package")
else()
  message(STATUS "deps: Catch2 system below floor; vendoring ${CATCH2_TAG}")
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        ${CATCH2_TAG}
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(Catch2)
  # Catch.cmake / catch_discover_tests live in extras/ when FetchContent'd.
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()
