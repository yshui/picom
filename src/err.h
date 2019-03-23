// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2019 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "compiler.h"

// Functions for error reporting, adopted from Linux

// INFO in user space we can probably be more liberal about what pointer we consider
// error. e.g. In x86_64 Linux, all addresses with the highest bit set is invalid in user
// space.
#define MAX_ERRNO 4095

static inline void *must_use ERR_PTR(intptr_t err) {
	return (void *)err;
}

static inline intptr_t must_use PTR_ERR(void *ptr) {
	return (intptr_t)ptr;
}

static inline bool must_use IS_ERR(void *ptr) {
	return unlikely((uintptr_t)ptr > (uintptr_t)-MAX_ERRNO);
}

static inline bool must_use IS_ERR_OR_NULL(void *ptr) {
	return unlikely(!ptr) || IS_ERR(ptr);
}

static inline intptr_t must_use PTR_ERR_OR_ZERO(void *ptr) {
	if (IS_ERR(ptr)) {
		return PTR_ERR(ptr);
	}
	return 0;
}
