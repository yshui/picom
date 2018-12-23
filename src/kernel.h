// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include "compiler.h"

/// Code for generating convolution kernels

typedef struct conv {
	int size;
	double data[];
} conv;

/// Calculate the sum of a rectangle part of the convolution kernel
/// the rectangle is defined by top left (x, y), and a size (width x height)
double attr_const sum_kernel(const conv *map, int x, int y, int width, int height);

/// Create a kernel with gaussian distribution of radius r
conv *gaussian_kernel(double r);
