// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <assert.h>
#include <stdbool.h>

enum curve_type {
	CURVE_LINEAR,
	CURVE_CUBIC_BEZIER,
	CURVE_STEP,
	CURVE_INVALID,
};

struct curve {
	enum curve_type type;
	union {
		struct curve_cubic_bezier {
			double ax, bx, cx;
			double ay, by, cy;
		} bezier;
		struct curve_step {
			int steps;
			bool jump_start, jump_end;
		} step;
	};
};

static const struct curve CURVE_LINEAR_INIT = {.type = CURVE_LINEAR};
static const struct curve CURVE_INVALID_INIT = {.type = CURVE_INVALID};

static inline struct curve curve_new_cubic_bezier(double x1, double y1, double x2, double y2) {
	double cx = 3. * x1;
	double bx = 3. * (x2 - x1) - cx;
	double cy = 3. * y1;
	double by = 3. * (y2 - y1) - cy;
	return (struct curve){
	    .type = CURVE_CUBIC_BEZIER,
	    .bezier = {.ax = 1. - cx - bx, .bx = bx, .cx = cx, .ay = 1. - cy - by, .by = by, .cy = cy},
	};
}
static inline struct curve curve_new_step(int steps, bool jump_start, bool jump_end) {
	assert(steps > 0);
	return (struct curve){
	    .type = CURVE_STEP,
	    .step = {.steps = steps, .jump_start = jump_start, .jump_end = jump_end},
	};
}
struct curve curve_parse(const char *str, const char **end, char **err);
/// Calculate the value of the curve at `progress`.
double curve_sample(const struct curve *curve, double progress);
char *curve_to_c(const struct curve *curve);
