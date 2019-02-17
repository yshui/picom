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

	// Compute sum of values which are "in range"
	int xstart = x, xend = width + x;
	if (xstart < 0) {
		xstart = 0;
	}
	if (xend > map->w) {
		xend = map->w;
	}
	int ystart = y, yend = height + y;
	if (ystart < 0) {
		ystart = 0;
	}
	if (yend > map->h) {
		yend = map->h;
	}
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
