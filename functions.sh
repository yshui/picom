#!/bin/bash

# Shared functions for various supporting scripts of compton
# Mostly copied from Gentoo gentoo-functions

GOOD=$'\e[32;01m'
WARN=$'\e[33;01m'
BAD=$'\e[31;01m'
HILITE=$'\e[36;01m'
BRACKET=$'\e[34;01m'
NORMAL=$'\e[0m'

# @FUNCTION: eerror
# @USAGE: [message]
# @DESCRIPTION:
# Show an error message.
eerror() {
  echo -e "$@" | while read -r ; do
    echo " $BAD*$NORMAL $REPLY" >&2
  done
  return 0
}

# @FUNCTION: einfo
# @USAGE: [message]
# @DESCRIPTION:
# Show a message.
einfo() {
  echo -e "$@" | while read -r ; do
    echo " $GOOD*$NORMAL $REPLY"
  done
  return 0
}

# @FUNCTION: die
# @USAGE:
# @DESCRIPTION:
# Print the call stack and the working directory, then quit the shell.
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
