// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/composite.h>
#include <xcb/present.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "common.h"
#include "config.h"
#include "kernel.h"
#include "log.h"
#include "picom.h"
#include "region.h"
#include "types.h"
#include "utils.h"
#include "win.h"
#include "x.h"

typedef struct xrender_data {
	backend_t base;
	/// If vsync is enabled and supported by the current system
	bool vsync;
	/// Target window
	xcb_window_t target_win;
	/// Painting target, it is either the root or the overlay
	xcb_render_picture_t target;
	/// Back buffers. Double buffer, with 1 for temporary render use
	xcb_render_picture_t back[3];
	/// The back buffer that is for temporary use
	/// Age of each back buffer.
	int buffer_age[3];
	/// The back buffer we should be painting into
	int curr_back;
	/// The corresponding pixmap to the back buffer
	xcb_pixmap_t back_pixmap[3];
	/// Pictures of pixel of different alpha value, used as a mask to
	/// paint transparent images
	xcb_render_picture_t alpha_pict[256];

	// XXX don't know if these are really needed

	/// 1x1 white picture
	xcb_render_picture_t white_pixel;
	/// 1x1 black picture
	xcb_render_picture_t black_pixel;

	/// Width and height of the target pixmap
	int target_width, target_height;

	xcb_special_event_t *present_event;

	/// Cache an X region to avoid creating and destroying it every frame. A
	/// workaround for yshui/picom#1166.
	xcb_xfixes_region_t present_region;
} xrender_data;

struct xrender_blur_context {
	enum blur_method method;
	/// Blur kernels converted to X format
	struct x_convolution_kernel **x_blur_kernel;

	int resize_width, resize_height;

	/// Number of blur kernels
	int x_blur_kernel_count;
};

struct xrender_image_data_inner {
	// struct backend_image_inner_base
	int refcount;
	bool has_alpha;

	// Pixmap that the client window draws to,
	// it will contain the content of client window.
	xcb_pixmap_t pixmap;
	// A Picture links to the Pixmap
	xcb_render_picture_t pict;
	int width, height;
	xcb_visualid_t visual;
	uint8_t depth;
	// Whether we own this image, e.g. we allocated it;
	// or not, e.g. this is a named pixmap of a X window.
	bool owned;
};

struct xrender_rounded_rectangle_cache {
	int refcount;
	// A cached picture of a rounded rectangle. Xorg rasterizes shapes on CPU so it's
	// exceedingly slow.
	xcb_render_picture_t p;
};

struct xrender_image {
	struct backend_image base;

	struct xrender_rounded_rectangle_cache *rounded_rectangle;
};

/// Make a picture of size width x height, which has a rounded rectangle of corner_radius
/// rendered in it.
struct xrender_rounded_rectangle_cache *
xrender_make_rounded_corner_cache(struct x_connection *c, xcb_render_picture_t src,
                                  int width, int height, int corner_radius) {
	auto picture = x_create_picture_with_standard(c, width, height,
	                                              XCB_PICT_STANDARD_ARGB_32, 0, NULL);
	if (picture == XCB_NONE) {
		return NULL;
	}

	int inner_height = height - 2 * corner_radius;
	int cap_height = corner_radius;
	if (inner_height < 0) {
		cap_height = height / 2;
		inner_height = 0;
	}
	auto points = ccalloc(cap_height * 4 + 4, xcb_render_pointfix_t);
	int point_count = 0;

#define ADD_POINT(px, py)                                                                \
	assert(point_count < cap_height * 4 + 4);                                        \
	points[point_count].x = DOUBLE_TO_XFIXED(px);                                    \
	points[point_count].y = DOUBLE_TO_XFIXED(py);                                    \
	point_count += 1;

	// The top cap
	for (int i = 0; i <= cap_height; i++) {
		double y = corner_radius - i;
		double delta = sqrt(corner_radius * corner_radius - y * y);
		double left = corner_radius - delta;
		double right = width - corner_radius + delta;
		if (left >= right) {
			continue;
		}
		ADD_POINT(left, i);
		ADD_POINT(right, i);
	}

	// The middle rectangle
	if (inner_height > 0) {
		ADD_POINT(0, cap_height + inner_height);
		ADD_POINT(width, cap_height + inner_height);
	}

	// The bottom cap
	for (int i = cap_height + inner_height + 1; i <= height; i++) {
		double y = corner_radius - (height - i);
		double delta = sqrt(corner_radius * corner_radius - y * y);
		double left = corner_radius - delta;
		double right = width - corner_radius + delta;
		if (left >= right) {
			break;
		}
		ADD_POINT(left, i);
		ADD_POINT(right, i);
	}
#undef ADD_POINT

	XCB_AWAIT_VOID(xcb_render_tri_strip, c->c, XCB_RENDER_PICT_OP_SRC, src, picture,
	               x_get_pictfmt_for_standard(c, XCB_PICT_STANDARD_A_8), 0, 0,
	               (uint32_t)point_count, points);
	free(points);
	auto ret = ccalloc(1, struct xrender_rounded_rectangle_cache);
	ret->p = picture;
	ret->refcount = 1;
	return ret;
}

static xcb_render_picture_t
xrender_process_mask(struct xrender_data *xd, struct xrender_image *mask,
                     xcb_render_picture_t alpha_pict, bool *allocated) {
	auto inner = (struct xrender_image_data_inner *)mask->base.inner;
	if (!mask->base.color_inverted && mask->base.corner_radius == 0) {
		*allocated = false;
		return inner->pict;
	}
	auto const tmpw = to_u16_checked(inner->width);
	auto const tmph = to_u16_checked(inner->height);
	*allocated = true;
	x_clear_picture_clip_region(xd->base.c, inner->pict);
	auto ret = x_create_picture_with_visual(
	    xd->base.c, inner->width, inner->height, inner->visual, XCB_RENDER_CP_REPEAT,
	    (xcb_render_create_picture_value_list_t[]){XCB_RENDER_REPEAT_PAD});
	xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_SRC, inner->pict, XCB_NONE,
	                     ret, 0, 0, 0, 0, 0, 0, tmpw, tmph);
	// Remember: the mask has a 1-pixel border
	if (mask->base.corner_radius != 0) {
		if (mask->rounded_rectangle == NULL) {
			mask->rounded_rectangle = xrender_make_rounded_corner_cache(
			    xd->base.c, xd->white_pixel, inner->width - 2,
			    inner->height - 2, (int)mask->base.corner_radius);
		}
		xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_IN_REVERSE,
		                     mask->rounded_rectangle->p, XCB_NONE, ret, 0, 0, 0,
		                     0, 1, 1, (uint16_t)(tmpw - 2), (uint16_t)(tmph - 2));
	}

	if (mask->base.color_inverted) {
		xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_XOR, xd->white_pixel,
		                     XCB_NONE, ret, 0, 0, 0, 0, 0, 0, tmpw, tmph);
	}

	if (alpha_pict != XCB_NONE) {
		xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_SRC, ret, alpha_pict,
		                     ret, 0, 0, 0, 0, 0, 0, to_u16_checked(inner->width),
		                     to_u16_checked(inner->height));
	}

	return ret;
}

static void xrender_compose_impl(struct xrender_data *xd, struct xrender_image *xrimg,
                                 coord_t dst, struct xrender_image *mask, coord_t mask_dst,
                                 const region_t *reg_paint, const region_t *reg_visible,
                                 xcb_render_picture_t result) {
	const struct backend_image *img = &xrimg->base;
	bool mask_allocated = false;
	auto mask_pict = xd->alpha_pict[(int)(img->opacity * MAX_ALPHA)];
	if (mask != NULL) {
		mask_pict = xrender_process_mask(
		    xd, mask, img->opacity < 1.0 ? mask_pict : XCB_NONE, &mask_allocated);
	}
	auto inner = (struct xrender_image_data_inner *)img->inner;
	region_t reg;

	bool has_alpha = inner->has_alpha || img->opacity != 1;
	auto const tmpw = to_u16_checked(inner->width);
	auto const tmph = to_u16_checked(inner->height);
	auto const tmpew = to_u16_checked(img->ewidth);
	auto const tmpeh = to_u16_checked(img->eheight);
	// Remember: the mask has a 1-pixel border
	auto const mask_dst_x = to_i16_checked(dst.x - mask_dst.x + 1);
	auto const mask_dst_y = to_i16_checked(dst.y - mask_dst.y + 1);
	const xcb_render_color_t dim_color = {
	    .red = 0, .green = 0, .blue = 0, .alpha = (uint16_t)(0xffff * img->dim)};

	// Clip region of rendered_pict might be set during rendering, clear it to
	// make sure we get everything into the buffer
	x_clear_picture_clip_region(xd->base.c, inner->pict);

	pixman_region32_init(&reg);
	pixman_region32_intersect(&reg, (region_t *)reg_paint, (region_t *)reg_visible);
	x_set_picture_clip_region(xd->base.c, result, 0, 0, &reg);
	if (img->corner_radius != 0 && xrimg->rounded_rectangle == NULL) {
		xrimg->rounded_rectangle = xrender_make_rounded_corner_cache(
		    xd->base.c, xd->white_pixel, inner->width, inner->height,
		    (int)img->corner_radius);
	}
	if (((img->color_inverted || img->dim != 0) && has_alpha) || img->corner_radius != 0) {
		// Apply image properties using a temporary image, because the source
		// image is transparent or will get transparent corners. Otherwise the
		// properties can be applied directly on the target image.
		// Also force a 32-bit ARGB visual for transparent corners, otherwise the
		// corners become black.
		auto visual =
		    (img->corner_radius != 0 && inner->depth != 32)
		        ? x_get_visual_for_standard(xd->base.c, XCB_PICT_STANDARD_ARGB_32)
		        : inner->visual;
		auto tmp_pict = x_create_picture_with_visual(
		    xd->base.c, inner->width, inner->height, visual, 0, NULL);

		// Set clip region translated to source coordinate
		x_set_picture_clip_region(xd->base.c, tmp_pict, to_i16_checked(-dst.x),
		                          to_i16_checked(-dst.y), &reg);
		// Copy source -> tmp
		xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_SRC, inner->pict,
		                     XCB_NONE, tmp_pict, 0, 0, 0, 0, 0, 0, tmpw, tmph);

		if (img->color_inverted) {
			if (inner->has_alpha) {
				auto tmp_pict2 = x_create_picture_with_visual(
				    xd->base.c, tmpw, tmph, inner->visual, 0, NULL);
				xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_SRC,
				                     tmp_pict, XCB_NONE, tmp_pict2, 0, 0,
				                     0, 0, 0, 0, tmpw, tmph);

				xcb_render_composite(xd->base.c->c,
				                     XCB_RENDER_PICT_OP_DIFFERENCE,
				                     xd->white_pixel, XCB_NONE, tmp_pict,
				                     0, 0, 0, 0, 0, 0, tmpw, tmph);
				xcb_render_composite(
				    xd->base.c->c, XCB_RENDER_PICT_OP_IN_REVERSE, tmp_pict2,
				    XCB_NONE, tmp_pict, 0, 0, 0, 0, 0, 0, tmpw, tmph);
				x_free_picture(xd->base.c, tmp_pict2);
			} else {
				xcb_render_composite(xd->base.c->c,
				                     XCB_RENDER_PICT_OP_DIFFERENCE,
				                     xd->white_pixel, XCB_NONE, tmp_pict,
				                     0, 0, 0, 0, 0, 0, tmpw, tmph);
			}
		}

		if (img->dim != 0) {
			// Dim the actually content of window
			xcb_rectangle_t rect = {
			    .x = 0,
			    .y = 0,
			    .width = tmpw,
			    .height = tmph,
			};

			xcb_render_fill_rectangles(xd->base.c->c, XCB_RENDER_PICT_OP_OVER,
			                           tmp_pict, dim_color, 1, &rect);
		}

		if (img->corner_radius != 0 && xrimg->rounded_rectangle != NULL) {
			// Clip tmp_pict with a rounded rectangle
			xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_IN_REVERSE,
			                     xrimg->rounded_rectangle->p, XCB_NONE,
			                     tmp_pict, 0, 0, 0, 0, 0, 0, tmpw, tmph);
		}

		xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_OVER, tmp_pict,
		                     mask_pict, result, 0, 0, mask_dst_x, mask_dst_y,
		                     to_i16_checked(dst.x), to_i16_checked(dst.y), tmpew,
		                     tmpeh);
		xcb_render_free_picture(xd->base.c->c, tmp_pict);
	} else {
		uint8_t op = (has_alpha ? XCB_RENDER_PICT_OP_OVER : XCB_RENDER_PICT_OP_SRC);

		xcb_render_composite(xd->base.c->c, op, inner->pict, mask_pict, result, 0,
		                     0, mask_dst_x, mask_dst_y, to_i16_checked(dst.x),
		                     to_i16_checked(dst.y), tmpew, tmpeh);
		if (img->dim != 0 || img->color_inverted) {
			// Apply properties, if we reach here, then has_alpha == false
			assert(!has_alpha);
			if (img->color_inverted) {
				xcb_render_composite(xd->base.c->c,
				                     XCB_RENDER_PICT_OP_DIFFERENCE,
				                     xd->white_pixel, XCB_NONE, result, 0,
				                     0, 0, 0, to_i16_checked(dst.x),
				                     to_i16_checked(dst.y), tmpew, tmpeh);
			}

			if (img->dim != 0) {
				// Dim the actually content of window
				xcb_rectangle_t rect = {
				    .x = to_i16_checked(dst.x),
				    .y = to_i16_checked(dst.y),
				    .width = tmpew,
				    .height = tmpeh,
				};

				xcb_render_fill_rectangles(xd->base.c->c,
				                           XCB_RENDER_PICT_OP_OVER,
				                           result, dim_color, 1, &rect);
			}
		}
	}
	if (mask_allocated) {
		x_free_picture(xd->base.c, mask_pict);
	}
	pixman_region32_fini(&reg);
}

static void
xrender_compose(backend_t *base, image_handle image_, coord_t dst, image_handle mask_,
                coord_t mask_dst, const region_t *reg_paint, const region_t *reg_visible) {
	auto xd = (struct xrender_data *)base;
	auto image = (struct xrender_image *)image_;
	auto mask = (struct xrender_image *)mask_;
	return xrender_compose_impl(xd, image, dst, mask, mask_dst, reg_paint,
	                            reg_visible, xd->back[2]);
}

static void xrender_fill(backend_t *base, struct color c, const region_t *clip) {
	auto xd = (struct xrender_data *)base;
	const rect_t *extent = pixman_region32_extents((region_t *)clip);
	x_set_picture_clip_region(base->c, xd->back[2], 0, 0, clip);
	// color is in X fixed point representation
	xcb_render_fill_rectangles(
	    base->c->c, XCB_RENDER_PICT_OP_OVER, xd->back[2],
	    (xcb_render_color_t){.red = (uint16_t)(c.red * 0xffff),
	                         .green = (uint16_t)(c.green * 0xffff),
	                         .blue = (uint16_t)(c.blue * 0xffff),
	                         .alpha = (uint16_t)(c.alpha * 0xffff)},
	    1,
	    (xcb_rectangle_t[]){{.x = to_i16_checked(extent->x1),
	                         .y = to_i16_checked(extent->y1),
	                         .width = to_u16_checked(extent->x2 - extent->x1),
	                         .height = to_u16_checked(extent->y2 - extent->y1)}});
}

static bool
xrender_blur(backend_t *backend_data, double opacity, void *ctx_, image_handle mask_,
             coord_t mask_dst, const region_t *reg_blur, const region_t *reg_visible) {
	auto bctx = (struct xrender_blur_context *)ctx_;
	auto mask = (struct xrender_image *)mask_;
	if (bctx->method == BLUR_METHOD_NONE) {
		return true;
	}

	auto xd = (struct xrender_data *)backend_data;
	auto c = xd->base.c;
	region_t reg_op;
	pixman_region32_init(&reg_op);
	pixman_region32_intersect(&reg_op, (region_t *)reg_blur, (region_t *)reg_visible);
	if (!pixman_region32_not_empty(&reg_op)) {
		pixman_region32_fini(&reg_op);
		return true;
	}

	region_t reg_op_resized =
	    resize_region(&reg_op, bctx->resize_width, bctx->resize_height);

	const pixman_box32_t *extent_resized = pixman_region32_extents(&reg_op_resized);
	auto const height_resized = to_u16_checked(extent_resized->y2 - extent_resized->y1);
	auto const width_resized = to_u16_checked(extent_resized->x2 - extent_resized->x1);
	static const char *filter0 = "Nearest";        // The "null" filter
	static const char *filter = "convolution";

	// Create a buffer for storing blurred picture, make it just big enough
	// for the blur region
	const uint32_t pic_attrs_mask = XCB_RENDER_CP_REPEAT;
	const xcb_render_create_picture_value_list_t pic_attrs = {.repeat = XCB_RENDER_REPEAT_PAD};
	xcb_render_picture_t tmp_picture[2] = {
	    x_create_picture_with_visual(xd->base.c, width_resized, height_resized,
	                                 xd->base.c->screen_info->root_visual,
	                                 pic_attrs_mask, &pic_attrs),
	    x_create_picture_with_visual(xd->base.c, width_resized, height_resized,
	                                 xd->base.c->screen_info->root_visual,
	                                 pic_attrs_mask, &pic_attrs)};

	if (!tmp_picture[0] || !tmp_picture[1]) {
		log_error("Failed to build intermediate Picture.");
		pixman_region32_fini(&reg_op);
		pixman_region32_fini(&reg_op_resized);
		return false;
	}

	region_t clip;
	pixman_region32_init(&clip);
	pixman_region32_copy(&clip, &reg_op_resized);
	pixman_region32_translate(&clip, -extent_resized->x1, -extent_resized->y1);
	x_set_picture_clip_region(c, tmp_picture[0], 0, 0, &clip);
	x_set_picture_clip_region(c, tmp_picture[1], 0, 0, &clip);
	pixman_region32_fini(&clip);

	xcb_render_picture_t src_pict = xd->back[2], dst_pict = tmp_picture[0];
	auto mask_pict = xd->alpha_pict[(int)(opacity * MAX_ALPHA)];
	bool mask_allocated = false;
	if (mask != NULL) {
		mask_pict = xrender_process_mask(
		    xd, mask, opacity != 1.0 ? mask_pict : XCB_NONE, &mask_allocated);
	}
	int current = 0;
	x_set_picture_clip_region(c, src_pict, 0, 0, &reg_op_resized);

	// For more than 1 pass, we do:
	//   back -(pass 1)-> tmp0 -(pass 2)-> tmp1 ...
	//   -(pass n-1)-> tmp0 or tmp1 -(pass n)-> back
	// For 1 pass, we do
	//   back -(pass 1)-> tmp0 -(copy)-> target_buffer
	int i;
	for (i = 0; i < bctx->x_blur_kernel_count; i++) {
		// Copy from source picture to destination. The filter must
		// be applied on source picture, to get the nearby pixels outside the
		// window.
		xcb_render_set_picture_filter(c->c, src_pict,
		                              to_u16_checked(strlen(filter)), filter,
		                              to_u32_checked(bctx->x_blur_kernel[i]->size),
		                              bctx->x_blur_kernel[i]->kernel);

		if (i == 0) {
			// First pass, back buffer -> tmp picture
			// (we do this even if this is also the last pass, because we
			// cannot do back buffer -> back buffer)
			xcb_render_composite(c->c, XCB_RENDER_PICT_OP_SRC, src_pict, XCB_NONE,
			                     dst_pict, to_i16_checked(extent_resized->x1),
			                     to_i16_checked(extent_resized->y1), 0, 0, 0,
			                     0, width_resized, height_resized);
		} else if (i < bctx->x_blur_kernel_count - 1) {
			// This is not the last pass or the first pass,
			// tmp picture 1 -> tmp picture 2
			xcb_render_composite(c->c, XCB_RENDER_PICT_OP_SRC, src_pict,
			                     XCB_NONE, dst_pict, 0, 0, 0, 0, 0, 0,
			                     width_resized, height_resized);
		} else {
			x_set_picture_clip_region(c, xd->back[2], 0, 0, &reg_op);
			// This is the last pass, and we are doing more than 1 pass
			xcb_render_composite(
			    c->c, XCB_RENDER_PICT_OP_OVER, src_pict, mask_pict, xd->back[2],
			    0, 0, to_i16_checked(extent_resized->x1 - mask_dst.x + 1),
			    to_i16_checked(extent_resized->y1 - mask_dst.y + 1),
			    to_i16_checked(extent_resized->x1),
			    to_i16_checked(extent_resized->y1), width_resized, height_resized);
		}

		// reset filter
		xcb_render_set_picture_filter(
		    c->c, src_pict, to_u16_checked(strlen(filter0)), filter0, 0, NULL);

		src_pict = tmp_picture[current];
		dst_pict = tmp_picture[!current];
		current = !current;
	}

	// There is only 1 pass
	if (i == 1) {
		x_set_picture_clip_region(c, xd->back[2], 0, 0, &reg_op);
		xcb_render_composite(
		    c->c, XCB_RENDER_PICT_OP_OVER, src_pict, mask_pict, xd->back[2], 0, 0,
		    to_i16_checked(extent_resized->x1 - mask_dst.x + 1),
		    to_i16_checked(extent_resized->y1 - mask_dst.y + 1),
		    to_i16_checked(extent_resized->x1),
		    to_i16_checked(extent_resized->y1), width_resized, height_resized);
	}

	if (mask_allocated) {
		x_free_picture(c, mask_pict);
	}
	x_free_picture(c, tmp_picture[0]);
	x_free_picture(c, tmp_picture[1]);
	pixman_region32_fini(&reg_op);
	pixman_region32_fini(&reg_op_resized);
	return true;
}

static image_handle xrender_bind_pixmap(backend_t *base, xcb_pixmap_t pixmap,
                                        struct xvisual_info fmt, bool owned) {
	xcb_generic_error_t *e;
	auto r = xcb_get_geometry_reply(base->c->c, xcb_get_geometry(base->c->c, pixmap), &e);
	if (!r) {
		log_error("Invalid pixmap: %#010x", pixmap);
		x_print_error(e->full_sequence, e->major_code, e->minor_code, e->error_code);
		free(e);
		return NULL;
	}

	auto img = ccalloc(1, struct xrender_image);
	auto inner = ccalloc(1, struct xrender_image_data_inner);
	inner->depth = (uint8_t)fmt.visual_depth;
	inner->width = img->base.ewidth = r->width;
	inner->height = img->base.eheight = r->height;
	inner->pixmap = pixmap;
	inner->has_alpha = fmt.alpha_size != 0;
	xcb_render_create_picture_value_list_t pic_attrs = {.repeat = XCB_RENDER_REPEAT_NORMAL};
	inner->pict = x_create_picture_with_visual_and_pixmap(
	    base->c, fmt.visual, pixmap, XCB_RENDER_CP_REPEAT, &pic_attrs);
	inner->owned = owned;
	inner->visual = fmt.visual;
	inner->refcount = 1;

	img->base.inner = (struct backend_image_inner_base *)inner;
	img->base.opacity = 1;
	img->rounded_rectangle = NULL;
	free(r);

	if (inner->pict == XCB_NONE) {
		free(inner);
		free(img);
		return NULL;
	}
	return (image_handle)img;
}
static void
xrender_release_image_inner(backend_t *base, struct xrender_image_data_inner *inner) {
	x_free_picture(base->c, inner->pict);
	if (inner->owned) {
		xcb_free_pixmap(base->c->c, inner->pixmap);
	}
	free(inner);
}

static void
xrender_release_rounded_corner_cache(backend_t *base,
                                     struct xrender_rounded_rectangle_cache *cache) {
	if (!cache) {
		return;
	}

	assert(cache->refcount > 0);
	cache->refcount--;
	if (cache->refcount == 0) {
		x_free_picture(base->c, cache->p);
		free(cache);
	}
}

static void xrender_release_image(backend_t *base, image_handle image) {
	auto img = (struct xrender_image *)image;
	xrender_release_rounded_corner_cache(base, img->rounded_rectangle);
	img->rounded_rectangle = NULL;
	img->base.inner->refcount -= 1;
	if (img->base.inner->refcount == 0) {
		xrender_release_image_inner(
		    base, (struct xrender_image_data_inner *)img->base.inner);
	}
	free(img);
}

static void xrender_deinit(backend_t *backend_data) {
	auto xd = (struct xrender_data *)backend_data;
	for (int i = 0; i < 256; i++) {
		x_free_picture(xd->base.c, xd->alpha_pict[i]);
	}
	x_free_picture(xd->base.c, xd->target);
	for (int i = 0; i < 3; i++) {
		if (xd->back[i] != XCB_NONE) {
			x_free_picture(xd->base.c, xd->back[i]);
		}
		if (xd->back_pixmap[i] != XCB_NONE) {
			xcb_free_pixmap(xd->base.c->c, xd->back_pixmap[i]);
		}
	}
	x_destroy_region(xd->base.c, xd->present_region);
	if (xd->present_event) {
		xcb_unregister_for_special_event(xd->base.c->c, xd->present_event);
	}
	x_free_picture(xd->base.c, xd->white_pixel);
	x_free_picture(xd->base.c, xd->black_pixel);
	free(xd);
}

static void xrender_present(backend_t *base, const region_t *region) {
	auto xd = (struct xrender_data *)base;
	const rect_t *extent = pixman_region32_extents((region_t *)region);
	int16_t orig_x = to_i16_checked(extent->x1), orig_y = to_i16_checked(extent->y1);
	uint16_t region_width = to_u16_checked(extent->x2 - extent->x1),
	         region_height = to_u16_checked(extent->y2 - extent->y1);

	// limit the region of update
	x_set_picture_clip_region(base->c, xd->back[2], 0, 0, region);

	if (xd->vsync) {
		// compose() sets clip region on the back buffer, so clear it first
		x_clear_picture_clip_region(base->c, xd->back[xd->curr_back]);

		// Update the back buffer first, then present
		xcb_render_composite(base->c->c, XCB_RENDER_PICT_OP_SRC, xd->back[2],
		                     XCB_NONE, xd->back[xd->curr_back], orig_x, orig_y, 0,
		                     0, orig_x, orig_y, region_width, region_height);

		// Make sure we got reply from PresentPixmap before waiting for events,
		// to avoid deadlock
		auto e = xcb_request_check(
		    base->c->c,
		    xcb_present_pixmap_checked(
		        base->c->c, xd->target_win, xd->back_pixmap[xd->curr_back], 0, XCB_NONE,
		        x_set_region(base->c, xd->present_region, region) ? xd->present_region
		                                                          : XCB_NONE,
		        0, 0, XCB_NONE, XCB_NONE, XCB_NONE, 0, 0, 0, 0, 0, NULL));
		if (e) {
			log_error("Failed to present pixmap");
			free(e);
			return;
		}
		// TODO(yshui) don't block wait for present completion
		auto pev = (xcb_present_generic_event_t *)xcb_wait_for_special_event(
		    base->c->c, xd->present_event);
		if (!pev) {
			// We don't know what happened, maybe X died
			// But reset buffer age, so in case we do recover, we will
			// render correctly.
			xd->buffer_age[0] = xd->buffer_age[1] = -1;
			return;
		}
		assert(pev->evtype == XCB_PRESENT_COMPLETE_NOTIFY);
		auto pcev = (xcb_present_complete_notify_event_t *)pev;
		// log_trace("Present complete: %d %ld", pcev->mode, pcev->msc);
		xd->buffer_age[xd->curr_back] = 1;

		// buffer_age < 0 means that back buffer is empty
		if (xd->buffer_age[1 - xd->curr_back] > 0) {
			xd->buffer_age[1 - xd->curr_back]++;
		}
		if (pcev->mode == XCB_PRESENT_COMPLETE_MODE_FLIP) {
			// We cannot use the pixmap we used anymore
			xd->curr_back = 1 - xd->curr_back;
		}
		free(pev);
	} else {
		// No vsync needed, draw into the target picture directly
		xcb_render_composite(base->c->c, XCB_RENDER_PICT_OP_SRC, xd->back[2],
		                     XCB_NONE, xd->target, orig_x, orig_y, 0, 0, orig_x,
		                     orig_y, region_width, region_height);
	}
}

static int xrender_buffer_age(backend_t *backend_data) {
	auto xd = (struct xrender_data *)backend_data;
	if (!xd->vsync) {
		// Only the target picture really holds the screen content, and its
		// content is always up to date. So buffer age is always 1.
		return 1;
	}
	return xd->buffer_age[xd->curr_back];
}

static struct xrender_image_data_inner *
xrender_new_inner(backend_t *base, int w, int h, xcb_visualid_t visual, uint8_t depth) {
	auto new_inner = ccalloc(1, struct xrender_image_data_inner);
	new_inner->pixmap = x_create_pixmap(base->c, depth, w, h);
	if (new_inner->pixmap == XCB_NONE) {
		log_error("Failed to create pixmap for copy");
		free(new_inner);
		return NULL;
	}
	new_inner->pict = x_create_picture_with_visual_and_pixmap(
	    base->c, visual, new_inner->pixmap, 0, NULL);
	if (new_inner->pict == XCB_NONE) {
		log_error("Failed to create picture for copy");
		xcb_free_pixmap(base->c->c, new_inner->pixmap);
		free(new_inner);
		return NULL;
	}
	new_inner->width = w;
	new_inner->height = h;
	new_inner->visual = visual;
	new_inner->depth = depth;
	new_inner->refcount = 1;
	new_inner->owned = true;
	return new_inner;
}

static image_handle xrender_make_mask(backend_t *base, geometry_t size, const region_t *reg) {
	auto xd = (struct xrender_data *)base;
	// Give the mask a 1 pixel wide border to emulate the clamp to border behavior of
	// OpenGL textures.
	auto w16 = to_u16_checked(size.width + 2);
	auto h16 = to_u16_checked(size.height + 2);
	auto inner = xrender_new_inner(
	    base, size.width + 2, size.height + 2,
	    x_get_visual_for_standard(base->c, XCB_PICT_STANDARD_ARGB_32), 32);
	xcb_render_change_picture(base->c->c, inner->pict, XCB_RENDER_CP_REPEAT,
	                          (uint32_t[]){XCB_RENDER_REPEAT_PAD});
	const rect_t *extent = pixman_region32_extents((region_t *)reg);
	x_set_picture_clip_region(base->c, xd->back[2], 1, 1, reg);
	xcb_render_fill_rectangles(
	    base->c->c, XCB_RENDER_PICT_OP_SRC, inner->pict,
	    (xcb_render_color_t){.red = 0, .green = 0, .blue = 0, .alpha = 0xffff}, 1,
	    (xcb_rectangle_t[]){{.x = to_i16_checked(extent->x1 + 1),
	                         .y = to_i16_checked(extent->y1 + 1),
	                         .width = to_u16_checked(extent->x2 - extent->x1),
	                         .height = to_u16_checked(extent->y2 - extent->y1)}});
	x_clear_picture_clip_region(xd->base.c, inner->pict);

	// Paint the border transparent
	xcb_render_fill_rectangles(
	    base->c->c, XCB_RENDER_PICT_OP_SRC, inner->pict,
	    (xcb_render_color_t){.red = 0, .green = 0, .blue = 0, .alpha = 0}, 4,
	    (xcb_rectangle_t[]){{.x = 0, .y = 0, .width = w16, .height = 1},
	                        {.x = 0, .y = 0, .width = 1, .height = h16},
	                        {.x = 0, .y = (short)(h16 - 1), .width = w16, .height = 1},
	                        {.x = (short)(w16 - 1), .y = 0, .width = 1, .height = h16}});
	inner->refcount = 1;

	auto img = ccalloc(1, struct xrender_image);
	img->base.eheight = size.height + 2;
	img->base.ewidth = size.width + 2;
	img->base.border_width = 0;
	img->base.color_inverted = false;
	img->base.corner_radius = 0;
	img->base.max_brightness = 1;
	img->base.opacity = 1;
	img->base.dim = 0;
	img->base.inner = (struct backend_image_inner_base *)inner;
	img->rounded_rectangle = NULL;
	return (image_handle)img;
}

static bool
xrender_decouple_image(backend_t *base, struct backend_image *img, const region_t *reg) {
	if (img->inner->refcount == 1) {
		return true;
	}
	auto inner = (struct xrender_image_data_inner *)img->inner;
	// Force new pixmap to a 32-bit ARGB visual to allow for transparent frames around
	// non-transparent windows
	auto visual = (inner->depth == 32)
	                  ? inner->visual
	                  : x_get_visual_for_standard(base->c, XCB_PICT_STANDARD_ARGB_32);
	auto inner2 = xrender_new_inner(base, inner->width, inner->height, visual, 32);
	if (!inner2) {
		return false;
	}

	x_set_picture_clip_region(base->c, inner->pict, 0, 0, reg);
	xcb_render_composite(base->c->c, XCB_RENDER_PICT_OP_SRC, inner->pict, XCB_NONE,
	                     inner2->pict, 0, 0, 0, 0, 0, 0, to_u16_checked(inner->width),
	                     to_u16_checked(inner->height));

	img->inner = (struct backend_image_inner_base *)inner2;
	inner->refcount--;
	return true;
}

static bool xrender_image_op(backend_t *base, enum image_operations op, image_handle image,
                             const region_t *reg_op, const region_t *reg_visible, void *arg) {
	auto xd = (struct xrender_data *)base;
	auto img = (struct backend_image *)image;
	region_t reg;
	double *dargs = arg;

	pixman_region32_init(&reg);
	pixman_region32_intersect(&reg, (region_t *)reg_op, (region_t *)reg_visible);

	switch (op) {
	case IMAGE_OP_APPLY_ALPHA:
		assert(reg_op);

		if (!pixman_region32_not_empty(&reg)) {
			break;
		}

		if (dargs[0] == 1) {
			break;
		}

		if (!xrender_decouple_image(base, img, reg_visible)) {
			pixman_region32_fini(&reg);
			return false;
		}

		auto inner = (struct xrender_image_data_inner *)img->inner;
		auto alpha_pict = xd->alpha_pict[(int)((1 - dargs[0]) * MAX_ALPHA)];
		x_set_picture_clip_region(base->c, inner->pict, 0, 0, &reg);
		xcb_render_composite(base->c->c, XCB_RENDER_PICT_OP_OUT_REVERSE,
		                     alpha_pict, XCB_NONE, inner->pict, 0, 0, 0, 0, 0, 0,
		                     to_u16_checked(inner->width),
		                     to_u16_checked(inner->height));
		inner->has_alpha = true;
		break;
	}
	pixman_region32_fini(&reg);
	return true;
}

static void *xrender_create_blur_context(backend_t *base attr_unused,
                                         enum blur_method method, void *args) {
	auto ret = ccalloc(1, struct xrender_blur_context);
	if (!method || method >= BLUR_METHOD_INVALID) {
		ret->method = BLUR_METHOD_NONE;
		return ret;
	}
	if (method == BLUR_METHOD_DUAL_KAWASE) {
		log_warn("Blur method 'dual_kawase' is not compatible with the 'xrender' "
		         "backend.");
		ret->method = BLUR_METHOD_NONE;
		return ret;
	}

	ret->method = BLUR_METHOD_KERNEL;
	struct conv **kernels;
	int kernel_count;
	if (method == BLUR_METHOD_KERNEL) {
		kernels = ((struct kernel_blur_args *)args)->kernels;
		kernel_count = ((struct kernel_blur_args *)args)->kernel_count;
	} else {
		kernels = generate_blur_kernel(method, args, &kernel_count);
	}

	ret->x_blur_kernel = ccalloc(kernel_count, struct x_convolution_kernel *);
	for (int i = 0; i < kernel_count; i++) {
		int center = kernels[i]->h * kernels[i]->w / 2;
		x_create_convolution_kernel(kernels[i], kernels[i]->data[center],
		                            &ret->x_blur_kernel[i]);
		ret->resize_width += kernels[i]->w / 2;
		ret->resize_height += kernels[i]->h / 2;
	}
	ret->x_blur_kernel_count = kernel_count;

	if (method != BLUR_METHOD_KERNEL) {
		// Kernels generated by generate_blur_kernel, so we need to free them.
		for (int i = 0; i < kernel_count; i++) {
			free(kernels[i]);
		}
		free(kernels);
	}
	return ret;
}

static void xrender_destroy_blur_context(backend_t *base attr_unused, void *ctx_) {
	struct xrender_blur_context *ctx = ctx_;
	for (int i = 0; i < ctx->x_blur_kernel_count; i++) {
		free(ctx->x_blur_kernel[i]);
	}
	free(ctx->x_blur_kernel);
	free(ctx);
}

static void xrender_get_blur_size(void *blur_context, int *width, int *height) {
	struct xrender_blur_context *ctx = blur_context;
	*width = ctx->resize_width;
	*height = ctx->resize_height;
}

static backend_t *xrender_init(session_t *ps, xcb_window_t target) {
	auto c = session_get_x_connection(ps);
	auto options = session_get_options(ps);
	if (options->dithered_present) {
		log_warn("\"dithered-present\" is not supported by the xrender backend.");
	}

	auto xd = ccalloc(1, struct xrender_data);
	init_backend_base(&xd->base, ps);

	for (int i = 0; i <= MAX_ALPHA; ++i) {
		double o = (double)i / (double)MAX_ALPHA;
		xd->alpha_pict[i] = solid_picture(c, false, o, 0, 0, 0);
		assert(xd->alpha_pict[i] != XCB_NONE);
	}

	auto root_extent = session_get_root_extent(ps);
	xd->target_width = root_extent.width;
	xd->target_height = root_extent.height;
	xd->black_pixel = solid_picture(c, true, 1, 0, 0, 0);
	xd->white_pixel = solid_picture(c, true, 1, 1, 1, 1);

	xd->target_win = target;
	xcb_render_create_picture_value_list_t pa = {
	    .subwindowmode = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS,
	};
	xd->target = x_create_picture_with_visual_and_pixmap(
	    c, c->screen_info->root_visual, xd->target_win, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);

	xd->vsync = options->vsync;
	if (session_has_present_extension(ps)) {
		auto eid = x_new_id(c);
		auto e = xcb_request_check(c->c, xcb_present_select_input_checked(
		                                     c->c, eid, xd->target_win,
		                                     XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
		if (e) {
			log_error("Cannot select present input, vsync will be disabled");
			xd->vsync = false;
			free(e);
		}

		xd->present_event =
		    xcb_register_for_special_xge(c->c, &xcb_present_id, eid, NULL);
		if (!xd->present_event) {
			log_error("Cannot register for special XGE, vsync will be "
			          "disabled");
			xd->vsync = false;
		}
	} else {
		xd->vsync = false;
	}

	if (xd->vsync) {
		xd->present_region = x_create_region(c, session_get_screen_reg(ps));
	}

	// We might need to do double buffering for vsync, and buffer 0 and 1 are for
	// double buffering.
	int first_buffer_index = xd->vsync ? 0 : 2;
	for (int i = first_buffer_index; i < 3; i++) {
		xd->back_pixmap[i] = x_create_pixmap(c, c->screen_info->root_depth,
		                                     to_u16_checked(root_extent.width),
		                                     to_u16_checked(root_extent.height));
		const uint32_t pic_attrs_mask = XCB_RENDER_CP_REPEAT;
		const xcb_render_create_picture_value_list_t pic_attrs = {
		    .repeat = XCB_RENDER_REPEAT_PAD};
		xd->back[i] = x_create_picture_with_visual_and_pixmap(
		    c, c->screen_info->root_visual, xd->back_pixmap[i], pic_attrs_mask,
		    &pic_attrs);
		xd->buffer_age[i] = -1;
		if (xd->back_pixmap[i] == XCB_NONE || xd->back[i] == XCB_NONE) {
			log_error("Cannot create pixmap for rendering");
			goto err;
		}
	}
	xd->curr_back = 0;

	return &xd->base;
err:
	xrender_deinit(&xd->base);
	return NULL;
}

image_handle xrender_clone_image(backend_t *base attr_unused, image_handle image,
                                 const region_t *reg_visible attr_unused) {
	auto new_img = ccalloc(1, struct xrender_image);
	*new_img = *(struct xrender_image *)image;
	new_img->base.inner->refcount++;
	if (new_img->rounded_rectangle) {
		new_img->rounded_rectangle->refcount++;
	}
	return (image_handle)new_img;
}

static bool xrender_set_image_property(backend_t *base, enum image_properties op,
                                       image_handle image, const void *args) {
	auto xrimg = (struct xrender_image *)image;
	if (op == IMAGE_PROPERTY_CORNER_RADIUS &&
	    ((const double *)args)[0] != xrimg->base.corner_radius) {
		// Free cached rounded rectangle if corner radius changed
		xrender_release_rounded_corner_cache(base, xrimg->rounded_rectangle);
		xrimg->rounded_rectangle = NULL;
	}
	return default_set_image_property(base, op, image, args);
}

struct backend_operations xrender_ops = {
    .init = xrender_init,
    .deinit = xrender_deinit,
    .compose = xrender_compose,
    .fill = xrender_fill,
    .blur = xrender_blur,
    .present = xrender_present,
    .bind_pixmap = xrender_bind_pixmap,
    .create_shadow_context = default_create_shadow_context,
    .destroy_shadow_context = default_destroy_shadow_context,
    .render_shadow = default_render_shadow,
    .make_mask = xrender_make_mask,
    .release_image = xrender_release_image,
    .is_image_transparent = default_is_image_transparent,
    .buffer_age = xrender_buffer_age,
    .max_buffer_age = 2,
    .set_image_property = xrender_set_image_property,
    .image_op = xrender_image_op,
    .clone_image = xrender_clone_image,
    .create_blur_context = xrender_create_blur_context,
    .destroy_blur_context = xrender_destroy_blur_context,
    .get_blur_size = xrender_get_blur_size};

// vim: set noet sw=8 ts=8:
