// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#define HASH_ITER2(head, el)                                                             \
	for (__typeof__(head) el = (head), __tmp = el != NULL ? el->hh.next : NULL;      \
	     el != NULL; el = __tmp, __tmp = el != NULL ? el->hh.next : NULL)
