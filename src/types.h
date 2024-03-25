// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

/// Some common types

#include <stdint.h>

/// Enumeration type to represent switches.
typedef enum {
	OFF = 0,        // false
	ON,             // true
	UNSET
} switch_t;

enum tristate { TRI_FALSE = -1, TRI_UNKNOWN = 0, TRI_TRUE = 1 };

/// A structure representing margins around a rectangle.
typedef struct {
	int top;
	int left;
	int bottom;
	int right;
} margin_t;

struct color {
	double red, green, blue, alpha;
};

typedef uint32_t opacity_t;

#define MARGIN_INIT                                                                      \
	{ 0, 0, 0, 0 }
