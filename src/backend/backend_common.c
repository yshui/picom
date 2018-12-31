#include <xcb/xcb_image.h>

#include "backend.h"
#include "backend_common.h"
#include "x.h"
#include "common.h"

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

	pixmap = x_create_pixmap(ps, argb ? 32 : 8, ps->root, 1, 1);
	if (!pixmap)
		return XCB_NONE;

	pa.repeat = 1;
	picture = x_create_picture_with_standard_and_pixmap(
	    ps, argb ? XCB_PICT_STANDARD_ARGB_32 : XCB_PICT_STANDARD_A_8, pixmap,
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

static xcb_image_t *make_shadow(session_t *ps, double opacity, int width, int height) {
	xcb_image_t *ximage;
	int ylimit, xlimit;
	int swidth = width + ps->cgsize;
	int sheight = height + ps->cgsize;
	int center = ps->cgsize / 2;
	int x, y;
	unsigned char d;
	int x_diff;
	int opacity_int = (int)(opacity * 25);

	ximage = xcb_image_create_native(ps->c, swidth, sheight,
	                                 XCB_IMAGE_FORMAT_Z_PIXMAP, 8, 0, 0, NULL);

	if (!ximage) {
		log_error("failed to create an X image");
		return 0;
	}

	unsigned char *data = ximage->data;
	uint32_t sstride = ximage->stride;

	/*
	 * Build the gaussian in sections
	 */

	/*
	 * center (fill the complete data array)
	 */

	// XXX If the center part of the shadow would be entirely covered by
	// the body of the window, we shouldn't need to fill the center here.
	// XXX In general, we want to just fill the part that is not behind
	// the window, in order to reduce CPU load and make transparent window
	// look correct
	if (ps->cgsize > 0) {
		d = ps->shadow_top[opacity_int * (ps->cgsize + 1) + ps->cgsize];
	} else {
		d = (unsigned char)(sum_kernel(ps->gaussian_map, center, center, width,
		                               height) *
		                    opacity * 255.0);
	}
	memset(data, d, sheight * swidth);

	/*
	 * corners
	 */

	ylimit = ps->cgsize;
	if (ylimit > sheight / 2)
		ylimit = (sheight + 1) / 2;

	xlimit = ps->cgsize;
	if (xlimit > swidth / 2)
		xlimit = (swidth + 1) / 2;

	for (y = 0; y < ylimit; y++) {
		for (x = 0; x < xlimit; x++) {
			if (xlimit == ps->cgsize && ylimit == ps->cgsize) {
				d = ps->shadow_corner[opacity_int * (ps->cgsize + 1) *
				                          (ps->cgsize + 1) +
				                      y * (ps->cgsize + 1) + x];
			} else {
				d = (unsigned char)(sum_kernel(ps->gaussian_map, x - center,
				                               y - center, width, height) *
				                    opacity * 255.0);
			}
			data[y * sstride + x] = d;
			data[(sheight - y - 1) * sstride + x] = d;
			data[(sheight - y - 1) * sstride + (swidth - x - 1)] = d;
			data[y * sstride + (swidth - x - 1)] = d;
		}
	}

	/*
	 * top/bottom
	 */

	x_diff = swidth - (ps->cgsize * 2);
	if (x_diff > 0 && ylimit > 0) {
		for (y = 0; y < ylimit; y++) {
			if (ylimit == ps->cgsize) {
				d = ps->shadow_top[opacity_int * (ps->cgsize + 1) + y];
			} else {
				d = (unsigned char)(sum_kernel(ps->gaussian_map, center,
				                               y - center, width, height) *
				                    opacity * 255.0);
			}
			memset(&data[y * sstride + ps->cgsize], d, x_diff);
			memset(&data[(sheight - y - 1) * sstride + ps->cgsize], d, x_diff);
		}
	}

	/*
	 * sides
	 */

	for (x = 0; x < xlimit; x++) {
		if (xlimit == ps->cgsize) {
			d = ps->shadow_top[opacity_int * (ps->cgsize + 1) + x];
		} else {
			d = (unsigned char)(sum_kernel(ps->gaussian_map, x - center,
			                               center, width, height) *
			                    opacity * 255.0);
		}
		for (y = ps->cgsize; y < sheight - ps->cgsize; y++) {
			data[y * sstride + x] = d;
			data[y * sstride + (swidth - x - 1)] = d;
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
	xcb_pixmap_t shadow_pixmap = None, shadow_pixmap_argb = None;
	xcb_render_picture_t shadow_picture = None, shadow_picture_argb = None;
	xcb_gcontext_t gc = None;

	shadow_image = make_shadow(ps, opacity, width, height);
	if (!shadow_image) {
		log_error("Failed to make shadow");
		return false;
	}

	shadow_pixmap =
	    x_create_pixmap(ps, 8, ps->root, shadow_image->width, shadow_image->height);
	shadow_pixmap_argb =
	    x_create_pixmap(ps, 32, ps->root, shadow_image->width, shadow_image->height);

	if (!shadow_pixmap || !shadow_pixmap_argb) {
		log_error("Failed to create shadow pixmaps");
		goto shadow_picture_err;
	}

	shadow_picture = x_create_picture_with_standard_and_pixmap(
	    ps, XCB_PICT_STANDARD_A_8, shadow_pixmap, 0, NULL);
	shadow_picture_argb = x_create_picture_with_standard_and_pixmap(
	    ps, XCB_PICT_STANDARD_ARGB_32, shadow_pixmap_argb, 0, NULL);
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

/// paint all windows
void paint_all_new(session_t *ps, region_t *region, win *const t) {
	auto bi = backend_list[ps->o.backend];
	assert(bi);

#ifdef DEBUG_REPAINT
	static struct timespec last_paint = {0};
#endif

	// Ignore out-of-screen damages
	pixman_region32_intersect(region, region, &ps->screen_reg);

	region_t reg_tmp, *reg_paint;
	pixman_region32_init(&reg_tmp);
	if (t) {
		// Calculate the region upon which the root window (wallpaper) is to be
		// painted based on the ignore region of the lowest window, if available
		pixman_region32_subtract(&reg_tmp, region, t->reg_ignore);
		reg_paint = &reg_tmp;
	} else {
		reg_paint = region;
	}

	if (bi->prepare)
		bi->prepare(ps->backend_data, ps, reg_paint);

	// Windows are sorted from bottom to top
	// Each window has a reg_ignore, which is the region obscured by all the windows
	// on top of that window. This is used to reduce the number of pixels painted.
	//
	// Whether this is beneficial is to be determined XXX
	for (win *w = t; w; w = w->prev_trans) {
		// Calculate the region based on the reg_ignore of the next (higher)
		// window and the bounding region
		// XXX XXX
		pixman_region32_subtract(&reg_tmp, region, w->reg_ignore);

		if (pixman_region32_not_empty(&reg_tmp)) {
			// Render window content
			// XXX do this in preprocess?
			bi->render_win(ps->backend_data, ps, w, w->win_data, &reg_tmp);

			// Blur window background
			bool win_transparent =
			    bi->is_win_transparent(ps->backend_data, w, w->win_data);
			bool frame_transparent =
			    bi->is_frame_transparent(ps->backend_data, w, w->win_data);
			if (w->blur_background &&
			    (win_transparent ||
			     (ps->o.blur_background_frame && frame_transparent))) {
				// Minimize the region we try to blur, if the window
				// itself is not opaque, only the frame is.
				region_t reg_blur = win_get_bounding_shape_global_by_val(w);
				if (win_is_solid(ps, w)) {
					region_t reg_noframe;
					pixman_region32_init(&reg_noframe);
					win_get_region_noframe_local(w, &reg_noframe);
					pixman_region32_translate(&reg_noframe, w->g.x,
					                          w->g.y);
					pixman_region32_subtract(&reg_blur, &reg_blur,
					                         &reg_noframe);
					pixman_region32_fini(&reg_noframe);
				}
				bi->blur(ps->backend_data, ps,
				         (double)w->opacity / OPAQUE, &reg_blur);
				pixman_region32_fini(&reg_blur);
			}

			// Draw window on target
			bi->compose(ps->backend_data, ps, w, w->win_data, w->g.x, w->g.y,
			            &reg_tmp);

			if (bi->finish_render_win)
				bi->finish_render_win(ps->backend_data, ps, w, w->win_data);
		}
	}

	// Free up all temporary regions
	pixman_region32_fini(&reg_tmp);

	if (bi->present) {
		// Present the rendered scene
		// Vsync is done here
		bi->present(ps->backend_data, ps);
	}

#ifdef DEBUG_REPAINT
	print_timestamp(ps);
	struct timespec now = get_time_timespec();
	struct timespec diff = {0};
	timespec_subtract(&diff, &now, &last_paint);
	printf("[ %5ld:%09ld ] ", diff.tv_sec, diff.tv_nsec);
	last_paint = now;
	printf("paint:");
	for (win *w = t; w; w = w->prev_trans)
		printf(" %#010lx", w->id);
	putchar('\n');
	fflush(stdout);
#endif

	// Check if fading is finished on all painted windows
	win *pprev = NULL;
	for (win *w = t; w; w = pprev) {
		pprev = w->prev_trans;
		win_check_fade_finished(ps, &w);
	}
}

// vim: set noet sw=8 ts=8 :
