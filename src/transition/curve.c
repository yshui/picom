// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "compiler.h"
#include "utils/misc.h"
#include "utils/str.h"

#include "curve.h"

static double curve_sample_linear(const struct curve *this attr_unused, double progress) {
	return progress;
}

static char *curve_linear_to_c(const struct curve * /*this*/) {
	return strdup("{.type = CURVE_LINEAR},");
}

// Cubic bezier interpolator.
//
// Stolen from servo:
// https://searchfox.org/mozilla-central/rev/5da2d56d12/servo/components/style/bezier.rs

static inline double cubic_bezier_sample_x(const struct curve_cubic_bezier *self, double t) {
	return ((self->ax * t + self->bx) * t + self->cx) * t;
}

static inline double cubic_bezier_sample_y(const struct curve_cubic_bezier *self, double t) {
	return ((self->ay * t + self->by) * t + self->cy) * t;
}

static inline double
cubic_bezier_sample_derivative_x(const struct curve_cubic_bezier *self, double t) {
	return (3.0 * self->ax * t + 2.0 * self->bx) * t + self->cx;
}

// Solve for the `t` in cubic bezier function that corresponds to `x`
static inline double cubic_bezier_solve_x(const struct curve_cubic_bezier *this, double x) {
	static const int NEWTON_METHOD_ITERATIONS = 8;
	double t = x;
	// Fast path: try Newton's method.
	for (int i = 0; i < NEWTON_METHOD_ITERATIONS; i++) {
		double x2 = cubic_bezier_sample_x(this, t);
		if (fabs(x2 - x) < 1e-7) {
			return t;
		}
		double dx = cubic_bezier_sample_derivative_x(this, t);
		if (fabs(dx) < 1e-6) {
			break;
		}
		t -= (x2 - x) / dx;
	}

	// Slow path: Use bisection.
	double low = 0.0, high = 1.0;
	t = x;
	while (high - low > 1e-7) {
		double x2 = cubic_bezier_sample_x(this, t);
		if (fabs(x2 - x) < 1e-7) {
			return t;
		}
		if (x > x2) {
			low = t;
		} else {
			high = t;
		}
		t = (high - low) / 2.0 + low;
	}
	return t;
}

static double
curve_sample_cubic_bezier(const struct curve_cubic_bezier *curve, double progress) {
	assert(progress >= 0 && progress <= 1);
	if (progress == 0 || progress == 1) {
		return progress;
	}
	double t = cubic_bezier_solve_x(curve, progress);
	return cubic_bezier_sample_y(curve, t);
}

static char *curve_cubic_bezier_to_c(const struct curve_cubic_bezier *curve) {
	char *buf = NULL;
	casprintf(&buf,
	          "{.type = CURVE_CUBIC_BEZIER, .bezier = { .ax = %a, .bx = %a, "
	          ".cx = %a, .ay = %a, .by = %a, .cy = %a }},",
	          curve->ax, curve->bx, curve->cx, curve->ay, curve->by, curve->cy);
	return buf;
}

static double curve_sample_step(const struct curve_step *this, double progress) {
	double y_steps = this->steps - 1 + this->jump_end + this->jump_start,
	       x_steps = this->steps;
	if (progress == 1) {
		return 1;
	}
	if (progress == 0) {
		return this->jump_start ? 1 / y_steps : 0;
	}

	double scaled = progress * x_steps;
	double quantized = this->jump_start ? ceil(scaled) : floor(scaled);
	return quantized / y_steps;
}

static char *curve_step_to_c(const struct curve_step *this) {
	char *buf = NULL;
	casprintf(&buf,
	          "{.type = CURVE_STEP, .step = { .steps = %d, .jump_start = %s, "
	          ".jump_end = %s }},",
	          this->steps, this->jump_start ? "true" : "false",
	          this->jump_end ? "true" : "false");
	return buf;
}

struct curve parse_linear(const char *str, const char **end, char **err) {
	*end = str;
	*err = NULL;
	return CURVE_LINEAR_INIT;
}

struct curve parse_steps(const char *input_str, const char **out_end, char **err) {
	const char *str = input_str;
	*err = NULL;
	if (*str != '(') {
		casprintf(err, "Invalid steps %s.", str);
		return CURVE_INVALID_INIT;
	}
	str += 1;
	str = skip_space(str);
	char *end;
	auto steps = strtol(str, &end, 10);
	if (end == str || steps > INT_MAX) {
		casprintf(err, "Invalid step count at \"%s\".", str);
		return CURVE_INVALID_INIT;
	}
	str = skip_space(end);
	if (*str != ',') {
		casprintf(err, "Invalid steps argument list \"%s\".", input_str);
		return CURVE_INVALID_INIT;
	}
	str = skip_space(str + 1);
	bool jump_start =
	    starts_with(str, "jump-start", true) || starts_with(str, "jump-both", true);
	bool jump_end =
	    starts_with(str, "jump-end", true) || starts_with(str, "jump-both", true);
	if (!jump_start && !jump_end && !starts_with(str, "jump-none", true)) {
		casprintf(err, "Invalid jump setting for steps \"%s\".", str);
		return CURVE_INVALID_INIT;
	}
	str += jump_start ? (jump_end ? 9 : 10) : (jump_end ? 8 : 9);
	str = skip_space(str);
	if (*str != ')') {
		casprintf(err, "Invalid steps argument list \"%s\".", input_str);
		return CURVE_INVALID_INIT;
	}
	*out_end = str + 1;
	return curve_new_step((int)steps, jump_start, jump_end);
}

struct curve parse_cubic_bezier(const char *input_str, const char **out_end, char **err) {
	double numbers[4];
	const char *str = input_str;
	if (*str != '(') {
		casprintf(err, "Invalid cubic-bazier %s.", str);
		return CURVE_INVALID_INIT;
	}
	str += 1;
	for (int i = 0; i < 4; i++) {
		str = skip_space(str);

		const char *end = NULL;
		numbers[i] = strtod_simple(str, &end);
		if (end == str) {
			casprintf(err, "Invalid number %s.", str);
			return CURVE_INVALID_INIT;
		}
		str = skip_space(end);
		const char expected = i == 3 ? ')' : ',';
		if (*str != expected) {
			casprintf(err, "Invalid cubic-bazier argument list %s.", input_str);
			return CURVE_INVALID_INIT;
		}
		str += 1;
	}
	*out_end = str;
	return curve_new_cubic_bezier(numbers[0], numbers[1], numbers[2], numbers[3]);
}

typedef struct curve (*curve_parser)(const char *str, const char **end, char **err);

static const struct {
	curve_parser parse;
	const char *name;
} curve_parsers[] = {
    {parse_cubic_bezier, "cubic-bezier"},
    {parse_linear, "linear"},
    {parse_steps, "steps"},
};

struct curve curve_parse(const char *str, const char **end, char **err) {
	str = skip_space(str);
	for (size_t i = 0; i < ARR_SIZE(curve_parsers); i++) {
		auto name_len = strlen(curve_parsers[i].name);
		if (strncasecmp(str, curve_parsers[i].name, name_len) == 0) {
			return curve_parsers[i].parse(str + name_len, end, err);
		}
	}
	casprintf(err, "Unknown curve type \"%s\".", str);
	return CURVE_INVALID_INIT;
}

double curve_sample(const struct curve *curve, double progress) {
	switch (curve->type) {
	case CURVE_LINEAR: return curve_sample_linear(curve, progress);
	case CURVE_STEP: return curve_sample_step(&curve->step, progress);
	case CURVE_CUBIC_BEZIER:
		return curve_sample_cubic_bezier(&curve->bezier, progress);
	case CURVE_INVALID:
	default: unreachable();
	}
}

char *curve_to_c(const struct curve *curve) {
	switch (curve->type) {
	case CURVE_LINEAR: return curve_linear_to_c(curve);
	case CURVE_STEP: return curve_step_to_c(&curve->step);
	case CURVE_CUBIC_BEZIER: return curve_cubic_bezier_to_c(&curve->bezier);
	case CURVE_INVALID:
	default: unreachable();
	}
}
