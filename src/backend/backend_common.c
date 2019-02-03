// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <string.h>
#include <xcb/xcb_image.h>
#include <xcb/render.h>
#include <xcb/xcb_renderutil.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "kernel.h"
#include "common.h"
#include "log.h"
#include "x.h"
#include "win.h"

/**
 * Generate a 1x1 <code>Picture</code> of a particular color.
 */
xcb_render_picture_t
solid_picture(session_t *ps, bool argb, double a, double r, double g, double b) {
	xcb_pixmap_t pixmap;
	xcb_render_picture_t picture;
	xcb_render_create_picture_value_list_t pa;
	xcb_render_color_t col;
	xcb_rectangle_t rect;

	pixmap = x_create_pixmap(ps->c, argb ? 32 : 8, ps->root, 1, 1);
	if (!pixmap)
		return XCB_NONE;

	pa.repeat = 1;
	picture = x_create_picture_with_standard_and_pixmap(
	    ps->c, argb ? XCB_PICT_STANDARD_ARGB_32 : XCB_PICT_STANDARD_A_8, pixmap,
	    XCB_RENDER_CP_REPEAT, &pa);

	if (!picture) {
		xcb_free_pixmap(ps->c, pixmap);
		return XCB_NONE;
	}

	col.alpha = a * 0xffff;
	col.red = r * 0xffff;
	col.green = g * 0xffff;
	col.blue = b * 0xffff;

	rect.x = 0;
	rect.y = 0;
	rect.width = 1;
	rect.height = 1;

	xcb_render_fill_rectangles(ps->c, XCB_RENDER_PICT_OP_SRC, picture, col, 1, &rect);
	xcb_free_pixmap(ps->c, pixmap);

	return picture;
}

xcb_image_t *make_shadow(xcb_connection_t *c, const conv *kernel,
                         double opacity, int width, int height) {
	/*
	 * We classify shadows into 4 kinds of regions
	 *    r = shadow radius
	 * (0, 0) is the top left of the window itself
	 *         -r     r      width-r  width+r
	 *       -r +-----+---------+-----+
	 *          |  1  |    2    |  1  |
	 *        r +-----+---------+-----+
	 *          |  2  |    3    |  2  |
	 * height-r +-----+---------+-----+
	 *          |  1  |    2    |  1  |
	 * height+r +-----+---------+-----+
	 */
	xcb_image_t *ximage;
	const double *shadow_sum = kernel->rsum;
	int d = kernel->size, r = d / 2;
	int swidth = width + r * 2, sheight = height + r * 2;

	assert(d % 2 == 1);
	assert(d > 0);

	ximage = xcb_image_create_native(c, swidth, sheight, XCB_IMAGE_FORMAT_Z_PIXMAP, 8,
	                                 0, 0, NULL);
	if (!ximage) {
		log_error("failed to create an X image");
		return 0;
	}

	unsigned char *data = ximage->data;
	uint32_t sstride = ximage->stride;

	// If the window body is smaller than the kernel, we do convolution directly
	if (width < r * 2 && height < r * 2) {
		for (int y = 0; y < sheight; y++) {
			for (int x = 0; x < swidth; x++) {
				double sum = sum_kernel_normalized(
				    kernel, d - x - 1, d - y - 1, width, height);
				data[y * sstride + x] = sum * 255.0;
			}
		}
		return ximage;
	}

	if (height < r * 2) {
		// If the window height is smaller than the kernel, we divide
		// the window like this:
		// -r     r         width-r  width+r
		// +------+-------------+------+
		// |      |             |      |
		// +------+-------------+------+
		for (int y = 0; y < sheight; y++) {
			for (int x = 0; x < r * 2; x++) {
				double sum = sum_kernel_normalized(kernel, d - x - 1,
				                                   d - y - 1, d, height) *
				             255.0;
				data[y * sstride + x] = sum;
				data[y * sstride + swidth - x - 1] = sum;
			}
		}
		for (int y = 0; y < sheight; y++) {
			double sum =
			    sum_kernel_normalized(kernel, 0, d - y - 1, d, height) * 255.0;
			memset(&data[y * sstride + r * 2], sum, width - 2 * r);
		}
		return ximage;
	}
	if (width < r * 2) {
		// Similarly, for width smaller than kernel
		for (int y = 0; y < r * 2; y++) {
			for (int x = 0; x < swidth; x++) {
				double sum = sum_kernel_normalized(kernel, d - x - 1,
				                                   d - y - 1, width, d) *
				             255.0;
				data[y * sstride + x] = sum;
				data[(sheight - y - 1) * sstride + x] = sum;
			}
		}
		for (int x = 0; x < swidth; x++) {
			double sum =
			    sum_kernel_normalized(kernel, d - x - 1, 0, width, d) * 255.0;
			for (int y = r * 2; y < height; y++) {
				data[y * sstride + x] = sum;
			}
		}
		return ximage;
	}

	// Fill part 3
	for (int y = r; y < height + r; y++) {
		memset(data + sstride * y + r, 255, width);
	}

	// Part 1
	for (int y = 0; y < r * 2; y++) {
		for (int x = 0; x < r * 2; x++) {
			double tmpsum = shadow_sum[y * d + x] * opacity * 255.0;
			data[y * sstride + x] = tmpsum;
			data[(sheight - y - 1) * sstride + x] = tmpsum;
			data[(sheight - y - 1) * sstride + (swidth - x - 1)] = tmpsum;
			data[y * sstride + (swidth - x - 1)] = tmpsum;
		}
	}

	// Part 2, top/bottom
	for (int y = 0; y < r * 2; y++) {
		double tmpsum = shadow_sum[d * y + d - 1] * opacity * 255.0;
		memset(&data[y * sstride + r * 2], tmpsum, width - r * 2);
		memset(&data[(sheight - y - 1) * sstride + r * 2], tmpsum, width - r * 2);
	}

	// Part 2, left/right
	for (int x = 0; x < r * 2; x++) {
		double tmpsum = shadow_sum[d * (d - 1) + x] * opacity * 255.0;
		for (int y = r * 2; y < height; y++) {
			data[y * sstride + x] = tmpsum;
			data[y * sstride + (swidth - x - 1)] = tmpsum;
		}
	}

	return ximage;
}

/**
 * Generate shadow <code>Picture</code> for a window.
 */
bool build_shadow(session_t *ps, double opacity, const int width, const int height,
                  xcb_render_picture_t shadow_pixel, xcb_pixmap_t *pixmap,
                  xcb_render_picture_t *pict) {
	xcb_image_t *shadow_image = NULL;
	xcb_pixmap_t shadow_pixmap = XCB_NONE, shadow_pixmap_argb = XCB_NONE;
	xcb_render_picture_t shadow_picture = XCB_NONE, shadow_picture_argb = XCB_NONE;
	xcb_gcontext_t gc = XCB_NONE;

	shadow_image =
	    make_shadow(ps->c, ps->gaussian_map, opacity, width, height);
	if (!shadow_image) {
		log_error("Failed to make shadow");
		return false;
	}

	shadow_pixmap =
	    x_create_pixmap(ps->c, 8, ps->root, shadow_image->width, shadow_image->height);
	shadow_pixmap_argb =
	    x_create_pixmap(ps->c, 32, ps->root, shadow_image->width, shadow_image->height);

	if (!shadow_pixmap || !shadow_pixmap_argb) {
		log_error("Failed to create shadow pixmaps");
		goto shadow_picture_err;
	}

	shadow_picture = x_create_picture_with_standard_and_pixmap(
	    ps->c, XCB_PICT_STANDARD_A_8, shadow_pixmap, 0, NULL);
	shadow_picture_argb = x_create_picture_with_standard_and_pixmap(
	    ps->c, XCB_PICT_STANDARD_ARGB_32, shadow_pixmap_argb, 0, NULL);
	if (!shadow_picture || !shadow_picture_argb)
		goto shadow_picture_err;

	gc = xcb_generate_id(ps->c);
	xcb_create_gc(ps->c, gc, shadow_pixmap, 0, NULL);

	xcb_image_put(ps->c, shadow_pixmap, gc, shadow_image, 0, 0, 0);
	xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, shadow_pixel, shadow_picture,
	                     shadow_picture_argb, 0, 0, 0, 0, 0, 0, shadow_image->width,
	                     shadow_image->height);

	*pixmap = shadow_pixmap_argb;
	*pict = shadow_picture_argb;

	xcb_free_gc(ps->c, gc);
	xcb_image_destroy(shadow_image);
	xcb_free_pixmap(ps->c, shadow_pixmap);
	xcb_render_free_picture(ps->c, shadow_picture);

	return true;

shadow_picture_err:
	if (shadow_image)
		xcb_image_destroy(shadow_image);
	if (shadow_pixmap)
		xcb_free_pixmap(ps->c, shadow_pixmap);
	if (shadow_pixmap_argb)
		xcb_free_pixmap(ps->c, shadow_pixmap_argb);
	if (shadow_picture)
		xcb_render_free_picture(ps->c, shadow_picture);
	if (shadow_picture_argb)
		xcb_render_free_picture(ps->c, shadow_picture_argb);
	if (gc)
		xcb_free_gc(ps->c, gc);

	return false;
}

bool default_is_win_transparent(void *backend_data, win *w, void *win_data) {
	return w->mode != WMODE_SOLID;
}

bool default_is_frame_transparent(void *backend_data, win *w, void *win_data) {
	return w->frame_opacity != 1;
}
