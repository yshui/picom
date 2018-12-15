// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdio.h>

/// Print out an error message.
#define printf_err(format, ...) \
  fprintf(stderr, format "\n", ## __VA_ARGS__)

/// Print out an error message with function name.
#define printf_errf(format, ...) \
  printf_err("%s" format,  __func__, ## __VA_ARGS__)

/// Print out an error message with function name, and quit with a
/// specific exit code.
#define printf_errfq(code, format, ...) { \
  printf_err("%s" format,  __func__, ## __VA_ARGS__); \
  exit(code); \
}

/// Print out a debug message.
#define printf_dbg(format, ...) \
  printf(format, ## __VA_ARGS__); \
  fflush(stdout)

/// Print out a debug message with function name.
#define printf_dbgf(format, ...) \
  printf_dbg("%s" format, __func__, ## __VA_ARGS__)
