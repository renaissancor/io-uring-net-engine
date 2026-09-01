# 03 — CMake

Build-system conventions for `iouring-net-server`. Two product-specific
concerns that the library does not face: **consuming an installed
library via `find_package`** and **wiring a pre-build codegen step**.
Everything else (warnings, presets, sanitizer matrix) mirrors the
library; see [`iouring-net-lib/docs/05-cmake.md`](../../../engine-uring/doc/05-cmake.md).

---

## Top-level shape

```cmake
cmake_minimum_required(VERSION 3.25)
project(iouring_net_server
  VERSION   0.0.1
  LANGUAGES CXX
  DESCRIPTION "Reference product on top of iouring_net")

include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Consume the library
find_package(iouring_net 0.0.1 REQUIRED)

# Project-wide warning helper, mirroring library's iouring_net_apply_warnings
include(compiler-warnings)

# Codegen runs at configure time AND defines the iouring_server_stub
# and iouring_client_proxy targets from the generated outputs. There
# is no separate add_subdirectory(generated) — the generated files
# live in the build tree, and CMakeLists.txt files do not belong in
# the build tree.
include(codegen)

add_subdirectory(server)
add_subdirectory(client)

option(IOURING_SERVER_BUILD_TESTS "Build the test suite" ON)
if(IOURING_SERVER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
```

The structure mirrors the library's top-level CMakeLists at
[`iouring-net-lib/CMakeLists.txt`](../../../engine-uring/CMakeLists.txt)
intentionally — a contributor moving between the two repos finds the
same layout.

---

## `find_package(iouring_net)` — the consumption pattern

The library installs `iouring_netConfig.cmake` to
`<prefix>/lib/cmake/iouring_net/`. The minimal call:

```cmake
find_package(iouring_net 0.0.1 REQUIRED)
```

CMake resolves this by searching, in order:
1. `iouring_net_DIR` cache variable.
2. `CMAKE_PREFIX_PATH` entries (`<entry>/lib/cmake/iouring_net/`).
3. System default prefixes (`/usr`, `/usr/local`).

The imported target is `iouring_net::iouring_net`. Linking it
propagates all PUBLIC dependencies of the library:

```cmake
target_link_libraries(my_target PRIVATE iouring_net::iouring_net)
# → Threads::Threads, fmt::fmt, liburing::uring, tl::expected
#   are all visible to my_target without further configure steps.
```

The product **must not**:
- `find_package(fmt)` itself — the library already does, and a
  mismatched fmt version is a real footgun.
- `find_package(liburing)` itself — same reasoning.
- Use `target_include_directories(my_target PRIVATE
  /path/to/iouring-net-lib/src)` — that breaks the install contract.
  Headers come from `iouring_net::iouring_net`'s `INTERFACE_INCLUDE_DIRECTORIES`.

### Version compatibility

The library's `iouring_netConfigVersion.cmake` is generated with
`COMPATIBILITY SameMajorVersion`. So `find_package(iouring_net 0.1.0
REQUIRED)` will accept any `0.x` install but reject `1.x`. Until v1.0,
treat the version pin as advisory — pin **by git tag** in CI, not by
version number alone.

---

## Codegen integration — `cmake/codegen.cmake`

The codegen pipeline runs at *configure time*, not build time, so the
emitted `.cpp` files exist before CMake collects sources. (Build-time
codegen via `add_custom_command` would also work but complicates IDE
indexing — clangd reports missing headers until the first build.)

```cmake
# cmake/codegen.cmake

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

# Final location for generated outputs (consumed by stub/proxy targets).
set(IOURING_SERVER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Staging location — written first, swapped in atomically. Distinct
# directory under the build tree so a partial write never leaves the
# final dir in a half-emitted state (e.g., new packet_layout.h but
# stale server_stub.cpp).
set(IOURING_SERVER_GEN_STAGE ${CMAKE_CURRENT_BINARY_DIR}/generated.stage)

# Manifest of expected outputs. The generator MUST produce exactly
# these files; missing or empty entries fail configure.
set(IOURING_SERVER_GEN_OUTPUTS
  packet_ids.h
  packet_layout.h
  server_stub.h
  server_stub.cpp
  client_proxy.h
  client_proxy.cpp)

# Acquire the codegen lock BEFORE touching the staging directory.
# Otherwise two parallel CMake invocations sharing this build dir
# could both clean and rewrite generated.stage/ at the same time,
# corrupting the manifest check that follows.
file(LOCK ${CMAKE_CURRENT_BINARY_DIR}/.codegen.lock GUARD FUNCTION
     TIMEOUT 30)

# Clean prior staging tree (now safely under the lock) so a stale
# file from a previous schema can never linger.
file(REMOVE_RECURSE ${IOURING_SERVER_GEN_STAGE})
file(MAKE_DIRECTORY ${IOURING_SERVER_GEN_STAGE})

# Run the generator into the staging dir.
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/codegen/rpc_gen.py
          --schema ${PROJECT_SOURCE_DIR}/proto/packets.json
          --out    ${IOURING_SERVER_GEN_STAGE}
  RESULT_VARIABLE _gen_rc
  OUTPUT_VARIABLE _gen_out
  ERROR_VARIABLE  _gen_err)
if(NOT _gen_rc EQUAL 0)
  message(FATAL_ERROR "codegen failed:\n${_gen_out}\n${_gen_err}")
endif()

# Manifest validation, part 1: every expected file must exist AND be
# non-empty. Guards against silent generator bugs that emit a header
# but skip its .cpp counterpart.
foreach(_out IN LISTS IOURING_SERVER_GEN_OUTPUTS)
  set(_path ${IOURING_SERVER_GEN_STAGE}/${_out})
  if(NOT EXISTS ${_path})
    message(FATAL_ERROR "codegen manifest violation: missing ${_out}\n"
                        "stdout:\n${_gen_out}\nstderr:\n${_gen_err}")
  endif()
  file(SIZE ${_path} _sz)
  if(_sz EQUAL 0)
    message(FATAL_ERROR "codegen manifest violation: empty ${_out}\n"
                        "stdout:\n${_gen_out}\nstderr:\n${_gen_err}")
  endif()
endforeach()

# Manifest validation, part 2: exactly these files. A generator bug
# that emits an extra file (e.g., a leftover *.cpp from a packet that
# was deleted from the schema) is treated as failure; staging is
# discarded.
file(GLOB _staged_files RELATIVE ${IOURING_SERVER_GEN_STAGE}
     ${IOURING_SERVER_GEN_STAGE}/*)
list(SORT _staged_files)
set(_expected ${IOURING_SERVER_GEN_OUTPUTS})
list(SORT _expected)
if(NOT _staged_files STREQUAL _expected)
  message(FATAL_ERROR
    "codegen manifest violation: staging dir contents differ from manifest\n"
    "  expected: ${_expected}\n"
    "  actual:   ${_staged_files}\n"
    "stdout:\n${_gen_out}\nstderr:\n${_gen_err}")
endif()

# Swap stage → final. NOTE on atomicity:
#
# file(RENAME) invokes rename(2). On Linux, rename(2) of a directory is
# atomic ONLY when the destination is absent or is an empty directory.
# A non-empty destination cannot be replaced atomically with plain
# POSIX rename; that requires Linux-specific
# renameat2(RENAME_EXCHANGE), which CMake does not expose.
#
# We therefore do REMOVE_RECURSE followed by RENAME. This is sufficient
# for the failure mode we care about (generator crashed mid-write —
# the staging dir is discarded next configure; the final dir was
# never touched until validation passed). It is NOT a race-free swap
# for concurrent readers: between REMOVE_RECURSE and RENAME, a parallel
# CMake/IDE process sharing this build dir would see GEN_DIR missing.
#
# We document concurrent shared access to one build dir as out of
# contract. The file(LOCK ...) acquired at the top of this section
# (with GUARD FUNCTION) is still held here and serializes cooperating
# CMake configures against each other for the entire codegen sequence
# — clean, run, validate, swap.
file(REMOVE_RECURSE ${IOURING_SERVER_GEN_DIR})
file(RENAME ${IOURING_SERVER_GEN_STAGE} ${IOURING_SERVER_GEN_DIR}
     RESULT _rn)
if(NOT _rn EQUAL 0)
  message(FATAL_ERROR "codegen: rename stage → final failed: ${_rn}")
endif()

# Re-run on reconfigure if any input mutates. Every script the
# pipeline depends on must be listed; otherwise editing
# layout_gen.py (etc.) silently uses cached output.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/proto/packets.json
  ${PROJECT_SOURCE_DIR}/codegen/rpc_gen.py
  ${PROJECT_SOURCE_DIR}/codegen/ids_gen.py
  ${PROJECT_SOURCE_DIR}/codegen/layout_gen.py
  ${PROJECT_SOURCE_DIR}/codegen/stub_gen.py
  ${PROJECT_SOURCE_DIR}/codegen/proxy_gen.py)
```

What this guarantees, scoped honestly:

- **Crash-safety against partial generator output.** A generator that
  fails after writing some staged files cannot pollute the working
  `generated/` directory: validation runs on the staging dir, and the
  final dir is only touched after the manifest check passes. The
  previous good generation survives.
- **No stale files within one generation.** `REMOVE_RECURSE` on the
  staging dir before each run plus the "exactly these files" manifest
  check means an old packet's emitted `.cpp` cannot survive a schema
  rename or sneak in as an unexpected output.
- **Cooperative concurrency for shared build dirs.** `file(LOCK ...)`
  serializes concurrent CMake configures that happen to share this
  build directory. Within one configure, `generated/` reaches its
  next valid state without contention.
- **Complete reconfigure-on-input-change.** All 5 generator scripts +
  `packets.json` are in `CMAKE_CONFIGURE_DEPENDS`; editing any of
  them triggers reconfigure and regeneration.
- Editing `packets.json` triggers reconfigure → regenerate → rebuild.
- `generated/` lives in the build tree, not the source tree.
  Gitignore-safe.

What this **does not** guarantee:

- **POSIX-atomic non-empty directory replacement** is not provided by
  CMake; `file(RENAME)` cannot replace a non-empty dir in one
  syscall. The `REMOVE_RECURSE` + `RENAME` sequence has a brief
  window where `generated/` is absent. A separate process (parallel
  CMake invocation, IDE indexer) reading `generated/` during that
  window can observe it missing. Concurrent shared access to one
  build directory by uncooperating readers is **explicitly out of
  contract**; in-tree CMake/build flows are serialized by the
  `file(LOCK)` above. If you need cross-process atomicity for
  uncooperating readers, the implementation path is either
  symlink-indirection on POSIX (`generated/` is a symlink the swap
  redirects) or `renameat2(RENAME_EXCHANGE)` via a small helper
  binary — both are out of scope for v1.

### Generated targets — defined in `cmake/codegen.cmake`, NOT via `add_subdirectory(generated)`

There is no `generated/CMakeLists.txt` and no
`add_subdirectory(generated)`. The generated outputs live in
`${CMAKE_CURRENT_BINARY_DIR}/generated`; pointing
`add_subdirectory` at a build-tree path is fragile (it requires
generating a CMakeLists.txt there too, or the directory to exist
before configure begins). Instead, the target definitions live in
`cmake/codegen.cmake` immediately after the swap step:

```cmake
add_library(iouring_server_stub STATIC
  ${IOURING_SERVER_GEN_DIR}/server_stub.cpp)
target_include_directories(iouring_server_stub PUBLIC
  ${IOURING_SERVER_GEN_DIR})
target_link_libraries(iouring_server_stub PUBLIC
  iouring_net::iouring_net)
iouring_server_apply_warnings(iouring_server_stub)

add_library(iouring_client_proxy STATIC
  ${IOURING_SERVER_GEN_DIR}/client_proxy.cpp)
target_include_directories(iouring_client_proxy PUBLIC
  ${IOURING_SERVER_GEN_DIR})
target_link_libraries(iouring_client_proxy PUBLIC
  iouring_net::iouring_net)
iouring_server_apply_warnings(iouring_client_proxy)
```

Splitting stub vs proxy is intentional: the server doesn't pay code
weight for the client's proxies, and the client doesn't link the
server's dispatcher. They share only the underlying schema.

### `server/CMakeLists.txt`

```cmake
add_executable(server
  main.cpp
  lifecycle.cpp
  dispatch.cpp
  handlers/move_handler.cpp
  handlers/attack_handler.cpp
  handlers/sync_handler.cpp)

target_link_libraries(server PRIVATE
  iouring_server_stub
  iouring_net::iouring_net)
target_compile_features(server PRIVATE cxx_std_20)
iouring_server_apply_warnings(server)
```

### `client/CMakeLists.txt`

```cmake
add_executable(client main.cpp)
target_link_libraries(client PRIVATE
  iouring_client_proxy
  iouring_net::iouring_net)
target_compile_features(client PRIVATE cxx_std_20)
iouring_server_apply_warnings(client)
```

---

## Presets

Mirror the library's `CMakePresets.json` exactly: `default`, `tsan`,
`release`, `floor`. The product preset file adds one cache variable
beyond the library's: `IOURING_NET_INSTALL_PREFIX` (sets
`CMAKE_PREFIX_PATH` if defined). Example:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "default",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/default",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_CXX_FLAGS_DEBUG": "-O0 -g -fsanitize=address,undefined",
        "CMAKE_EXE_LINKER_FLAGS_DEBUG": "-fsanitize=address,undefined"
      }
    },
    ...
  ]
}
```

The floor preset must build against a `floor`-preset library install
(g++-12, FetchContent deps). This is the cross-cut that proves both
repos cohere on the lowest supported toolchain.

---

## Anti-patterns

- **Don't** add the library as a CMake `FetchContent` source.
  FetchContent skips the install/export pipeline — the whole point of
  the two-repo split.
- **Don't** `add_subdirectory(/path/to/iouring-net-lib)` to "embed"
  the library. Same reason.
- **Don't** copy library headers into this repo. `find_package` resolves
  them; copying creates skew.
- **Don't** write a CMake `find_module` (a `Findiouring_net.cmake`) —
  the library ships a config package; the find-module path is wrong.
- **Don't** call `set(CMAKE_CXX_STANDARD 23)` to enable some standalone
  product feature. Stay on C++20 to match the library.

---

## Cross-references

- [`02-build-and-toolchain.md`](02-build-and-toolchain.md) — Python and
  install-prefix prerequisites for the snippets above.
- [`05-codegen.md`](05-codegen.md) — what the Python scripts emit, and
  how to extend the schema.
- [`iouring-net-lib/docs/05-cmake.md`](../../../engine-uring/doc/05-cmake.md)
  — library-side CMake authority (warning helper, sanitizer presets).
- [`iouring-net-lib/docs/09-project-split.md`](../../2026-05-14-project-split.md)
  § "The seam — how the two projects connect" — the install contract
  this document implements.
