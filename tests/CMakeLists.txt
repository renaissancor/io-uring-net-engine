# tests/CMakeLists.txt
#
# Catch2 v3 main lives in Catch2::Catch2WithMain (provided by both
# find_package and FetchContent paths). Per-category test subdirectories
# will be added under tests/<category>/ as subsystems land.

include(Catch)

add_executable(iouring_net-test
  smoke_test.cpp
  diagnostic/profiler_scope_test.cpp
  memory/packet_pool_test.cpp
  net/echo_smoke_test.cpp
  runtime/thread_test.cpp
  sds/cstr_hash_map_test.cpp
  sds/malloc_vector_test.cpp
  sds/ring_buffer_test.cpp
  sds/spsc_queue_test.cpp
  sync/atomic_test.cpp
  sync/mutex_test.cpp)

target_link_libraries(iouring_net-test PRIVATE
  iouring_net
  Catch2::Catch2WithMain)

iouring_net_apply_warnings(iouring_net-test)

# DISCOVERY_MODE PRE_TEST runs the binary at `ctest` time, not at build
# time. Required for the TSan preset on Ubuntu 24.04, where the default
# build-time test-discovery run crashes with
# `FATAL: ThreadSanitizer: unexpected memory mapping` because noble's
# vm.mmap_rnd_bits=32 conflicts with TSan's hardcoded address layout.
# See docs/08-test-strategy.md § "TSan on Ubuntu 24.04".
catch_discover_tests(iouring_net-test
  DISCOVERY_MODE PRE_TEST
  EXTRA_ARGS --allow-running-no-tests)
