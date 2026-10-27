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

/// Sample the closed form solution of a mass-spring-damper system released from a unit
/// displacement with zero initial velocity, see
/// https://en.wikipedia.org/wiki/Mass-spring-damper_model
///
/// `progress` is the time elapsed, in units of the transition's duration. The system
/// starts fully displaced and decays towards its resting position, so the curve is the
/// displacement that has already been recovered, i.e. `1 - x(t)`.
static double curve_sample_spring(const struct curve_spring *this, double progress) {
	assert(progress >= 0 && progress <= 1);
	// The spring generally hasn't fully settled when the transition ends. Land
	// exactly on the end value instead of leaving the variable slightly off.
	if (progress == 1) {
		return 1;
	}

	double displacement;
	if (this->frequency == 0) {
		// Critically damped: x(t) = e^(-w0 t) * (1 + w0 t)
		displacement = exp(-this->decay * progress) * (1 + this->decay * progress);
	} else if (this->overdamped) {
		// Overdamped: x(t) = e^(-z w0 t) * (cosh(wa t) + z w0 / wa * sinh(wa t)).
		// Expanded into a sum of two decaying exponentials, because the
		// hyperbolic functions overflow long before their product with the
		// envelope does.
		double slow = this->decay - this->frequency,
		       fast = this->decay + this->frequency;
		displacement = (fast * exp(-slow * progress) - slow * exp(-fast * progress)) /
		               (2 * this->frequency);
	} else {
		// Underdamped: x(t) = e^(-z w0 t) * (cos(wd t) + z w0 / wd * sin(wd t))
		double phase = this->frequency * progress;
		displacement = exp(-this->decay * progress) *
		               (cos(phase) + this->decay / this->frequency * sin(phase));
	}
	return 1 - displacement;
}

static char *curve_spring_to_c(const struct curve_spring *this) {
	char *buf = NULL;
	casprintf(&buf,
	          "{.type = CURVE_SPRING, .spring = { .decay = %a, .frequency = %a, "
	          ".overdamped = %s }},",
	          this->decay, this->frequency, this->overdamped ? "true" : "false");
	return buf;
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

/// Parse a spring curve: spring(stiffness, damping, mass).
struct curve parse_spring(const char *input_str, const char **out_end, char **err) {
	// 0 = stiffness, 1 = damping, 2 = mass
	static const char *const names[] = {"stiffness", "damping", "mass"};
	double numbers[3];
	const char *str = input_str;
	*err = NULL;
	if (*str != '(') {
		casprintf(err, "Invalid spring %s.", str);
		return CURVE_INVALID_INIT;
	}
	str += 1;
	for (int i = 0; i < 3; i++) {
		str = skip_space(str);

		const char *end = NULL;
		numbers[i] = strtod_simple(str, &end);
		if (end == str) {
			casprintf(err, "Invalid spring %s at \"%s\".", names[i], str);
			return CURVE_INVALID_INIT;
		}
		// The damping coefficient may be 0 (an undamped spring oscillates
		// forever), stiffness and mass may not, they are divided by.
		if (i == 1 ? numbers[i] < 0 : numbers[i] <= 0) {
			casprintf(err, "Invalid spring %s at \"%s\". Must be %s.",
			          names[i], str, i == 1 ? "non-negative" : "positive");
			return CURVE_INVALID_INIT;
		}
		str = skip_space(end);
		const char expected = i == 2 ? ')' : ',';
		if (*str != expected) {
			casprintf(err, "Invalid spring argument list %s.", input_str);
			return CURVE_INVALID_INIT;
		}
		str += 1;
	}
	*out_end = str;
	return curve_new_spring(numbers[0], numbers[1], numbers[2]);
}

typedef struct curve (*curve_parser)(const char *str, const char **end, char **err);

static const struct {
	curve_parser parse;
	const char *name;
} curve_parsers[] = {
    {parse_cubic_bezier, "cubic-bezier"},
    {parse_linear, "linear"},
    {parse_steps, "steps"},
    {parse_spring, "spring"},
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
	case CURVE_SPRING: return curve_sample_spring(&curve->spring, progress);
	case CURVE_INVALID:
	default: unreachable();
	}
}

char *curve_to_c(const struct curve *curve) {
	switch (curve->type) {
	case CURVE_LINEAR: return curve_linear_to_c(curve);
	case CURVE_STEP: return curve_step_to_c(&curve->step);
	case CURVE_CUBIC_BEZIER: return curve_cubic_bezier_to_c(&curve->bezier);
	case CURVE_SPRING: return curve_spring_to_c(&curve->spring);
	case CURVE_INVALID:
	default: unreachable();
	}
}

#ifdef UNIT_TEST
#include <test.h>

TEST_CASE(spring_curve_parsing) {
	const char *end = NULL;
	char *err = NULL;

	// zeta = 25 / (2 * sqrt(200 * 1)) = 0.8838..., underdamped.
	// decay = c / 2m = 12.5, wd = sqrt(k/m - c^2/4m^2) = sqrt(43.75).
	auto under = curve_parse("spring(200, 25, 1)", &end, &err);
	TEST_EQUAL(err, NULL);
	TEST_EQUAL(under.type, CURVE_SPRING);
	TEST_EQUAL(under.spring.overdamped, false);
	TEST_TRUE(fabs(under.spring.decay - 12.5) < 1e-9);
	TEST_TRUE(fabs(under.spring.frequency - sqrt(43.75)) < 1e-9);
	TEST_STREQUAL(end, "");

	// zeta = 20 / (2 * sqrt(100 * 1)) = 1, critically damped.
	auto critical = curve_parse("spring(100, 20, 1)", &end, &err);
	TEST_EQUAL(err, NULL);
	TEST_EQUAL(critical.type, CURVE_SPRING);
	TEST_EQUAL(critical.spring.overdamped, false);
	TEST_TRUE(fabs(critical.spring.decay - 10) < 1e-9);
	TEST_EQUAL(critical.spring.frequency, 0);

	// zeta = 30 / (2 * sqrt(100 * 1)) = 1.5, overdamped.
	// decay = 15, wa = w0 * sqrt(zeta^2 - 1) = 10 * sqrt(1.25).
	auto over = curve_parse("spring(100, 30, 1)", &end, &err);
	TEST_EQUAL(err, NULL);
	TEST_EQUAL(over.type, CURVE_SPRING);
	TEST_EQUAL(over.spring.overdamped, true);
	TEST_TRUE(fabs(over.spring.decay - 15) < 1e-9);
	TEST_TRUE(fabs(over.spring.frequency - 10 * sqrt(1.25)) < 1e-9);

	// An undamped spring is legal, it just never settles.
	auto undamped = curve_parse("spring(100, 0, 1)", &end, &err);
	TEST_EQUAL(err, NULL);
	TEST_EQUAL(undamped.type, CURVE_SPRING);
	TEST_EQUAL(undamped.spring.decay, 0);
	TEST_TRUE(fabs(undamped.spring.frequency - 10) < 1e-9);

	// Mass scales both coefficients.
	auto heavy = curve_parse("spring( 100 , 20 , 4 )", &end, &err);
	TEST_EQUAL(err, NULL);
	TEST_EQUAL(heavy.type, CURVE_SPRING);
	TEST_TRUE(fabs(heavy.spring.decay - 2.5) < 1e-9);
}

TEST_CASE(spring_curve_parsing_errors) {
	static const char *const invalid[] = {
	    "spring(200, 25)",                 // too few arguments
	    "spring(200, 25, 1, true)",        // too many arguments
	    "spring(0, 25, 1)",                // zero stiffness
	    "spring(-1, 25, 1)",               // negative stiffness
	    "spring(200, -5, 1)",              // negative damping
	    "spring(200, 25, 0)",              // zero mass
	    "spring(200, 25, -1)",             // negative mass
	    "spring(a, 25, 1)",                // not a number
	    "spring 200, 25, 1)",              // missing parenthesis
	};
	for (size_t i = 0; i < ARR_SIZE(invalid); i++) {
		const char *end = NULL;
		char *err = NULL;
		auto curve = curve_parse(invalid[i], &end, &err);
		TEST_EQUAL(curve.type, CURVE_INVALID);
		TEST_NOTEQUAL(err, NULL);
		free(err);
	}
}

/// Integrate `m x'' + c x' + k x = 0` with `x(0) = 1, x'(0) = 0` up to `t` with RK4, and
/// return the recovered displacement `1 - x(t)`. Used as an independent reference for the
/// closed form solution.
static double spring_reference(double stiffness, double damping, double mass, double t) {
	static const double dt = 1e-4;
	double x = 1, v = 0;
	for (double elapsed = 0; elapsed < t; elapsed += dt) {
		double step = min2(dt, t - elapsed);
#define ACCEL(x_, v_) ((-stiffness * (x_) - damping * (v_)) / mass)
		double k1x = v, k1v = ACCEL(x, v);
		double k2x = v + step / 2 * k1v,
		       k2v = ACCEL(x + step / 2 * k1x, v + step / 2 * k1v);
		double k3x = v + step / 2 * k2v,
		       k3v = ACCEL(x + step / 2 * k2x, v + step / 2 * k2v);
		double k4x = v + step * k3v, k4v = ACCEL(x + step * k3x, v + step * k3v);
#undef ACCEL
		x += step / 6 * (k1x + 2 * k2x + 2 * k3x + k4x);
		v += step / 6 * (k1v + 2 * k2v + 2 * k3v + k4v);
	}
	return 1 - x;
}

TEST_CASE(spring_curve_sampling) {
	// Underdamped, critically damped, and overdamped respectively.
	static const double parameters[][3] = {
	    {200, 25, 1},
	    {100, 20, 1},
	    {100, 30, 1},
	};
	for (size_t i = 0; i < ARR_SIZE(parameters); i++) {
		auto curve =
		    curve_new_spring(parameters[i][0], parameters[i][1], parameters[i][2]);
		// Every curve starts at 0 and is truncated to land on 1.
		TEST_EQUAL(curve_sample(&curve, 0), 0);
		TEST_EQUAL(curve_sample(&curve, 1), 1);

		// And matches a numerical integration of the same system in between.
		for (int step = 1; step < 20; step++) {
			double t = step / 20.0;
			double expected = spring_reference(
			    parameters[i][0], parameters[i][1], parameters[i][2], t);
			TEST_TRUE(fabs(curve_sample(&curve, t) - expected) < 1e-6);
		}
	}

	// An underdamped spring overshoots its target...
	auto under = curve_new_spring(200, 10, 1);
	bool overshoots = false;
	for (int step = 1; step < 100; step++) {
		overshoots = overshoots || curve_sample(&under, step / 100.0) > 1;
	}
	TEST_TRUE(overshoots);

	// ... while critically damped and overdamped springs approach it monotonically.
	static const double monotonic[][3] = {{100, 20, 1}, {100, 30, 1}};
	for (size_t i = 0; i < ARR_SIZE(monotonic); i++) {
		auto curve =
		    curve_new_spring(monotonic[i][0], monotonic[i][1], monotonic[i][2]);
		double previous = 0;
		for (int step = 1; step < 100; step++) {
			double value = curve_sample(&curve, step / 100.0);
			TEST_TRUE(value > previous);
			TEST_TRUE(value <= 1);
			previous = value;
		}
	}
}

TEST_CASE(spring_curve_near_critical) {
	// The damped frequency vanishes as zeta approaches 1. Sampling must stay finite
	// on both sides of the discontinuity, and agree with the critically damped curve.
	auto critical = curve_new_spring(100, 20, 1);
	static const double offsets[] = {-1e-3, -1e-5, -1e-9, 0, 1e-9, 1e-5, 1e-3};
	for (size_t i = 0; i < ARR_SIZE(offsets); i++) {
		// zeta = c / (2 * sqrt(k * m)), so c = 20 * zeta for k = 100, m = 1.
		auto curve = curve_new_spring(100, 20 * (1 + offsets[i]), 1);
		for (int step = 0; step < 100; step++) {
			double value = curve_sample(&curve, step / 100.0);
			TEST_TRUE(isfinite(value));
			TEST_TRUE(fabs(value - curve_sample(&critical, step / 100.0)) < 1e-2);
		}
	}
}

/// Very stiff springs make the hyperbolic form of the overdamped solution overflow. The
/// equivalent sum-of-exponentials form we use must not.
TEST_CASE(spring_curve_extreme_parameters) {
	auto curve = curve_new_spring(1e8, 1e8, 1);
	TEST_EQUAL(curve.spring.overdamped, true);
	for (int step = 0; step <= 100; step++) {
		TEST_TRUE(isfinite(curve_sample(&curve, step / 100.0)));
	}
}
#endif
