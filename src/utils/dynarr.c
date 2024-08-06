// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include "dynarr.h"
char *dynarr_join(char **arr, const char *sep) {
	size_t total_len = 0;
	dynarr_foreach(arr, i) {
		total_len += strlen(*i);
	}

	char *ret = malloc(total_len + strlen(sep) * (dynarr_len(arr) - 1) + 1);
	size_t pos = 0;
	allocchk(ret);
	dynarr_foreach(arr, i) {
		if (i != arr) {
			strcpy(ret + pos, sep);
			pos += strlen(sep);
		}
		strcpy(ret + pos, *i);
		pos += strlen(*i);
		free(*i);
	}
	dynarr_free_pod(arr);
	ret[pos] = '\0';
	return ret;
}
