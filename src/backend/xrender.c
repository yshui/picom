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
	/// The idle fence for the present extension
	xcb_sync_fence_t idle_fence;
	bool present_in_progress;
	/// The target window
	xcb_window_t target_win;
	/// The painting target, it is either the root or the overlay
	xcb_render_picture_t target;
	/// A back buffer
	xcb_render_picture_t back[2];
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

	/// 1x1 picture of the shadow color
	xcb_render_picture_t shadow_pixel;
	/// convolution kernel for the shadow
	conv *shadow_kernel;

	/// Blur kernels converted to X format
	xcb_render_fixed_t *x_blur_kern[MAX_BLUR_PASS];
	/// Number of elements in each blur kernel
	size_t x_blur_kern_size[MAX_BLUR_PASS];

	xcb_special_event_t *present_event;
} xrender_data;

#if 0
/**
 * Paint root window content.
 */
static void
paint_root(session_t *ps, const region_t *reg_paint) {
  if (!ps->root_tile_paint.pixmap)
    get_root_tile(ps);

  paint_region(ps, NULL, 0, 0, ps->root_width, ps->root_height, 1.0, reg_paint,
    ps->root_tile_paint.pict);
}
#endif

struct _xrender_win_data {
	// Pixmap that the client window draws to,
	// it will contain the content of client window.
	xcb_pixmap_t pixmap;
	// A Picture links to the Pixmap
	xcb_render_picture_t pict;
	// A buffer used for rendering
	xcb_render_picture_t buffer;
	// The rendered content of the window (dimmed, inverted
	// color, etc.). This is either `buffer` or `pict`
	xcb_render_picture_t rendered_pict;
	xcb_pixmap_t shadow_pixmap;
	xcb_render_picture_t shadow_pict;
};

static void compose(void *backend_data, session_t *ps, win *w, void *win_data, int dst_x,
                    int dst_y, const region_t *reg_paint) {
	struct _xrender_data *xd = backend_data;
	struct _xrender_win_data *wd = win_data;
	bool blend = default_is_frame_transparent(NULL, w, win_data) ||
	             default_is_win_transparent(NULL, w, win_data);
	int op = (blend ? XCB_RENDER_PICT_OP_OVER : XCB_RENDER_PICT_OP_SRC);
	auto alpha_pict = xd->alpha_pict[(int)(w->opacity * 255.0)];

	// XXX Move shadow drawing into a separate function,
	//     also do shadow excluding outside of backend
	// XXX This is needed to implement full-shadow
	if (w->shadow) {
		// Put shadow on background
		region_t shadow_reg = win_extents_by_val(w);
		region_t bshape = win_get_bounding_shape_global_by_val(w);
		region_t reg_tmp;
		pixman_region32_init(&reg_tmp);
		// Shadow doesn't need to be painted underneath the body of the window
		// Because no one can see it
		pixman_region32_subtract(&reg_tmp, &shadow_reg, w->reg_ignore);

		// Mask out the region we don't want shadow on
		if (pixman_region32_not_empty(&ps->shadow_exclude_reg))
			pixman_region32_subtract(&reg_tmp, &reg_tmp, &ps->shadow_exclude_reg);

		// Might be worth while to crop the region to shadow border
		pixman_region32_intersect_rect(&reg_tmp, &reg_tmp, w->g.x + w->shadow_dx,
		                               w->g.y + w->shadow_dy, w->shadow_width,
		                               w->shadow_height);

		// Crop the shadow to the damage region. If we draw out side of
		// the damage region, we could be drawing over perfectly good
		// content, and destroying it.
		pixman_region32_intersect(&reg_tmp, &reg_tmp, (region_t *)reg_paint);

		if (ps->o.xinerama_shadow_crop && w->xinerama_scr >= 0 &&
		    w->xinerama_scr < ps->xinerama_nscrs)
			// There can be a window where number of screens is updated,
			// but the screen number attached to the windows have not.
			//
			// Window screen number will be updated eventually, so here we
			// just check to make sure we don't access out of bounds.
			pixman_region32_intersect(
			    &reg_tmp, &reg_tmp, &ps->xinerama_scr_regs[w->xinerama_scr]);

		// Mask out the body of the window from the shadow
		// Doing it here instead of in make_shadow() for saving GPU
		// power and handling shaped windows (XXX unconfirmed)
		pixman_region32_subtract(&reg_tmp, &reg_tmp, &bshape);
		pixman_region32_fini(&bshape);

		// Detect if the region is empty before painting
		if (pixman_region32_not_empty(&reg_tmp)) {
			x_set_picture_clip_region(ps->c, xd->back[xd->curr_back], 0, 0,
			                          &reg_tmp);
			xcb_render_composite(
			    ps->c, XCB_RENDER_PICT_OP_OVER, wd->shadow_pict, alpha_pict,
			    xd->back[xd->curr_back], 0, 0, 0, 0, dst_x + w->shadow_dx,
			    dst_y + w->shadow_dy, w->shadow_width, w->shadow_height);
		}
		pixman_region32_fini(&reg_tmp);
		pixman_region32_fini(&shadow_reg);
	}

	// Clip region of rendered_pict might be set during rendering, clear it to make
	// sure we get everything into the buffer
	x_clear_picture_clip_region(ps->c, wd->rendered_pict);

	x_set_picture_clip_region(ps->c, xd->back[xd->curr_back], 0, 0, reg_paint);
	xcb_render_composite(ps->c, op, wd->rendered_pict, alpha_pict, xd->back[xd->curr_back],
	                     0, 0, 0, 0, dst_x, dst_y, w->widthb, w->heightb);
}

static bool
blur(void *backend_data, session_t *ps, double opacity, const region_t *reg_paint) {
	struct _xrender_data *xd = backend_data;
	const pixman_box32_t *reg = pixman_region32_extents((region_t *)reg_paint);
	const int height = reg->y2 - reg->y1;
	const int width = reg->x2 - reg->x1;
	static const char *filter0 = "Nearest";        // The "null" filter
	static const char *filter = "convolution";

	// Create a buffer for storing blurred picture, make it just big enough
	// for the blur region
	xcb_render_picture_t tmp_picture[2] = {
	    x_create_picture_with_visual(ps, width, height, ps->vis, 0, NULL),
	    x_create_picture_with_visual(ps, width, height, ps->vis, 0, NULL)};

	region_t clip;
	pixman_region32_init(&clip);
	pixman_region32_copy(&clip, (region_t *)reg_paint);
	pixman_region32_translate(&clip, -reg->x1, -reg->y1);

	if (!tmp_picture[0] || !tmp_picture[1]) {
		log_error("Failed to build intermediate Picture.");
		return false;
	}

	x_set_picture_clip_region(ps->c, tmp_picture[0], 0, 0, &clip);
	x_set_picture_clip_region(ps->c, tmp_picture[1], 0, 0, &clip);

	// The multipass blur implemented here is not correct, but this is what old
	// compton did anyway. XXX
	xcb_render_picture_t src_pict = xd->back[xd->curr_back], dst_pict = tmp_picture[0];
	auto alpha_pict = xd->alpha_pict[(int)(opacity * 255)];
	int current = 0;
	int src_x = reg->x1, src_y = reg->y1;

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
		xcb_render_set_picture_filter(ps->c, src_pict, strlen(filter), filter,
		                              xd->x_blur_kern_size[i], xd->x_blur_kern[i]);

		if (ps->o.blur_kerns[i + 1] || i == 0) {
			// This is not the last pass, or this is the first pass
			xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, src_pict,
			                     XCB_NONE, dst_pict, src_x, src_y, 0, 0, 0, 0,
			                     width, height);
		} else {
			// This is the last pass, and this is also not the first
			xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_OVER, src_pict,
			                     alpha_pict, xd->back[xd->curr_back], 0, 0, 0,
			                     0, reg->x1, reg->y1, width, height);
		}

		// reset filter
		xcb_render_set_picture_filter(ps->c, src_pict, strlen(filter0), filter0,
		                              0, NULL);

		src_pict = tmp_picture[current];
		dst_pict = tmp_picture[!current];
		src_x = 0;
		src_y = 0;
		current = !current;
	}

	// There is only 1 pass
	if (i == 1) {
		xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_OVER, src_pict, alpha_pict,
		                     xd->back[xd->curr_back], 0, 0, 0, 0, reg->x1,
		                     reg->y1, width, height);
	}

	xcb_render_free_picture(ps->c, tmp_picture[0]);
	xcb_render_free_picture(ps->c, tmp_picture[1]);
	return true;
}

static void render_win(void *backend_data, session_t *ps, win *w, void *win_data,
                       const region_t *reg_paint) {
	struct _xrender_data *xd = backend_data;
	struct _xrender_win_data *wd = win_data;

	w->pixmap_damaged = false;

	if (!w->invert_color && w->frame_opacity == 1 && !w->dim) {
		// No extra processing needed
		wd->rendered_pict = wd->pict;
		return;
	}

	region_t reg_paint_local;
	pixman_region32_init(&reg_paint_local);
	pixman_region32_copy(&reg_paint_local, (region_t *)reg_paint);
	pixman_region32_translate(&reg_paint_local, -w->g.x, -w->g.y);

	// We don't want to modify the content of the original window when we process
	// it, so we create a buffer.
	if (wd->buffer == XCB_NONE) {
		wd->buffer = x_create_picture_with_pictfmt(ps, w->widthb, w->heightb,
		                                           w->pictfmt, 0, NULL);
	}

	// Copy the content of the window over to the buffer
	x_clear_picture_clip_region(ps->c, wd->buffer);
	wd->rendered_pict = wd->buffer;
	xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, wd->pict, XCB_NONE,
	                     wd->rendered_pict, 0, 0, 0, 0, 0, 0, w->widthb, w->heightb);

	if (w->invert_color) {
		// Handle invert color
		x_set_picture_clip_region(ps->c, wd->rendered_pict, 0, 0, &reg_paint_local);

		xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_DIFFERENCE,
		                     xd->white_pixel, XCB_NONE, wd->rendered_pict, 0, 0,
		                     0, 0, 0, 0, w->widthb, w->heightb);
		// We use an extra PictOpInReverse operation to get correct pixel
		// alpha. There could be a better solution.
		if (win_has_alpha(w))
			xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_IN_REVERSE,
			                     wd->pict, XCB_NONE, wd->rendered_pict, 0, 0,
			                     0, 0, 0, 0, w->widthb, w->heightb);
	}

	if (w->frame_opacity != 1) {
		// Handle transparent frame
		// Step 1: clip paint area to frame
		region_t frame_reg;
		pixman_region32_init(&frame_reg);
		pixman_region32_copy(&frame_reg, &w->bounding_shape);

		region_t body_reg = win_get_region_noframe_local_by_val(w);
		pixman_region32_subtract(&frame_reg, &frame_reg, &body_reg);

		// Draw the frame with frame opacity
		xcb_render_picture_t alpha_pict =
		    xd->alpha_pict[(int)(w->frame_opacity * w->opacity * 255)];
		x_set_picture_clip_region(ps->c, wd->rendered_pict, 0, 0, &frame_reg);

		// Step 2: multiply alpha value
		// XXX test
		xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, xd->white_pixel,
		                     alpha_pict, wd->rendered_pict, 0, 0, 0, 0, 0, 0,
		                     w->widthb, w->heightb);
	}

	if (w->dim) {
		// Handle dimming

		double dim_opacity = ps->o.inactive_dim;
		if (!ps->o.inactive_dim_fixed)
			dim_opacity *= w->opacity;

		xcb_render_color_t color = {
		    .red = 0, .green = 0, .blue = 0, .alpha = 0xffff * dim_opacity};

		// Dim the actually content of window
		xcb_rectangle_t rect = {
		    .x = 0,
		    .y = 0,
		    .width = w->widthb,
		    .height = w->heightb,
		};

		x_clear_picture_clip_region(ps->c, wd->rendered_pict);
		xcb_render_fill_rectangles(ps->c, XCB_RENDER_PICT_OP_OVER,
		                           wd->rendered_pict, color, 1, &rect);
	}

	pixman_region32_fini(&reg_paint_local);
}

static void *prepare_win(void *backend_data, session_t *ps, win *w) {
	auto wd = ccalloc(1, struct _xrender_win_data);
	struct _xrender_data *xd = backend_data;
	assert(w->a.map_state == XCB_MAP_STATE_VIEWABLE);
	if (ps->has_name_pixmap) {
		wd->pixmap = xcb_generate_id(ps->c);
		xcb_composite_name_window_pixmap_checked(ps->c, w->id, wd->pixmap);
	}

	xcb_drawable_t draw = wd->pixmap;
	if (!draw)
		draw = w->id;

	log_trace("%s %x", w->name, wd->pixmap);
	wd->pict = x_create_picture_with_pictfmt_and_pixmap(ps->c, w->pictfmt, draw, 0, NULL);
	wd->buffer = XCB_NONE;

	// XXX delay allocating shadow pict until compose() will dramatical
	//     improve performance, probably because otherwise shadow pict
	//     can be created and destroyed multiple times per draw.
	//
	//     However doing that breaks a assumption the backend API makes (i.e.
	//     either all needed data is here, or none is), therefore we will
	//     leave this here until we have chance to re-think the backend API
	if (w->shadow) {
		xcb_pixmap_t pixmap;
		build_shadow(ps, 1, w->widthb, w->heightb, xd->shadow_kernel,
		             xd->shadow_pixel, &pixmap, &wd->shadow_pict);
		xcb_free_pixmap(ps->c, pixmap);
	}
	return wd;
}

static void release_win(void *backend_data, session_t *ps, win *w, void *win_data) {
	struct _xrender_win_data *wd = win_data;
	xcb_free_pixmap(ps->c, wd->pixmap);
	// xcb_free_pixmap(ps->c, wd->shadow_pixmap);
	xcb_render_free_picture(ps->c, wd->pict);
	xcb_render_free_picture(ps->c, wd->shadow_pict);
	if (wd->buffer != XCB_NONE)
		xcb_render_free_picture(ps->c, wd->buffer);
	free(wd);
}

static void *init(session_t *ps) {
	auto xd = ccalloc(1, struct _xrender_data);

	for (int i = 0; i < 256; ++i) {
		double o = (double)i / 255.0;
		xd->alpha_pict[i] = solid_picture(ps, false, o, 0, 0, 0);
		assert(xd->alpha_pict[i] != XCB_NONE);
	}

	xd->black_pixel = solid_picture(ps, true, 1, 0, 0, 0);
	xd->white_pixel = solid_picture(ps, true, 1, 1, 1, 1);
	xd->shadow_pixel = solid_picture(ps, true, 1, ps->o.shadow_red,
	                                 ps->o.shadow_green, ps->o.shadow_blue);
	xd->shadow_kernel = gaussian_kernel(ps->o.shadow_radius);
	xd->present_in_progress = false;
	sum_kernel_preprocess(xd->shadow_kernel);

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

	// We might need to do double buffering for vsync
	int pixmap_needed = ps->o.vsync ? 2 : 1;
	for (int i = 0; i < pixmap_needed; i++) {
		xd->back_pixmap[i] = x_create_pixmap(ps->c, pictfmt->depth, ps->root,
		                                     ps->root_width, ps->root_height);
		xd->back[i] = x_create_picture_with_pictfmt_and_pixmap(
		    ps->c, pictfmt, xd->back_pixmap[i], 0, NULL);
	}
	xd->curr_back = 0;

	xcb_pixmap_t root_pixmap = x_get_root_back_pixmap(ps);
	if (root_pixmap == XCB_NONE) {
		xd->root_pict = solid_picture(ps, false, 1, 0.5, 0.5, 0.5);
	} else {
		xd->root_pict = x_create_picture_with_visual_and_pixmap(
		    ps->c, ps->vis, root_pixmap, 0, NULL);
	}

	if (ps->present_exists) {
		auto eid = xcb_generate_id(ps->c);
		auto e =
		    xcb_request_check(ps->c, xcb_present_select_input_checked(
		                                 ps->c, eid, xd->target_win,
		                                 XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
		if (e) {
			log_error("Cannot select present input, vsync will be disabled");
			free(e);
		}

		xd->present_event =
		    xcb_register_for_special_xge(ps->c, &xcb_present_id, eid, NULL);
		if (!xd->present_event) {
			log_error("Cannot register for special XGE, vsync will be "
			          "disabled");
		}
	}
	for (int i = 0; ps->o.blur_kerns[i]; i++) {
		assert(i < MAX_BLUR_PASS - 1);
		xd->x_blur_kern_size[i] = x_picture_filter_from_conv(
		    ps->o.blur_kerns[i], 1, &xd->x_blur_kern[i], (size_t[]){0});
	}
	return xd;
}

static void deinit(void *backend_data, session_t *ps) {
	struct _xrender_data *xd = backend_data;
	for (int i = 0; i < 256; i++)
		xcb_render_free_picture(ps->c, xd->alpha_pict[i]);
	xcb_render_free_picture(ps->c, xd->white_pixel);
	xcb_render_free_picture(ps->c, xd->black_pixel);
	free_conv(xd->shadow_kernel);
	free(xd);
}

static void *root_change(void *backend_data, session_t *ps) {
	deinit(backend_data, ps);
	return init(ps);
}

static void prepare(void *backend_data, session_t *ps, const region_t *reg_paint) {
	struct _xrender_data *xd = backend_data;
	if (ps->o.vsync != VSYNC_NONE && ps->present_exists && xd->present_in_progress) {
		// TODO don't block wait for present completion
		xcb_present_generic_event_t *pev =
		    (void *)xcb_wait_for_special_event(ps->c, xd->present_event);
		assert(pev->evtype == XCB_PRESENT_COMPLETE_NOTIFY);
		xcb_present_complete_notify_event_t *pcev = (void *)pev;
		// log_trace("Present complete: %d %ld", pcev->mode, pcev->msc);
		if (pcev->mode == XCB_PRESENT_COMPLETE_MODE_FLIP) {
			// We cannot use the pixmap we used anymore
			xd->curr_back = 1 - xd->curr_back;
		}
		free(pev);

		xd->present_in_progress = false;
	}

	// Paint the root pixmap (i.e. wallpaper)
	// Limit the paint area
	x_set_picture_clip_region(ps->c, xd->back[xd->curr_back], 0, 0, reg_paint);

	xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, xd->root_pict, XCB_NONE,
	                     xd->back[xd->curr_back], 0, 0, 0, 0, 0, 0, ps->root_width,
	                     ps->root_height);
}

static void present(void *backend_data, session_t *ps) {
	struct _xrender_data *xd = backend_data;

	if (ps->o.vsync != VSYNC_NONE && ps->present_exists) {
		// Only reset the fence when we are sure we will trigger it again.
		// To make sure rendering won't get stuck if user toggles vsync on the
		// fly.
		xcb_present_pixmap(ps->c, xd->target_win, xd->back_pixmap[xd->curr_back],
		                   0, XCB_NONE, XCB_NONE, 0, 0, XCB_NONE, XCB_NONE,
		                   XCB_NONE, 0, 0, 0, 0, 0, NULL);
		xd->present_in_progress = true;
	} else {
		// compose() sets clip region, so clear it first to make
		// sure we update the whole screen.
		x_clear_picture_clip_region(ps->c, xd->back[xd->curr_back]);

		// TODO buffer-age-like optimization might be possible here.
		//      but that will require a different backend API
		xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC,
		                     xd->back[xd->curr_back], XCB_NONE, xd->target, 0, 0,
		                     0, 0, 0, 0, ps->root_width, ps->root_height);
	}
}

struct backend_info xrender_backend = {
    .init = init,
    .deinit = deinit,
    .blur = blur,
    .present = present,
    .prepare = prepare,
    .compose = compose,
    .root_change = root_change,
    .render_win = render_win,
    .prepare_win = prepare_win,
    .release_win = release_win,
    .is_win_transparent = default_is_win_transparent,
    .is_frame_transparent = default_is_frame_transparent,
    .max_buffer_age = 2,
};

// vim: set noet sw=8 ts=8:
