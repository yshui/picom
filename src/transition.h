// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdbool.h>
#include "compiler.h"

struct animatable;
enum transition_event;

/// Callback when the transition state changes. Callback might be called by:
///   - `animatable_set_target` generates TRANSITION_COMPLETED when the specified duration
///     is 0. also generates TRANSITION_CANCELLED if the animatable was already animating.
///   - `animatable_cancel` generates TRANSITION_CANCELED
///   - `animatable_early_stop` generates TRANSITION_STOPPED_EARLY
///   - `animatable_step` generates TRANSITION_COMPLETED when the animation is completed.
/// Callback is guaranteed to be called exactly once for each `animatable_set_target`
/// call, unless an animatable is freed before the transition is completed.
typedef void (*transition_callback_fn)(enum transition_event event, void *data);

enum transition_event {
	TRANSITION_COMPLETED,
	TRANSITION_INTERRUPTED,
	TRANSITION_SKIPPED,
};

/// The base type for step_state.
struct step_state_base {
	/// The current value of the `animatable`.
	/// If the `animatable` is not animated, this equals to `animatable->target`.
	double current;
};

struct curve {
	/// The interpolator function for an animatable. This function should calculate
	/// the current value of the `animatable` based on its `start`, `target`,
	/// `duration` and `progress`.
	double (*sample)(const struct curve *this, double progress);
	/// Free the interpolator.
	void (*free)(const struct curve *this);
};

/// An animatable value
struct animatable {
	/// The starting value.
	/// When this `animatable` is not animated, this is the current value.
	double start;
	/// The target value.
	/// If the `animatable` is not animated, this equals to `start`.
	double target;
	/// The animation duration in unspecified units.
	/// If the `animatable` is not animated, this is 0.
	double duration;
	/// The current progress of the animation in the same units as `duration`.
	/// If the `animatable` is not animated, this is 0.
	double elapsed;

	transition_callback_fn callback;
	void *callback_data;

	/// The function for calculating the current value. If
	/// `step_state` is not NULL, the `step` function is used;
	/// otherwise, the `interpolator` function is used.
	/// The interpolator function.
	const struct curve *curve;
};

// =============================== API ===============================

/// Get the current value of an `animatable`.
double animatable_get(const struct animatable *a);
/// Get the animation progress as a percentage of the total duration.
double animatable_get_progress(const struct animatable *a);
/// Advance the animation by a given amount. `elapsed` cannot be negative.
void animatable_advance(struct animatable *a, double elapsed);
/// Returns whether an `animatable` is currently animating.
bool animatable_is_animating(const struct animatable *a);
/// Interrupt the current animation of an `animatable`. This stops the animation and
/// the `animatable` will retain its current value.
///
/// Returns true if the `animatable` was animated before this function is called.
bool animatable_interrupt(struct animatable *a);
/// Skip the current animation of an `animatable` and set its value to its target.
///
/// Returns true if the `animatable` was animated before this function is called.
bool animatable_skip(struct animatable *a);
/// Change the target value of an `animatable`. Specify a duration, an interpolator
/// function, and a callback function.
///
/// If the `animatable` is already animating, the animation will be canceled first.
///
/// Note, In some cases this function does not start the animation, for example, if the
/// target value is the same as the current value of the animatable, or if the duration is
/// 0. If the animation is not started, the callback function will not be called. The
/// animatable's current animation, if it has one, will be canceled regardless.
///
/// Returns if the animatable is now animated.
bool animatable_set_target(struct animatable *a, double target, double duration,
                           const struct curve *curve, transition_callback_fn cb, void *data);
/// Create a new animatable.
struct animatable animatable_new(double value);

// ========================== Interpolators ==========================

const struct curve *curve_new_linear(void);
const struct curve *curve_new_cubic_bezier(double x1, double y1, double x2, double y2);
const struct curve *curve_new_step(int steps, bool jump_start, bool jump_end);
const struct curve *curve_parse(const char *str, const char **end, char **err);
