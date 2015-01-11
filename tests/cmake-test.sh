#!/bin/bash

# Test script for CMake build

BASE_DIR=$(dirname "$0")/..
. "${BASE_DIR}/functions.sh"

BUILD_DIR="build"

cmake_prepare() {
  [ ! -e "CMakeLists.txt" ] && ln -s {_,}CMakeLists.txt
}

cmake_build() {
  einfo Building compton with cmake $@

  [ -e "${BUILD_DIR}" ] && rm -r "${BUILD_DIR}"
  mkdir "${BUILD_DIR}" && cd "${BUILD_DIR}" || die
  cmake ${@} .. || die
  make VERBOSE=1 -B || die
  cd -

  einfo Build completed successfully
}

show_build_help_msg() {
  "${BUILD_DIR}/compton" -h | less
}

main() {
  cmake_prepare
  cmake_build "${@}"
  # show_build_help_msg
}

main "${@}"
