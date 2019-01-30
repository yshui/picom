// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <string.h>

#include "compiler.h"
#include "string_utils.h"
#include "utils.h"

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
