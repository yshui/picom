// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <assert.h>
#include <math.h>

#include "compiler.h"
#include "kernel.h"
#include "log.h"
#include "utils.h"

/// Sum a region convolution kernel. Region is defined by a width x height rectangle whose
/// top left corner is at (x, y)
double sum_kernel(const conv *map, int x, int y, int width, int height) {
	double ret = 0;

	// Compute sum of values which are "in range"
	int xstart = normalize_i_range(x, 0, map->w),
	    xend = normalize_i_range(width + x, 0, map->w);
	int ystart = normalize_i_range(y, 0, map->h),
	    yend = normalize_i_range(height + y, 0, map->h);
	assert(yend >= ystart && xend >= xstart);

	int d = map->w;
	if (map->rsum) {
		// See sum_kernel_preprocess
		double v1 = xstart ? map->rsum[(yend - 1) * d + xstart - 1] : 0;
		double v2 = ystart ? map->rsum[(ystart - 1) * d + xend - 1] : 0;
		double v3 = (xstart && ystart) ? map->rsum[(ystart - 1) * d + xstart - 1] : 0;
		return map->rsum[(yend - 1) * d + xend - 1] - v1 - v2 + v3;
	}

	for (int yi = ystart; yi < yend; yi++) {
		for (int xi = xstart; xi < xend; xi++) {
			ret += map->data[yi * d + xi];
		}
	}

	return ret;
}

double sum_kernel_normalized(const conv *map, int x, int y, int width, int height) {
	double ret = sum_kernel(map, x, y, width, height);
	if (ret < 0) {
		ret = 0;
	}
	if (ret > 1) {
		ret = 1;
	}
	return ret;
}

static inline double attr_const gaussian(double r, double x, double y) {
	// Formula can be found here:
	// https://en.wikipedia.org/wiki/Gaussian_blur#Mathematics
	// Except a special case for r == 0 to produce sharp shadows
	if (r == 0)
		return 1;
	return exp(-0.5 * (x * x + y * y) / (r * r)) / (2 * M_PI * r * r);
}

conv *gaussian_kernel(double r, int size) {
	conv *c;
	int center = size / 2;
	double t;
	assert(size % 2 == 1);

	c = cvalloc(sizeof(conv) + (size_t)(size * size) * sizeof(double));
	c->w = c->h = size;
	c->rsum = NULL;
	t = 0.0;

	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			double g = gaussian(r, x - center, y - center);
			t += g;
			c->data[y * size + x] = g;
		}
	}

	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			c->data[y * size + x] /= t;
		}
	}

	return c;
}

/// Estimate the element of the sum of the first row in a gaussian kernel with standard
/// deviation `r` and size `size`,
static inline double estimate_first_row_sum(double size, double r) {
	// `factor` is integral of gaussian from -size to size
	double factor = erf(size / r / sqrt(2));
	// `a` is gaussian at (size, 0)
	double a = exp(-0.5 * size * size / (r * r)) / sqrt(2 * M_PI) / r;
	// The sum of the whole kernel is normalized to 1, i.e. each element is divided by
	// factor sqaured. So the sum of the first row is a * factor / factor^2 = a /
	// factor
	return a / factor;
}

/// Pick a suitable gaussian kernel standard deviation for a given kernel size. The
/// returned radius is the maximum possible radius (<= size*2) that satisfies no sum of
/// the rows in the kernel are less than `row_limit` (up to certain precision).
double gaussian_kernel_std_for_size(double size, double row_limit) {
	assert(size > 0);
	if (row_limit >= 1.0 / 2.0 / size) {
		return size * 2;
	}
	double l = 0, r = size * 2;
	while (r - l > 1e-2) {
		double mid = (l + r) / 2.0;
		double vmid = estimate_first_row_sum(size, mid);
		if (vmid > row_limit) {
			r = mid;
		} else {
			l = mid;
		}
	}
	return (l + r) / 2.0;
}

/// Create a gaussian kernel with auto detected standard deviation. The choosen standard
/// deviation tries to make sure the outer most pixels of the shadow are completely
/// transparent, so the transition from shadow to the background is smooth.
///
/// @param[in] shadow_radius the radius of the shadow
conv *gaussian_kernel_autodetect_deviation(double shadow_radius) {
	assert(shadow_radius >= 0);
	int size = (int)(shadow_radius * 2 + 1);

	if (shadow_radius == 0) {
		return gaussian_kernel(0, size);
	}
	double std = gaussian_kernel_std_for_size(shadow_radius, 0.5 / 256.0);
	return gaussian_kernel(std, size);
}

/// preprocess kernels to make shadow generation faster
/// shadow_sum[x*d+y] is the sum of the kernel from (0, 0) to (x, y), inclusive
void sum_kernel_preprocess(conv *map) {
	if (map->rsum) {
		free(map->rsum);
	}

	auto sum = map->rsum = ccalloc(map->w * map->h, double);
	sum[0] = map->data[0];

	for (int x = 1; x < map->w; x++) {
		sum[x] = sum[x - 1] + map->data[x];
	}

	const int d = map->w;
	for (int y = 1; y < map->h; y++) {
		sum[y * d] = sum[(y - 1) * d] + map->data[y * d];
		for (int x = 1; x < map->w; x++) {
			double tmp = sum[(y - 1) * d + x] + sum[y * d + x - 1] -
			             sum[(y - 1) * d + x - 1];
			sum[y * d + x] = tmp + map->data[y * d + x];
		}
	}
}

// vim: set noet sw=8 ts=8 :
