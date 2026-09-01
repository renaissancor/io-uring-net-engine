# server_core carries every translation unit with logic in it; the
# executable is main.cpp only. Tests link server_core directly, so the
# unit layer never spawns a process and never re-lists sources.
add_library(server_core STATIC
  options.cpp
  seam_check.cpp)

target_include_directories(server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(server_core PUBLIC iouring_net::iouring_net)
target_compile_features(server_core PUBLIC cxx_std_20)
iouring_server_apply_warnings(server_core)

add_executable(server main.cpp)
set_target_properties(server PROPERTIES OUTPUT_NAME iouring_net-server)
target_link_libraries(server PRIVATE server_core)
iouring_server_apply_warnings(server)
