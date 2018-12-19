// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <math.h>

#include "kernel.h"
#include "utils.h"

/*
 * A picture will help
 *
 *      -center   0                width  width+center
 *  -center +-----+-------------------+-----+
 *          |     |                   |     |
 *          |     |                   |     |
 *        0 +-----+-------------------+-----+
 *          |     |                   |     |
 *          |     |                   |     |
 *          |     |                   |     |
 *   height +-----+-------------------+-----+
 *          |     |                   |     |
 * height+  |     |                   |     |
 *  center  +-----+-------------------+-----+
 */

double attr_const attr_pure sum_kernel(const conv *map, int x, int y, int width,
                                       int height) {
	int fx, fy;
	const double *g_data;
	const double *g_line = map->data;
	int g_size = map->size;
	int center = g_size / 2;
	int fx_start, fx_end;
	int fy_start, fy_end;
	double v;

	/*
	 * Compute set of filter values which are "in range",
	 * that's the set with:
	 *    0 <= x + (fx-center) && x + (fx-center) < width &&
	 *  0 <= y + (fy-center) && y + (fy-center) < height
	 *
	 *  0 <= x + (fx - center)    x + fx - center < width
	 *  center - x <= fx    fx < width + center - x
	 */

	fx_start = center - x;
	if (fx_start < 0)
		fx_start = 0;
	fx_end = width + center - x;
	if (fx_end > g_size)
		fx_end = g_size;

	fy_start = center - y;
	if (fy_start < 0)
		fy_start = 0;
	fy_end = height + center - y;
	if (fy_end > g_size)
		fy_end = g_size;

	g_line = g_line + fy_start * g_size + fx_start;

	v = 0;

	for (fy = fy_start; fy < fy_end; fy++) {
		g_data = g_line;
		g_line += g_size;

		for (fx = fx_start; fx < fx_end; fx++) {
			v += *g_data++;
		}
	}

	if (v > 1)
		v = 1;

	return v;
}

static double attr_const attr_pure gaussian(double r, double x, double y) {
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
	t = 0.0;

	/*printf_errf("(): %f", r);*/
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			double g = gaussian(r, x - center, y - center);
			t += g;
			c->data[y * size + x] = g;
			/*printf("%f ", c->data[y*size+x]);*/
		}
		/*printf("\n");*/
	}

	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			c->data[y * size + x] /= t;
			/*printf("%f ", c->data[y*size+x]);*/
		}
		/*printf("\n");*/
	}

	return c;
}

// vim: set noet sw=8 ts=8 :
