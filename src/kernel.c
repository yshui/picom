// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <assert.h>
#include <math.h>

#include "compiler.h"
#include "kernel.h"
#include "utils.h"

/// Sum a region convolution kernel. Region is defined by a width x height rectangle whose
/// top left corner is at (x, y)
double sum_kernel(const conv *map, int x, int y, int width, int height) {
	double ret = 0;
	/*
	 * Compute set of filter values which are "in range"
	 */

	int xstart = x;
	if (xstart < 0)
		xstart = 0;
	int xend = width + x;
	if (xend > map->size)
		xend = map->size;

	int ystart = y;
	if (ystart < 0)
		ystart = 0;
	int yend = height + y;
	if (yend > map->size)
		yend = map->size;

	assert(yend > 0 && xend > 0);

	int d = map->size;
	if (map->rsum) {
		double v1 = xstart ? map->rsum[(yend - 1) * d + xstart - 1] : 0;
		double v2 = ystart ? map->rsum[(ystart - 1) * d + xend - 1] : 0;
		double v3 =
		    (xstart && ystart) ? map->rsum[(ystart - 1) * d + xstart - 1] : 0;
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

static double attr_const gaussian(double r, double x, double y) {
	// Formula can be found here:
	// https://en.wikipedia.org/wiki/Gaussian_blur#Mathematics
	// Except a special case for r == 0 to produce sharp shadows
	if (r == 0)
		return 1;
	return exp(-0.5 * (x * x + y * y) / (r * r)) / (2 * M_PI * r * r);
}

conv *gaussian_kernel(double r) {
	conv *c;
	int size = r * 2 + 1;
	int center = size / 2;
	double t;

	c = cvalloc(sizeof(conv) + size * size * sizeof(double));
	c->size = size;
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

/// preprocess kernels to make shadow generation faster
/// shadow_sum[x*d+y] is the sum of the kernel from (0, 0) to (x, y), inclusive
void shadow_preprocess(conv *map) {
	const int d = map->size;

	if (map->rsum)
		free(map->rsum);

	auto sum = map->rsum = ccalloc(d * d, double);
	sum[0] = map->data[0];

	for (int x = 1; x < d; x++) {
		sum[x] = sum[x - 1] + map->data[x];
	}

	for (int y = 1; y < d; y++) {
		sum[y * d] = sum[(y - 1) * d] + map->data[y * d];
		for (int x = 1; x < d; x++) {
			double tmp = sum[(y - 1) * d + x] + sum[y * d + x - 1] -
			             sum[(y - 1) * d + x - 1];
			sum[y * d + x] = tmp + map->data[y * d + x];
		}
	}
}

// vim: set noet sw=8 ts=8 :
