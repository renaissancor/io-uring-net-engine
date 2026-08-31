# tests/CMakeLists.txt
#
# One Catch2 binary for Layers 1–2 of the pyramid in
# docs/06-test-strategy.md, mirroring iouring-net-lib/tests/. Test
# sources sort into subdirectories by layer:
#
#   unit/         Layer 1 — no I/O, links server_core directly
#   integration/  Layer 2 — in-process server threads + loopback client
#                 (arrives when the library exports its app layer)
#
# Layer 3 (two-process) has exactly one PR-gated representative below;
# the full replay harness is nightly-only per docs/06-test-strategy.md.

include(Catch)

add_executable(iouring_server-test
  unit/options_test.cpp
  unit/seam_check_test.cpp)

target_link_libraries(iouring_server-test PRIVATE
  server_core
  Catch2::Catch2WithMain)

iouring_server_apply_warnings(iouring_server-test)

# DISCOVERY_MODE PRE_TEST: required for the TSan preset on Ubuntu 24.04
# (build-time discovery crashes under TSan; see
# iouring-net-lib/docs/08-test-strategy.md § "TSan on Ubuntu 24.04").
catch_discover_tests(iouring_server-test
  DISCOVERY_MODE PRE_TEST
  EXTRA_ARGS --allow-running-no-tests)

# Layer-3 representative: spawns the real binary, proving the
# executable + install-contract path end-to-end on every PR.
add_test(NAME server_seam_smoke COMMAND server --dry-run)
