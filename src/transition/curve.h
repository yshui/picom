// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <assert.h>
#include <math.h>
#include <stdbool.h>

enum curve_type {
	CURVE_LINEAR,
	CURVE_CUBIC_BEZIER,
	CURVE_STEP,
	CURVE_SPRING,
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
		/// A mass-spring-damper system, released from a unit displacement with
		/// zero initial velocity. Stored as the coefficients of its closed form
		/// solution, see `curve_new_spring`.
		struct curve_spring {
			/// The exponential decay rate of the envelope, i.e. the damping
			/// ratio times the undamped angular frequency (zeta * omega_0).
			double decay;
			/// The angular frequency of the oscillation, omega_0 *
			/// sqrt(|1 - zeta^2|). This is the damped frequency when
			/// underdamped, and the hyperbolic rate when overdamped. Zero
			/// when the system is critically damped.
			double frequency;
			/// Whether the system is overdamped (zeta > 1). Selects between
			/// the trigonometric and the hyperbolic solution.
			bool overdamped;
		} spring;
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
/// Create a spring curve from the physical parameters of a mass-spring-damper system:
/// the spring constant `stiffness`, the damping coefficient `damping`, and the `mass`
/// attached to the spring. Time is measured in units of the transition's duration, so
/// these parameters describe the shape of the curve, and `duration` decides how long it
/// takes to play out.
static inline struct curve curve_new_spring(double stiffness, double damping, double mass) {
	assert(stiffness > 0 && damping >= 0 && mass > 0);
	// The undamped angular frequency, and the damping ratio.
	double omega0 = sqrt(stiffness / mass);
	double zeta = damping / (2 * sqrt(stiffness * mass));
	// Near zeta == 1 the damped frequency vanishes and the underdamped and
	// overdamped solutions both degenerate into the critically damped one. Snap to
	// it, so we never divide by a frequency that is (almost) zero.
	if (fabs(zeta - 1) < 1e-6) {
		return (struct curve){
		    .type = CURVE_SPRING,
		    .spring = {.decay = omega0, .frequency = 0, .overdamped = false},
		};
	}
	return (struct curve){
	    .type = CURVE_SPRING,
	    .spring = {.decay = zeta * omega0,
	               .frequency = omega0 * sqrt(fabs(1 - zeta * zeta)),
	               .overdamped = zeta > 1},
	};
}
struct curve curve_parse(const char *str, const char **end, char **err);
/// Calculate the value of the curve at `progress`.
double curve_sample(const struct curve *curve, double progress);
char *curve_to_c(const struct curve *curve);
