cmake_minimum_required(VERSION 3.5)
project(
  picom
  LANGUAGES C
  VERSION 7
)

set(VER "v${PROJECT_VERSION}")
find_program(GIT git)

if(GIT)
  execute_process(
    COMMAND ${GIT} rev-parse --short=5 HEAD
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    RESULT_VARIABLE GIT_RETURN
    OUTPUT_VARIABLE GIT_VER
  )
  if(GIT_RETURN EQUAL 0)
    string(STRIP ${GIT_VER} GIT_VER)
    set(VER "vgit-${GIT_VER}")
  endif()
endif()

message(STATUS "Using version number: ${VER}")

add_definitions(-DPICOM_VERSION=${VER})
add_definitions(-D_GNU_SOURCE)

option(SANITIZE "Build picom with sanitizers")

include(CheckCCompilerFlag)
include(CheckIncludeFile)

if(SANITIZE)
  set(SANITIZERS address undefined)
  foreach(CURRENT_TEST integer nullability)
    check_c_compiler_flag("-fsanitize=${CURRENT_TEST}" "HAS_${CURRENT_TEST}")
    if(HAS_${CURRENT_TEST})
      list(APPEND SANITIZERS ${CURRENT_TEST})
    endif()
  endforeach()
  string(JOIN "," SANITIZERS_FLAG ${SANITIZERS})
  string(APPEND CMAKE_C_FLAGS " -fsanitize=${SANITIZERS_FLAG}")
  message(STATUS "Enabled sanitizers: ${SANITIZERS_FLAG}")

  foreach(CURRENT_TEST unsigned-integer-overflow)
    check_c_compiler_flag("-fno-sanitize=${CURRENT_TEST}" "HAS_DISABLE_${CURRENT_TEST}")
    if(HAS_DISABLE_${CURRENT_TEST})
      list(APPEND DISABLED_SANITIZERS ${CURRENT_TEST})
    endif()
  endforeach()
  string(JOIN "," DISABLED_SANITIZERS_FLAG ${DISABLED_SANITIZERS})
  string(APPEND CMAKE_C_FLAGS " -fno-sanitize=${DISABLED_SANITIZERS_FLAG}")
  message(STATUS "Disabled sanitizers: ${DISABLED_SANITIZERS_FLAG}")
endif()

option(MODULARIZE "Build picom using clang's -fmodules feature")

check_include_file(stdc-predef.h HAS_STDC_PREDEF_H)
if(HAS_STDC_PREDEF_H)
  add_definitions(-DHAS_STDC_PREDEF_H)
endif()

add_subdirectory("subprojects/test.h")
add_subdirectory("src")
add_subdirectory("man")
