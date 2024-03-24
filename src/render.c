// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <stdlib.h>
#include <string.h>
#include <xcb/composite.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_renderutil.h>

#include "common.h"
#include "options.h"
#include "picom.h"
#include "transition.h"

#ifdef CONFIG_OPENGL
#include "backend/gl/glx.h"
#include "opengl.h"

#ifndef GLX_BACK_BUFFER_AGE_EXT
#define GLX_BACK_BUFFER_AGE_EXT 0x20F4
#endif

#endif

#include "compiler.h"
#include "config.h"
#include "kernel.h"
#include "log.h"
#include "region.h"
#include "types.h"
#include "utils.h"
#include "vsync.h"
#include "win.h"
#include "x.h"

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "render.h"

#define XRFILTER_CONVOLUTION "convolution"
#define XRFILTER_GAUSSIAN "gaussian"
#define XRFILTER_BINOMIAL "binomial"

/**
 * Bind texture in paint_t if we are using GLX backend.
 */
static inline bool paint_bind_tex(session_t *ps, paint_t *ppaint, int wid, int hei,
                                  bool repeat, int depth, xcb_visualid_t visual, bool force) {
#ifdef CONFIG_OPENGL
	// XXX This is a mess. But this will go away after the backend refactor.
	if (!ppaint->pixmap) {
		return false;
	}

	struct glx_fbconfig_info *fbcfg;
	auto c = session_get_x_connection(ps);
	auto psglx = session_get_psglx(ps);
	if (!visual) {
		assert(depth == 32);
		if (!psglx->argb_fbconfig.cfg) {
			glx_find_fbconfig(c,
			                  (struct xvisual_info){.red_size = 8,
			                                        .green_size = 8,
			                                        .blue_size = 8,
			                                        .alpha_size = 8,
			                                        .visual_depth = 32},
			                  &psglx->argb_fbconfig);
		}
		if (!psglx->argb_fbconfig.cfg) {
			log_error("Failed to find appropriate FBConfig for 32 bit depth");
			return false;
		}
		fbcfg = &psglx->argb_fbconfig;
	} else {
		auto m = x_get_visual_info(c, visual);
		if (m.visual_depth < 0) {
			return false;
		}

		if (depth && depth != m.visual_depth) {
			log_error("Mismatching visual depth: %d != %d", depth, m.visual_depth);
			return false;
		}

		if (!ppaint->fbcfg.cfg) {
			glx_find_fbconfig(session_get_x_connection(ps), m, &ppaint->fbcfg);
		}
		if (!ppaint->fbcfg.cfg) {
			log_error("Failed to find appropriate FBConfig for X pixmap");
			return false;
		}
		fbcfg = &ppaint->fbcfg;
	}

	if (force || !glx_tex_bound(ppaint->ptex, ppaint->pixmap)) {
		return glx_bind_pixmap(ps, &ppaint->ptex, ppaint->pixmap, wid, hei,
		                       repeat, fbcfg);
	}
#else
	(void)ps;
	(void)ppaint;
	(void)wid;
	(void)hei;
	(void)repeat;
	(void)depth;
	(void)visual;
	(void)force;
#endif
	return true;
}

/**
 * Check if current backend uses XRender for rendering.
 */
static inline bool bkend_use_xrender(session_t *ps) {
	auto backend = session_get_options(ps)->backend;
	return BKEND_XRENDER == backend || BKEND_XR_GLX_HYBRID == backend;
}

int maximum_buffer_age(session_t *ps) {
	if (bkend_use_glx(ps) && session_get_options(ps)->use_damage) {
		return CGLX_MAX_BUFFER_AGE;
	}
	return 1;
}

static int get_buffer_age(session_t *ps) {
	auto options = session_get_options(ps);
#ifdef CONFIG_OPENGL
	if (bkend_use_glx(ps)) {
		if (!glxext.has_GLX_EXT_buffer_age && options->use_damage) {
			log_warn("GLX_EXT_buffer_age not supported by your driver,"
			         "`use-damage` has to be disabled");
			options->use_damage = false;
		}
		if (options->use_damage) {
			unsigned int val;
			glXQueryDrawable(session_get_x_connection(ps)->dpy,
			                 session_get_target_window(ps),
			                 GLX_BACK_BUFFER_AGE_EXT, &val);
			return (int)val ?: -1;
		}
		return -1;
	}
#endif
	return options->use_damage ? 1 : -1;
}

/**
 * Reset filter on a <code>Picture</code>.
 */
static inline void xrfilter_reset(session_t *ps, xcb_render_picture_t p) {
#define FILTER "Nearest"
	xcb_render_set_picture_filter(session_get_x_connection(ps)->c, p, strlen(FILTER),
	                              FILTER, 0, NULL);
#undef FILTER
}

/// Set the input/output clip region of the target buffer (not the actual target!)
static inline void attr_nonnull(1, 2) set_tgt_clip(session_t *ps, region_t *reg) {
	switch (session_get_options(ps)->backend) {
	case BKEND_XRENDER:
	case BKEND_XR_GLX_HYBRID:
		x_set_picture_clip_region(session_get_x_connection(ps),
		                          session_get_tgt_buffer(ps)->pict, 0, 0, reg);
		break;
#ifdef CONFIG_OPENGL
	case BKEND_GLX: glx_set_clip(ps, reg); break;
#endif
	default: assert(false);
	}
}

/**
 * Free paint_t.
 */
void free_paint(session_t *ps, paint_t *ppaint) {
#ifdef CONFIG_OPENGL
	free_paint_glx(ps, ppaint);
#endif
	auto c = session_get_x_connection(ps);
	if (ppaint->pict != XCB_NONE) {
		x_free_picture(c, ppaint->pict);
		ppaint->pict = XCB_NONE;
	}
	if (ppaint->pixmap) {
		xcb_free_pixmap(c->c, ppaint->pixmap);
		ppaint->pixmap = XCB_NONE;
	}
}

uint32_t
make_circle(int cx, int cy, int radius, uint32_t max_ntraps, xcb_render_trapezoid_t traps[]) {
	uint32_t n = 0, k = 0;
	int y1, y2;
	double w;
	while (k < max_ntraps) {
		y1 = (int)(-radius * cos(M_PI * k / max_ntraps));
		traps[n].top = (cy + y1) * 65536;
		traps[n].left.p1.y = (cy + y1) * 65536;
		traps[n].right.p1.y = (cy + y1) * 65536;
		w = sqrt(radius * radius - y1 * y1) * 65536;
		traps[n].left.p1.x = (int)((cx * 65536) - w);
		traps[n].right.p1.x = (int)((cx * 65536) + w);

		do {
			k++;
			y2 = (int)(-radius * cos(M_PI * k / max_ntraps));
		} while (y1 == y2);

		traps[n].bottom = (cy + y2) * 65536;
		traps[n].left.p2.y = (cy + y2) * 65536;
		traps[n].right.p2.y = (cy + y2) * 65536;
		w = sqrt(radius * radius - y2 * y2) * 65536;
		traps[n].left.p2.x = (int)((cx * 65536) - w);
		traps[n].right.p2.x = (int)((cx * 65536) + w);
		n++;
	}
	return n;
}

uint32_t make_rectangle(int x, int y, int wid, int hei, xcb_render_trapezoid_t traps[]) {
	traps[0].top = y * 65536;
	traps[0].left.p1.y = y * 65536;
	traps[0].left.p1.x = x * 65536;
	traps[0].left.p2.y = (y + hei) * 65536;
	traps[0].left.p2.x = x * 65536;
	traps[0].bottom = (y + hei) * 65536;
	traps[0].right.p1.x = (x + wid) * 65536;
	traps[0].right.p1.y = y * 65536;
	traps[0].right.p2.x = (x + wid) * 65536;
	traps[0].right.p2.y = (y + hei) * 65536;
	return 1;
}

uint32_t make_rounded_window_shape(xcb_render_trapezoid_t traps[], uint32_t max_ntraps,
                                   int cr, int wid, int hei) {
	uint32_t n = make_circle(cr, cr, cr, max_ntraps, traps);
	n += make_circle(wid - cr, cr, cr, max_ntraps, traps + n);
	n += make_circle(wid - cr, hei - cr, cr, max_ntraps, traps + n);
	n += make_circle(cr, hei - cr, cr, max_ntraps, traps + n);
	n += make_rectangle(0, cr, wid, hei - 2 * cr, traps + n);
	n += make_rectangle(cr, 0, wid - 2 * cr, cr, traps + n);
	n += make_rectangle(cr, hei - cr, wid - 2 * cr, cr, traps + n);
	return n;
}

void render(session_t *ps, int x, int y, int dx, int dy, int wid, int hei, int fullwid,
            int fullhei, double opacity, bool argb, bool neg, int cr,
            xcb_render_picture_t pict, glx_texture_t *ptex, const region_t *reg_paint,
            const glx_prog_main_t *pprogram, clip_t *clip) {
	auto options = session_get_options(ps);
	auto c = session_get_x_connection(ps);
	switch (options->backend) {
	case BKEND_XRENDER:
	case BKEND_XR_GLX_HYBRID: {
		auto alpha_step = (int)(opacity * MAX_ALPHA);
		xcb_render_picture_t alpha_pict = session_get_alpha_pictures(ps)[alpha_step];
		if (alpha_step != 0) {
			if (cr) {
				xcb_render_picture_t p_tmp = x_create_picture_with_standard(
				    c, fullwid, fullhei, XCB_PICT_STANDARD_ARGB_32, 0, 0);
				xcb_render_color_t trans = {
				    .red = 0, .blue = 0, .green = 0, .alpha = 0};
				const xcb_rectangle_t rect = {
				    .x = 0,
				    .y = 0,
				    .width = to_u16_checked(fullwid),
				    .height = to_u16_checked(fullhei)};
				xcb_render_fill_rectangles(c->c, XCB_RENDER_PICT_OP_SRC,
				                           p_tmp, trans, 1, &rect);

				uint32_t max_ntraps = to_u32_checked(cr);
				xcb_render_trapezoid_t traps[4 * max_ntraps + 3];

				uint32_t n = make_rounded_window_shape(
				    traps, max_ntraps, cr, fullwid, fullhei);

				xcb_render_trapezoids(
				    c->c, XCB_RENDER_PICT_OP_OVER, alpha_pict, p_tmp,
				    x_get_pictfmt_for_standard(c, XCB_PICT_STANDARD_A_8),
				    0, 0, n, traps);

				xcb_render_composite(
				    c->c, XCB_RENDER_PICT_OP_OVER, pict, p_tmp,
				    session_get_tgt_buffer(ps)->pict, to_i16_checked(x),
				    to_i16_checked(y), to_i16_checked(x), to_i16_checked(y),
				    to_i16_checked(dx), to_i16_checked(dy),
				    to_u16_checked(wid), to_u16_checked(hei));

				x_free_picture(c, p_tmp);

			} else {
				xcb_render_picture_t p_tmp = alpha_pict;
				if (clip) {
					p_tmp = x_create_picture_with_standard(
					    c, wid, hei, XCB_PICT_STANDARD_ARGB_32, 0, 0);

					xcb_render_color_t black = {
					    .red = 255, .blue = 255, .green = 255, .alpha = 255};
					const xcb_rectangle_t rect = {
					    .x = 0,
					    .y = 0,
					    .width = to_u16_checked(wid),
					    .height = to_u16_checked(hei)};
					xcb_render_fill_rectangles(c->c, XCB_RENDER_PICT_OP_SRC,
					                           p_tmp, black, 1, &rect);
					if (alpha_pict) {
						xcb_render_composite(
						    c->c, XCB_RENDER_PICT_OP_SRC,
						    alpha_pict, XCB_NONE, p_tmp, 0, 0, 0,
						    0, 0, 0, to_u16_checked(wid),
						    to_u16_checked(hei));
					}
					xcb_render_composite(
					    c->c, XCB_RENDER_PICT_OP_OUT_REVERSE,
					    clip->pict, XCB_NONE, p_tmp, 0, 0, 0, 0,
					    to_i16_checked(clip->x), to_i16_checked(clip->y),
					    to_u16_checked(wid), to_u16_checked(hei));
				}
				uint8_t op = ((!argb && !alpha_pict && !clip)
				                  ? XCB_RENDER_PICT_OP_SRC
				                  : XCB_RENDER_PICT_OP_OVER);

				xcb_render_composite(
				    c->c, op, pict, p_tmp, session_get_tgt_buffer(ps)->pict,
				    to_i16_checked(x), to_i16_checked(y), 0, 0,
				    to_i16_checked(dx), to_i16_checked(dy),
				    to_u16_checked(wid), to_u16_checked(hei));
				if (clip) {
					x_free_picture(c, p_tmp);
				}
			}
		}
		break;
	}
#ifdef CONFIG_OPENGL
	case BKEND_GLX: {
		auto psglx = session_get_psglx(ps);
		glx_render(ps, ptex, x, y, dx, dy, wid, hei, psglx->z, opacity, argb, neg,
		           reg_paint, pprogram);
		psglx->z += 1;
		break;
	}
#endif
	default: assert(0);
	}
#ifndef CONFIG_OPENGL
	(void)neg;
	(void)ptex;
	(void)reg_paint;
	(void)pprogram;
#endif
}

static inline void
paint_region(session_t *ps, const struct managed_win *w, int x, int y, int wid, int hei,
             double opacity, const region_t *reg_paint, xcb_render_picture_t pict) {
	const int dx = (w ? w->g.x : 0) + x;
	const int dy = (w ? w->g.y : 0) + y;
	const int fullwid = w ? w->widthb : 0;
	const int fullhei = w ? w->heightb : 0;
	const bool argb =
	    (w && (win_has_alpha(w) || session_get_options(ps)->force_win_blend));
	const bool neg = (w && w->invert_color);

	render(ps, x, y, dx, dy, wid, hei, fullwid, fullhei, opacity, argb, neg,
	       w ? w->corner_radius : 0, pict,
	       (w ? w->paint.ptex : session_get_root_tile_paint(ps)->ptex), reg_paint,
#ifdef CONFIG_OPENGL
	       w ? &session_get_psglx(ps)->glx_prog_win : NULL
#else
	       NULL
#endif
	       ,
	       XCB_NONE);
}

/**
 * Check whether a paint_t contains enough data.
 */
static inline bool paint_isvalid(session_t *ps, const paint_t *ppaint) {
	// Don't check for presence of Pixmap here, because older X Composite doesn't
	// provide it
	if (!ppaint) {
		return false;
	}

	if (bkend_use_xrender(ps) && !ppaint->pict) {
		return false;
	}

#ifdef CONFIG_OPENGL
	if (BKEND_GLX == session_get_options(ps)->backend &&
	    !glx_tex_bound(ppaint->ptex, XCB_NONE)) {
		return false;
	}
#endif

	return true;
}

/**
 * Paint a window itself and dim it if asked.
 */
void paint_one(session_t *ps, struct managed_win *w, const region_t *reg_paint) {
	auto options = session_get_options(ps);
	auto c = session_get_x_connection(ps);
	// Fetch Pixmap
	if (!w->paint.pixmap) {
		w->paint.pixmap = x_new_id(c);
		set_ignore_cookie(
		    c, xcb_composite_name_window_pixmap(c->c, w->base.id, w->paint.pixmap));
	}

	xcb_drawable_t draw = w->paint.pixmap;
	if (!draw) {
		log_error("Failed to get pixmap from window %#010x (%s), window won't be "
		          "visible",
		          w->base.id, w->name);
		return;
	}

	// XRender: Build picture
	if (bkend_use_xrender(ps) && !w->paint.pict) {
		xcb_render_create_picture_value_list_t pa = {
		    .subwindowmode = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS,
		};

		w->paint.pict = x_create_picture_with_pictfmt_and_pixmap(
		    c, w->pictfmt, draw, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
	}

	// GLX: Build texture
	// Let glx_bind_pixmap() determine pixmap size, because if the user
	// is resizing windows, the width and height we get may not be up-to-date,
	// causing the jittering issue M4he reported in #7.
	if (!paint_bind_tex(ps, &w->paint, 0, 0, false, 0, w->a.visual,
	                    (!options->glx_no_rebind_pixmap && w->pixmap_damaged))) {
		log_error("Failed to bind texture for window %#010x.", w->base.id);
	}
	w->pixmap_damaged = false;

	if (!paint_isvalid(ps, &w->paint)) {
		log_error("Window %#010x is missing painting data.", w->base.id);
		return;
	}

	const int x = w->g.x;
	const int y = w->g.y;
	const uint16_t wid = to_u16_checked(w->widthb);
	const uint16_t hei = to_u16_checked(w->heightb);
	const double window_opacity = animatable_get(&w->opacity);

	xcb_render_picture_t pict = w->paint.pict;

	// Invert window color, if required
	if (bkend_use_xrender(ps) && w->invert_color) {
		xcb_render_picture_t newpict =
		    x_create_picture_with_pictfmt(c, wid, hei, w->pictfmt, 0, NULL);
		if (newpict) {
			// Apply clipping region to save some CPU
			if (reg_paint) {
				region_t reg;
				pixman_region32_init(&reg);
				pixman_region32_copy(&reg, (region_t *)reg_paint);
				pixman_region32_translate(&reg, -x, -y);
				// FIXME XFixesSetPictureClipRegion(ps->dpy, newpict, 0,
				// 0, reg);
				pixman_region32_fini(&reg);
			}

			xcb_render_composite(c->c, XCB_RENDER_PICT_OP_SRC, pict, XCB_NONE,
			                     newpict, 0, 0, 0, 0, 0, 0, wid, hei);
			xcb_render_composite(c->c, XCB_RENDER_PICT_OP_DIFFERENCE,
			                     session_get_white_picture(ps), XCB_NONE,
			                     newpict, 0, 0, 0, 0, 0, 0, wid, hei);
			// We use an extra PictOpInReverse operation to get correct
			// pixel alpha. There could be a better solution.
			if (win_has_alpha(w)) {
				xcb_render_composite(c->c, XCB_RENDER_PICT_OP_IN_REVERSE,
				                     pict, XCB_NONE, newpict, 0, 0, 0, 0,
				                     0, 0, wid, hei);
			}
			pict = newpict;
		}
	}

	if (w->frame_opacity == 1) {
		paint_region(ps, w, 0, 0, wid, hei, window_opacity, reg_paint, pict);
	} else {
		// Painting parameters
		const margin_t extents = win_calc_frame_extents(w);
		auto const t = extents.top;
		auto const l = extents.left;
		auto const b = extents.bottom;
		auto const r = extents.right;

#define COMP_BDR(cx, cy, cwid, chei)                                                     \
	paint_region(ps, w, (cx), (cy), (cwid), (chei),                                  \
	             w->frame_opacity *window_opacity, reg_paint, pict)

		// Sanitize the margins, in case some broken WM makes
		// top_width + bottom_width > height in some cases.

		do {
			// top
			int body_height = hei;
			// ctop = checked top
			// Make sure top margin is smaller than height
			int ctop = min2(body_height, t);
			if (ctop > 0) {
				COMP_BDR(0, 0, wid, ctop);
			}

			body_height -= ctop;
			if (body_height <= 0) {
				break;
			}

			// bottom
			// cbot = checked bottom
			// Make sure bottom margin is not too large
			int cbot = min2(body_height, b);
			if (cbot > 0) {
				COMP_BDR(0, hei - cbot, wid, cbot);
			}

			// Height of window exclude the margin
			body_height -= cbot;
			if (body_height <= 0) {
				break;
			}

			// left
			int body_width = wid;
			int cleft = min2(body_width, l);
			if (cleft > 0) {
				COMP_BDR(0, ctop, cleft, body_height);
			}

			body_width -= cleft;
			if (body_width <= 0) {
				break;
			}

			// right
			int cright = min2(body_width, r);
			if (cright > 0) {
				COMP_BDR(wid - cright, ctop, cright, body_height);
			}

			body_width -= cright;
			if (body_width <= 0) {
				break;
			}

			// body
			paint_region(ps, w, cleft, ctop, body_width, body_height,
			             window_opacity, reg_paint, pict);
		} while (0);
	}

#undef COMP_BDR

	if (pict != w->paint.pict) {
		x_free_picture(c, pict);
		pict = XCB_NONE;
	}

	// Dimming the window if needed
	if (w->dim) {
		double dim_opacity = options->inactive_dim;
		if (!options->inactive_dim_fixed) {
			dim_opacity *= window_opacity;
		}

		switch (options->backend) {
		case BKEND_XRENDER:
		case BKEND_XR_GLX_HYBRID: {
			auto cval = (uint16_t)(0xffff * dim_opacity);

			// Premultiply color
			xcb_render_color_t color = {
			    .red = 0,
			    .green = 0,
			    .blue = 0,
			    .alpha = cval,
			};

			xcb_rectangle_t rect = {
			    .x = to_i16_checked(x),
			    .y = to_i16_checked(y),
			    .width = wid,
			    .height = hei,
			};

			xcb_render_fill_rectangles(c->c, XCB_RENDER_PICT_OP_OVER,
			                           session_get_tgt_buffer(ps)->pict,
			                           color, 1, &rect);
		} break;
#ifdef CONFIG_OPENGL
		case BKEND_GLX:
			glx_dim_dst(ps, x, y, wid, hei, (int)(session_get_psglx(ps)->z - 0.7),
			            (float)dim_opacity, reg_paint);
			break;
#endif
		default: assert(false);
		}
	}
}

static bool get_root_tile(session_t *ps) {
	/*
	if (options->paint_on_overlay) {
	  return ps->root_picture;
	} */

	assert(!session_get_root_tile_paint(ps)->pixmap);
	auto c = session_get_x_connection(ps);
	auto atoms = session_get_atoms(ps);
	session_set_root_tile_fill(ps, false);

	bool fill = false;
	xcb_pixmap_t pixmap = x_get_root_back_pixmap(c, atoms);

	xcb_get_geometry_reply_t *r;
	if (pixmap) {
		r = xcb_get_geometry_reply(c->c, xcb_get_geometry(c->c, pixmap), NULL);
	}

	// Create a pixmap if there isn't any
	xcb_visualid_t visual;
	if (!pixmap || !r) {
		pixmap = x_create_pixmap(c, (uint8_t)c->screen_info->root_depth, 1, 1);
		if (pixmap == XCB_NONE) {
			log_error("Failed to create pixmaps for root tile.");
			return false;
		}
		visual = c->screen_info->root_visual;
		fill = true;
	} else {
		visual = r->depth == c->screen_info->root_depth
		             ? c->screen_info->root_visual
		             : x_get_visual_for_depth(c->screen_info, r->depth);
		free(r);
	}

	// Create Picture
	xcb_render_create_picture_value_list_t pa = {
	    .repeat = true,
	};
	auto root_tile_paint = session_get_root_tile_paint(ps);
	root_tile_paint->pict = x_create_picture_with_visual_and_pixmap(
	    c, visual, pixmap, XCB_RENDER_CP_REPEAT, &pa);

	// Fill pixmap if needed
	if (fill) {
		xcb_render_color_t col;
		xcb_rectangle_t rect;

		col.red = col.green = col.blue = 0x8080;
		col.alpha = 0xffff;

		rect.x = rect.y = 0;
		rect.width = rect.height = 1;

		xcb_render_fill_rectangles(c->c, XCB_RENDER_PICT_OP_SRC,
		                           root_tile_paint->pict, col, 1, &rect);
	}

	session_set_root_tile_fill(ps, fill);
	root_tile_paint->pixmap = pixmap;
#ifdef CONFIG_OPENGL
	if (session_get_options(ps)->backend == BKEND_GLX) {
		return paint_bind_tex(ps, root_tile_paint, 0, 0, true, 0, visual, false);
	}
#endif

	return true;
}

/**
 * Paint root window content.
 */
static void paint_root(session_t *ps, const region_t *reg_paint) {
	// If there is no root tile pixmap, try getting one.
	// If that fails, give up.
	auto root_tile_paint = session_get_root_tile_paint(ps);
	if (!root_tile_paint->pixmap && !get_root_tile(ps)) {
		return;
	}

	auto root_extent = session_get_root_extent(ps);
	paint_region(ps, NULL, 0, 0, root_extent.width, root_extent.height, 1.0,
	             reg_paint, root_tile_paint->pict);
}

/**
 * Generate shadow <code>Picture</code> for a window.
 */
static bool win_build_shadow(session_t *ps, struct managed_win *w, double opacity) {
	const int width = w->widthb;
	const int height = w->heightb;
	// log_trace("(): building shadow for %s %d %d", w->name, width, height);

	xcb_image_t *shadow_image = NULL;
	xcb_pixmap_t shadow_pixmap = XCB_NONE, shadow_pixmap_argb = XCB_NONE;
	xcb_render_picture_t shadow_picture = XCB_NONE, shadow_picture_argb = XCB_NONE;
	xcb_gcontext_t gc = XCB_NONE;

	auto c = session_get_x_connection(ps);
	auto shadow_context = (conv *)session_get_backend_shadow_context(ps);
	shadow_image = make_shadow(c, shadow_context, opacity, width, height);
	if (!shadow_image) {
		log_error("failed to make shadow");
		return XCB_NONE;
	}

	shadow_pixmap = x_create_pixmap(c, 8, shadow_image->width, shadow_image->height);
	shadow_pixmap_argb =
	    x_create_pixmap(c, 32, shadow_image->width, shadow_image->height);

	if (!shadow_pixmap || !shadow_pixmap_argb) {
		log_error("failed to create shadow pixmaps");
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
	xcb_create_gc(c->c, gc, shadow_pixmap, 0, NULL);

	xcb_image_put(c->c, shadow_pixmap, gc, shadow_image, 0, 0, 0);
	xcb_render_composite(c->c, XCB_RENDER_PICT_OP_SRC, session_get_cshadow_picture(ps),
	                     shadow_picture, shadow_picture_argb, 0, 0, 0, 0, 0, 0,
	                     shadow_image->width, shadow_image->height);

	assert(!w->shadow_paint.pixmap);
	w->shadow_paint.pixmap = shadow_pixmap_argb;
	assert(!w->shadow_paint.pict);
	w->shadow_paint.pict = shadow_picture_argb;

	xcb_free_gc(c->c, gc);
	xcb_image_destroy(shadow_image);
	xcb_free_pixmap(c->c, shadow_pixmap);
	x_free_picture(c, shadow_picture);

	return true;

shadow_picture_err:
	if (shadow_image) {
		xcb_image_destroy(shadow_image);
	}
	if (shadow_pixmap) {
		xcb_free_pixmap(c->c, shadow_pixmap);
	}
	if (shadow_pixmap_argb) {
		xcb_free_pixmap(c->c, shadow_pixmap_argb);
	}
	if (shadow_picture) {
		x_free_picture(c, shadow_picture);
	}
	if (shadow_picture_argb) {
		x_free_picture(c, shadow_picture_argb);
	}
	if (gc) {
		xcb_free_gc(c->c, gc);
	}

	return false;
}

/**
 * Paint the shadow of a window.
 */
static inline void
win_paint_shadow(session_t *ps, struct managed_win *w, region_t *reg_paint) {
	// Bind shadow pixmap to GLX texture if needed
	paint_bind_tex(ps, &w->shadow_paint, 0, 0, false, 32, 0, false);

	if (!paint_isvalid(ps, &w->shadow_paint)) {
		log_error("Window %#010x is missing shadow data.", w->base.id);
		return;
	}

	xcb_render_picture_t td = XCB_NONE;
	auto options = session_get_options(ps);
	auto c = session_get_x_connection(ps);
	bool should_clip = (w->corner_radius > 0) &&
	                   (!options->wintype_option[w->window_type].full_shadow);
	if (should_clip) {
		if (options->backend == BKEND_XRENDER ||
		    options->backend == BKEND_XR_GLX_HYBRID) {
			uint32_t max_ntraps = to_u32_checked(w->corner_radius);
			xcb_render_trapezoid_t traps[4 * max_ntraps + 3];
			uint32_t n = make_rounded_window_shape(
			    traps, max_ntraps, w->corner_radius, w->widthb, w->heightb);

			td = x_create_picture_with_standard(
			    c, w->widthb, w->heightb, XCB_PICT_STANDARD_ARGB_32, 0, 0);
			xcb_render_color_t trans = {
			    .red = 0, .blue = 0, .green = 0, .alpha = 0};
			const xcb_rectangle_t rect = {.x = 0,
			                              .y = 0,
			                              .width = to_u16_checked(w->widthb),
			                              .height = to_u16_checked(w->heightb)};
			xcb_render_fill_rectangles(c->c, XCB_RENDER_PICT_OP_SRC, td,
			                           trans, 1, &rect);

			auto solid = solid_picture(c, false, 1, 0, 0, 0);
			xcb_render_trapezoids(
			    c->c, XCB_RENDER_PICT_OP_OVER, solid, td,
			    x_get_pictfmt_for_standard(c, XCB_PICT_STANDARD_A_8), 0, 0, n,
			    traps);
			x_free_picture(c, solid);
		} else {
			// Not implemented
		}
	}

	clip_t clip = {
	    .pict = td,
	    .x = -(w->shadow_dx),
	    .y = -(w->shadow_dy),
	};
	render(ps, 0, 0, w->g.x + w->shadow_dx, w->g.y + w->shadow_dy, w->shadow_width,
	       w->shadow_height, w->widthb, w->heightb, w->shadow_opacity, true, false, 0,
	       w->shadow_paint.pict, w->shadow_paint.ptex, reg_paint, NULL,
	       should_clip ? &clip : NULL);
	if (td) {
		x_free_picture(c, td);
	}
}

/**
 * @brief Blur an area on a buffer.
 *
 * @param ps current session
 * @param tgt_buffer a buffer as both source and destination
 * @param x x pos
 * @param y y pos
 * @param wid width
 * @param hei height
 * @param blur_kerns blur kernels, ending with a NULL, guaranteed to have at
 *                    least one kernel
 * @param reg_clip a clipping region to be applied on intermediate buffers
 *
 * @return true if successful, false otherwise
 */
static bool
xr_blur_dst(session_t *ps, xcb_render_picture_t tgt_buffer, int16_t x, int16_t y,
            uint16_t wid, uint16_t hei, struct x_convolution_kernel **blur_kerns,
            int nkernels, const region_t *reg_clip, xcb_render_picture_t rounded) {
	assert(blur_kerns);
	assert(blur_kerns[0]);

	// Directly copying from tgt_buffer to it does not work, so we create a
	// Picture in the middle.
	auto c = session_get_x_connection(ps);
	xcb_render_picture_t tmp_picture =
	    x_create_picture_with_visual(c, wid, hei, c->screen_info->root_visual, 0, NULL);

	if (!tmp_picture) {
		log_error("Failed to build intermediate Picture.");
		return false;
	}

	if (reg_clip && tmp_picture) {
		x_set_picture_clip_region(c, tmp_picture, 0, 0, reg_clip);
	}

	xcb_render_picture_t src_pict = tgt_buffer, dst_pict = tmp_picture;
	for (int i = 0; i < nkernels; ++i) {
		xcb_render_fixed_t *convolution_blur = blur_kerns[i]->kernel;
		// `x / 65536.0` converts from X fixed point to double
		int kwid = (int)((double)convolution_blur[0] / 65536.0),
		    khei = (int)((double)convolution_blur[1] / 65536.0);
		bool rd_from_tgt = (tgt_buffer == src_pict);

		// Copy from source picture to destination. The filter must
		// be applied on source picture, to get the nearby pixels outside the
		// window.
		xcb_render_set_picture_filter(
		    c->c, src_pict, strlen(XRFILTER_CONVOLUTION), XRFILTER_CONVOLUTION,
		    (uint32_t)(kwid * khei + 2), convolution_blur);
		xcb_render_composite(c->c, XCB_RENDER_PICT_OP_SRC, src_pict, XCB_NONE,
		                     dst_pict, (rd_from_tgt ? x : 0),
		                     (rd_from_tgt ? y : 0), 0, 0, (rd_from_tgt ? 0 : x),
		                     (rd_from_tgt ? 0 : y), wid, hei);
		xrfilter_reset(ps, src_pict);

		{
			xcb_render_picture_t tmp = src_pict;
			src_pict = dst_pict;
			dst_pict = tmp;
		}
	}

	if (src_pict != tgt_buffer) {
		xcb_render_composite(c->c, XCB_RENDER_PICT_OP_OVER, src_pict, rounded,
		                     tgt_buffer, 0, 0, 0, 0, x, y, wid, hei);
	}

	x_free_picture(c, tmp_picture);

	return true;
}

/**
 * Blur the background of a window.
 */
static inline void
win_blur_background(session_t *ps, struct managed_win *w, xcb_render_picture_t tgt_buffer,
                    const region_t *reg_paint) {
	const int16_t x = w->g.x;
	const int16_t y = w->g.y;
	auto const wid = to_u16_checked(w->widthb);
	auto const hei = to_u16_checked(w->heightb);
	const int cr = w ? w->corner_radius : 0;
	const double window_opacity = animatable_get(&w->opacity);

	double factor_center = 1.0;
	auto options = session_get_options(ps);
	auto c = session_get_x_connection(ps);
	auto blur_kerns_cache = session_get_blur_kern_cache(ps);
	// Adjust blur strength according to window opacity, to make it appear
	// better during fading
	if (!options->blur_background_fixed) {
		double pct = 1.0 - window_opacity * (1.0 - 1.0 / 9.0);
		factor_center = pct * 8.0 / (1.1 - pct);
	}

	switch (options->backend) {
	case BKEND_XRENDER:
	case BKEND_XR_GLX_HYBRID: {
		// Normalize blur kernels
		for (int i = 0; i < options->blur_kernel_count; i++) {
			// Note: `x * 65536` converts double `x` to a X fixed point
			// representation. `x / 65536` is the other way.
			auto kern_src = options->blur_kerns[i];
			auto kern_dst = blur_kerns_cache[i];

			assert(!kern_dst || (kern_src->w == kern_dst->kernel[0] / 65536 &&
			                     kern_src->h == kern_dst->kernel[1] / 65536));

			// Skip for fixed factor_center if the cache exists already
			if (options->blur_background_fixed && kern_dst) {
				continue;
			}

			x_create_convolution_kernel(kern_src, factor_center,
			                            &blur_kerns_cache[i]);
		}

		xcb_render_picture_t td = XCB_NONE;
		if (cr) {
			uint32_t max_ntraps = to_u32_checked(cr);
			xcb_render_trapezoid_t traps[4 * max_ntraps + 3];
			uint32_t n =
			    make_rounded_window_shape(traps, max_ntraps, cr, wid, hei);

			td = x_create_picture_with_standard(
			    c, wid, hei, XCB_PICT_STANDARD_ARGB_32, 0, 0);
			xcb_render_color_t trans = {
			    .red = 0, .blue = 0, .green = 0, .alpha = 0};
			const xcb_rectangle_t rect = {.x = 0,
			                              .y = 0,
			                              .width = to_u16_checked(wid),
			                              .height = to_u16_checked(hei)};
			xcb_render_fill_rectangles(c->c, XCB_RENDER_PICT_OP_SRC, td,
			                           trans, 1, &rect);

			auto solid = solid_picture(c, false, 1, 0, 0, 0);

			xcb_render_trapezoids(
			    c->c, XCB_RENDER_PICT_OP_OVER, solid, td,
			    x_get_pictfmt_for_standard(c, XCB_PICT_STANDARD_A_8), 0, 0, n,
			    traps);
			x_free_picture(c, solid);
		}

		// Minimize the region we try to blur, if the window itself is not
		// opaque, only the frame is.
		region_t reg_blur = win_get_bounding_shape_global_by_val(w);
		if (w->mode == WMODE_FRAME_TRANS && !options->force_win_blend) {
			region_t reg_noframe;
			pixman_region32_init(&reg_noframe);
			win_get_region_noframe_local(w, &reg_noframe);
			pixman_region32_translate(&reg_noframe, w->g.x, w->g.y);
			pixman_region32_subtract(&reg_blur, &reg_blur, &reg_noframe);
			pixman_region32_fini(&reg_noframe);
		}

		// Translate global coordinates to local ones
		pixman_region32_translate(&reg_blur, -x, -y);
		xr_blur_dst(ps, tgt_buffer, x, y, wid, hei, blur_kerns_cache,
		            options->blur_kernel_count, &reg_blur, td);
		if (td) {
			x_free_picture(c, td);
		}
		pixman_region32_clear(&reg_blur);
	} break;
#ifdef CONFIG_OPENGL
	case BKEND_GLX: {
		glx_blur_dst(ps, x, y, wid, hei, (float)session_get_psglx(ps)->z - 0.5F,
		             (float)factor_center, reg_paint, &w->glx_blur_cache);
		break;
	}
#endif
	default: assert(0);
	}
#ifndef CONFIG_OPENGL
	(void)reg_paint;
#endif
}

/// paint all windows
/// region = ??
/// region_real = the damage region
void paint_all(session_t *ps, struct managed_win *t) {
	auto options = session_get_options(ps);
	auto c = session_get_x_connection(ps);
	auto tgt_picture = session_get_tgt_picture(ps);
	session_xsync_wait_fence(ps);

	region_t region;
	pixman_region32_init(&region);
	damage_ring_collect(session_get_damage_ring(ps), session_get_screen_reg(ps),
	                    &region, get_buffer_age(ps));
	if (!pixman_region32_not_empty(&region)) {
		return;
	}

#ifdef DEBUG_REPAINT
	static struct timespec last_paint = {0};
#endif

	if (options->resize_damage > 0) {
		resize_region_in_place(&region, options->resize_damage, options->resize_damage);
	}

	// Remove the damaged area out of screen
	pixman_region32_intersect(&region, &region, session_get_screen_reg(ps));

	if (!paint_isvalid(ps, session_get_tgt_buffer(ps))) {
		auto tgt_buffer = session_get_tgt_buffer(ps);
		auto root_extent = session_get_root_extent(ps);
		if (!tgt_buffer->pixmap) {
			free_paint(ps, tgt_buffer);
			tgt_buffer->pixmap =
			    x_create_pixmap(c, c->screen_info->root_depth,
			                    root_extent.width, root_extent.height);
			if (tgt_buffer->pixmap == XCB_NONE) {
				log_fatal("Failed to allocate a screen-sized pixmap for"
				          "painting");
				exit(1);
			}
		}

		if (BKEND_GLX != options->backend) {
			tgt_buffer->pict = x_create_picture_with_visual_and_pixmap(
			    c, c->screen_info->root_visual,
			    session_get_tgt_buffer(ps)->pixmap, 0, 0);
		}
	}

	if (BKEND_XRENDER == options->backend) {
		x_set_picture_clip_region(c, tgt_picture, 0, 0, &region);
	}

#ifdef CONFIG_OPENGL
	if (bkend_use_glx(ps)) {
		session_get_psglx(ps)->z = 0;
	}
#endif

	region_t reg_tmp, *reg_paint;
	pixman_region32_init(&reg_tmp);
	if (t) {
		// Calculate the region upon which the root window is to be
		// painted based on the ignore region of the lowest window, if
		// available
		pixman_region32_subtract(&reg_tmp, &region, t->reg_ignore);
		reg_paint = &reg_tmp;
	} else {
		reg_paint = &region;
	}

	// Region on screen we don't want any shadows on
	region_t reg_shadow_clip;
	pixman_region32_init(&reg_shadow_clip);

	set_tgt_clip(ps, reg_paint);
	paint_root(ps, reg_paint);

	// Windows are sorted from bottom to top
	// Each window has a reg_ignore, which is the region obscured by all the
	// windows on top of that window. This is used to reduce the number of
	// pixels painted.
	//
	// Whether this is beneficial is to be determined XXX
	for (auto w = t; w; w = w->prev_trans) {
		region_t bshape_no_corners =
		    win_get_bounding_shape_global_without_corners_by_val(w);
		region_t bshape_corners = win_get_bounding_shape_global_by_val(w);
		// Painting shadow
		if (w->shadow) {
			// Lazy shadow building
			if (!w->shadow_paint.pixmap) {
				if (!win_build_shadow(ps, w, 1)) {
					log_error("build shadow failed");
				}
			}

			// Shadow doesn't need to be painted underneath the body
			// of the windows above. Because no one can see it
			pixman_region32_subtract(&reg_tmp, &region, w->reg_ignore);

			// Mask out the region we don't want shadow on
			auto shadow_exclude_reg = session_get_shadow_exclude_reg(ps);
			if (pixman_region32_not_empty(shadow_exclude_reg)) {
				pixman_region32_subtract(&reg_tmp, &reg_tmp, shadow_exclude_reg);
			}
			if (pixman_region32_not_empty(&reg_shadow_clip)) {
				pixman_region32_subtract(&reg_tmp, &reg_tmp, &reg_shadow_clip);
			}

			// Might be worth while to crop the region to shadow
			// border
			assert(w->shadow_width >= 0 && w->shadow_height >= 0);
			pixman_region32_intersect_rect(
			    &reg_tmp, &reg_tmp, w->g.x + w->shadow_dx, w->g.y + w->shadow_dy,
			    (uint)w->shadow_width, (uint)w->shadow_height);

			// Mask out the body of the window from the shadow if
			// needed Doing it here instead of in make_shadow() for
			// saving GPU power and handling shaped windows (XXX
			// unconfirmed)
			if (!options->wintype_option[w->window_type].full_shadow) {
				pixman_region32_subtract(&reg_tmp, &reg_tmp, &bshape_no_corners);
			}

			auto monitors = session_get_monitors(ps);
			if (options->crop_shadow_to_monitor && w->randr_monitor >= 0 &&
			    w->randr_monitor < monitors->count) {
				// There can be a window where number of monitors is
				// updated, but the monitor number attached to the window
				// have not.
				//
				// Window monitor number will be updated eventually, so
				// here we just check to make sure we don't access out of
				// bounds.
				pixman_region32_intersect(
				    &reg_tmp, &reg_tmp, &monitors->regions[w->randr_monitor]);
			}

			// Detect if the region is empty before painting
			if (pixman_region32_not_empty(&reg_tmp)) {
				set_tgt_clip(ps, &reg_tmp);
				win_paint_shadow(ps, w, &reg_tmp);
			}
		}

		// Only clip shadows above visible windows
		if (animatable_get(&w->opacity) * MAX_ALPHA >= 1) {
			if (w->clip_shadow_above) {
				// Add window bounds to shadow-clip region
				pixman_region32_union(&reg_shadow_clip, &reg_shadow_clip,
				                      &bshape_corners);
			} else {
				// Remove overlapping window bounds from shadow-clip
				// region
				pixman_region32_subtract(
				    &reg_shadow_clip, &reg_shadow_clip, &bshape_corners);
			}
		}

		// Calculate the paint region based on the reg_ignore of the current
		// window and its bounding region.
		// Remember, reg_ignore is the union of all windows above the current
		// window.
		pixman_region32_subtract(&reg_tmp, &region, w->reg_ignore);
		pixman_region32_intersect(&reg_tmp, &reg_tmp, &bshape_corners);
		pixman_region32_fini(&bshape_corners);
		pixman_region32_fini(&bshape_no_corners);

		if (pixman_region32_not_empty(&reg_tmp)) {
			set_tgt_clip(ps, &reg_tmp);

#ifdef CONFIG_OPENGL
			// If rounded corners backup the region first
			if (w->corner_radius > 0 && options->backend == BKEND_GLX) {
				const int16_t x = w->g.x;
				const int16_t y = w->g.y;
				auto const wid = to_u16_checked(w->widthb);
				auto const hei = to_u16_checked(w->heightb);
				glx_bind_texture(ps, &w->glx_texture_bg, x, y, wid, hei);
			}
#endif

			// Blur window background
			if (w->blur_background &&
			    (w->mode == WMODE_TRANS ||
			     (options->blur_background_frame && w->mode == WMODE_FRAME_TRANS) ||
			     options->force_win_blend)) {
				win_blur_background(
				    ps, w, session_get_tgt_buffer(ps)->pict, &reg_tmp);
			}

			// Painting the window
			paint_one(ps, w, &reg_tmp);

#ifdef CONFIG_OPENGL
			// Rounded corners for XRender is implemented inside render()
			// Round window corners
			if (w->corner_radius > 0 && options->backend == BKEND_GLX) {
				auto const wid = to_u16_checked(w->widthb);
				auto const hei = to_u16_checked(w->heightb);
				glx_round_corners_dst(ps, w, w->glx_texture_bg, w->g.x,
				                      w->g.y, wid, hei,
				                      (float)session_get_psglx(ps)->z - 0.5F,
				                      (float)w->corner_radius, &reg_tmp);
			}
#endif
		}
	}

	// Free up all temporary regions
	pixman_region32_fini(&reg_tmp);
	pixman_region32_fini(&reg_shadow_clip);

	// Move the head of the damage ring
	damage_ring_advance(session_get_damage_ring(ps));

	// Do this as early as possible
	set_tgt_clip(ps, session_get_screen_reg(ps));

	if (options->vsync) {
		// Make sure all previous requests are processed to achieve best
		// effect
		xcb_aux_sync(c->c);
#ifdef CONFIG_OPENGL
		if (glx_has_context(ps)) {
			if (options->vsync_use_glfinish) {
				glFinish();
			} else {
				glFlush();
			}
			glXWaitX();
		}
#endif
	}

	session_vsync_wait(ps);

	auto root_extent = session_get_root_extent(ps);
	auto rwidth = to_u16_checked(root_extent.width);
	auto rheight = to_u16_checked(root_extent.height);
	auto tgt_buffer = session_get_tgt_buffer(ps);
	switch (options->backend) {
	case BKEND_XRENDER:
		if (options->monitor_repaint) {
			// Copy the screen content to a new picture, and highlight the
			// paint region. This is not very efficient, but since it's for
			// debug only, we don't really care

			// First we create a new picture, and copy content from the buffer
			// to it
			auto pictfmt =
			    x_get_pictform_for_visual(c, c->screen_info->root_visual);
			xcb_render_picture_t new_pict = x_create_picture_with_pictfmt(
			    c, rwidth, rheight, pictfmt, 0, NULL);
			xcb_render_composite(c->c, XCB_RENDER_PICT_OP_SRC,
			                     tgt_buffer->pict, XCB_NONE, new_pict, 0, 0,
			                     0, 0, 0, 0, rwidth, rheight);

			// Next, we set the region of paint and highlight it
			x_set_picture_clip_region(c, new_pict, 0, 0, &region);
			xcb_render_composite(c->c, XCB_RENDER_PICT_OP_OVER,
			                     session_get_white_picture(ps),
			                     session_get_alpha_pictures(ps)[MAX_ALPHA / 2],
			                     new_pict, 0, 0, 0, 0, 0, 0, rwidth, rheight);

			// Finally, clear clip regions of new_pict and the screen, and put
			// the whole thing on screen
			auto screen_reg = session_get_screen_reg(ps);
			x_set_picture_clip_region(c, new_pict, 0, 0, screen_reg);
			x_set_picture_clip_region(c, tgt_picture, 0, 0, screen_reg);
			xcb_render_composite(c->c, XCB_RENDER_PICT_OP_SRC, new_pict, XCB_NONE,
			                     tgt_picture, 0, 0, 0, 0, 0, 0, rwidth, rheight);
			x_free_picture(c, new_pict);
		} else {
			xcb_render_composite(c->c, XCB_RENDER_PICT_OP_SRC,
			                     tgt_buffer->pict, XCB_NONE, tgt_picture, 0,
			                     0, 0, 0, 0, 0, rwidth, rheight);
		}
		break;
#ifdef CONFIG_OPENGL
	case BKEND_XR_GLX_HYBRID:
		xcb_aux_sync(c->c);
		if (options->vsync_use_glfinish) {
			glFinish();
		} else {
			glFlush();
		}
		glXWaitX();
		assert(tgt_buffer->pixmap);
		paint_bind_tex(ps, tgt_buffer, root_extent.width, root_extent.height,
		               false, c->screen_info->root_depth,
		               c->screen_info->root_visual, !options->glx_no_rebind_pixmap);
		if (options->vsync_use_glfinish) {
			glFinish();
		} else {
			glFlush();
		}
		glXWaitX();
		glx_render(ps, tgt_buffer->ptex, 0, 0, 0, 0, root_extent.width,
		           root_extent.height, 0, 1.0, false, false, &region, NULL);
		fallthrough();
	case BKEND_GLX: glXSwapBuffers(c->dpy, session_get_target_window(ps)); break;
#endif
	default: assert(0);
	}

	xcb_aux_sync(c->c);

#ifdef CONFIG_OPENGL
	if (glx_has_context(ps)) {
		glFlush();
		glXWaitX();
	}
#endif

#ifdef DEBUG_REPAINT
	struct timespec now = get_time_timespec();
	struct timespec diff = {0};
	timespec_subtract(&diff, &now, &last_paint);
	log_trace("[ %5ld:%09ld ] ", diff.tv_sec, diff.tv_nsec);
	last_paint = now;
	log_trace("paint:");
	for (win *w = t; w; w = w->prev_trans)
		log_trace(" %#010lx", w->id);
#endif

	// Free the paint region
	pixman_region32_fini(&region);
}

/**
 * Query needed X Render / OpenGL filters to check for their existence.
 */
static bool xr_init_blur(session_t *ps) {
	auto c = session_get_x_connection(ps);
	bool xrfilter_convolution_exists = false;
	// Query filters
	xcb_render_query_filters_reply_t *pf = xcb_render_query_filters_reply(
	    c->c, xcb_render_query_filters(c->c, session_get_target_window(ps)), NULL);
	if (pf) {
		xcb_str_iterator_t iter = xcb_render_query_filters_filters_iterator(pf);
		for (; iter.rem; xcb_str_next(&iter)) {
			int len = xcb_str_name_length(iter.data);
			char *name = xcb_str_name(iter.data);
			// Check for the convolution filter
			if (strlen(XRFILTER_CONVOLUTION) == len &&
			    !memcmp(XRFILTER_CONVOLUTION, name, strlen(XRFILTER_CONVOLUTION))) {
				xrfilter_convolution_exists = true;
			}
		}
		free(pf);
	}

	// Turn features off if any required filter is not present
	if (!xrfilter_convolution_exists) {
		log_error("Xrender convolution filter "
		          "unsupported by your X server. "
		          "Background blur is not possible.");
		return false;
	}

	return true;
}

bool init_render(session_t *ps) {
	auto options = session_get_options(ps);
	auto c = session_get_x_connection(ps);
	if (options->backend == BKEND_DUMMY) {
		return false;
	}

	// Initialize OpenGL as early as possible
#ifdef CONFIG_OPENGL
	glxext_init(c->dpy, c->screen);
#endif
	if (bkend_use_glx(ps)) {
#ifdef CONFIG_OPENGL
		if (!glx_init(ps, true)) {
			return false;
		}
#else
		log_error("GLX backend support not compiled in.");
		return false;
#endif
	}

	// Initialize VSync
	if (!vsync_init(ps)) {
		return false;
	}

	// Initialize window GL shader
	if (BKEND_GLX == options->backend && options->glx_fshader_win_str) {
#ifdef CONFIG_OPENGL
		auto psglx = session_get_psglx(ps);
		if (!glx_load_prog_main(NULL, options->glx_fshader_win_str,
		                        &psglx->glx_prog_win)) {
			return false;
		}
#else
		log_error("GLSL supported not compiled in, can't load "
		          "shader.");
		return false;
#endif
	}

	auto alpha_picts = session_get_alpha_pictures(ps);
	for (int i = 0; i <= MAX_ALPHA; ++i) {
		double o = (double)i / MAX_ALPHA;
		alpha_picts[i] = solid_picture(c, false, o, 0, 0, 0);
		if (alpha_picts[i] == XCB_NONE) {
			log_error("Failed to init alpha pictures.");
			return false;
		}
	}

	// Blur filter
	if (options->blur_method && options->blur_method != BLUR_METHOD_KERNEL) {
		log_warn("Old backends only support blur method \"kernel\". Your blur "
		         "setting will not be applied");
		options->blur_method = BLUR_METHOD_NONE;
	}

	if (options->blur_method == BLUR_METHOD_KERNEL) {
		auto blur_kerns_cache =
		    ccalloc(options->blur_kernel_count, struct x_convolution_kernel *);
		session_set_blur_kern_cache(ps, blur_kerns_cache);

		bool ret = false;
		if (options->backend == BKEND_GLX) {
#ifdef CONFIG_OPENGL
			ret = glx_init_blur(ps);
#else
			assert(false);
#endif
		} else {
			ret = xr_init_blur(ps);
		}
		if (!ret) {
			return ret;
		}
	}

	auto black_picture = solid_picture(c, true, 1, 0, 0, 0);
	auto white_picture = solid_picture(c, true, 1, 1, 1, 1);

	if (black_picture == XCB_NONE || white_picture == XCB_NONE) {
		log_error("Failed to create solid xrender pictures.");
		return false;
	}

	session_set_black_picture(ps, black_picture);
	session_set_white_picture(ps, white_picture);
	// Generates another Picture for shadows if the color is modified by
	// user
	if (options->shadow_red == 0 && options->shadow_green == 0 &&
	    options->shadow_blue == 0) {
		session_set_cshadow_picture(ps, black_picture);
	} else {
		auto cshadow_picture =
		    solid_picture(c, true, 1, options->shadow_red, options->shadow_green,
		                  options->shadow_blue);
		if (cshadow_picture == XCB_NONE) {
			log_error("Failed to create shadow picture.");
			return false;
		}
		session_set_cshadow_picture(ps, cshadow_picture);
	}

	// Initialize our rounded corners fragment shader
	if (options->corner_radius > 0 && options->backend == BKEND_GLX) {
#ifdef CONFIG_OPENGL
		if (!glx_init_rounded_corners(ps)) {
			log_error("Failed to init rounded corners shader.");
			return false;
		}
#else
		assert(false);
#endif
	}
	return true;
}

/**
 * Free root tile related things.
 */
void free_root_tile(session_t *ps) {
	auto c = session_get_x_connection(ps);
	auto root_tile_paint = session_get_root_tile_paint(ps);
	x_free_picture(c, root_tile_paint->pict);
#ifdef CONFIG_OPENGL
	free_texture(ps, &root_tile_paint->ptex);
#else
	assert(!root_tile_paint->ptex);
#endif
	if (session_get_root_tile_fill(ps)) {
		xcb_free_pixmap(c->c, root_tile_paint->pixmap);
	}
	root_tile_paint->pixmap = XCB_NONE;
	session_set_root_tile_fill(ps, false);
}

void deinit_render(session_t *ps) {
	auto c = session_get_x_connection(ps);
	// Free alpha_picts
	auto alpha_picts = session_get_alpha_pictures(ps);
	for (int i = 0; i <= MAX_ALPHA; ++i) {
		x_free_picture(c, alpha_picts[i]);
	}

	// Free cshadow_picture and black_picture
	auto cshadow_picture = session_get_cshadow_picture(ps);
	auto black_picture = session_get_black_picture(ps);
	auto white_picture = session_get_white_picture(ps);
	if (cshadow_picture != black_picture) {
		x_free_picture(c, cshadow_picture);
	}

	x_free_picture(c, black_picture);
	x_free_picture(c, white_picture);
	session_set_black_picture(ps, XCB_NONE);
	session_set_white_picture(ps, XCB_NONE);
	session_set_cshadow_picture(ps, XCB_NONE);

	// Free other X resources
	free_root_tile(ps);

#ifdef CONFIG_OPENGL
	session_get_root_tile_paint(ps)->fbcfg = (struct glx_fbconfig_info){0};
	if (glx_has_context(ps)) {
		glx_destroy(ps, session_get_psglx(ps));
		session_set_psglx(ps, NULL);
	}
#endif

	auto options = session_get_options(ps);
	auto blur_kerns_cache = session_get_blur_kern_cache(ps);
	if (options->blur_method != BLUR_METHOD_NONE) {
		for (int i = 0; i < options->blur_kernel_count; i++) {
			free(blur_kerns_cache[i]);
		}
		free(blur_kerns_cache);
	}
	session_set_blur_kern_cache(ps, NULL);
}

// vim: set ts=8 sw=8 noet :
