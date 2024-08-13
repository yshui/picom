// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "misc.h"

#define mstrncmp(s1, s2) strncmp((s1), (s2), strlen(s1))

char *mstrjoin(const char *src1, const char *src2);
char *mstrjoin3(const char *src1, const char *src2, const char *src3);
void mstrextend(char **psrc1, const char *src2);
const char *trim_both(const char *src, size_t *length);

/// Parse a floating point number of form (+|-)?[0-9]*(\.[0-9]*)
double strtod_simple(const char *, const char **);

static inline int uitostr(unsigned int n, char *buf) {
	int ret = 0;
	unsigned int tmp = n;
	while (tmp > 0) {
		tmp /= 10;
		ret++;
	}

	if (ret == 0) {
		ret = 1;
	}

	int pos = ret;
	while (pos--) {
		buf[pos] = (char)(n % 10 + '0');
		n /= 10;
	}
	return ret;
}

/// Like `asprintf`, but it aborts the program if memory allocation fails.
static inline size_t __attribute__((format(printf, 2, 3)))
casprintf(char **strp, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int ret = vasprintf(strp, fmt, ap);
	va_end(ap);

	BUG_ON(ret < 0);
	return (size_t)ret;
}

/// Convert a double into a string. Avoid using *printf functions to print floating points
/// directly because they are locale dependent.
static inline void dtostr(double n, char **buf) {
	BUG_ON(safe_isnan(n));
	BUG_ON(safe_isinf(n));
	if (fabs(n) > 1e9) {
		// The number is so big that it's not meaningful to keep decimal places.
		casprintf(buf, "%.0f", n);
		return;
	}

	if (n > 0) {
		casprintf(buf, "%.0f.%03d", floor(n), (int)(fmod(n, 1) * 1000));
	} else {
		casprintf(buf, "-%.0f.%03d", floor(-n), (int)(fmod(-n, 1) * 1000));
	}
}

static inline const char *skip_space_const(const char *src) {
	if (!src) {
		return NULL;
	}
	while (*src && isspace((unsigned char)*src)) {
		src++;
	}
	return src;
}

static inline char *skip_space_mut(char *src) {
	if (!src) {
		return NULL;
	}
	while (*src && isspace((unsigned char)*src)) {
		src++;
	}
	return src;
}

#define skip_space(x)                                                                    \
	_Generic((x), char *: skip_space_mut, const char *: skip_space_const)(x)

static inline bool starts_with(const char *str, const char *needle, bool ignore_case) {
	if (ignore_case) {
		return strncasecmp(str, needle, strlen(needle)) == 0;
	}
	return strncmp(str, needle, strlen(needle)) == 0;
}

/// Similar to `asprintf`, but it reuses the allocated memory pointed to by `*strp`, and
/// reallocates it if it's not big enough.
int asnprintf(char **strp, size_t *capacity, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
