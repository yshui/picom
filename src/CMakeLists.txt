set(SRC
    compton.c
    win.c
    c2.c
    x.c
    config.c
    vsync.c
    utils.c
    diagnostic.c
    string_utils.c
    render.c
    kernel.c
    log.c
    options.c
    event.c
    cache.c
    atom.c
)

include(FindPkgConfig)
add_library(libev UNKNOWN IMPORTED)
pkg_check_modules(LIBEV_PC libev)
if(LIBEV_PC_FOUND)
  link_directories(${LIBEV_LIBRARY_DIRS})
endif()

add_executable(picom ${SRC})
target_link_libraries(picom PRIVATE test.h)

if(MODULARIZE)
  check_c_compiler_flag("-fmodules" HAS_MODULARIZE)
  if(NOT HAS_MODULARIZE)
    message(SEND_ERROR "Compiler doesn't support -fmodules")
  endif()
  add_custom_target(modulemap DEPENDS compton.modulemap)
  add_dependencies(picom modulemap)
  target_compile_options(
    picom PRIVATE -fmodules
                  -fmodule-map-file=${CMAKE_CURRENT_SOURCE_DIR}/compton.modulemap
  )
endif()

check_include_file(uthash.h HAS_UTHASH)

if(NOT HAS_UTHASH)
  message(SEND_ERROR "uthash not found")
endif()

if(NOT LIBEV_PC_FOUND)
  find_library(LIBEV ev)
  if(NOT LIBEV)
    message(SEND_ERROR "libev not found")
  else()
    message(STATUS "Looking for libev - found")
    target_link_libraries(picom PRIVATE ev)
  endif()
else()
  target_link_libraries(picom PRIVATE ${LIBEV_LIBRARIES})
endif()
