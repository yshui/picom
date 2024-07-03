// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

/// Some common types

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

enum blur_method {
	BLUR_METHOD_NONE = 0,
	BLUR_METHOD_KERNEL,
	BLUR_METHOD_BOX,
	BLUR_METHOD_GAUSSIAN,
	BLUR_METHOD_DUAL_KAWASE,
	BLUR_METHOD_INVALID,
};

/// Enumeration type to represent switches.
typedef enum {
	OFF = 0,        // false
	ON,             // true
	UNSET
} switch_t;

enum tristate { TRI_FALSE = -1, TRI_UNKNOWN = 0, TRI_TRUE = 1 };

/// Return value if it's not TRI_UNKNOWN, otherwise return fallback.
static inline enum tristate tri_or(enum tristate value, enum tristate fallback) {
	return value ?: fallback;
}

static inline bool tri_or_bool(enum tristate value, bool fallback) {
	return value == TRI_UNKNOWN ? fallback : value == TRI_TRUE;
}

static inline enum tristate tri_from_bool(bool value) {
	return value ? TRI_TRUE : TRI_FALSE;
}

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

struct ibox {
	ivec2 origin;
	ivec2 size;
};

static const vec2 SCALE_IDENTITY = {1.0, 1.0};

static inline vec2 ivec2_as(ivec2 a) {
	return (vec2){
	    .x = a.x,
	    .y = a.y,
	};
}

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

static inline vec2 vec2_add(vec2 a, vec2 b) {
	return (vec2){
	    .x = a.x + b.x,
	    .y = a.y + b.y,
	};
}

static inline vec2 vec2_ceil(vec2 a) {
	return (vec2){
	    .x = ceil(a.x),
	    .y = ceil(a.y),
	};
}

static inline vec2 vec2_floor(vec2 a) {
	return (vec2){
	    .x = floor(a.x),
	    .y = floor(a.y),
	};
}

static inline bool vec2_eq(vec2 a, vec2 b) {
	return a.x == b.x && a.y == b.y;
}

static inline vec2 vec2_scale(vec2 a, vec2 scale) {
	return (vec2){
	    .x = a.x * scale.x,
	    .y = a.y * scale.y,
	};
}

/// Check if two boxes have a non-zero intersection area.
static inline bool ibox_overlap(struct ibox a, struct ibox b) {
	if (a.size.width <= 0 || a.size.height <= 0 || b.size.width <= 0 || b.size.height <= 0) {
		return false;
	}
	if (a.origin.x <= INT_MAX - a.size.width && a.origin.y <= INT_MAX - a.size.height &&
	    (a.origin.x + a.size.width <= b.origin.x ||
	     a.origin.y + a.size.height <= b.origin.y)) {
		return false;
	}
	if (b.origin.x <= INT_MAX - b.size.width && b.origin.y <= INT_MAX - b.size.height &&
	    (b.origin.x + b.size.width <= a.origin.x ||
	     b.origin.y + b.size.height <= a.origin.y)) {
		return false;
	}
	return true;
}

static inline bool ibox_eq(struct ibox a, struct ibox b) {
	return ivec2_eq(a.origin, b.origin) && ivec2_eq(a.size, b.size);
}

static inline ivec2 ivec2_scale_ceil(ivec2 a, vec2 scale) {
	vec2 scaled = vec2_scale(ivec2_as(a), scale);
	return vec2_as(vec2_ceil(scaled));
}

static inline ivec2 ivec2_scale_floor(ivec2 a, vec2 scale) {
	vec2 scaled = vec2_scale(ivec2_as(a), scale);
	return vec2_as(vec2_floor(scaled));
}

#define MARGIN_INIT                                                                      \
	{ 0, 0, 0, 0 }
