// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <string.h>
#include <math.h>
#include <xcb/render.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_renderutil.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "common.h"
#include "kernel.h"
#include "config.h"
#include "utils.h"
#include "log.h"
#include "win.h"
#include "x.h"

/**
 * Generate a 1x1 <code>Picture</code> of a particular color.
 */
xcb_render_picture_t solid_picture(xcb_connection_t *c, xcb_drawable_t d, bool argb,
                                   double a, double r, double g, double b) {
	xcb_pixmap_t pixmap;
	xcb_render_picture_t picture;
	xcb_render_create_picture_value_list_t pa;
	xcb_render_color_t col;
	xcb_rectangle_t rect;

	pixmap = x_create_pixmap(c, argb ? 32 : 8, d, 1, 1);
	if (!pixmap)
		return XCB_NONE;

	pa.repeat = 1;
	picture = x_create_picture_with_standard_and_pixmap(
	    c, argb ? XCB_PICT_STANDARD_ARGB_32 : XCB_PICT_STANDARD_A_8, pixmap,
	    XCB_RENDER_CP_REPEAT, &pa);

	if (!picture) {
		xcb_free_pixmap(c, pixmap);
		return XCB_NONE;
	}

	col.alpha = (uint16_t)(a * 0xffff);
	col.red = (uint16_t)(r * 0xffff);
	col.green = (uint16_t)(g * 0xffff);
	col.blue = (uint16_t)(b * 0xffff);

	rect.x = 0;
	rect.y = 0;
	rect.width = 1;
	rect.height = 1;

	xcb_render_fill_rectangles(c, XCB_RENDER_PICT_OP_SRC, picture, col, 1, &rect);
	xcb_free_pixmap(c, pixmap);

	return picture;
}

xcb_image_t *
make_shadow(xcb_connection_t *c, const conv *kernel, double opacity, int width, int height) {
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
	assert(shadow_sum);
	// We only support square kernels for shadow
	assert(kernel->w == kernel->h);
	int d = kernel->w;
	int r = d / 2;
	int swidth = width + r * 2, sheight = height + r * 2;

	assert(d % 2 == 1);
	assert(d > 0);

	ximage = xcb_image_create_native(c, to_u16_checked(swidth), to_u16_checked(sheight),
	                                 XCB_IMAGE_FORMAT_Z_PIXMAP, 8, 0, 0, NULL);
	if (!ximage) {
		log_error("failed to create an X image");
		return 0;
	}

	unsigned char *data = ximage->data;
	long sstride = ximage->stride;

	// If the window body is smaller than the kernel, we do convolution directly
	if (width < r * 2 && height < r * 2) {
		for (int y = 0; y < sheight; y++) {
			for (int x = 0; x < swidth; x++) {
				double sum = sum_kernel_normalized(
				    kernel, d - x - 1, d - y - 1, width, height);
				data[y * sstride + x] = (uint8_t)(sum * 255.0);
			}
		}
		return ximage;
	}

	if (height < r * 2) {
		// Implies width >= r * 2
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
				data[y * sstride + x] = (uint8_t)sum;
				data[y * sstride + swidth - x - 1] = (uint8_t)sum;
			}
		}
		for (int y = 0; y < sheight; y++) {
			double sum =
			    sum_kernel_normalized(kernel, 0, d - y - 1, d, height) * 255.0;
			memset(&data[y * sstride + r * 2], (uint8_t)sum,
			       (size_t)(width - 2 * r));
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
				data[y * sstride + x] = (uint8_t)sum;
				data[(sheight - y - 1) * sstride + x] = (uint8_t)sum;
			}
		}
		for (int x = 0; x < swidth; x++) {
			double sum =
			    sum_kernel_normalized(kernel, d - x - 1, 0, width, d) * 255.0;
			for (int y = r * 2; y < height; y++) {
				data[y * sstride + x] = (uint8_t)sum;
			}
		}
		return ximage;
	}

	// Implies: width >= r * 2 && height >= r * 2

	// Fill part 3
	for (int y = r; y < height + r; y++) {
		memset(data + sstride * y + r, (uint8_t)(255 * opacity), (size_t)width);
	}

	// Part 1
	for (int y = 0; y < r * 2; y++) {
		for (int x = 0; x < r * 2; x++) {
			double tmpsum = shadow_sum[y * d + x] * opacity * 255.0;
			data[y * sstride + x] = (uint8_t)tmpsum;
			data[(sheight - y - 1) * sstride + x] = (uint8_t)tmpsum;
			data[(sheight - y - 1) * sstride + (swidth - x - 1)] = (uint8_t)tmpsum;
			data[y * sstride + (swidth - x - 1)] = (uint8_t)tmpsum;
		}
	}

	// Part 2, top/bottom
	for (int y = 0; y < r * 2; y++) {
		double tmpsum = shadow_sum[d * y + d - 1] * opacity * 255.0;
		memset(&data[y * sstride + r * 2], (uint8_t)tmpsum, (size_t)(width - r * 2));
		memset(&data[(sheight - y - 1) * sstride + r * 2], (uint8_t)tmpsum,
		       (size_t)(width - r * 2));
	}

	// Part 2, left/right
	for (int x = 0; x < r * 2; x++) {
		double tmpsum = shadow_sum[d * (d - 1) + x] * opacity * 255.0;
		for (int y = r * 2; y < height; y++) {
			data[y * sstride + x] = (uint8_t)tmpsum;
			data[y * sstride + (swidth - x - 1)] = (uint8_t)tmpsum;
		}
	}

	return ximage;
}

/**
 * Generate shadow <code>Picture</code> for a window.
 */
bool build_shadow(xcb_connection_t *c, xcb_drawable_t d, double opacity, const int width,
                  const int height, const conv *kernel, xcb_render_picture_t shadow_pixel,
                  xcb_pixmap_t *pixmap, xcb_render_picture_t *pict) {
	xcb_image_t *shadow_image = NULL;
	xcb_pixmap_t shadow_pixmap = XCB_NONE, shadow_pixmap_argb = XCB_NONE;
	xcb_render_picture_t shadow_picture = XCB_NONE, shadow_picture_argb = XCB_NONE;
	xcb_gcontext_t gc = XCB_NONE;

	shadow_image = make_shadow(c, kernel, opacity, width, height);
	if (!shadow_image) {
		log_error("Failed to make shadow");
		return false;
	}

	shadow_pixmap = x_create_pixmap(c, 8, d, shadow_image->width, shadow_image->height);
	shadow_pixmap_argb =
	    x_create_pixmap(c, 32, d, shadow_image->width, shadow_image->height);

	if (!shadow_pixmap || !shadow_pixmap_argb) {
		log_error("Failed to create shadow pixmaps");
		goto shadow_picture_err;
	}

	shadow_picture = x_create_picture_with_standard_and_pixmap(
	    c, XCB_PICT_STANDARD_A_8, shadow_pixmap, 0, NULL);
	shadow_picture_argb = x_create_picture_with_standard_and_pixmap(
	    c, XCB_PICT_STANDARD_ARGB_32, shadow_pixmap_argb, 0, NULL);
	if (!shadow_picture || !shadow_picture_argb) {
		goto shadow_picture_err;
	}

	gc = x_new_id(c);
	xcb_create_gc(c, gc, shadow_pixmap, 0, NULL);

	xcb_image_put(c, shadow_pixmap, gc, shadow_image, 0, 0, 0);
	xcb_render_composite(c, XCB_RENDER_PICT_OP_SRC, shadow_pixel, shadow_picture,
	                     shadow_picture_argb, 0, 0, 0, 0, 0, 0, shadow_image->width,
	                     shadow_image->height);

	*pixmap = shadow_pixmap_argb;
	*pict = shadow_picture_argb;

	xcb_free_gc(c, gc);
	xcb_image_destroy(shadow_image);
	xcb_free_pixmap(c, shadow_pixmap);
	xcb_render_free_picture(c, shadow_picture);

	return true;

shadow_picture_err:
	if (shadow_image) {
		xcb_image_destroy(shadow_image);
	}
	if (shadow_pixmap) {
		xcb_free_pixmap(c, shadow_pixmap);
	}
	if (shadow_pixmap_argb) {
		xcb_free_pixmap(c, shadow_pixmap_argb);
	}
	if (shadow_picture) {
		xcb_render_free_picture(c, shadow_picture);
	}
	if (shadow_picture_argb) {
		xcb_render_free_picture(c, shadow_picture_argb);
	}
	if (gc) {
		xcb_free_gc(c, gc);
	}

	return false;
}

void *
default_backend_render_shadow(backend_t *backend_data, int width, int height,
                              const conv *kernel, double r, double g, double b, double a) {
	xcb_pixmap_t shadow_pixel = solid_picture(backend_data->c, backend_data->root,
	                                          true, 1, r, g, b),
	             shadow = XCB_NONE;
	xcb_render_picture_t pict = XCB_NONE;

	build_shadow(backend_data->c, backend_data->root, a, width, height, kernel,
	             shadow_pixel, &shadow, &pict);

	auto visual = x_get_visual_for_standard(backend_data->c, XCB_PICT_STANDARD_ARGB_32);
	void *ret = backend_data->ops->bind_pixmap(
	    backend_data, shadow, x_get_visual_info(backend_data->c, visual), true);
	xcb_render_free_picture(backend_data->c, pict);
	return ret;
}

static struct conv **
generate_box_blur_kernel(struct box_blur_args *args, int *kernel_count) {
	int r = args->size * 2 + 1;
	assert(r > 0);
	auto ret = ccalloc(2, struct conv *);
	ret[0] = cvalloc(sizeof(struct conv) + sizeof(double) * (size_t)r);
	ret[1] = cvalloc(sizeof(struct conv) + sizeof(double) * (size_t)r);
	ret[0]->w = r;
	ret[0]->h = 1;
	ret[1]->w = 1;
	ret[1]->h = r;
	for (int i = 0; i < r; i++) {
		ret[0]->data[i] = 1;
		ret[1]->data[i] = 1;
	}
	*kernel_count = 2;
	return ret;
}

static struct conv **
generate_gaussian_blur_kernel(struct gaussian_blur_args *args, int *kernel_count) {
	int r = args->size * 2 + 1;
	assert(r > 0);
	auto ret = ccalloc(2, struct conv *);
	ret[0] = cvalloc(sizeof(struct conv) + sizeof(double) * (size_t)r);
	ret[1] = cvalloc(sizeof(struct conv) + sizeof(double) * (size_t)r);
	ret[0]->w = r;
	ret[0]->h = 1;
	ret[1]->w = 1;
	ret[1]->h = r;
	for (int i = 0; i <= args->size; i++) {
		ret[0]->data[i] = ret[0]->data[r - i - 1] =
		    1.0 / (sqrt(2.0 * M_PI) * args->deviation) *
		    exp(-(args->size - i) * (args->size - i) /
		        (2 * args->deviation * args->deviation));
		ret[1]->data[i] = ret[1]->data[r - i - 1] = ret[0]->data[i];
	}
	*kernel_count = 2;
	return ret;
}

/// Generate blur kernels for gaussian and box blur methods. Generated kernel is not
/// normalized, and the center element will always be 1.
struct conv **generate_blur_kernel(enum blur_method method, void *args, int *kernel_count) {
	switch (method) {
	case BLUR_METHOD_BOX: return generate_box_blur_kernel(args, kernel_count);
	case BLUR_METHOD_GAUSSIAN:
		return generate_gaussian_blur_kernel(args, kernel_count);
	default: break;
	}
	return NULL;
}

void init_backend_base(struct backend_base *base, session_t *ps) {
	base->c = ps->c;
	base->loop = ps->loop;
	base->root = ps->root;
	base->busy = false;
	base->ops = NULL;
}
