// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdbool.h>

struct animatable;
enum transition_event;
/// The interpolator function for an animatable. This function should calculate the
/// current value of the `animatable` based on its `start`, `target`, `duration` and
/// `progress`.
typedef double (*interpolator_fn)(const struct animatable *);
/// The step function for an animatable. This function should advance the animation state
/// by one step. This function is called _after_ `progress` is incremented. If `progress`
/// is 0 when the function is called, it means an animation has just started, and this
/// function should initialize the state. If `step_state` is NULL when this function is
/// called, this function should allocate and initialize `step_state`.
/// `steps` is the number of steps to advance. This is always 1 or more, unless `progress`
/// is 0 or `step_state` is NULL, in which case `steps` is 0.
typedef void (*step_fn)(struct animatable *, unsigned int steps);

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
	TRANSITION_CANCELED,
	TRANSITION_STOPPED_EARLY,
};

/// The base type for step_state.
struct step_state_base {
	/// The current value of the `animatable`.
	/// If the `animatable` is not animated, this equals to `animatable->target`.
	double current;
};

/// An animatable value
struct animatable {
	/// The starting value.
	/// When this `animatable` is not animated, this is the current value.
	double start;
	/// The target value.
	/// If the `animatable` is not animated, this equals to `start`.
	double target;
	/// The animation duration in number of steps.
	/// If the `animatable` is not animated, this is 0.
	unsigned int duration;
	/// The current progress of the animation. From 0 to `duration - 1`.
	/// If the `animatable` is not animated, this is 0.
	unsigned int progress;

	transition_callback_fn callback;
	void *callback_data;

	/// Step function state.
	struct step_state_base *step_state;
	/// The function for calculating the current value. If
	/// `step_state` is not NULL, the `step` function is used;
	/// otherwise, the `interpolator` function is used.
	union {
		/// The interpolator function.
		interpolator_fn interpolator;
		/// The step function.
		step_fn step;
	};
};

// =============================== API ===============================

/// Get the current value of an `animatable`.
double animatable_get(const struct animatable *a);
/// Get the animation progress as a percentage of the total duration.
double animatable_get_progress(const struct animatable *a);
/// Advance the animation by a given number of steps.
void animatable_step(struct animatable *a, unsigned int steps);
/// Returns whether an `animatable` is currently animating.
bool animatable_is_animating(const struct animatable *a);
/// Cancel the current animation of an `animatable`. This stops the animation and
/// the `animatable` will retain its current value.
///
/// Returns true if the `animatable` was animated before this function is called.
bool animatable_cancel(struct animatable *a);
/// Cancel the current animation of an `animatable` and set its value to its target.
///
/// Returns true if the `animatable` was animated before this function is called.
bool animatable_early_stop(struct animatable *a);
/// Change the target value of an `animatable`.
/// If the `animatable` is already animating, the animation will be canceled first.
void animatable_set_target(struct animatable *a, double target, unsigned int duration,
                           transition_callback_fn cb, void *data);
/// Create a new animatable.
struct animatable animatable_new(double value, interpolator_fn interpolator, step_fn step);

// ========================== Interpolators ==========================

double linear_interpolator(const struct animatable *a);
