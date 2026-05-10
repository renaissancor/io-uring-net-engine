# 05 — CMake

How CMake is configured: target shape, warning set, sanitizer presets,
and the canonical `cmake/deps.cmake` dependency-resolution pattern.

For the *what* (language standard, kernel requirements, deps versions),
see `02-build-and-toolchain.md`. For the *how* of installing on a fresh
box, see `06-system-setup.md`.

---

## Target shape

- One `add_library(iouring_net STATIC ...)` target plus per-test
  executables. **Do not** create one library per layer — keeps ABI
  surface visible and preserves single-include consumer ergonomics.
- Optional shared variant via a CMake option:
  `IOURING_NET_BUILD_SHARED=OFF` (default OFF).
- Language standard expressed via:
  `target_compile_features(iouring_net PUBLIC cxx_std_20)`.

---

## Warning set

Lives in `cmake/compiler-warnings.cmake`, applied to every project
target via `target_compile_options`:

```
-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
-Wcast-align -Woverloaded-virtual -Wconversion -Wsign-conversion
-Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
-Werror
```

Ratcheted in. Not all enabled at v0 — turn on incrementally as code
matures.

---

## Sanitizer presets (`CMakePresets.json`)

| Preset    | Flags                                            | Use                                   |
|-----------|--------------------------------------------------|---------------------------------------|
| `default` | `-O0 -g -fsanitize=address,undefined`            | dev default                           |
| `tsan`    | `-O1 -g -fsanitize=thread`                       | mandatory for any concurrency PR      |
| `release` | `-O2 -DNDEBUG -fuse-ld=lld`                      | release/perf builds                   |

ASan and TSan **cannot** combine in the same binary. Run two CI jobs.
See `07-ci-and-reproducibility.md` for the matrix.

---

## Dependency resolution (`cmake/deps.cmake`)

`find_package(... CONFIG QUIET)` is a fast-path optimization, not a
contract. A single `cmake/deps.cmake` owns version floors and tag pins;
`FetchContent` blocks are always present and locked to git tags.
**`master` / `main` references are not permitted.**

`liburing` is autotools, not CMake — `FetchContent_MakeAvailable` will
not work. Use `ExternalProject_Add` to run `configure && make`, then
expose an `IMPORTED` target.

```cmake
# cmake/deps.cmake — single canonical dep list
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

# liburing — autotools fallback when the system version is below floor
pkg_check_modules(LIBURING IMPORTED_TARGET liburing>=${LIBURING_FLOOR})
if(NOT LIBURING_FOUND)
  set(LIBURING_INSTALL_DIR ${CMAKE_BINARY_DIR}/_deps/liburing-install)
  set(LIBURING_LIB         ${LIBURING_INSTALL_DIR}/lib/liburing.a)
  set(LIBURING_INC         ${LIBURING_INSTALL_DIR}/include)
  ExternalProject_Add(liburing_ep
    GIT_REPOSITORY https://github.com/axboe/liburing.git
    GIT_TAG ${LIBURING_TAG}
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=${LIBURING_INSTALL_DIR}
    BUILD_COMMAND     make -j
    INSTALL_COMMAND   make install
    BUILD_IN_SOURCE   1
    BUILD_BYPRODUCTS  ${LIBURING_LIB})         # required for Ninja
  file(MAKE_DIRECTORY ${LIBURING_INC})         # so INTERFACE dir exists at config time
  add_library(liburing::uring STATIC IMPORTED GLOBAL)
  add_dependencies(liburing::uring liburing_ep)
  set_target_properties(liburing::uring PROPERTIES
    IMPORTED_LOCATION             ${LIBURING_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${LIBURING_INC})
endif()

# fmt / Catch2 / tl::expected — CMake-native FetchContent
find_package(fmt ${FMT_FLOOR} CONFIG QUIET)
if(NOT fmt_FOUND)
  FetchContent_Declare(fmt GIT_REPOSITORY https://github.com/fmtlib/fmt.git GIT_TAG ${FMT_TAG})
  FetchContent_MakeAvailable(fmt)
endif()
find_package(Catch2 3 CONFIG QUIET)
if(NOT Catch2_FOUND)
  FetchContent_Declare(Catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG ${CATCH2_TAG})
  FetchContent_MakeAvailable(Catch2)
endif()
FetchContent_Declare(tl_expected GIT_REPOSITORY https://github.com/TartanLlama/expected.git GIT_TAG ${TL_EXP_TAG})
FetchContent_MakeAvailable(tl_expected)

# Tag pins exported as a sourceable env file for scripts/emit-snapshot.sh
configure_file(
  ${CMAKE_SOURCE_DIR}/cmake/deps-pins.env.in
  ${CMAKE_BINARY_DIR}/deps-pins.env @ONLY)
# deps-pins.env.in contents:
#   LIBURING_TAG=@LIBURING_TAG@
#   FMT_TAG=@FMT_TAG@
#   CATCH2_TAG=@CATCH2_TAG@
#   TL_EXP_TAG=@TL_EXP_TAG@
```

---

## Project type aliases

The project surface uses `expected<T, E>` (not `std::expected` / not
`tl::expected`). The alias header `src/error/expected.h` re-exports the
polyfill — see `04-coding-style.md` § "Project type aliases" for the
binding declaration.

---

## Install layout

CMake copies `src/**/*.h` to `<prefix>/include/iouring_net/`,
preserving relative paths. External consumers write:

```cpp
#include <iouring_net/data_structure/ring_buffer.h>
```

No separate `include/` directory inside the source tree — see
`02-build-and-toolchain.md` § "Repository layout" for the full
in-tree path convention.

---

## No package manager

`find_package` and `FetchContent` only. No `vcpkg`, no `conan`. The
point of the project is to demonstrate Linux systems programming, not
a build orchestrator.
