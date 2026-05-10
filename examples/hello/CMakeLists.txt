# examples/hello/CMakeLists.txt
#
# Smallest possible consumer of iouring_net::iouring_net. Linking the
# alias propagates fmt::fmt and tl::expected through PUBLIC dependencies
# and adds src/ to the include path via BUILD_INTERFACE.

add_executable(iouring_net-hello main.cpp)

target_link_libraries(iouring_net-hello PRIVATE iouring_net::iouring_net)

iouring_net_apply_warnings(iouring_net-hello)
