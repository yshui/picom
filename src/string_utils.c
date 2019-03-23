// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <string.h>

#include <test.h>

#include "compiler.h"
#include "string_utils.h"
#include "utils.h"

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
	if (*src == '-') {
		neg = -1;
		src++;
	} else if (*src == '+') {
		src++;
	}

	double ret = 0;
	while (*src >= '0' && *src <= '9') {
		ret = ret * 10 + (*src - '0');
		src++;
	}

	if (*src == '.') {
		double frac = 0, mult = 0.1;
		src++;
		while (*src >= '0' && *src <= '9') {
			frac += mult * (*src - '0');
			mult *= 0.1;
			src++;
		}
		ret += frac;
	}

	*end = src;
	return ret * neg;
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
}
