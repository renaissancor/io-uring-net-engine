# tests/CMakeLists.txt
#
# Catch2 v3 main lives in Catch2::Catch2WithMain (provided by both
# find_package and FetchContent paths). Per-category test subdirectories
# will be added under tests/<category>/ as subsystems land.

include(Catch)

add_executable(iouring_net-test
  smoke_test.cpp)

target_link_libraries(iouring_net-test PRIVATE
  iouring_net
  Catch2::Catch2WithMain)

iouring_net_apply_warnings(iouring_net-test)

catch_discover_tests(iouring_net-test)
