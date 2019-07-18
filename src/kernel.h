// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdlib.h>
#include "compiler.h"

/// Code for generating convolution kernels

typedef struct conv {
	int w, h;
	double *rsum;
	double data[];
} conv;

/// Calculate the sum of a rectangle part of the convolution kernel
/// the rectangle is defined by top left (x, y), and a size (width x height)
double attr_pure sum_kernel(const conv *map, int x, int y, int width, int height);
double attr_pure sum_kernel_normalized(const conv *map, int x, int y, int width, int height);

/// Create a kernel with gaussian distribution with standard deviation `r`, and size
/// `size`.
conv *gaussian_kernel(double r, int size);

/// Create a gaussian kernel with auto detected standard deviation. The choosen standard
/// deviation tries to make sure the outer most pixels of the shadow are completely
/// transparent.
///
/// @param[in] shadow_radius the radius of the shadow
conv *gaussian_kernel_autodetect_deviation(int shadow_radius);

/// preprocess kernels to make shadow generation faster
/// shadow_sum[x*d+y] is the sum of the kernel from (0, 0) to (x, y), inclusive
void sum_kernel_preprocess(conv *map);

static inline void free_conv(conv *k) {
	free(k->rsum);
	free(k);
}
