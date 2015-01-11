#!/bin/bash

# Make a release source tarball containing pre-built documentation

BASE_DIR=$(dirname "$0")
. "${BASE_DIR}/functions.sh"

main() {
  TMP=/tmp
  mkdir -p "${TMP}"

  VER="$(make version)"
  P="compton-${VER}"
  git archive --format=tar -o "${TMP}/${P}.tar" --prefix="${P}/" HEAD || die
  cd "${TMP}" || die
  tar xf "${TMP}/${P}.tar" || die
  sed -i "s/\(COMPTON_VERSION ?=\).*/\1 ${VER}/" "${P}/Makefile" || die
  cd "${P}" || die
  make docs || die
  cd .. || die
  tar cJf "${P}.tar.xz" "${P}" || die
  rm -r "${P}" "${P}.tar" || die
  einfo Archive is now on $(realpath ${P}.tar.xz)
}

main
