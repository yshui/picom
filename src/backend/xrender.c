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
#include "region.h"
#include "utils.h"
#include "win.h"
#include "x.h"

#define auto __auto_type

typedef struct _xrender_data {
	backend_t base;
	/// If vsync is enabled and supported by the current system
	bool vsync;
	xcb_visualid_t default_visual;
	/// The idle fence for the present extension
	xcb_sync_fence_t idle_fence;
	/// The target window
	xcb_window_t target_win;
	/// The painting target, it is either the root or the overlay
	xcb_render_picture_t target;
	/// A back buffer
	xcb_render_picture_t back[2];
	/// Age of each back buffer
	int buffer_age[2];
	/// The back buffer we should be painting into
	int curr_back;
	/// The corresponding pixmap to the back buffer
	xcb_pixmap_t back_pixmap[2];
	/// The original root window content, usually the wallpaper.
	/// We save it so we don't loss the wallpaper when we paint over
	/// it.
	xcb_render_picture_t root_pict;
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

	/// Blur kernels converted to X format
	xcb_render_fixed_t *x_blur_kern[MAX_BLUR_PASS];
	/// Number of elements in each blur kernel
	size_t x_blur_kern_size[MAX_BLUR_PASS];

	xcb_special_event_t *present_event;
} xrender_data;

struct _xrender_image_data {
	// Pixmap that the client window draws to,
	// it will contain the content of client window.
	xcb_pixmap_t pixmap;
	// A Picture links to the Pixmap
	xcb_render_picture_t pict;
	long width, height;
	bool has_alpha;
	double opacity;
	xcb_visualid_t visual;
	uint8_t depth;
	bool owned;
};

static void compose(backend_t *base, void *img_data, int dst_x, int dst_y,
                    const region_t *reg_paint, const region_t *reg_visible) {
	struct _xrender_data *xd = (void *)base;
	struct _xrender_image_data *img = img_data;
	int op = (img->has_alpha ? XCB_RENDER_PICT_OP_OVER : XCB_RENDER_PICT_OP_SRC);
	auto alpha_pict = xd->alpha_pict[(int)(img->opacity * 255.0)];
	region_t reg;
	pixman_region32_init(&reg);
	pixman_region32_intersect(&reg, (region_t *)reg_paint, (region_t *)reg_visible);

	// Clip region of rendered_pict might be set during rendering, clear it to make
	// sure we get everything into the buffer
	x_clear_picture_clip_region(base->c, img->pict);

	x_set_picture_clip_region(base->c, xd->back[xd->curr_back], 0, 0, &reg);
	xcb_render_composite(base->c, op, img->pict, alpha_pict, xd->back[xd->curr_back],
	                     0, 0, 0, 0, dst_x, dst_y, img->width, img->height);
	pixman_region32_fini(&reg);
}

static bool blur(backend_t *backend_data, double opacity, const region_t *reg_blur,
                 const region_t *reg_visible) {
	struct _xrender_data *xd = (void *)backend_data;
	xcb_connection_t *c = xd->base.c;
	region_t reg_op;
	pixman_region32_init(&reg_op);
	pixman_region32_intersect(&reg_op, (region_t *)reg_blur, (region_t *)reg_visible);
	if (!pixman_region32_not_empty(&reg_op)) {
		pixman_region32_fini(&reg_op);
		return true;
	}

	const pixman_box32_t *extent = pixman_region32_extents(&reg_op);
	const int height = extent->y2 - extent->y1;
	const int width = extent->x2 - extent->x1;
	int src_x = extent->x1, src_y = extent->y1;
	static const char *filter0 = "Nearest";        // The "null" filter
	static const char *filter = "convolution";

	// Create a buffer for storing blurred picture, make it just big enough
	// for the blur region
	xcb_render_picture_t tmp_picture[2] = {
	    x_create_picture_with_visual(xd->base.c, xd->base.root, width, height,
	                                 xd->default_visual, 0, NULL),
	    x_create_picture_with_visual(xd->base.c, xd->base.root, width, height,
	                                 xd->default_visual, 0, NULL)};

	if (!tmp_picture[0] || !tmp_picture[1]) {
		log_error("Failed to build intermediate Picture.");
		pixman_region32_fini(&reg_op);
		return false;
	}

	region_t clip;
	pixman_region32_init(&clip);
	pixman_region32_copy(&clip, &reg_op);
	pixman_region32_translate(&clip, -src_x, -src_y);
	x_set_picture_clip_region(c, tmp_picture[0], 0, 0, &clip);
	x_set_picture_clip_region(c, tmp_picture[1], 0, 0, &clip);
	pixman_region32_fini(&clip);

	// The multipass blur implemented here is not correct, but this is what old
	// compton did anyway. XXX
	xcb_render_picture_t src_pict = xd->back[xd->curr_back], dst_pict = tmp_picture[0];
	auto alpha_pict = xd->alpha_pict[(int)(opacity * 255)];
	int current = 0;
	x_set_picture_clip_region(c, src_pict, 0, 0, &reg_op);

	// For more than 1 pass, we do:
	//   back -(pass 1)-> tmp0 -(pass 2)-> tmp1 ...
	//   -(pass n-1)-> tmp0 or tmp1 -(pass n)-> back
	// For 1 pass, we do
	//   back -(pass 1)-> tmp0 -(copy)-> target_buffer
	int i;
	for (i = 0; xd->x_blur_kern[i]; i++) {
		assert(i < MAX_BLUR_PASS - 1);

		// Copy from source picture to destination. The filter must
		// be applied on source picture, to get the nearby pixels outside the
		// window.
		// TODO cache converted blur_kerns
		xcb_render_set_picture_filter(c, src_pict, strlen(filter), filter,
		                              xd->x_blur_kern_size[i], xd->x_blur_kern[i]);

		if (xd->x_blur_kern[i + 1] || i == 0) {
			// This is not the last pass, or this is the first pass
			xcb_render_composite(c, XCB_RENDER_PICT_OP_SRC, src_pict,
			                     XCB_NONE, dst_pict, src_x, src_y, 0, 0, 0, 0,
			                     width, height);
		} else {
			// This is the last pass, and this is also not the first
			xcb_render_composite(c, XCB_RENDER_PICT_OP_OVER, src_pict,
			                     alpha_pict, xd->back[xd->curr_back], 0, 0, 0,
			                     0, src_x, src_y, width, height);
		}

		// reset filter
		xcb_render_set_picture_filter(c, src_pict, strlen(filter0), filter0, 0, NULL);

		src_pict = tmp_picture[current];
		dst_pict = tmp_picture[!current];
		src_x = 0;
		src_y = 0;
		current = !current;
	}

	// There is only 1 pass
	if (i == 1) {
		xcb_render_composite(c, XCB_RENDER_PICT_OP_OVER, src_pict, alpha_pict,
		                     xd->back[xd->curr_back], 0, 0, 0, 0, extent->x1, extent->y1,
		                     width, height);
	}

	xcb_render_free_picture(c, tmp_picture[0]);
	xcb_render_free_picture(c, tmp_picture[1]);
	pixman_region32_fini(&reg_op);
	return true;
}

static void *
bind_pixmap(backend_t *base, xcb_pixmap_t pixmap, struct xvisual_info fmt, bool owned) {
	xcb_generic_error_t *e;
	auto r = xcb_get_geometry_reply(base->c, xcb_get_geometry(base->c, pixmap), &e);
	if (!r) {
		log_error("Invalid pixmap: %#010x", pixmap);
		x_print_error(e->full_sequence, e->major_code, e->minor_code, e->error_code);
		return NULL;
	}

	auto img = ccalloc(1, struct _xrender_image_data);
	img->depth = fmt.visual_depth;
	img->width = r->width;
	img->height = r->height;
	img->pixmap = pixmap;
	img->opacity = 1;
	img->has_alpha = fmt.alpha_size != 0;
	img->pict =
	    x_create_picture_with_visual_and_pixmap(base->c, fmt.visual, pixmap, 0, NULL);
	img->owned = owned;
	img->visual = fmt.visual;
	if (img->pict == XCB_NONE) {
		free(img);
		return NULL;
	}
	return img;
}

static void release_image(backend_t *base, void *image) {
	struct _xrender_image_data *img = image;
	xcb_render_free_picture(base->c, img->pict);
	if (img->owned) {
		xcb_free_pixmap(base->c, img->pixmap);
	}
}

static void deinit(backend_t *backend_data) {
	struct _xrender_data *xd = (void *)backend_data;
	for (int i = 0; i < 256; i++) {
		xcb_render_free_picture(xd->base.c, xd->alpha_pict[i]);
	}
	xcb_render_free_picture(xd->base.c, xd->target);
	xcb_render_free_picture(xd->base.c, xd->root_pict);
	for (int i = 0; i < 2; i++) {
		xcb_render_free_picture(xd->base.c, xd->back[i]);
		xcb_free_pixmap(xd->base.c, xd->back_pixmap[i]);
	}
	for (int i = 0; xd->x_blur_kern[i]; i++) {
		free(xd->x_blur_kern[i]);
	}
	if (xd->present_event) {
		xcb_unregister_for_special_event(xd->base.c, xd->present_event);
	}
	xcb_render_free_picture(xd->base.c, xd->white_pixel);
	xcb_render_free_picture(xd->base.c, xd->black_pixel);
	free(xd);
}

static void present(backend_t *base) {
	struct _xrender_data *xd = (void *)base;

	if (xd->vsync) {
		// Make sure we got reply from PresentPixmap before waiting for events,
		// to avoid deadlock
		auto e = xcb_request_check(
		    base->c, xcb_present_pixmap_checked(
		                 xd->base.c, xd->target_win,
		                 xd->back_pixmap[xd->curr_back], 0, XCB_NONE, XCB_NONE, 0,
		                 0, XCB_NONE, XCB_NONE, XCB_NONE, 0, 0, 0, 0, 0, NULL));
		if (e) {
			log_error("Failed to present pixmap");
			free(e);
			return;
		}
		// TODO don't block wait for present completion
		xcb_present_generic_event_t *pev =
		    (void *)xcb_wait_for_special_event(base->c, xd->present_event);
		if (!pev) {
			// We don't know what happened, maybe X died
			// But reset buffer age, so in case we do recover, we will
			// render correctly.
			xd->buffer_age[0] = xd->buffer_age[1] = -1;
			return;
		}
		assert(pev->evtype == XCB_PRESENT_COMPLETE_NOTIFY);
		xcb_present_complete_notify_event_t *pcev = (void *)pev;
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
		// compose() sets clip region, so clear it first to make
		// sure we update the whole screen.
		x_clear_picture_clip_region(xd->base.c, xd->back[xd->curr_back]);

		// TODO buffer-age-like optimization might be possible here.
		//      but that will require a different backend API
		xcb_render_composite(base->c, XCB_RENDER_PICT_OP_SRC,
		                     xd->back[xd->curr_back], XCB_NONE, xd->target, 0, 0,
		                     0, 0, 0, 0, xd->target_width, xd->target_height);
		xd->buffer_age[xd->curr_back] = 1;
	}
}

static int buffer_age(backend_t *backend_data) {
	struct _xrender_data *xd = (void *)backend_data;
	return xd->buffer_age[xd->curr_back];
}

static bool is_image_transparent(backend_t *bd, void *image) {
	struct _xrender_image_data *img = image;
	return img->has_alpha;
}

static bool image_op(backend_t *base, enum image_operations op, void *image,
                     const region_t *reg_op, const region_t *reg_visible, void *arg) {
	struct _xrender_data *xd = (void *)base;
	struct _xrender_image_data *img = image;
	region_t reg;
	double dim_opacity;
	double alpha_multiplier;
	if (op == IMAGE_OP_APPLY_ALPHA_ALL) {
		alpha_multiplier = *(double *)arg;
		img->opacity *= alpha_multiplier;
		img->has_alpha = true;
		return true;
	}

	pixman_region32_init(&reg);
	pixman_region32_intersect(&reg, (region_t *)reg_op, (region_t *)reg_visible);
	if (!pixman_region32_not_empty(&reg)) {
		pixman_region32_fini(&reg);
		return true;
	}

	switch (op) {
	case IMAGE_OP_INVERT_COLOR:
		x_set_picture_clip_region(base->c, img->pict, 0, 0, &reg);
		if (img->has_alpha) {
			auto tmp_pict =
			    x_create_picture_with_visual(base->c, base->root, img->width,
			                                 img->height, img->visual, 0, NULL);
			xcb_render_composite(base->c, XCB_RENDER_PICT_OP_SRC, img->pict,
			                     XCB_NONE, tmp_pict, 0, 0, 0, 0, 0, 0,
			                     img->width, img->height);

			xcb_render_composite(base->c, XCB_RENDER_PICT_OP_DIFFERENCE,
			                     xd->white_pixel, XCB_NONE, tmp_pict, 0, 0, 0,
			                     0, 0, 0, img->width, img->height);
			// We use an extra PictOpInReverse operation to get correct pixel
			// alpha. There could be a better solution.
			xcb_render_composite(base->c, XCB_RENDER_PICT_OP_IN_REVERSE,
			                     tmp_pict, XCB_NONE, img->pict, 0, 0, 0, 0, 0,
			                     0, img->width, img->height);
			xcb_render_free_picture(base->c, tmp_pict);
		} else {
			xcb_render_composite(base->c, XCB_RENDER_PICT_OP_DIFFERENCE,
			                     xd->white_pixel, XCB_NONE, img->pict, 0, 0,
			                     0, 0, 0, 0, img->width, img->height);
		}
		break;
	case IMAGE_OP_DIM:
		x_set_picture_clip_region(base->c, img->pict, 0, 0, &reg);
		dim_opacity = *(double *)arg;

		xcb_render_color_t color = {
		    .red = 0, .green = 0, .blue = 0, .alpha = 0xffff * dim_opacity};

		// Dim the actually content of window
		xcb_rectangle_t rect = {
		    .x = 0,
		    .y = 0,
		    .width = img->width,
		    .height = img->height,
		};

		xcb_render_fill_rectangles(base->c, XCB_RENDER_PICT_OP_OVER, img->pict,
		                           color, 1, &rect);
		break;
	case IMAGE_OP_APPLY_ALPHA:
		alpha_multiplier = *(double *)arg;
		if (alpha_multiplier == 1) {
			break;
		}

		auto alpha_pict = xd->alpha_pict[(int)(alpha_multiplier * 255)];
		x_set_picture_clip_region(base->c, img->pict, 0, 0, &reg);
		xcb_render_composite(base->c, XCB_RENDER_PICT_OP_IN, img->pict, XCB_NONE,
		                     alpha_pict, 0, 0, 0, 0, 0, 0, img->width, img->height);
		img->has_alpha = true;
		break;
	case IMAGE_OP_APPLY_ALPHA_ALL: assert(false);
	}
	pixman_region32_fini(&reg);
	return true;
}

static void *copy(backend_t *base, const void *image, const region_t *reg) {
	const struct _xrender_image_data *img = image;
	struct _xrender_data *xd = (void *)base;
	auto new_img = ccalloc(1, struct _xrender_image_data);
	assert(img->visual != XCB_NONE);
	log_trace("xrender: copying %#010x visual %#x", img->pixmap, img->visual);
	x_set_picture_clip_region(base->c, img->pict, 0, 0, reg);
	new_img->has_alpha = img->has_alpha;
	new_img->width = img->width;
	new_img->height = img->height;
	new_img->visual = img->visual;
	new_img->pixmap =
	    x_create_pixmap(base->c, img->depth, base->root, img->width, img->height);
	new_img->opacity = 1;
	new_img->owned = true;
	if (new_img->pixmap == XCB_NONE) {
		free(new_img);
		return NULL;
	}
	new_img->pict = x_create_picture_with_visual_and_pixmap(base->c, img->visual,
	                                                        new_img->pixmap, 0, NULL);
	if (new_img->pixmap == XCB_NONE) {
		xcb_free_pixmap(base->c, new_img->pixmap);
		free(new_img);
		return NULL;
	}

	auto alpha_pict =
	    img->opacity == 1 ? XCB_NONE : xd->alpha_pict[(int)(img->opacity * 255)];
	xcb_render_composite(base->c, XCB_RENDER_PICT_OP_SRC, img->pict, alpha_pict,
	                     new_img->pict, 0, 0, 0, 0, 0, 0, img->width, img->height);
	return new_img;
}

static struct backend_operations xrender_ops = {
    .deinit = deinit,
    .blur = blur,
    .present = present,
    .compose = compose,
    .bind_pixmap = bind_pixmap,
    .release_image = release_image,
    .render_shadow = default_backend_render_shadow,
    //.prepare_win = prepare_win,
    //.release_win = release_win,
    .is_image_transparent = is_image_transparent,
    .buffer_age = buffer_age,
    .max_buffer_age = 2,

    .image_op = image_op,
    .copy = copy,
};

backend_t *backend_xrender_init(session_t *ps) {
	auto xd = ccalloc(1, struct _xrender_data);

	xd->base.c = ps->c;
	xd->base.root = ps->root;
	xd->base.ops = &xrender_ops;

	for (int i = 0; i < 256; ++i) {
		double o = (double)i / 255.0;
		xd->alpha_pict[i] = solid_picture(ps->c, ps->root, false, o, 0, 0, 0);
		assert(xd->alpha_pict[i] != XCB_NONE);
	}

	xd->target_width = ps->root_width;
	xd->target_height = ps->root_height;
	xd->default_visual = ps->vis;
	xd->black_pixel = solid_picture(ps->c, ps->root, true, 1, 0, 0, 0);
	xd->white_pixel = solid_picture(ps->c, ps->root, true, 1, 1, 1, 1);

	if (ps->overlay != XCB_NONE) {
		xd->target = x_create_picture_with_visual_and_pixmap(
		    ps->c, ps->vis, ps->overlay, 0, NULL);
		xd->target_win = ps->overlay;
	} else {
		xcb_render_create_picture_value_list_t pa = {
		    .subwindowmode = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS,
		};
		xd->target = x_create_picture_with_visual_and_pixmap(
		    ps->c, ps->vis, ps->root, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
		xd->target_win = ps->root;
	}

	auto pictfmt = x_get_pictform_for_visual(ps->c, ps->vis);
	if (!pictfmt) {
		log_fatal("Default visual is invalid");
		abort();
	}

	xd->vsync = ps->o.vsync != VSYNC_NONE;
	if (ps->present_exists) {
		auto eid = xcb_generate_id(ps->c);
		auto e =
		    xcb_request_check(ps->c, xcb_present_select_input_checked(
		                                 ps->c, eid, xd->target_win,
		                                 XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
		if (e) {
			log_error("Cannot select present input, vsync will be disabled");
			xd->vsync = false;
			free(e);
		}

		xd->present_event =
		    xcb_register_for_special_xge(ps->c, &xcb_present_id, eid, NULL);
		if (!xd->present_event) {
			log_error("Cannot register for special XGE, vsync will be "
			          "disabled");
			xd->vsync = false;
		}
	} else {
		xd->vsync = false;
	}

	// We might need to do double buffering for vsync
	int pixmap_needed = xd->vsync ? 2 : 1;
	for (int i = 0; i < pixmap_needed; i++) {
		xd->back_pixmap[i] = x_create_pixmap(ps->c, pictfmt->depth, ps->root,
		                                     ps->root_width, ps->root_height);
		xd->back[i] = x_create_picture_with_pictfmt_and_pixmap(
		    ps->c, pictfmt, xd->back_pixmap[i], 0, NULL);
		xd->buffer_age[i] = -1;
		if (xd->back_pixmap[i] == XCB_NONE || xd->back[i] == XCB_NONE) {
			log_error("Cannot create pixmap for rendering");
			goto err;
		}
	}
	xd->curr_back = 0;

	xcb_pixmap_t root_pixmap = x_get_root_back_pixmap(ps);
	if (root_pixmap == XCB_NONE) {
		xd->root_pict = solid_picture(ps->c, ps->root, false, 1, 0.5, 0.5, 0.5);
	} else {
		xd->root_pict = x_create_picture_with_visual_and_pixmap(
		    ps->c, ps->vis, root_pixmap, 0, NULL);
	}
	for (int i = 0; ps->o.blur_kerns[i]; i++) {
		assert(i < MAX_BLUR_PASS - 1);
		xd->x_blur_kern_size[i] = x_picture_filter_from_conv(
		    ps->o.blur_kerns[i], 1, &xd->x_blur_kern[i], (size_t[]){0});
	}
	return &xd->base;
err:
	deinit(&xd->base);
	return NULL;
}

// vim: set noet sw=8 ts=8:
