// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdbool.h>
#include "compiler.h"

// ========================== Interpolators ==========================

struct curve {
	/// The interpolator function for an animatable. This function should calculate
	/// the current value of the `animatable` based on its `start`, `target`,
	/// `duration` and `progress`.
	double (*sample)(const struct curve *this, double progress);
	/// Free the interpolator.
	void (*free)(const struct curve *this);
};

const struct curve *curve_new_linear(void);
const struct curve *curve_new_cubic_bezier(double x1, double y1, double x2, double y2);
const struct curve *curve_new_step(int steps, bool jump_start, bool jump_end);
const struct curve *curve_parse(const char *str, const char **end, char **err);
