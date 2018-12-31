#include <xcb/xcb.h>

#include "backend.h"
#include "config.h"
#include "win.h"
#include "region.h"
#include "common.h"
#include "compiler.h"

backend_info_t *backend_list[NUM_BKEND] = {[BKEND_XRENDER] = &xrender_backend};

bool default_is_win_transparent(void *backend_data, win *w, void *win_data) {
	return w->mode != WMODE_SOLID;
}

bool default_is_frame_transparent(void *backend_data, win *w, void *win_data) {
	return w->frame_opacity != 1;
}

/// paint all windows
void paint_all_new(session_t *ps, region_t *_region, win *const t) {
	auto bi = backend_list[ps->o.backend];
	assert(bi);

	const region_t *region;
	if (!_region) {
		region = &ps->screen_reg;
	} else {
		// Ignore out-of-screen damages
		pixman_region32_intersect(_region, _region, &ps->screen_reg);
		region = _region;
	}

#ifdef DEBUG_REPAINT
	static struct timespec last_paint = {0};
#endif

	region_t reg_tmp;
	const region_t *reg_paint;
	pixman_region32_init(&reg_tmp);
	if (t) {
		// Calculate the region upon which the root window (wallpaper) is to be
		// painted based on the ignore region of the lowest window, if available
		pixman_region32_subtract(&reg_tmp, (region_t *)region, t->reg_ignore);
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
		pixman_region32_subtract(&reg_tmp, (region_t *)region, w->reg_ignore);

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
