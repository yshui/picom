// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/composite.h>
#include <xcb/present.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>

#include <picom/types.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "backend/driver.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "picom.h"
#include "region.h"
#include "utils/kernel.h"
#include "utils/misc.h"
#include "x.h"

struct xrender_image_data_inner {
	ivec2 size;
	enum backend_image_format format;
	struct xrender_rounded_rectangle_cache *rounded_rectangle;
	// Pixmap that the client window draws to,
	// it will contain the content of client window.
	xcb_pixmap_t pixmap;
	// A Picture links to the Pixmap
	xcb_render_picture_t pict;
	xcb_render_pictformat_t pictfmt;
	uint8_t depth;
	// Whether we allocated it this pixmap.
	// or not, i.e. this pixmap is passed in via xrender_bind_pixmap
	bool is_pixmap_internal;
	bool has_alpha;
};

typedef struct xrender_data {
	struct backend_base base;
	/// Quirks
	uint32_t quirks;
	/// Target window
	xcb_window_t target_win;
	/// Painting target, it is either the root or the overlay
	xcb_render_picture_t target;
	/// Back buffers. Double buffer, with 1 for temporary render use
	xcb_render_picture_t back[2];
	/// Fake image to represent the back buffer
	struct xrender_image_data_inner back_image;
	/// Damaged region of the back image since the last present
	region_t back_damaged;
	/// The back buffer that is for temporary use
	/// Age of each back buffer.
	int buffer_age[2];
	/// The back buffer we should be painting into
	int curr_back;
	/// The corresponding pixmap to the back buffer
	xcb_pixmap_t back_pixmap[2];
	/// Pictures of pixel of different alpha value, used as a mask to
	/// paint transparent images
	xcb_render_picture_t alpha_pict[256];

	// XXX don't know if these are really needed

	/// 1x1 white picture
	xcb_render_picture_t white_pixel;
	/// 1x1 black picture
	xcb_render_picture_t black_pixel;

	xcb_special_event_t *present_event;

	/// Cache an X region to avoid creating and destroying it every frame. A
	/// workaround for yshui/picom#1166.
	xcb_xfixes_region_t present_region;
	/// If vsync is enabled and supported by the current system
	bool vsync;
} xrender_data;

struct xrender_blur_context {
	enum blur_method method;
	/// Blur kernels converted to X format
	struct x_convolution_kernel **x_blur_kernel;

	int resize_width, resize_height;

	/// Number of blur kernels
	int x_blur_kernel_count;
};

struct xrender_rounded_rectangle_cache {
	// A cached picture of a rounded rectangle. Xorg rasterizes shapes on CPU so it's
	// exceedingly slow.
	xcb_render_picture_t p;
	int radius;
};

static void
set_picture_scale(struct x_connection *c, xcb_render_picture_t picture, vec2 scale) {
	xcb_render_transform_t transform = {
	    .matrix11 = DOUBLE_TO_XFIXED(1.0 / scale.x),
	    .matrix22 = DOUBLE_TO_XFIXED(1.0 / scale.y),
	    .matrix33 = DOUBLE_TO_XFIXED(1.0),
	};
	x_set_error_action_abort(c, xcb_render_set_picture_transform(c->c, picture, transform));
}

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
	ret->radius = corner_radius;
	return ret;
}

static void
xrender_release_rounded_corner_cache(backend_t *base,
                                     struct xrender_rounded_rectangle_cache *cache) {
	if (!cache) {
		return;
	}

	x_free_picture(base->c, cache->p);
	free(cache);
}

static inline void xrender_set_picture_repeat(struct xrender_data *xd,
                                              xcb_render_picture_t pict, uint32_t repeat) {
	xcb_render_change_picture_value_list_t values = {
	    .repeat = repeat,
	};
	x_set_error_action_abort(
	    xd->base.c, xcb_render_change_picture(xd->base.c->c, pict, XCB_RENDER_CP_REPEAT,
	                                          (uint32_t *)&values));
}

static inline void xrender_record_back_damage(struct xrender_data *xd,
                                              struct xrender_image_data_inner *target,
                                              const region_t *region) {
	if (target == &xd->back_image && xd->vsync) {
		pixman_region32_union(&xd->back_damaged, &xd->back_damaged, region);
	}
}

/// Normalize a mask, applying inversion and corner radius.
///
/// @param extent the extent covered by mask region, in mask coordinate
/// @param alpha_pict the picture to use for alpha mask
/// @param new_origin the new origin of the normalized mask picture
/// @param allocated whether the returned picture is newly allocated
static xcb_render_picture_t
xrender_process_mask(struct xrender_data *xd, const struct backend_mask_image *mask,
                     rect_t extent, xcb_render_picture_t alpha_pict, ivec2 *new_origin,
                     bool *allocated) {
	auto inner = (struct xrender_image_data_inner *)mask->image;
	if (!inner) {
		*allocated = false;
		return alpha_pict;
	}
	if (!mask->inverted && mask->corner_radius == 0 && alpha_pict == XCB_NONE) {
		*allocated = false;
		return inner->pict;
	}
	auto const w_u16 = to_u16_checked(extent.x2 - extent.x1);
	auto const h_u16 = to_u16_checked(extent.y2 - extent.y1);
	*allocated = true;
	*new_origin =
	    (ivec2){.x = extent.x1 + mask->origin.x, .y = extent.y1 + mask->origin.y};
	x_clear_picture_clip_region(xd->base.c, inner->pict);
	auto ret = x_create_picture_with_pictfmt(
	    xd->base.c, extent.x2 - extent.x1, extent.y2 - extent.y1, inner->pictfmt,
	    inner->depth, XCB_RENDER_CP_REPEAT,
	    (xcb_render_create_picture_value_list_t[]){XCB_RENDER_REPEAT_NONE});
	xrender_set_picture_repeat(xd, inner->pict, XCB_RENDER_REPEAT_NONE);
	xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_SRC, inner->pict, XCB_NONE,
	                     ret, to_i16_checked(extent.x1 - mask->origin.x),
	                     to_i16_checked(extent.y1 - mask->origin.y), 0, 0, 0, 0,
	                     w_u16, h_u16);
	if (mask->corner_radius != 0) {
		if (inner->rounded_rectangle != NULL &&
		    inner->rounded_rectangle->radius != (int)mask->corner_radius) {
			xrender_release_rounded_corner_cache(&xd->base,
			                                     inner->rounded_rectangle);
			inner->rounded_rectangle = NULL;
		}
		if (inner->rounded_rectangle == NULL) {
			inner->rounded_rectangle = xrender_make_rounded_corner_cache(
			    xd->base.c, xd->white_pixel, inner->size.width,
			    inner->size.height, (int)mask->corner_radius);
		}
		xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_IN_REVERSE,
		                     inner->rounded_rectangle->p, XCB_NONE, ret,
		                     to_i16_checked(extent.x1), to_i16_checked(extent.y1),
		                     0, 0, 0, 0, w_u16, h_u16);
	}

	if (mask->inverted) {
		xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_XOR, xd->white_pixel,
		                     XCB_NONE, ret, 0, 0, 0, 0, 0, 0, w_u16, h_u16);
	}

	if (alpha_pict != XCB_NONE) {
		xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_IN_REVERSE, alpha_pict,
		                     XCB_NONE, ret, 0, 0, 0, 0, 0, 0, w_u16, h_u16);
	}

	return ret;
}

static bool xrender_blit(struct backend_base *base, ivec2 origin,
                         image_handle target_handle, const struct backend_blit_args *args) {
	auto xd = (struct xrender_data *)base;
	auto inner = (struct xrender_image_data_inner *)args->source_image;
	auto target = (struct xrender_image_data_inner *)target_handle;
	bool mask_allocated = false;
	auto mask_pict = xd->alpha_pict[(int)(args->opacity * MAX_ALPHA)];
	auto extent = *pixman_region32_extents(args->target_mask);
	if (!pixman_region32_not_empty(args->target_mask)) {
		return true;
	}
	int16_t mask_pict_dst_x = 0, mask_pict_dst_y = 0;
	if (args->source_mask != NULL) {
		ivec2 mask_origin = args->source_mask->origin;
		mask_pict = xrender_process_mask(xd, args->source_mask, extent,
		                                 args->opacity < 1.0 ? mask_pict : XCB_NONE,
		                                 &mask_origin, &mask_allocated);
		mask_pict_dst_x = to_i16_checked(-mask_origin.x);
		mask_pict_dst_y = to_i16_checked(-mask_origin.y);
	}

	// After this point, mask_pict and mask->region have different origins.

	bool has_alpha = inner->has_alpha || args->opacity != 1;
	auto const tmpw = to_u16_checked(inner->size.width);
	auto const tmph = to_u16_checked(inner->size.height);
	auto const tmpew = to_u16_saturated(args->effective_size.width * args->scale.x);
	auto const tmpeh = to_u16_saturated(args->effective_size.height * args->scale.y);
	const xcb_render_color_t dim_color = {
	    .red = 0, .green = 0, .blue = 0, .alpha = (uint16_t)(0xffff * args->dim)};

	// Clip region of rendered_pict might be set during rendering, clear it to
	// make sure we get everything into the buffer
	x_clear_picture_clip_region(xd->base.c, inner->pict);
	xrender_set_picture_repeat(xd, inner->pict, XCB_RENDER_REPEAT_NORMAL);

	x_set_picture_clip_region(xd->base.c, target->pict, 0, 0, args->target_mask);
	if (args->corner_radius != 0) {
		if (inner->rounded_rectangle != NULL &&
		    inner->rounded_rectangle->radius != (int)args->corner_radius) {
			xrender_release_rounded_corner_cache(&xd->base,
			                                     inner->rounded_rectangle);
			inner->rounded_rectangle = NULL;
		}
		if (inner->rounded_rectangle == NULL) {
			inner->rounded_rectangle = xrender_make_rounded_corner_cache(
			    xd->base.c, xd->white_pixel, inner->size.width,
			    inner->size.height, (int)args->corner_radius);
		}
	}

	set_picture_scale(xd->base.c, mask_pict, args->scale);

	if (((args->color_inverted || args->dim != 0) && has_alpha) ||
	    args->corner_radius != 0) {
		// Apply image properties using a temporary image, because the source
		// image is transparent or will get transparent corners. Otherwise the
		// properties can be applied directly on the target image.
		// Also force a 32-bit ARGB format for transparent corners, otherwise the
		// corners become black.
		auto pictfmt = inner->pictfmt;
		uint8_t depth = inner->depth;
		if (args->corner_radius != 0 && inner->depth != 32) {
			pictfmt = x_get_pictfmt_for_standard(xd->base.c,
			                                     XCB_PICT_STANDARD_ARGB_32);
			depth = 32;
		}
		auto tmp_pict = x_create_picture_with_pictfmt(
		    xd->base.c, inner->size.width, inner->size.height, pictfmt, depth, 0, NULL);

		vec2 inverse_scale = (vec2){
		    .x = 1.0 / args->scale.x,
		    .y = 1.0 / args->scale.y,
		};
		if (vec2_eq(args->scale, SCALE_IDENTITY)) {
			x_set_picture_clip_region(
			    xd->base.c, tmp_pict, to_i16_checked(-origin.x),
			    to_i16_checked(-origin.y), args->target_mask);
		} else {
			// We need to scale the target_mask back so it's in the source's
			// coordinate space.
			scoped_region_t source_mask_region;
			pixman_region32_init(&source_mask_region);
			pixman_region32_copy(&source_mask_region, args->target_mask);
			region_scale(&source_mask_region, origin, inverse_scale);
			x_set_picture_clip_region(
			    xd->base.c, tmp_pict, to_i16_checked(-origin.x),
			    to_i16_checked(-origin.y), &source_mask_region);
		}
		// Copy source -> tmp
		xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_SRC, inner->pict,
		                     XCB_NONE, tmp_pict, 0, 0, 0, 0, 0, 0, tmpw, tmph);

		if (args->color_inverted) {
			if (inner->has_alpha) {
				auto tmp_pict2 = x_create_picture_with_pictfmt(
				    xd->base.c, tmpw, tmph, inner->pictfmt, inner->depth,
				    0, NULL);
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

		if (args->dim != 0) {
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

		if (args->corner_radius != 0 && inner->rounded_rectangle != NULL) {
			// Clip tmp_pict with a rounded rectangle
			xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_IN_REVERSE,
			                     inner->rounded_rectangle->p, XCB_NONE,
			                     tmp_pict, 0, 0, 0, 0, 0, 0, tmpw, tmph);
		}

		set_picture_scale(xd->base.c, tmp_pict, args->scale);
		// Transformations don't affect the picture's clip region, so we need to
		// set it again
		x_set_picture_clip_region(xd->base.c, tmp_pict, to_i16_checked(-origin.x),
		                          to_i16_checked(-origin.y), args->target_mask);

		xcb_render_composite(xd->base.c->c, XCB_RENDER_PICT_OP_OVER, tmp_pict,
		                     mask_pict, target->pict, 0, 0, mask_pict_dst_x,
		                     mask_pict_dst_y, to_i16_checked(origin.x),
		                     to_i16_checked(origin.y), tmpew, tmpeh);
		xcb_render_free_picture(xd->base.c->c, tmp_pict);
	} else {
		uint8_t op = (has_alpha ? XCB_RENDER_PICT_OP_OVER : XCB_RENDER_PICT_OP_SRC);

		set_picture_scale(xd->base.c, inner->pict, args->scale);

		xcb_render_composite(xd->base.c->c, op, inner->pict, mask_pict,
		                     target->pict, 0, 0, mask_pict_dst_x, mask_pict_dst_y,
		                     to_i16_checked(origin.x), to_i16_checked(origin.y),
		                     tmpew, tmpeh);
		if (args->dim != 0 || args->color_inverted) {
			// Apply properties, if we reach here, then has_alpha == false
			assert(!has_alpha);
			if (args->color_inverted) {
				xcb_render_composite(
				    xd->base.c->c, XCB_RENDER_PICT_OP_DIFFERENCE,
				    xd->white_pixel, XCB_NONE, target->pict, 0, 0, 0, 0,
				    to_i16_checked(origin.x), to_i16_checked(origin.y),
				    tmpew, tmpeh);
			}

			if (args->dim != 0) {
				// Dim the actually content of window
				xcb_rectangle_t rect = {
				    .x = to_i16_checked(origin.x),
				    .y = to_i16_checked(origin.y),
				    .width = tmpew,
				    .height = tmpeh,
				};

				xcb_render_fill_rectangles(
				    xd->base.c->c, XCB_RENDER_PICT_OP_OVER, target->pict,
				    dim_color, 1, &rect);
			}
		}
	}
	if (mask_allocated) {
		x_free_picture(xd->base.c, mask_pict);
	}
	xrender_record_back_damage(xd, target, args->target_mask);
	return true;
}

static bool
xrender_clear(struct backend_base *base, image_handle target_handle, struct color color) {
	auto xd = (struct xrender_data *)base;
	auto target = (struct xrender_image_data_inner *)target_handle;
	xcb_render_color_t col = {
	    .red = (uint16_t)(color.red * 0xffff),
	    .green = (uint16_t)(color.green * 0xffff),
	    .blue = (uint16_t)(color.blue * 0xffff),
	    .alpha = (uint16_t)(color.alpha * 0xffff),
	};
	x_clear_picture_clip_region(base->c, target->pict);
	xcb_render_fill_rectangles(
	    xd->base.c->c, XCB_RENDER_PICT_OP_SRC, target->pict, col, 1,
	    (xcb_rectangle_t[]){{.x = 0,
	                         .y = 0,
	                         .width = to_u16_checked(target->size.width),
	                         .height = to_u16_checked(target->size.height)}});
	if (target == &xd->back_image) {
		pixman_region32_clear(&xd->back_damaged);
		pixman_region32_union_rect(&xd->back_damaged, &xd->back_damaged, 0, 0,
		                           (unsigned)target->size.width,
		                           (unsigned)target->size.height);
	}
	return true;
}

static bool
xrender_copy_area(struct backend_base *base, ivec2 origin, image_handle target_handle,
                  image_handle source_handle, const region_t *region) {
	auto xd = (struct xrender_data *)base;
	auto source = (struct xrender_image_data_inner *)source_handle;
	auto target = (struct xrender_image_data_inner *)target_handle;
	auto extent = pixman_region32_extents(region);
	x_set_picture_clip_region(base->c, source->pict, 0, 0, region);
	x_clear_picture_clip_region(base->c, target->pict);
	xrender_set_picture_repeat(xd, source->pict, XCB_RENDER_REPEAT_PAD);
	xcb_render_composite(
	    base->c->c, XCB_RENDER_PICT_OP_SRC, source->pict, XCB_NONE, target->pict,
	    to_i16_checked(extent->x1), to_i16_checked(extent->y1), 0, 0,
	    to_i16_checked(origin.x + extent->x1), to_i16_checked(origin.y + extent->y1),
	    to_u16_checked(extent->x2 - extent->x1), to_u16_checked(extent->y2 - extent->y1));
	xrender_record_back_damage(xd, target, region);
	return true;
}

static bool xrender_blur(struct backend_base *base, ivec2 origin,
                         image_handle target_handle, const struct backend_blur_args *args) {
	auto bctx = (struct xrender_blur_context *)args->blur_context;
	auto source = (struct xrender_image_data_inner *)args->source_image;
	auto target = (struct xrender_image_data_inner *)target_handle;
	if (bctx->method == BLUR_METHOD_NONE) {
		return true;
	}

	auto xd = (struct xrender_data *)base;
	auto c = xd->base.c;
	if (!pixman_region32_not_empty(args->target_mask)) {
		return true;
	}

	region_t reg_op_resized =
	    resize_region(args->target_mask, bctx->resize_width, bctx->resize_height);

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
	    x_create_picture_with_pictfmt(xd->base.c, width_resized, height_resized,
	                                  source->pictfmt, source->depth, pic_attrs_mask,
	                                  &pic_attrs),
	    x_create_picture_with_pictfmt(xd->base.c, width_resized, height_resized,
	                                  source->pictfmt, source->depth, pic_attrs_mask,
	                                  &pic_attrs)};

	if (!tmp_picture[0] || !tmp_picture[1]) {
		log_error("Failed to build intermediate Picture.");
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

	xcb_render_picture_t src_pict = source->pict;
	auto mask_pict = xd->alpha_pict[(int)(args->opacity * MAX_ALPHA)];
	bool mask_allocated = false;
	ivec2 mask_pict_origin = {};
	if (args->source_mask != NULL) {
		// Translate the target mask region to the mask's coordinate
		auto mask_extent = *pixman_region32_extents(args->target_mask);
		region_translate_rect(
		    mask_extent, ivec2_neg(ivec2_add(origin, args->source_mask->origin)));
		mask_pict_origin = args->source_mask->origin;
		mask_pict = xrender_process_mask(xd, args->source_mask, mask_extent,
		                                 args->opacity != 1.0 ? mask_pict : XCB_NONE,
		                                 &mask_pict_origin, &mask_allocated);
		mask_pict_origin.x -= extent_resized->x1;
		mask_pict_origin.y -= extent_resized->y1;
	}
	x_set_picture_clip_region(c, src_pict, 0, 0, &reg_op_resized);
	x_set_picture_clip_region(c, target->pict, 0, 0, args->target_mask);

	// For more than 1 pass, we do:
	//   source -(pass 1)-> tmp0 -(pass 2)-> tmp1 ...
	//   -(pass n-1)-> tmp0 or tmp1 -(pass n)-> target
	// For 1 pass, we do:
	// (if source == target)
	//   source -(pass 1)-> tmp0 -(copy)-> target
	// (if source != target)
	//   source -(pass 1)-> target
	xcb_render_picture_t dst_pict = target == source ? tmp_picture[0] : target->pict;
	ivec2 src_origin = {.x = extent_resized->x1, .y = extent_resized->y1};
	ivec2 dst_origin = {};
	int npasses = bctx->x_blur_kernel_count;
	if (source == target && npasses == 1) {
		npasses = 2;
	}
	for (int i = 0; i < npasses; i++) {
		// Copy from source picture to destination. The filter must
		// be applied on source picture, to get the nearby pixels outside the
		// window.
		xcb_render_picture_t pass_mask_pict =
		    dst_pict == target->pict ? mask_pict : XCB_NONE;
		const uint8_t op = dst_pict == target->pict ? XCB_RENDER_PICT_OP_OVER
		                                            : XCB_RENDER_PICT_OP_SRC;
		if (i < bctx->x_blur_kernel_count) {
			xcb_render_set_picture_filter(
			    c->c, src_pict, to_u16_checked(strlen(filter)), filter,
			    to_u32_checked(bctx->x_blur_kernel[i]->size),
			    bctx->x_blur_kernel[i]->kernel);
		}

		// clang-format off
		xcb_render_composite(c->c, op, src_pict, pass_mask_pict, dst_pict,
		    to_i16_checked(src_origin.x)         , to_i16_checked(src_origin.y),
		    to_i16_checked(-mask_pict_origin.x)  , to_i16_checked(-mask_pict_origin.y),
		    to_i16_checked(dst_origin.x)         , to_i16_checked(dst_origin.y),
		    width_resized                        , height_resized);
		// clang-format on

		// reset filter
		xcb_render_set_picture_filter(
		    c->c, src_pict, to_u16_checked(strlen(filter0)), filter0, 0, NULL);

		auto next_tmp = src_pict == source->pict ? tmp_picture[1] : src_pict;
		src_pict = dst_pict;
		if (i + 1 == npasses - 1) {
			// Intermediary to target
			dst_pict = target->pict;
			dst_origin = (ivec2){.x = origin.x + extent_resized->x1,
			                     .y = origin.y + extent_resized->y1};
		} else {
			// Intermediary to intermediary
			dst_pict = next_tmp;
			dst_origin = (ivec2){.x = 0, .y = 0};
		}
		src_origin = (ivec2){.x = 0, .y = 0};
	}

	if (mask_allocated) {
		x_free_picture(c, mask_pict);
	}
	x_free_picture(c, tmp_picture[0]);
	x_free_picture(c, tmp_picture[1]);
	pixman_region32_fini(&reg_op_resized);

	xrender_record_back_damage(xd, target, args->target_mask);
	return true;
}

static image_handle
xrender_bind_pixmap(backend_t *base, xcb_pixmap_t pixmap, struct xvisual_info fmt) {
	xcb_generic_error_t *e;
	auto r = xcb_get_geometry_reply(base->c->c, xcb_get_geometry(base->c->c, pixmap), &e);
	if (!r) {
		log_error("Invalid pixmap: %#010x", pixmap);
		x_print_error(e->full_sequence, e->major_code, e->minor_code, e->error_code);
		free(e);
		return NULL;
	}

	auto img = ccalloc(1, struct xrender_image_data_inner);
	img->depth = (uint8_t)fmt.visual_depth;
	img->has_alpha = fmt.alpha_size > 0;
	img->size = (ivec2){
	    .width = r->width,
	    .height = r->height,
	};
	img->format = BACKEND_IMAGE_FORMAT_PIXMAP;
	img->pixmap = pixmap;
	xcb_render_create_picture_value_list_t pic_attrs = {.repeat = XCB_RENDER_REPEAT_NORMAL};
	img->pict = x_create_picture_with_visual_and_pixmap(
	    base->c, fmt.visual, pixmap, XCB_RENDER_CP_REPEAT, &pic_attrs);
	auto pictfmt_info = x_get_pictform_for_visual(base->c, fmt.visual);
	img->pictfmt = pictfmt_info->id;
	assert(pictfmt_info->depth == img->depth);
	img->is_pixmap_internal = false;
	free(r);

	if (img->pict == XCB_NONE) {
		free(img);
		return NULL;
	}
	return (image_handle)img;
}

static xcb_pixmap_t xrender_release_image(backend_t *base, image_handle image) {
	auto img = (struct xrender_image_data_inner *)image;
	auto xd = (struct xrender_data *)base;
	if (img == &xd->back_image) {
		return XCB_NONE;
	}

	xrender_release_rounded_corner_cache(base, img->rounded_rectangle);
	x_free_picture(base->c, img->pict);
	if (img->is_pixmap_internal && img->pixmap != XCB_NONE) {
		xcb_free_pixmap(base->c->c, img->pixmap);
		img->pixmap = XCB_NONE;
	}

	auto pixmap = img->pixmap;
	free(img);
	return pixmap;
}

static void xrender_deinit(backend_t *backend_data) {
	auto xd = (struct xrender_data *)backend_data;
	for (int i = 0; i < 256; i++) {
		x_free_picture(xd->base.c, xd->alpha_pict[i]);
	}
	x_free_picture(xd->base.c, xd->target);
	for (int i = 0; i < 2; i++) {
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

static bool xrender_present(struct backend_base *base) {
	auto xd = (struct xrender_data *)base;
	if (xd->vsync) {
		// Make sure we got reply from PresentPixmap before waiting for events,
		// to avoid deadlock
		auto e = xcb_request_check(
		    base->c->c,
		    xcb_present_pixmap_checked(
		        base->c->c, xd->target_win, xd->back_pixmap[xd->curr_back], 0, XCB_NONE,
		        x_set_region(base->c, xd->present_region, &xd->back_damaged)
		            ? xd->present_region
		            : XCB_NONE,
		        0, 0, XCB_NONE, XCB_NONE, XCB_NONE, 0, 0, 0, 0, 0, NULL));
		if (e) {
			log_error("Failed to present pixmap");
			free(e);
			return false;
		}
		// TODO(yshui) don't block wait for present completion
		auto pev = (xcb_present_generic_event_t *)xcb_wait_for_special_event(
		    base->c->c, xd->present_event);
		if (!pev) {
			// We don't know what happened, maybe X died
			// But reset buffer age, so in case we do recover, we will
			// render correctly.
			xd->buffer_age[0] = xd->buffer_age[1] = -1;
			return false;
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
			xd->back_image.pict = xd->back[xd->curr_back];
		}
		free(pev);
	}
	// Without vsync, we are rendering into the front buffer directly
	pixman_region32_clear(&xd->back_damaged);
	return true;
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

static bool xrender_apply_alpha(struct backend_base *base, image_handle image,
                                double alpha, const region_t *reg_op) {
	auto xd = (struct xrender_data *)base;
	auto img = (struct xrender_image_data_inner *)image;
	assert(reg_op);

	if (!pixman_region32_not_empty(reg_op) || alpha == 1) {
		return true;
	}

	auto alpha_pict = xd->alpha_pict[(int)((1 - alpha) * MAX_ALPHA)];
	x_set_picture_clip_region(base->c, img->pict, 0, 0, reg_op);
	xcb_render_composite(base->c->c, XCB_RENDER_PICT_OP_OUT_REVERSE, alpha_pict, XCB_NONE,
	                     img->pict, 0, 0, 0, 0, 0, 0, to_u16_checked(img->size.width),
	                     to_u16_checked(img->size.height));
	xrender_record_back_damage(xd, img, reg_op);
	return true;
}

static void *
xrender_create_blur_context(backend_t *base attr_unused, enum blur_method method,
                            enum backend_image_format format attr_unused, void *args) {
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
const struct backend_operations xrender_ops;
static backend_t *xrender_init(session_t *ps, xcb_window_t target) {
	if (ps->o.dithered_present) {
		log_warn("\"dithered-present\" is not supported by the xrender backend, "
		         "it will be ignored.");
	}
	if (ps->o.max_brightness < 1.0) {
		log_warn("\"max-brightness\" is not supported by the xrender backend, it "
		         "will be ignored.");
	}

	auto xd = ccalloc(1, struct xrender_data);
	init_backend_base(&xd->base, ps);
	xd->base.ops = xrender_ops;

	for (int i = 0; i <= MAX_ALPHA; ++i) {
		double o = (double)i / (double)MAX_ALPHA;
		xd->alpha_pict[i] = solid_picture(&ps->c, false, o, 0, 0, 0);
		assert(xd->alpha_pict[i] != XCB_NONE);
	}

	auto root_pictfmt = x_get_pictform_for_visual(&ps->c, ps->c.screen_info->root_visual);
	assert(root_pictfmt->depth == ps->c.screen_info->root_depth);
	xd->back_image = (struct xrender_image_data_inner){
	    .pictfmt = root_pictfmt->id,
	    .depth = ps->c.screen_info->root_depth,
	    .has_alpha = false,
	    .format = BACKEND_IMAGE_FORMAT_PIXMAP,
	    .size = {.width = ps->root_width, .height = ps->root_height},
	};
	xd->black_pixel = solid_picture(&ps->c, true, 1, 0, 0, 0);
	xd->white_pixel = solid_picture(&ps->c, true, 1, 1, 1, 1);

	xd->target_win = target;
	xcb_render_create_picture_value_list_t pa = {
	    .subwindowmode = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS,
	};
	xd->target = x_create_picture_with_visual_and_pixmap(
	    &ps->c, ps->c.screen_info->root_visual, xd->target_win,
	    XCB_RENDER_CP_SUBWINDOW_MODE, &pa);

	xd->vsync = ps->o.vsync;
	if (ps->present_exists) {
		auto eid = x_new_id(&ps->c);
		auto e =
		    xcb_request_check(ps->c.c, xcb_present_select_input_checked(
		                                   ps->c.c, eid, xd->target_win,
		                                   XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
		if (e) {
			log_error("Cannot select present input, vsync will be disabled");
			xd->vsync = false;
			free(e);
		}

		xd->present_event =
		    xcb_register_for_special_xge(ps->c.c, &xcb_present_id, eid, NULL);
		if (!xd->present_event) {
			log_error("Cannot register for special XGE, vsync will be "
			          "disabled");
			xd->vsync = false;
		}
	} else {
		xd->vsync = false;
	}

	if (xd->vsync) {
		xd->present_region = x_create_region(&ps->c, &ps->screen_reg);
	}

	// We might need to do double buffering for vsync, and buffer 0 and 1 are for
	// double buffering.
	int buffer_count = xd->vsync ? 2 : 0;
	for (int i = 0; i < buffer_count; i++) {
		xd->back_pixmap[i] = x_create_pixmap(&ps->c, ps->c.screen_info->root_depth,
		                                     to_u16_checked(ps->root_width),
		                                     to_u16_checked(ps->root_height));
		const uint32_t pic_attrs_mask = XCB_RENDER_CP_REPEAT;
		const xcb_render_create_picture_value_list_t pic_attrs = {
		    .repeat = XCB_RENDER_REPEAT_PAD};
		xd->back[i] = x_create_picture_with_visual_and_pixmap(
		    &ps->c, ps->c.screen_info->root_visual, xd->back_pixmap[i],
		    pic_attrs_mask, &pic_attrs);
		xd->buffer_age[i] = -1;
		if (xd->back_pixmap[i] == XCB_NONE || xd->back[i] == XCB_NONE) {
			log_error("Cannot create pixmap for rendering");
			goto err;
		}
	}
	xd->curr_back = 0;
	xd->back_image.pict = xd->vsync ? xd->back[xd->curr_back] : xd->target;

	auto drivers = detect_driver(xd->base.c->c, &xd->base, xd->target_win);
	if (drivers & DRIVER_MODESETTING) {
		// I believe other xf86-video drivers have accelerated blur?
		xd->quirks |= BACKEND_QUIRK_SLOW_BLUR;
	}

	return &xd->base;
err:
	xrender_deinit(&xd->base);
	return NULL;
}

static image_handle
xrender_new_image(struct backend_base *base, enum backend_image_format format, ivec2 size) {
	auto xd = (struct xrender_data *)base;
	auto img = ccalloc(1, struct xrender_image_data_inner);
	img->format = format;
	img->size = size;
	img->has_alpha = true;
	if (format == BACKEND_IMAGE_FORMAT_MASK) {
		img->depth = 8;
		img->pictfmt = x_get_pictfmt_for_standard(base->c, XCB_PICT_STANDARD_A_8);
	} else {
		img->depth = 32;
		img->pictfmt =
		    x_get_pictfmt_for_standard(xd->base.c, XCB_PICT_STANDARD_ARGB_32);
	}
	img->pixmap = x_create_pixmap(xd->base.c, img->depth, to_u16_checked(size.width),
	                              to_u16_checked(size.height));
	if (img->pixmap == XCB_NONE) {
		free(img);
		return NULL;
	}
	img->pict = x_create_picture_with_pictfmt_and_pixmap(xd->base.c, img->pictfmt,
	                                                     img->pixmap, 0, NULL);
	if (img->pict == XCB_NONE) {
		xcb_free_pixmap(xd->base.c->c, img->pixmap);
		free(img);
		return NULL;
	}
	img->is_pixmap_internal = true;
	return (image_handle)img;
}

static uint32_t xrender_image_capabilities(struct backend_base *base attr_unused,
                                           image_handle image attr_unused) {
	// All of xrender's picture can be used as both a source and a destination.
	return BACKEND_IMAGE_CAP_DST | BACKEND_IMAGE_CAP_SRC;
}

static bool xrender_is_format_supported(struct backend_base *base attr_unused,
                                        enum backend_image_format format) {
	return format == BACKEND_IMAGE_FORMAT_MASK || format == BACKEND_IMAGE_FORMAT_PIXMAP;
}

static image_handle xrender_back_buffer(struct backend_base *base) {
	auto xd = (struct xrender_data *)base;
	return (image_handle)&xd->back_image;
}

uint32_t xrender_quirks(struct backend_base *base) {
	return ((struct xrender_data *)base)->quirks;
}

static int xrender_max_buffer_age(struct backend_base *base) {
	return ((struct xrender_data *)base)->vsync ? 2 : 1;
}

#define PICOM_BACKEND_XRENDER_MAJOR (0UL)
#define PICOM_BACKEND_XRENDER_MINOR (1UL)

static void xrender_version(struct backend_base * /*base*/, uint64_t *major, uint64_t *minor) {
	*major = PICOM_BACKEND_XRENDER_MAJOR;
	*minor = PICOM_BACKEND_XRENDER_MINOR;
}

const struct backend_operations xrender_ops = {
    .apply_alpha = xrender_apply_alpha,
    .back_buffer = xrender_back_buffer,
    .bind_pixmap = xrender_bind_pixmap,
    .blit = xrender_blit,
    .blur = xrender_blur,
    .clear = xrender_clear,
    .copy_area = xrender_copy_area,
    .copy_area_quantize = xrender_copy_area,
    .image_capabilities = xrender_image_capabilities,
    .is_format_supported = xrender_is_format_supported,
    .new_image = xrender_new_image,
    .present = xrender_present,
    .quirks = xrender_quirks,
    .version = xrender_version,
    .release_image = xrender_release_image,

    .init = xrender_init,
    .deinit = xrender_deinit,
    // TODO(yshui) make blur faster so we can use `backend_render_shadow_from_mask` for
    //             `render_shadow`, and `backend_compat_shadow_from_mask` for
    //             `shadow_from_mask`
    .buffer_age = xrender_buffer_age,
    .max_buffer_age = xrender_max_buffer_age,
    .create_blur_context = xrender_create_blur_context,
    .destroy_blur_context = xrender_destroy_blur_context,
    .get_blur_size = xrender_get_blur_size
    // end
};

BACKEND_ENTRYPOINT(xrender_register) {
	if (!backend_register(PICOM_BACKEND_MAJOR, PICOM_BACKEND_MINOR, "xrender",
	                      xrender_ops.init, true)) {
		log_error("Failed to register xrender backend");
	}
}

// vim: set noet sw=8 ts=8:
