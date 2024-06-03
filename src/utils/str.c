// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <stdarg.h>
#include <string.h>

#include <test.h>

#include "str.h"

#pragma GCC diagnostic push

// gcc warns about legitimate strncpy in mstrjoin and mstrextend
// strncpy(str, src1, len1) intentional truncates the null byte from src1.
// strncpy(str+len1, src2, len2) uses bound depends on the source argument,
// but str is allocated with len1+len2+1, so this strncpy can't overflow
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#pragma GCC diagnostic ignored "-Wstringop-overflow"

/**
 * Allocate the space and join two strings.
 */
char *mstrjoin(const char *src1, const char *src2) {
	auto len1 = strlen(src1);
	auto len2 = strlen(src2);
	auto len = len1 + len2 + 1;
	auto str = ccalloc(len, char);

	strncpy(str, src1, len1);
	strncpy(str + len1, src2, len2);
	str[len - 1] = '\0';

	return str;
}

TEST_CASE(mstrjoin) {
	char *str = mstrjoin("asdf", "qwer");
	TEST_STREQUAL(str, "asdfqwer");
	free(str);

	str = mstrjoin("", "qwer");
	TEST_STREQUAL(str, "qwer");
	free(str);

	str = mstrjoin("asdf", "");
	TEST_STREQUAL(str, "asdf");
	free(str);
}

/**
 * Concatenate a string on heap with another string.
 */
void mstrextend(char **psrc1, const char *src2) {
	if (!*psrc1) {
		*psrc1 = strdup(src2);
		return;
	}

	auto len1 = strlen(*psrc1);
	auto len2 = strlen(src2);
	auto len = len1 + len2 + 1;
	*psrc1 = crealloc(*psrc1, len);

	strncpy(*psrc1 + len1, src2, len2);
	(*psrc1)[len - 1] = '\0';
}

TEST_CASE(mstrextend) {
	char *str1 = NULL;
	mstrextend(&str1, "asdf");
	TEST_STREQUAL(str1, "asdf");

	mstrextend(&str1, "asd");
	TEST_STREQUAL(str1, "asdfasd");

	mstrextend(&str1, "");
	TEST_STREQUAL(str1, "asdfasd");
	free(str1);
}

#pragma GCC diagnostic pop

/// Parse a floating point number of form (+|-)?[0-9]*(\.[0-9]*)
double strtod_simple(const char *src, const char **end) {
	double neg = 1;
	bool succeeded = false;
	*end = src;
	if (*src == '-') {
		neg = -1;
		src++;
	} else if (*src == '+') {
		src++;
	}

	double ret = 0;
	while (*src >= '0' && *src <= '9') {
		ret = ret * 10 + (*src - '0');
		succeeded = true;
		src++;
	}

	if (*src == '.') {
		double frac = 0, mult = 0.1;
		src++;
		while (*src >= '0' && *src <= '9') {
			frac += mult * (*src - '0');
			mult *= 0.1;
			succeeded = true;
			src++;
		}
		ret += frac;
	}

	if (succeeded) {
		*end = src;
		return ret * neg;
	}
	return NAN;
}

TEST_CASE(strtod_simple) {
	const char *end;
	double result = strtod_simple("1.0", &end);
	TEST_EQUAL(result, 1);
	TEST_EQUAL(*end, '\0');

	result = strtod_simple("-1.0", &end);
	TEST_EQUAL(result, -1);
	TEST_EQUAL(*end, '\0');

	result = strtod_simple("+.5", &end);
	TEST_EQUAL(result, 0.5);
	TEST_EQUAL(*end, '\0');

	result = strtod_simple("+.", &end);
	TEST_TRUE(safe_isnan(result));
	TEST_EQUAL(*end, '+');
}

const char *trim_both(const char *src, size_t *length) {
	size_t i = 0;
	while (isspace(src[i])) {
		i++;
	}
	size_t j = strlen(src) - 1;
	while (j > i && isspace(src[j])) {
		j--;
	}
	*length = j - i + 1;
	return src + i;
}

TEST_CASE(trim_both) {
	size_t length;
	const char *str = trim_both("  \t\n\r\f", &length);
	TEST_EQUAL(length, 0);
	TEST_EQUAL(*str, '\0');

	str = trim_both(" asdfas  ", &length);
	TEST_EQUAL(length, 6);
	TEST_STRNEQUAL(str, "asdfas", length);

	str = trim_both("  asdf asdf   ", &length);
	TEST_EQUAL(length, 9);
	TEST_STRNEQUAL(str, "asdf asdf", length);
}

static int vasnprintf(char **strp, size_t *capacity, const char *fmt, va_list args) {
	va_list copy;
	va_copy(copy, args);
	int needed = vsnprintf(*strp, *capacity, fmt, copy);
	va_end(copy);

	if ((size_t)needed + 1 > *capacity) {
		char *new_str = malloc((size_t)needed + 1);
		allocchk(new_str);
		free(*strp);
		*strp = new_str;
		*capacity = (size_t)needed + 1;
	} else {
		return needed;
	}

	return vsnprintf(*strp, *capacity, fmt, args);
}

int asnprintf(char **strp, size_t *capacity, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int ret = vasnprintf(strp, capacity, fmt, args);
	va_end(args);
	return ret;
}
