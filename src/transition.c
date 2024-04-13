// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "compiler.h"
#include "transition.h"
#include "utils.h"

/// Get the current value of an `animatable`.
double animatable_get(const struct animatable *a) {
	if (a->duration) {
		assert(a->progress < a->duration);
		return a->interpolator->interpolate(a->interpolator, a);
	}
	return a->target;
}

double animatable_get_progress(const struct animatable *a) {
	if (a->duration) {
		return (double)a->progress / a->duration;
	}
	return 1;
}

/// Advance the animation by a given number of steps.
void animatable_step(struct animatable *a, unsigned int steps) {
	if (!a->duration || !steps) {
		return;
	}

	assert(a->progress < a->duration);
	if (steps > a->duration - a->progress) {
		steps = a->duration - a->progress;
	}
	a->progress += steps;

	if (a->progress == a->duration) {
		a->start = a->target;
		a->duration = 0;
		a->progress = 0;
		a->interpolator->free(a->interpolator);
		a->interpolator = NULL;
		if (a->callback) {
			a->callback(TRANSITION_COMPLETED, a->callback_data);
			a->callback = NULL;
			a->callback_data = NULL;
		}
	}
}

/// Returns whether an `animatable` is currently animating.
bool animatable_is_animating(const struct animatable *a) {
	assert(!a->duration || a->progress < a->duration);
	return a->duration;
}

/// Cancel the current animation of an `animatable`. This stops the animation and
/// the `animatable` will retain its current value.
///
/// Returns true if the `animatable` was animated before this function is called.
bool animatable_cancel(struct animatable *a) {
	if (!a->duration) {
		return false;
	}

	a->start = animatable_get(a);
	a->target = a->start;
	a->duration = 0;
	a->progress = 0;
	a->interpolator->free(a->interpolator);
	a->interpolator = NULL;
	if (a->callback) {
		a->callback(TRANSITION_CANCELED, a->callback_data);
		a->callback = NULL;
		a->callback_data = NULL;
	}
	return true;
}

/// Cancel the current animation of an `animatable` and set its value to its target.
///
/// Returns true if the `animatable` was animated before this function is called.
bool animatable_early_stop(struct animatable *a) {
	if (!a->duration) {
		return false;
	}

	a->start = a->target;
	a->duration = 0;
	a->progress = 0;
	a->interpolator->free(a->interpolator);
	a->interpolator = NULL;
	if (a->callback) {
		a->callback(TRANSITION_STOPPED_EARLY, a->callback_data);
		a->callback = NULL;
		a->callback_data = NULL;
	}
	return true;
}

/// Change the target value of an `animatable`.
/// If the `animatable` is already animating, the animation will be canceled first.
void animatable_set_target(struct animatable *a, double target, unsigned int duration,
                           const struct interpolator *interpolator,
                           transition_callback_fn cb, void *data) {
	animatable_cancel(a);
	if (!duration) {
		a->start = target;
		a->target = target;
		if (cb) {
			cb(TRANSITION_COMPLETED, data);
		}
		interpolator->free(interpolator);
		return;
	}

	a->target = target;
	a->duration = duration;
	a->progress = 0;
	a->callback = cb;
	a->callback_data = data;
	a->interpolator = interpolator;
}

/// Create a new animatable.
struct animatable animatable_new(double value) {
	struct animatable ret = {
	    .start = value,
	    .target = value,
	    .duration = 0,
	    .progress = 0,
	};
	return ret;
}

static double
linear_interpolator(const struct interpolator *this attr_unused, const struct animatable *a) {
	double t = (double)a->progress / a->duration;
	return (1 - t) * a->start + t * a->target;
}

static void noop_free(const struct interpolator *this attr_unused) {
}

const struct interpolator *linear_interpolator_new(void) {
	static const struct interpolator ret = {
	    .interpolate = linear_interpolator,
	    .free = noop_free,
	};
	return &ret;
}
