// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <ctype.h>
#include <stddef.h>

#define mstrncmp(s1, s2) strncmp((s1), (s2), strlen(s1))

char *mstrjoin(const char *src1, const char *src2);
char *
mstrjoin3(const char *src1, const char *src2, const char *src3);
void mstrextend(char **psrc1, const char *src2);

static inline const char *
skip_space_const(const char *src) {
  if (!src)
    return NULL;
  while (*src && isspace(*src))
    src++;
  return src;
}

static inline char *
skip_space_mut(char *src) {
  if (!src)
    return NULL;
  while (*src && isspace(*src))
    src++;
  return src;
}

#define skip_space(x) _Generic((x), \
  char *: skip_space_mut, \
  const char *: skip_space_const \
)(x)
