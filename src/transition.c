// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "compiler.h"
#include "transition.h"
#include "utils.h"

double animatable_get_progress(const struct animatable *a) {
	if (a->duration > 0) {
		return a->elapsed / a->duration;
	}
	return 1;
}

/// Get the current value of an `animatable`.
double animatable_get(const struct animatable *a) {
	if (a->duration > 0) {
		assert(a->elapsed < a->duration);
		double t = a->curve->sample(a->curve, animatable_get_progress(a));
		return (1 - t) * a->start + t * a->target;
	}
	return a->target;
}

/// Advance the animation by a given number of steps.
void animatable_advance(struct animatable *a, double elapsed) {
	if (a->duration == 0 || elapsed <= 0) {
		return;
	}

	assert(a->elapsed < a->duration);
	if (elapsed >= a->duration - a->elapsed) {
		a->elapsed = a->duration;
	} else {
		a->elapsed += elapsed;
	}

	if (a->elapsed == a->duration) {
		a->start = a->target;
		a->duration = 0;
		a->elapsed = 0;
		a->curve->free(a->curve);
		a->curve = NULL;
		if (a->callback) {
			a->callback(TRANSITION_COMPLETED, a->callback_data);
			a->callback = NULL;
			a->callback_data = NULL;
		}
	}
}

/// Returns whether an `animatable` is currently animating.
bool animatable_is_animating(const struct animatable *a) {
	assert(a->duration == 0 || a->elapsed < a->duration);
	return a->duration != 0;
}

/// Cancel the current animation of an `animatable`. This stops the animation and
/// the `animatable` will retain its current value.
///
/// Returns true if the `animatable` was animated before this function is called.
bool animatable_interrupt(struct animatable *a) {
	if (a->duration == 0) {
		return false;
	}

	a->start = animatable_get(a);
	a->target = a->start;
	a->duration = 0;
	a->elapsed = 0;
	a->curve->free(a->curve);
	a->curve = NULL;
	if (a->callback) {
		a->callback(TRANSITION_INTERRUPTED, a->callback_data);
		a->callback = NULL;
		a->callback_data = NULL;
	}
	return true;
}

/// Cancel the current animation of an `animatable` and set its value to its target.
///
/// Returns true if the `animatable` was animated before this function is called.
bool animatable_skip(struct animatable *a) {
	if (a->duration == 0) {
		return false;
	}

	a->start = a->target;
	a->duration = 0;
	a->elapsed = 0;
	a->curve->free(a->curve);
	a->curve = NULL;
	if (a->callback) {
		a->callback(TRANSITION_SKIPPED, a->callback_data);
		a->callback = NULL;
		a->callback_data = NULL;
	}
	return true;
}

/// Change the target value of an `animatable`.
/// If the `animatable` is already animating, the animation will be canceled first.
bool animatable_set_target(struct animatable *a, double target, double duration,
                           const struct curve *curve, transition_callback_fn cb, void *data) {
	animatable_interrupt(a);
	if (duration == 0 || a->start == target) {
		a->start = target;
		a->target = target;
		curve->free(curve);
		return false;
	}

	a->target = target;
	a->duration = duration;
	a->elapsed = 0;
	a->callback = cb;
	a->callback_data = data;
	a->curve = curve;
	return true;
}

/// Create a new animatable.
struct animatable animatable_new(double value) {
	struct animatable ret = {
	    .start = value,
	    .target = value,
	    .duration = 0,
	    .elapsed = 0,
	};
	return ret;
}

static double curve_sample_linear(const struct curve *this attr_unused, double progress) {
	return progress;
}

static void noop_free(const struct curve *this attr_unused) {
}

const struct curve *curve_new_linear(void) {
	static const struct curve ret = {
	    .sample = curve_sample_linear,
	    .free = noop_free,
	};
	return &ret;
}
