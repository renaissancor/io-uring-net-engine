# cmake/compiler-warnings.cmake
#
# Project-wide warning policy. Applied PRIVATE to project targets so
# consumers of an installed iouring_net don't inherit our -Werror.
#
# Ratcheted in: -Werror, -Wconversion, -Wsign-conversion are off at v0
# and turned on as the codebase matures. The full list lives in
# docs/05-cmake.md § "Warning set".

function(iouring_net_apply_warnings target)
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
