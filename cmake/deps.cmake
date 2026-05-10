# cmake/deps.cmake
#
# Single canonical dep list. find_package(... CONFIG QUIET) is a
# fast-path optimization; FetchContent (or ExternalProject for
# liburing's autotools build) is the always-present fallback.
#
# All git tags are pinned. master/main references are not permitted.
# See docs/05-cmake.md for the full design rationale.

include(FetchContent)
include(ExternalProject)
find_package(PkgConfig REQUIRED)
find_package(Threads   REQUIRED)   # exposes Threads::Threads (-pthread)

set(LIBURING_FLOOR "2.5")
set(LIBURING_TAG   "liburing-2.5")
set(FMT_FLOOR      "10")
set(FMT_TAG        "10.2.1")
set(CATCH2_FLOOR   "3.4.0")
set(CATCH2_TAG     "v3.4.0")
set(TL_EXP_TAG     "v1.1.0")

# ---------------------------------------------------------------------------
# liburing — autotools fallback when the system version is below floor
# ---------------------------------------------------------------------------
pkg_check_modules(LIBURING IMPORTED_TARGET GLOBAL liburing>=${LIBURING_FLOOR})
if(LIBURING_FOUND)
  message(STATUS "deps: liburing ${LIBURING_VERSION} via pkg-config")
  if(NOT TARGET liburing::uring)
    add_library(liburing::uring ALIAS PkgConfig::LIBURING)
  endif()
else()
  message(STATUS "deps: liburing system below floor; vendoring ${LIBURING_TAG}")
  set(LIBURING_INSTALL_DIR ${CMAKE_BINARY_DIR}/_deps/liburing-install)
  set(LIBURING_LIB         ${LIBURING_INSTALL_DIR}/lib/liburing.a)
  set(LIBURING_INC         ${LIBURING_INSTALL_DIR}/include)
  ExternalProject_Add(liburing_ep
    GIT_REPOSITORY    https://github.com/axboe/liburing.git
    GIT_TAG           ${LIBURING_TAG}
    GIT_SHALLOW       TRUE
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=${LIBURING_INSTALL_DIR}
    BUILD_COMMAND     make -j
    INSTALL_COMMAND   make install
    BUILD_IN_SOURCE   1
    BUILD_BYPRODUCTS  ${LIBURING_LIB})
  file(MAKE_DIRECTORY ${LIBURING_INC})
  add_library(liburing::uring STATIC IMPORTED GLOBAL)
  add_dependencies(liburing::uring liburing_ep)
  set_target_properties(liburing::uring PROPERTIES
    IMPORTED_LOCATION             ${LIBURING_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${LIBURING_INC})
endif()

# ---------------------------------------------------------------------------
# fmt
# ---------------------------------------------------------------------------
find_package(fmt ${FMT_FLOOR} CONFIG QUIET)
if(fmt_FOUND)
  message(STATUS "deps: fmt ${fmt_VERSION} via find_package")
else()
  message(STATUS "deps: fmt system below floor; vendoring ${FMT_TAG}")
  FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        ${FMT_TAG}
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(fmt)
endif()

# ---------------------------------------------------------------------------
# Catch2
# ---------------------------------------------------------------------------
find_package(Catch2 3 CONFIG QUIET)
if(Catch2_FOUND)
  message(STATUS "deps: Catch2 ${Catch2_VERSION} via find_package")
else()
  message(STATUS "deps: Catch2 system below floor; vendoring ${CATCH2_TAG}")
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        ${CATCH2_TAG}
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(Catch2)
endif()

# ---------------------------------------------------------------------------
# tl::expected — header-only, never available via apt; always fetched
# ---------------------------------------------------------------------------
FetchContent_Declare(tl_expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected.git
  GIT_TAG        ${TL_EXP_TAG}
  GIT_SHALLOW    TRUE)
# tl::expected's CMakeLists builds tests by default; suppress.
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tl_expected)

# ---------------------------------------------------------------------------
# Tag pins exported as a sourceable env file for scripts/emit-snapshot.sh
# ---------------------------------------------------------------------------
configure_file(
  ${CMAKE_SOURCE_DIR}/cmake/deps-pins.env.in
  ${CMAKE_BINARY_DIR}/deps-pins.env @ONLY)
