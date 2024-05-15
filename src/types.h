// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

/// Some common types

#include <limits.h>
#include <math.h>
#include <stdbool.h>
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

typedef struct vec2 {
	union {
		double x;
		double width;
	};
	union {
		double y;
		double height;
	};
} vec2;

typedef struct ivec2 {
	union {
		int x;
		int width;
	};
	union {
		int y;
		int height;
	};
} ivec2;

static inline ivec2 ivec2_add(ivec2 a, ivec2 b) {
	return (ivec2){
	    .x = a.x + b.x,
	    .y = a.y + b.y,
	};
}

static inline ivec2 ivec2_sub(ivec2 a, ivec2 b) {
	return (ivec2){
	    .x = a.x - b.x,
	    .y = a.y - b.y,
	};
}

static inline bool ivec2_eq(ivec2 a, ivec2 b) {
	return a.x == b.x && a.y == b.y;
}

static inline ivec2 ivec2_neg(ivec2 a) {
	return (ivec2){
	    .x = -a.x,
	    .y = -a.y,
	};
}

/// Saturating cast from a vec2 to a ivec2
static inline ivec2 vec2_as(vec2 a) {
	return (ivec2){
	    .x = (int)fmin(fmax(a.x, INT_MIN), INT_MAX),
	    .y = (int)fmin(fmax(a.y, INT_MIN), INT_MAX),
	};
}

#define MARGIN_INIT                                                                      \
	{ 0, 0, 0, 0 }
