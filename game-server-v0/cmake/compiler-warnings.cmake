# cmake/compiler-warnings.cmake
#
# Project-wide warning policy. Mirrors the library's
# iouring_net_apply_warnings (iouring-net-lib/cmake/compiler-warnings.cmake)
# so a contributor moving between the two repos sees one policy.
#
# Ratcheted in: -Werror, -Wconversion, -Wsign-conversion are off at v0
# and turned on as the codebase matures.

function(iouring_server_apply_warnings target)
  if(MSVC)
    # Reserved for hypothetical future Windows port; project is Linux-only.
    target_compile_options(${target} PRIVATE /W4)
    return()
  endif()

  target_compile_options(${target} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
  )

  # Ratchet candidates — turn on per-subsystem when the code is ready:
  #   -Werror -Wconversion -Wsign-conversion
endfunction()
