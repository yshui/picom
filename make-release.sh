#!/bin/bash

GOOD=$'\e[32;01m'
WARN=$'\e[33;01m'
BAD=$'\e[31;01m'
HILITE=$'\e[36;01m'
BRACKET=$'\e[34;01m'
NORMAL=$'\e[0m'

eerror() {
  echo -e "$@" | while read -r ; do
    echo " $BAD*$NORMAL $RC_INDENTATION$REPLY" >&2
  done
  return 0
}

einfo() {
  echo -e "$@" | while read -r ; do
    echo " $GOOD*$NORMAL $REPLY"
  done
  return 0
}

die() {
  eerror "Call stack:"
  (( n = ${#FUNCNAME[@]} - 1 ))
  while (( n > 0 )); do
    funcname=${FUNCNAME[$((n - 1))]}
    sourcefile=$(basename ${BASH_SOURCE[${n}]})
    lineno=${BASH_LINENO[$((n - 1))]}
    eerror "  ${sourcefile}:${lineno} - Called ${funcname}()"
    (( n-- ))
  done
  eerror "Working directory: '$(pwd)'"
  exit 1
}

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
