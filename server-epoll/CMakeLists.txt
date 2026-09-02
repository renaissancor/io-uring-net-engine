cmake_minimum_required(VERSION 3.25)
project(server_epoll_sds
  LANGUAGES CXX
  DESCRIPTION "The epoll control group rebuilt on the engine's sds:: primitives")

# This file builds ONLY server-sds. The baseline `server` stays on the plain
# Makefile next door and must not acquire a dependency on anything: it is the
# published control group and its build path is part of what was measured.
#
# The engine is consumed through an install prefix, exactly as server-uring
# does — find_package, never a relative include into ../engine-uring/src.
# If a header is missing here the fix is the engine's public FILE_SET.
#
#   cmake -S ../engine-uring -B ../build/engine -DCMAKE_BUILD_TYPE=Release
#   cmake --build ../build/engine
#   cmake --install ../build/engine --prefix ../build/prefix
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=../build/prefix
#   cmake --build build
#
# `make sds` in the Makefile runs the last two lines.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(iouring_net 0.0.1 CONFIG REQUIRED)

add_executable(server-sds server_sds.cpp)
target_compile_features(server-sds PRIVATE cxx_std_20)
# Same flags as the Makefile's measurement build: -O2 -DNDEBUG -g, the
# standard warning set. The sanitised variant is selected with
# -DSERVER_SDS_SANITIZE=ON (Makefile target `sds-asan`); never measure with it.
target_compile_options(server-sds PRIVATE -Wall -Wextra -Wpedantic -g)
target_link_libraries(server-sds PRIVATE iouring_net::iouring_net)

option(SERVER_SDS_SANITIZE "Build server-sds with ASan+UBSan (correctness only)" OFF)
if(SERVER_SDS_SANITIZE)
  target_compile_options(server-sds PRIVATE -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(server-sds PRIVATE -fsanitize=address,undefined)
  set_target_properties(server-sds PROPERTIES OUTPUT_NAME server-sds-asan)
else()
  target_compile_options(server-sds PRIVATE -O2)
  target_compile_definitions(server-sds PRIVATE NDEBUG)
endif()
