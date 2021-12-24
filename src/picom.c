// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE-mit for more information.
 *
 */

#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/sync.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/glx.h>
#include <xcb/present.h>
#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xfixes.h>
#include <xcb/xinerama.h>

#include <ev.h>
#include <test.h>

#include "common.h"
#include "compiler.h"
#include "config.h"
#include "err.h"
#include "kernel.h"
#include "picom.h"
#ifdef CONFIG_OPENGL
#include "opengl.h"
#endif
#include "backend/backend.h"
#include "c2.h"
#include "config.h"
#include "diagnostic.h"
#include "log.h"
#include "region.h"
#include "render.h"
#include "types.h"
#include "utils.h"
#include "win.h"
#include "x.h"
#ifdef CONFIG_DBUS
#include "dbus.h"
#endif
#include "atom.h"
#include "event.h"
#include "file_watch.h"
#include "list.h"
#include "options.h"
#include "uthash_extra.h"
#include "timing_functions.h"

/// Get session_t pointer from a pointer to a member of session_t
#define session_ptr(ptr, member)                                                         \
	({                                                                               \
		const __typeof__(((session_t *)0)->member) *__mptr = (ptr);              \
		(session_t *)((char *)__mptr - offsetof(session_t, member));             \
	})

static const long SWOPTI_TOLERANCE = 3000;

static bool must_use redirect_start(session_t *ps);

static void unredirect(session_t *ps);

// === Global constants ===

/// Name strings for window types.
const char *const WINTYPES[NUM_WINTYPES] = {
    "unknown",    "desktop", "dock",         "toolbar", "menu",
    "utility",    "splash",  "dialog",       "normal",  "dropdown_menu",
    "popup_menu", "tooltip", "notification", "combo",   "dnd",
};

// clang-format off
/// Names of backends.
const char *const BACKEND_STRS[] = {[BKEND_XRENDER] = "xrender",
                                    [BKEND_GLX] = "glx",
                                    [BKEND_XR_GLX_HYBRID] = "xr_glx_hybrid",
                                    [BKEND_DUMMY] = "dummy",
                                    NULL};
// clang-format on

// === Global variables ===

/// Pointer to current session, as a global variable. Only used by
/// xerror(), which could not have a pointer to current session passed in.
/// XXX Limit what xerror can access by not having this pointer
session_t *ps_g = NULL;

void set_root_flags(session_t *ps, uint64_t flags) {
	log_debug("Setting root flags: %" PRIu64, flags);
	ps->root_flags |= flags;
	ps->pending_updates = true;
}

void quit(session_t *ps) {
	ps->quit = true;
	ev_break(ps->loop, EVBREAK_ALL);
}

/**
 * Free Xinerama screen info.
 *
 * XXX consider moving to x.c
 */
static inline void free_xinerama_info(session_t *ps) {
	if (ps->xinerama_scr_regs) {
		for (int i = 0; i < ps->xinerama_nscrs; ++i)
			pixman_region32_fini(&ps->xinerama_scr_regs[i]);
		free(ps->xinerama_scr_regs);
		ps->xinerama_scr_regs = NULL;
	}
	ps->xinerama_nscrs = 0;
}

/**
 * Get current system clock in milliseconds.
 */
static inline int64_t get_time_ms(void) {
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (int64_t)tp.tv_sec * 1000 + (int64_t)tp.tv_nsec / 1000000;
}

// XXX Move to x.c
void cxinerama_upd_scrs(session_t *ps) {
	// XXX Consider deprecating Xinerama, switch to RandR when necessary
	free_xinerama_info(ps);

	if (!ps->o.xinerama_shadow_crop || !ps->xinerama_exists)
		return;

	xcb_xinerama_is_active_reply_t *active =
	    xcb_xinerama_is_active_reply(ps->c, xcb_xinerama_is_active(ps->c), NULL);
	if (!active || !active->state) {
		free(active);
		return;
	}
	free(active);

	auto xinerama_scrs =
	    xcb_xinerama_query_screens_reply(ps->c, xcb_xinerama_query_screens(ps->c), NULL);
	if (!xinerama_scrs) {
		return;
	}

	xcb_xinerama_screen_info_t *scrs =
	    xcb_xinerama_query_screens_screen_info(xinerama_scrs);
	ps->xinerama_nscrs = xcb_xinerama_query_screens_screen_info_length(xinerama_scrs);

	ps->xinerama_scr_regs = ccalloc(ps->xinerama_nscrs, region_t);
	for (int i = 0; i < ps->xinerama_nscrs; ++i) {
		const xcb_xinerama_screen_info_t *const s = &scrs[i];
		pixman_region32_init_rect(&ps->xinerama_scr_regs[i], s->x_org, s->y_org,
		                          s->width, s->height);
	}
	free(xinerama_scrs);
}

/**
 * Find matched window.
 *
 * XXX move to win.c
 */
static inline struct managed_win *find_win_all(session_t *ps, const xcb_window_t wid) {
	if (!wid || PointerRoot == wid || wid == ps->root || wid == ps->overlay)
		return NULL;

	auto w = find_managed_win(ps, wid);
	if (!w)
		w = find_toplevel(ps, wid);
	if (!w)
		w = find_managed_window_or_parent(ps, wid);
	return w;
}

void queue_redraw(session_t *ps) {
	// If --benchmark is used, redraw is always queued
	if (!ps->redraw_needed && !ps->o.benchmark) {
		ev_idle_start(ps->loop, &ps->draw_idle);
	}
	ps->redraw_needed = true;
}

/**
 * Get a region of the screen size.
 */
static inline void get_screen_region(session_t *ps, region_t *res) {
	pixman_box32_t b = {.x1 = 0, .y1 = 0, .x2 = ps->root_width, .y2 = ps->root_height};
	pixman_region32_fini(res);
	pixman_region32_init_rects(res, &b, 1);
}

void add_damage(session_t *ps, const region_t *damage) {
	// Ignore damage when screen isn't redirected
	if (!ps->redirected) {
		return;
	}

	if (!damage) {
		return;
	}
	log_trace("Adding damage: ");
	dump_region(damage);
	pixman_region32_union(ps->damage, ps->damage, (region_t *)damage);
}

// === Fading ===

/**
 * Get the time left before next fading point.
 *
 * In milliseconds.
 */
static double fade_timeout(session_t *ps) {
	auto now = get_time_ms();
	if (ps->o.fade_delta + ps->fade_time < now)
		return 0;

	auto diff = ps->o.fade_delta + ps->fade_time - now;

	diff = clamp(diff, 0, ps->o.fade_delta * 2);

	return (double)diff / 1000.0;
}

/**
 * Run fading on a window.
 *
 * @param steps steps of fading
 * @return whether we are still in fading mode
 */
static bool run_fade(session_t *ps, struct managed_win **_w, long long steps) {
	auto w = *_w;
	if (w->state == WSTATE_MAPPED || w->state == WSTATE_UNMAPPED) {
		// We are not fading
		assert(w->opacity_target == w->opacity);
		return false;
	}

	if (!win_should_fade(ps, w)) {
		log_debug("Window %#010x %s doesn't need fading", w->base.id, w->name);
		w->opacity = w->opacity_target;
	}
	if (w->opacity == w->opacity_target) {
		// We have reached target opacity.
		// We don't call win_check_fade_finished here because that could destroy
		// the window, but we still need the damage info from this window
		log_debug("Fading finished for window %#010x %s", w->base.id, w->name);
		return false;
	}

	if (steps) {
		log_trace("Window %#010x (%s) opacity was: %lf", w->base.id, w->name,
		          w->opacity);
		if (w->opacity < w->opacity_target) {
			w->opacity = clamp(w->opacity + ps->o.fade_in_step * (double)steps,
			                   0.0, w->opacity_target);
		} else {
			w->opacity = clamp(w->opacity - ps->o.fade_out_step * (double)steps,
			                   w->opacity_target, 1);
		}
		log_trace("... updated to: %lf", w->opacity);
	}

	// Note even if opacity == opacity_target here, we still want to run preprocess
	// one last time to finish state transition. So return true in that case too.
	return true;
}

// === Error handling ===

void discard_ignore(session_t *ps, unsigned long sequence) {
	while (ps->ignore_head) {
		if (sequence > ps->ignore_head->sequence) {
			ignore_t *next = ps->ignore_head->next;
			free(ps->ignore_head);
			ps->ignore_head = next;
			if (!ps->ignore_head) {
				ps->ignore_tail = &ps->ignore_head;
			}
		} else {
			break;
		}
	}
}

static int should_ignore(session_t *ps, unsigned long sequence) {
	if (ps == NULL) {
		// Do not ignore errors until the session has been initialized
		return false;
	}
	discard_ignore(ps, sequence);
	return ps->ignore_head && ps->ignore_head->sequence == sequence;
}

// === Windows ===

/**
 * Determine the event mask for a window.
 */
uint32_t determine_evmask(session_t *ps, xcb_window_t wid, win_evmode_t mode) {
	uint32_t evmask = 0;
	struct managed_win *w = NULL;

	// Check if it's a mapped frame window
	if (mode == WIN_EVMODE_FRAME ||
	    ((w = find_managed_win(ps, wid)) && w->a.map_state == XCB_MAP_STATE_VIEWABLE)) {
		evmask |= XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
		if (!ps->o.use_ewmh_active_win) {
			evmask |= XCB_EVENT_MASK_FOCUS_CHANGE;
		}
	}

	// Check if it's a mapped client window
	if (mode == WIN_EVMODE_CLIENT ||
	    ((w = find_toplevel(ps, wid)) && w->a.map_state == XCB_MAP_STATE_VIEWABLE)) {
		evmask |= XCB_EVENT_MASK_PROPERTY_CHANGE;
	}

	return evmask;
}

/**
 * Update current active window based on EWMH _NET_ACTIVE_WIN.
 *
 * Does not change anything if we fail to get the attribute or the window
 * returned could not be found.
 */
void update_ewmh_active_win(session_t *ps) {
	// Search for the window
	xcb_window_t wid =
	    wid_get_prop_window(ps->c, ps->root, ps->atoms->a_NET_ACTIVE_WINDOW);
	auto w = find_win_all(ps, wid);

	// Mark the window focused. No need to unfocus the previous one.
	if (w) {
		win_set_focused(ps, w);
	}
}

/**
 * Recheck currently focused window and set its <code>w->focused</code>
 * to true.
 *
 * @param ps current session
 * @return struct _win of currently focused window, NULL if not found
 */
static void recheck_focus(session_t *ps) {
	// Use EWMH _NET_ACTIVE_WINDOW if enabled
	if (ps->o.use_ewmh_active_win) {
		update_ewmh_active_win(ps);
		return;
	}

	// Determine the currently focused window so we can apply appropriate
	// opacity on it
	xcb_window_t wid = XCB_NONE;
	xcb_get_input_focus_reply_t *reply =
	    xcb_get_input_focus_reply(ps->c, xcb_get_input_focus(ps->c), NULL);

	if (reply) {
		wid = reply->focus;
		free(reply);
	}

	auto w = find_win_all(ps, wid);

	log_trace("%#010" PRIx32 " (%#010lx \"%s\") focused.", wid,
	          (w ? w->base.id : XCB_NONE), (w ? w->name : NULL));

	// And we set the focus state here
	if (w) {
		win_set_focused(ps, w);
		return;
	}
}

/**
 * Rebuild cached <code>screen_reg</code>.
 */
static void rebuild_screen_reg(session_t *ps) {
	get_screen_region(ps, &ps->screen_reg);
}

/**
 * Rebuild <code>shadow_exclude_reg</code>.
 */
static void rebuild_shadow_exclude_reg(session_t *ps) {
	bool ret = parse_geometry(ps, ps->o.shadow_exclude_reg_str, &ps->shadow_exclude_reg);
	if (!ret)
		exit(1);
}

/// Free up all the images and deinit the backend
static void destroy_backend(session_t *ps) {
	win_stack_foreach_managed_safe(w, &ps->window_stack) {
		// Wrapping up fading in progress
		if (win_skip_fading(ps, w)) {
			// `w` is freed by win_skip_fading
			continue;
		}

		if (ps->backend_data) {
			// Unmapped windows could still have shadow images, but not pixmap
			// images
			assert(!w->win_image || w->state != WSTATE_UNMAPPED);
			if (win_check_flags_any(w, WIN_FLAGS_IMAGES_STALE) &&
			    w->state == WSTATE_MAPPED) {
				log_warn("Stale flags set for mapped window %#010x "
				         "during backend destruction",
				         w->base.id);
				assert(false);
			}
			// Unmapped windows can still have stale flags set, because their
			// stale flags aren't handled until they are mapped.
			win_clear_flags(w, WIN_FLAGS_IMAGES_STALE);
			win_release_images(ps->backend_data, w);
		}
		free_paint(ps, &w->paint);
	}

	if (ps->backend_data && ps->root_image) {
		ps->backend_data->ops->release_image(ps->backend_data, ps->root_image);
		ps->root_image = NULL;
	}

	if (ps->backend_data) {
		// deinit backend
		if (ps->backend_blur_context) {
			ps->backend_data->ops->destroy_blur_context(
			    ps->backend_data, ps->backend_blur_context);
			ps->backend_blur_context = NULL;
		}
		ps->backend_data->ops->deinit(ps->backend_data);
		ps->backend_data = NULL;
	}
}

static bool initialize_blur(session_t *ps) {
	struct kernel_blur_args kargs;
	struct gaussian_blur_args gargs;
	struct box_blur_args bargs;
	struct dual_kawase_blur_args dkargs;

	void *args = NULL;
	switch (ps->o.blur_method) {
	case BLUR_METHOD_BOX:
		bargs.size = ps->o.blur_radius;
		args = (void *)&bargs;
		break;
	case BLUR_METHOD_KERNEL:
		kargs.kernel_count = ps->o.blur_kernel_count;
		kargs.kernels = ps->o.blur_kerns;
		args = (void *)&kargs;
		break;
	case BLUR_METHOD_GAUSSIAN:
		gargs.size = ps->o.blur_radius;
		gargs.deviation = ps->o.blur_deviation;
		args = (void *)&gargs;
		break;
	case BLUR_METHOD_DUAL_KAWASE:
		dkargs.size = ps->o.blur_radius;
		dkargs.strength = ps->o.blur_strength;
		args = (void *)&dkargs;
		break;
	default: return true;
	}

	ps->backend_blur_context = ps->backend_data->ops->create_blur_context(
	    ps->backend_data, ps->o.blur_method, args);
	return ps->backend_blur_context != NULL;
}

/// Init the backend and bind all the window pixmap to backend images
static bool initialize_backend(session_t *ps) {
	if (ps->o.experimental_backends) {
		assert(!ps->backend_data);
		// Reinitialize win_data
		assert(backend_list[ps->o.backend]);
		ps->backend_data = backend_list[ps->o.backend]->init(ps);
		if (!ps->backend_data) {
			log_fatal("Failed to initialize backend, aborting...");
			quit(ps);
			return false;
		}
		ps->backend_data->ops = backend_list[ps->o.backend];

		if (!initialize_blur(ps)) {
			log_fatal("Failed to prepare for background blur, aborting...");
			ps->backend_data->ops->deinit(ps->backend_data);
			ps->backend_data = NULL;
			quit(ps);
			return false;
		}

		// window_stack shouldn't include window that's
		// not in the hash table at this point. Since
		// there cannot be any fading windows.
		HASH_ITER2(ps->windows, _w) {
			if (!_w->managed) {
				continue;
			}
			auto w = (struct managed_win *)_w;
			assert(w->state == WSTATE_MAPPED || w->state == WSTATE_UNMAPPED);
			// We need to reacquire image
			log_debug("Marking window %#010x (%s) for update after "
			          "redirection",
			          w->base.id, w->name);
			win_set_flags(w, WIN_FLAGS_IMAGES_STALE);
			ps->pending_updates = true;
		}
	}

	// The old backends binds pixmap lazily, nothing to do here
	return true;
}

/// Handle configure event of the root window
static void configure_root(session_t *ps) {
	auto r = XCB_AWAIT(xcb_get_geometry, ps->c, ps->root);
	if (!r) {
		log_fatal("Failed to fetch root geometry");
		abort();
	}

	log_info("Root configuration changed, new geometry: %dx%d", r->width, r->height);
	bool has_root_change = false;
	if (ps->redirected) {
		// On root window changes
		if (ps->o.experimental_backends) {
			assert(ps->backend_data);
			has_root_change = ps->backend_data->ops->root_change != NULL;
		} else {
			// Old backend can handle root change
			has_root_change = true;
		}

		if (!has_root_change) {
			// deinit/reinit backend and free up resources if the backend
			// cannot handle root change
			destroy_backend(ps);
		}
		free_paint(ps, &ps->tgt_buffer);
	}

	ps->root_width = r->width;
	ps->root_height = r->height;

	rebuild_screen_reg(ps);
	rebuild_shadow_exclude_reg(ps);

	// Invalidate reg_ignore from the top
	auto top_w = win_stack_find_next_managed(ps, &ps->window_stack);
	if (top_w) {
		rc_region_unref(&top_w->reg_ignore);
		top_w->reg_ignore_valid = false;
	}

	if (ps->redirected) {
		for (int i = 0; i < ps->ndamage; i++) {
			pixman_region32_clear(&ps->damage_ring[i]);
		}
		ps->damage = ps->damage_ring + ps->ndamage - 1;
#ifdef CONFIG_OPENGL
		// GLX root change callback
		if (BKEND_GLX == ps->o.backend && !ps->o.experimental_backends) {
			glx_on_root_change(ps);
		}
#endif
		if (has_root_change) {
			if (ps->backend_data != NULL) {
				ps->backend_data->ops->root_change(ps->backend_data, ps);
			}
			// Old backend's root_change is not a specific function
		} else {
			if (!initialize_backend(ps)) {
				log_fatal("Failed to re-initialize backend after root "
				          "change, aborting...");
				ps->quit = true;
				/* TODO(yshui) only event handlers should request
				 * ev_break, otherwise it's too hard to keep track of what
				 * can break the event loop */
				ev_break(ps->loop, EVBREAK_ALL);
				return;
			}

			// Re-acquire the root pixmap.
			root_damaged(ps);
		}
		force_repaint(ps);
	}
	return;
}

static void handle_root_flags(session_t *ps) {
	if ((ps->root_flags & ROOT_FLAGS_SCREEN_CHANGE) != 0) {
		if (ps->o.xinerama_shadow_crop) {
			cxinerama_upd_scrs(ps);
		}

		if (ps->o.sw_opti && !ps->o.refresh_rate) {
			update_refresh_rate(ps);
			if (!ps->refresh_rate) {
				log_warn("Refresh rate detection failed. swopti will be "
				         "temporarily disabled");
			}
		}
		ps->root_flags &= ~(uint64_t)ROOT_FLAGS_SCREEN_CHANGE;
	}

	if ((ps->root_flags & ROOT_FLAGS_CONFIGURED) != 0) {
		configure_root(ps);
		ps->root_flags &= ~(uint64_t)ROOT_FLAGS_CONFIGURED;
	}
}

static struct managed_win *paint_preprocess(session_t *ps, bool *fade_running) {
	// XXX need better, more general name for `fade_running`. It really
	// means if fade is still ongoing after the current frame is rendered
	struct managed_win *bottom = NULL;
	*fade_running = false;

	// Fading step calculation
	long long steps = 0L;
	auto now = get_time_ms();
	if (ps->fade_time) {
		assert(now >= ps->fade_time);
		steps = (now - ps->fade_time) / ps->o.fade_delta;
	} else {
		// Reset fade_time if unset
		ps->fade_time = get_time_ms();
		steps = 0L;
	}
	ps->fade_time += steps * ps->o.fade_delta;

	// First, let's process fading
	win_stack_foreach_managed_safe(w, &ps->window_stack) {
		const winmode_t mode_old = w->mode;
		const bool was_painted = w->to_paint;
		const double opacity_old = w->opacity;

		if (win_should_dim(ps, w) != w->dim) {
			w->dim = win_should_dim(ps, w);
			add_damage_from_win(ps, w);
		}

		// Run fading
		if (run_fade(ps, &w, steps)) {
			*fade_running = true;
		}

		// Add window to damaged area if its opacity changes
		// If was_painted == false, and to_paint is also false, we don't care
		// If was_painted == false, but to_paint is true, damage will be added in
		// the loop below
		if (was_painted && w->opacity != opacity_old) {
			add_damage_from_win(ps, w);
		}

		if (win_check_fade_finished(ps, w)) {
			// the window has been destroyed because fading finished
			continue;
		}

		if (win_has_frame(w)) {
			w->frame_opacity = ps->o.frame_opacity;
		} else {
			w->frame_opacity = 1.0;
		}

		// Update window mode
		w->mode = win_calc_mode(w);

		// Destroy all reg_ignore above when frame opaque state changes on
		// SOLID mode
		if (was_painted && w->mode != mode_old) {
			w->reg_ignore_valid = false;
		}

		// Transition
		if(w->transition_time >= 0.0f && w->transition_time <= 1.0f) {
			// TODO: use refresh rate instead of using static 60 fps calculation
            w->transition_time += (1.0f / 60.0f) / ps->o.transition_duration; 
            double transition = ps->o.transition_timing_function(w->transition_time);
            if(w->transition_time > 1.0f)
                transition = 1.0f;

			add_damage_from_win(ps, w);
			unsigned int direction = w->transition_direction;

			// Smart direction
			if(direction > TRANSITIONDIR_TOP) {
				bool wide_enough = w->g.width > 80 * ps->root_width / 100;
				bool bigger_than_half = w->target_geometry.x > ps->root_width / 2;

				/*
					Not changing transition_direction cause 
				   	window geometry may change between transitions
				*/ 
				direction = w->transition_direction - (bigger_than_half || wide_enough ? 4 : 2);
			}

			int8_t xy = direction % 2;
			int16_t  xy_target = *( ((int16_t*) &w->target_geometry) + xy );
			int16_t* xy_source = ((int16_t*) &w->g) + xy;

			int start_location = xy_target + 
				(direction % 3 == 0? -1: 1) * ps->o.transition_offset;

            *xy_source = (int16_t) round(
                transition * (xy_target - start_location) +
                start_location
            );

            w->mode = WMODE_TRANS;
            *fade_running = true;
        }
	}

	// Opacity will not change, from now on.
	rc_region_t *last_reg_ignore = rc_region_new();

	bool unredir_possible = false;
	// Track whether it's the highest window to paint
	bool is_highest = true;
	bool reg_ignore_valid = true;
	win_stack_foreach_managed(w, &ps->window_stack) {
		__label__ skip_window;
		bool to_paint = true;
		// w->to_paint remembers whether this window is painted last time
		const bool was_painted = w->to_paint;

		// Destroy reg_ignore if some window above us invalidated it
		if (!reg_ignore_valid) {
			rc_region_unref(&w->reg_ignore);
		}

		// log_trace("%d %d %s", w->a.map_state, w->ever_damaged, w->name);

		// Give up if it's not damaged or invisible, or it's unmapped and its
		// pixmap is gone (for example due to a ConfigureNotify), or when it's
		// excluded
		if (w->state == WSTATE_UNMAPPED ||
		    unlikely(w->base.id == ps->debug_window ||
		             w->client_win == ps->debug_window)) {
			to_paint = false;
		} else if (!w->ever_damaged && w->state != WSTATE_UNMAPPING &&
		           w->state != WSTATE_DESTROYING) {
			// Unmapping clears w->ever_damaged, but the fact that the window
			// is fading out means it must have been damaged when it was still
			// mapped (because unmap_win_start will skip fading if it wasn't),
			// so we still need to paint it.
			log_trace("Window %#010x (%s) will not be painted because it has "
			          "not received any damages",
			          w->base.id, w->name);
			to_paint = false;
		} else if (unlikely(w->g.x + w->g.width < 1 || w->g.y + w->g.height < 1 ||
		                    w->g.x >= ps->root_width || w->g.y >= ps->root_height)) {
			log_trace("Window %#010x (%s) will not be painted because it is "
			          "positioned outside of the screen",
			          w->base.id, w->name);
			to_paint = false;
		} else if (unlikely((double)w->opacity * MAX_ALPHA < 1 && !w->blur_background)) {
			/* TODO(yshui) for consistency, even a window has 0 opacity, we
			 * still probably need to blur its background, so to_paint
			 * shouldn't be false for them. */
			log_trace("Window %#010x (%s) will not be painted because it has "
			          "0 opacity",
			          w->base.id, w->name);
			to_paint = false;
		} else if (w->paint_excluded) {
			log_trace("Window %#010x (%s) will not be painted because it is "
			          "excluded from painting",
			          w->base.id, w->name);
			to_paint = false;
		} else if (unlikely((w->flags & WIN_FLAGS_IMAGE_ERROR) != 0)) {
			log_trace("Window %#010x (%s) will not be painted because it has "
			          "image errors",
			          w->base.id, w->name);
			to_paint = false;
		}
		// log_trace("%s %d %d %d", w->name, to_paint, w->opacity,
		// w->paint_excluded);

		// Add window to damaged area if its painting status changes
		// or opacity changes
		if (to_paint != was_painted) {
			w->reg_ignore_valid = false;
			add_damage_from_win(ps, w);
		}

		// to_paint will never change after this point
		if (!to_paint) {
			goto skip_window;
		}

		log_trace("Window %#010x (%s) will be painted", w->base.id, w->name);

		// Calculate shadow opacity
		w->shadow_opacity = ps->o.shadow_opacity * w->opacity * ps->o.frame_opacity;

		// Generate ignore region for painting to reduce GPU load
		if (!w->reg_ignore) {
			w->reg_ignore = rc_region_ref(last_reg_ignore);
		}

		// If the window is solid, or we enabled clipping for transparent windows,
		// we add the window region to the ignored region
		// Otherwise last_reg_ignore shouldn't change
		if ((w->mode != WMODE_TRANS && !ps->o.force_win_blend) ||
		    ps->o.transparent_clipping) {
			// w->mode == WMODE_SOLID or WMODE_FRAME_TRANS
			region_t *tmp = rc_region_new();
			if (w->mode == WMODE_SOLID) {
				*tmp =
				    win_get_bounding_shape_global_without_corners_by_val(w);
			} else {
				// w->mode == WMODE_FRAME_TRANS
				win_get_region_noframe_local_without_corners(w, tmp);
				pixman_region32_intersect(tmp, tmp, &w->bounding_shape);
				pixman_region32_translate(tmp, w->g.x, w->g.y);
			}

			pixman_region32_union(tmp, tmp, last_reg_ignore);
			rc_region_unref(&last_reg_ignore);
			last_reg_ignore = tmp;
		}

		// (Un)redirect screen
		// We could definitely unredirect the screen when there's no window to
		// paint, but this is typically unnecessary, may cause flickering when
		// fading is enabled, and could create inconsistency when the wallpaper
		// is not correctly set.
		if (ps->o.unredir_if_possible && is_highest) {
			if (w->mode == WMODE_SOLID && !ps->o.force_win_blend &&
			    win_is_fullscreen(ps, w) && !w->unredir_if_possible_excluded) {
				unredir_possible = true;
			}
		}

		// Unredirect screen if some window is requesting compositor bypass, even
		// if that window is not on the top.
		if (ps->o.unredir_if_possible && win_is_bypassing_compositor(ps, w) &&
		    !w->unredir_if_possible_excluded) {
			// Here we deviate from EWMH a bit. EWMH says we must not
			// unredirect the screen if the window requesting bypassing would
			// look different after unredirecting. Instead we always follow
			// the request.
			unredir_possible = true;
		}

		w->prev_trans = bottom;
		if (bottom) {
			w->stacking_rank = bottom->stacking_rank + 1;
		} else {
			w->stacking_rank = 0;
		}
		bottom = w;

		// If the screen is not redirected and the window has redir_ignore set,
		// this window should not cause the screen to become redirected
		if (!(ps->o.wintype_option[w->window_type].redir_ignore && !ps->redirected)) {
			is_highest = false;
		}

	skip_window:
		reg_ignore_valid = reg_ignore_valid && w->reg_ignore_valid;
		w->reg_ignore_valid = true;

		// Avoid setting w->to_paint if w is freed
		if (w) {
			w->to_paint = to_paint;
		}
	}

	rc_region_unref(&last_reg_ignore);

	// If possible, unredirect all windows and stop painting
	if (ps->o.redirected_force != UNSET) {
		unredir_possible = !ps->o.redirected_force;
	} else if (ps->o.unredir_if_possible && is_highest && !ps->redirected) {
		// If there's no window to paint, and the screen isn't redirected,
		// don't redirect it.
		unredir_possible = true;
	}
	if (unredir_possible) {
		if (ps->redirected) {
			if (!ps->o.unredir_if_possible_delay || ps->tmout_unredir_hit) {
				unredirect(ps);
			} else if (!ev_is_active(&ps->unredir_timer)) {
				ev_timer_set(
				    &ps->unredir_timer,
				    (double)ps->o.unredir_if_possible_delay / 1000.0, 0);
				ev_timer_start(ps->loop, &ps->unredir_timer);
			}
		}
	} else {
		ev_timer_stop(ps->loop, &ps->unredir_timer);
		if (!ps->redirected) {
			if (!redirect_start(ps)) {
				return NULL;
			}
		}
	}

	return bottom;
}

void root_damaged(session_t *ps) {
	if (ps->root_tile_paint.pixmap) {
		free_root_tile(ps);
	}

	if (!ps->redirected) {
		return;
	}

	if (ps->backend_data) {
		if (ps->root_image) {
			ps->backend_data->ops->release_image(ps->backend_data, ps->root_image);
		}
		auto pixmap = x_get_root_back_pixmap(ps->c, ps->root, ps->atoms);
		if (pixmap != XCB_NONE) {
			ps->root_image = ps->backend_data->ops->bind_pixmap(
			    ps->backend_data, pixmap, x_get_visual_info(ps->c, ps->vis), false);
			ps->backend_data->ops->set_image_property(
			    ps->backend_data, IMAGE_PROPERTY_EFFECTIVE_SIZE,
			    ps->root_image, (int[]){ps->root_width, ps->root_height});
		}
	}

	// Mark screen damaged
	force_repaint(ps);
}

/**
 * Xlib error handler function.
 */
static int xerror(Display attr_unused *dpy, XErrorEvent *ev) {
	if (!should_ignore(ps_g, ev->serial)) {
		x_print_error(ev->serial, ev->request_code, ev->minor_code, ev->error_code);
	}
	return 0;
}

/**
 * XCB error handler function.
 */
void ev_xcb_error(session_t *ps, xcb_generic_error_t *err) {
	if (!should_ignore(ps, err->sequence)) {
		x_print_error(err->sequence, err->major_code, err->minor_code, err->error_code);
	}
}

/**
 * Force a full-screen repaint.
 */
void force_repaint(session_t *ps) {
	assert(pixman_region32_not_empty(&ps->screen_reg));
	queue_redraw(ps);
	add_damage(ps, &ps->screen_reg);
}

#ifdef CONFIG_DBUS
/** @name DBus hooks
 */
///@{

/**
 * Set no_fading_openclose option.
 *
 * Don't affect fading already in progress
 */
void opts_set_no_fading_openclose(session_t *ps, bool newval) {
	ps->o.no_fading_openclose = newval;
}

//!@}
#endif

/**
 * Setup window properties, then register us with the compositor selection (_NET_WM_CM_S)
 *
 * @return 0 if success, 1 if compositor already running, -1 if error.
 */
static int register_cm(session_t *ps) {
	assert(!ps->reg_win);

	ps->reg_win = x_new_id(ps->c);
	auto e = xcb_request_check(
	    ps->c, xcb_create_window_checked(ps->c, XCB_COPY_FROM_PARENT, ps->reg_win, ps->root,
	                                     0, 0, 1, 1, 0, XCB_NONE, ps->vis, 0, NULL));

	if (e) {
		log_fatal("Failed to create window.");
		free(e);
		return -1;
	}

	const xcb_atom_t prop_atoms[] = {
	    ps->atoms->aWM_NAME,
	    ps->atoms->a_NET_WM_NAME,
	    ps->atoms->aWM_ICON_NAME,
	};

	const bool prop_is_utf8[] = {false, true, false};

	// Set names and classes
	for (size_t i = 0; i < ARR_SIZE(prop_atoms); i++) {
		e = xcb_request_check(
		    ps->c, xcb_change_property_checked(
		               ps->c, XCB_PROP_MODE_REPLACE, ps->reg_win, prop_atoms[i],
		               prop_is_utf8[i] ? ps->atoms->aUTF8_STRING : XCB_ATOM_STRING,
		               8, strlen("picom"), "picom"));
		if (e) {
			log_error_x_error(e, "Failed to set window property %d",
			                  prop_atoms[i]);
			free(e);
		}
	}

	const char picom_class[] = "picom\0picom";
	e = xcb_request_check(
	    ps->c, xcb_change_property_checked(ps->c, XCB_PROP_MODE_REPLACE, ps->reg_win,
	                                       ps->atoms->aWM_CLASS, XCB_ATOM_STRING, 8,
	                                       ARR_SIZE(picom_class), picom_class));
	if (e) {
		log_error_x_error(e, "Failed to set the WM_CLASS property");
		free(e);
	}

	// Set WM_CLIENT_MACHINE. As per EWMH, because we set _NET_WM_PID, we must also
	// set WM_CLIENT_MACHINE.
	{
		const auto hostname_max = (unsigned long)sysconf(_SC_HOST_NAME_MAX);
		char *hostname = malloc(hostname_max);

		if (gethostname(hostname, hostname_max) == 0) {
			e = xcb_request_check(
			    ps->c, xcb_change_property_checked(
			               ps->c, XCB_PROP_MODE_REPLACE, ps->reg_win,
			               ps->atoms->aWM_CLIENT_MACHINE, XCB_ATOM_STRING, 8,
			               (uint32_t)strlen(hostname), hostname));
			if (e) {
				log_error_x_error(e, "Failed to set the WM_CLIENT_MACHINE"
				                     " property");
				free(e);
			}
		} else {
			log_error_errno("Failed to get hostname");
		}

		free(hostname);
	}

	// Set _NET_WM_PID
	{
		auto pid = getpid();
		xcb_change_property(ps->c, XCB_PROP_MODE_REPLACE, ps->reg_win,
		                    ps->atoms->a_NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);
	}

	// Set COMPTON_VERSION
	e = xcb_request_check(
	    ps->c, xcb_change_property_checked(
	               ps->c, XCB_PROP_MODE_REPLACE, ps->reg_win,
	               get_atom(ps->atoms, "COMPTON_VERSION"), XCB_ATOM_STRING, 8,
	               (uint32_t)strlen(COMPTON_VERSION), COMPTON_VERSION));
	if (e) {
		log_error_x_error(e, "Failed to set COMPTON_VERSION.");
		free(e);
	}

	// Acquire X Selection _NET_WM_CM_S?
	if (!ps->o.no_x_selection) {
		const char register_prop[] = "_NET_WM_CM_S";
		xcb_atom_t atom;

		char *buf = NULL;
		if (asprintf(&buf, "%s%d", register_prop, ps->scr) < 0) {
			log_fatal("Failed to allocate memory");
			return -1;
		}
		atom = get_atom(ps->atoms, buf);
		free(buf);

		xcb_get_selection_owner_reply_t *reply = xcb_get_selection_owner_reply(
		    ps->c, xcb_get_selection_owner(ps->c, atom), NULL);

		if (reply && reply->owner != XCB_NONE) {
			// Another compositor already running
			free(reply);
			return 1;
		}
		free(reply);
		xcb_set_selection_owner(ps->c, ps->reg_win, atom, 0);
	}

	return 0;
}

/**
 * Write PID to a file.
 */
static inline bool write_pid(session_t *ps) {
	if (!ps->o.write_pid_path) {
		return true;
	}

	FILE *f = fopen(ps->o.write_pid_path, "w");
	if (unlikely(!f)) {
		log_error("Failed to write PID to \"%s\".", ps->o.write_pid_path);
		return false;
	}

	fprintf(f, "%ld\n", (long)getpid());
	fclose(f);

	return true;
}

/**
 * Update refresh rate info with X Randr extension.
 */
void update_refresh_rate(session_t *ps) {
	xcb_randr_get_screen_info_reply_t *randr_info = xcb_randr_get_screen_info_reply(
	    ps->c, xcb_randr_get_screen_info(ps->c, ps->root), NULL);

	if (!randr_info)
		return;
	ps->refresh_rate = randr_info->rate;
	free(randr_info);

	if (ps->refresh_rate)
		ps->refresh_intv = US_PER_SEC / ps->refresh_rate;
	else
		ps->refresh_intv = 0;
}

/**
 * Initialize refresh-rated based software optimization.
 *
 * @return true for success, false otherwise
 */
static bool swopti_init(session_t *ps) {
	log_warn("--sw-opti is going to be deprecated. If you get real benefits from "
	         "using "
	         "this option, please open an issue to let us know.");
	// Prepare refresh rate
	// Check if user provides one
	ps->refresh_rate = ps->o.refresh_rate;
	if (ps->refresh_rate)
		ps->refresh_intv = US_PER_SEC / ps->refresh_rate;

	// Auto-detect refresh rate otherwise
	if (!ps->refresh_rate && ps->randr_exists) {
		update_refresh_rate(ps);
	}

	// Turn off vsync_sw if we can't get the refresh rate
	if (!ps->refresh_rate)
		return false;

	return true;
}

/**
 * Modify a struct timeval timeout value to render at a fixed pace.
 *
 * @param ps current session
 * @param[in,out] ptv pointer to the timeout
 */
static double swopti_handle_timeout(session_t *ps) {
	if (!ps->refresh_intv)
		return 0;

	// Get the microsecond offset of the time when the we reach the timeout
	// I don't think a 32-bit long could overflow here.
	long offset = (get_time_timeval().tv_usec - ps->paint_tm_offset) % ps->refresh_intv;
	// XXX this formula dones't work if refresh rate is not a whole number
	if (offset < 0)
		offset += ps->refresh_intv;

	// If the target time is sufficiently close to a refresh time, don't add
	// an offset, to avoid certain blocking conditions.
	if (offset < SWOPTI_TOLERANCE || offset > ps->refresh_intv - SWOPTI_TOLERANCE)
		return 0;

	// Add an offset so we wait until the next refresh after timeout
	return (double)(ps->refresh_intv - offset) / 1e6;
}

/**
 * Initialize X composite overlay window.
 */
static bool init_overlay(session_t *ps) {
	xcb_composite_get_overlay_window_reply_t *reply =
	    xcb_composite_get_overlay_window_reply(
	        ps->c, xcb_composite_get_overlay_window(ps->c, ps->root), NULL);
	if (reply) {
		ps->overlay = reply->overlay_win;
		free(reply);
	} else {
		ps->overlay = XCB_NONE;
	}
	if (ps->overlay != XCB_NONE) {
		// Set window region of the overlay window, code stolen from
		// compiz-0.8.8
		if (!XCB_AWAIT_VOID(xcb_shape_mask, ps->c, XCB_SHAPE_SO_SET,
		                    XCB_SHAPE_SK_BOUNDING, ps->overlay, 0, 0, 0)) {
			log_fatal("Failed to set the bounding shape of overlay, giving "
			          "up.");
			return false;
		}
		if (!XCB_AWAIT_VOID(xcb_shape_rectangles, ps->c, XCB_SHAPE_SO_SET,
		                    XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
		                    ps->overlay, 0, 0, 0, NULL)) {
			log_fatal("Failed to set the input shape of overlay, giving up.");
			return false;
		}

		// Listen to Expose events on the overlay
		xcb_change_window_attributes(ps->c, ps->overlay, XCB_CW_EVENT_MASK,
		                             (const uint32_t[]){XCB_EVENT_MASK_EXPOSURE});

		// Retrieve DamageNotify on root window if we are painting on an
		// overlay
		// root_damage = XDamageCreate(ps->dpy, root, XDamageReportNonEmpty);

		// Unmap the overlay, we will map it when needed in redirect_start
		XCB_AWAIT_VOID(xcb_unmap_window, ps->c, ps->overlay);
	} else {
		log_error("Cannot get X Composite overlay window. Falling "
		          "back to painting on root window.");
	}
	log_debug("overlay = %#010x", ps->overlay);

	return true;
}

static bool init_debug_window(session_t *ps) {
	xcb_colormap_t colormap = x_new_id(ps->c);
	ps->debug_window = x_new_id(ps->c);

	auto err = xcb_request_check(
	    ps->c, xcb_create_colormap_checked(ps->c, XCB_COLORMAP_ALLOC_NONE, colormap,
	                                       ps->root, ps->vis));
	if (err) {
		goto err_out;
	}

	err = xcb_request_check(
	    ps->c, xcb_create_window_checked(ps->c, (uint8_t)ps->depth, ps->debug_window,
	                                     ps->root, 0, 0, to_u16_checked(ps->root_width),
	                                     to_u16_checked(ps->root_height), 0,
	                                     XCB_WINDOW_CLASS_INPUT_OUTPUT, ps->vis,
	                                     XCB_CW_COLORMAP, (uint32_t[]){colormap, 0}));
	if (err) {
		goto err_out;
	}

	err = xcb_request_check(ps->c, xcb_map_window(ps->c, ps->debug_window));
	if (err) {
		goto err_out;
	}
	return true;

err_out:
	free(err);
	return false;
}

xcb_window_t session_get_target_window(session_t *ps) {
	if (ps->o.debug_mode) {
		return ps->debug_window;
	}
	return ps->overlay != XCB_NONE ? ps->overlay : ps->root;
}

uint8_t session_redirection_mode(session_t *ps) {
	if (ps->o.debug_mode) {
		// If the backend is not rendering to the screen, we don't need to
		// take over the screen.
		assert(ps->o.experimental_backends);
		return XCB_COMPOSITE_REDIRECT_AUTOMATIC;
	}
	if (ps->o.experimental_backends && !backend_list[ps->o.backend]->present) {
		// if the backend doesn't render anything, we don't need to take over the
		// screen.
		return XCB_COMPOSITE_REDIRECT_AUTOMATIC;
	}
	return XCB_COMPOSITE_REDIRECT_MANUAL;
}

/**
 * Redirect all windows.
 *
 * @return whether the operation succeeded or not
 */
static bool redirect_start(session_t *ps) {
	assert(!ps->redirected);
	log_debug("Redirecting the screen.");

	// Map overlay window. Done firstly according to this:
	// https://bugzilla.gnome.org/show_bug.cgi?id=597014
	if (ps->overlay != XCB_NONE) {
		xcb_map_window(ps->c, ps->overlay);
	}

	bool success = XCB_AWAIT_VOID(xcb_composite_redirect_subwindows, ps->c, ps->root,
	                              session_redirection_mode(ps));
	if (!success) {
		log_fatal("Another composite manager is already running "
		          "(and does not handle _NET_WM_CM_Sn correctly)");
		return false;
	}

	x_sync(ps->c);

	if (!initialize_backend(ps)) {
		return false;
	}

	if (ps->o.experimental_backends) {
		assert(ps->backend_data);
		ps->ndamage = ps->backend_data->ops->max_buffer_age;
	} else {
		ps->ndamage = maximum_buffer_age(ps);
	}
	ps->damage_ring = ccalloc(ps->ndamage, region_t);
	ps->damage = ps->damage_ring + ps->ndamage - 1;

	for (int i = 0; i < ps->ndamage; i++) {
		pixman_region32_init(&ps->damage_ring[i]);
	}

	// Must call XSync() here
	x_sync(ps->c);

	ps->redirected = true;
	ps->first_frame = true;

	// Re-detect driver since we now have a backend
	ps->drivers = detect_driver(ps->c, ps->backend_data, ps->root);
	apply_driver_workarounds(ps, ps->drivers);

	root_damaged(ps);

	// Repaint the whole screen
	force_repaint(ps);
	log_debug("Screen redirected.");
	return true;
}

/**
 * Unredirect all windows.
 */
static void unredirect(session_t *ps) {
	assert(ps->redirected);
	log_debug("Unredirecting the screen.");

	destroy_backend(ps);

	xcb_composite_unredirect_subwindows(ps->c, ps->root, session_redirection_mode(ps));
	// Unmap overlay window
	if (ps->overlay != XCB_NONE) {
		xcb_unmap_window(ps->c, ps->overlay);
	}

	// Free the damage ring
	for (int i = 0; i < ps->ndamage; ++i) {
		pixman_region32_fini(&ps->damage_ring[i]);
	}
	ps->ndamage = 0;
	free(ps->damage_ring);
	ps->damage_ring = ps->damage = NULL;

	// Must call XSync() here
	x_sync(ps->c);

	ps->redirected = false;
	log_debug("Screen unredirected.");
}

// Handle queued events before we go to sleep
static void handle_queued_x_events(EV_P attr_unused, ev_prepare *w, int revents attr_unused) {
	session_t *ps = session_ptr(w, event_check);
	xcb_generic_event_t *ev;
	while ((ev = xcb_poll_for_queued_event(ps->c))) {
		ev_handle(ps, ev);
		free(ev);
	};
	// Flush because if we go into sleep when there is still
	// requests in the outgoing buffer, they will not be sent
	// for an indefinite amount of time.
	// Use XFlush here too, we might still use some Xlib functions
	// because OpenGL.
	XFlush(ps->dpy);
	xcb_flush(ps->c);
	int err = xcb_connection_has_error(ps->c);
	if (err) {
		log_fatal("X11 server connection broke (error %d)", err);
		exit(1);
	}
}

static void handle_new_windows(session_t *ps) {
	list_foreach_safe(struct win, w, &ps->window_stack, stack_neighbour) {
		if (w->is_new) {
			auto new_w = fill_win(ps, w);
			if (!new_w->managed) {
				continue;
			}
			auto mw = (struct managed_win *)new_w;
			if (mw->a.map_state == XCB_MAP_STATE_VIEWABLE) {
				win_set_flags(mw, WIN_FLAGS_MAPPED);

				// This window might be damaged before we called fill_win
				// and created the damage handle. And there is no way for
				// us to find out. So just blindly mark it damaged
				mw->ever_damaged = true;
			}
		}
	}
}

static void refresh_windows(session_t *ps) {
	win_stack_foreach_managed(w, &ps->window_stack) {
		win_process_update_flags(ps, w);
	}
}

static void refresh_images(session_t *ps) {
	win_stack_foreach_managed(w, &ps->window_stack) {
		win_process_image_flags(ps, w);
	}
}

/**
 * Unredirection timeout callback.
 */
static void tmout_unredir_callback(EV_P attr_unused, ev_timer *w, int revents attr_unused) {
	session_t *ps = session_ptr(w, unredir_timer);
	ps->tmout_unredir_hit = true;
	queue_redraw(ps);
}

static void fade_timer_callback(EV_P attr_unused, ev_timer *w, int revents attr_unused) {
	session_t *ps = session_ptr(w, fade_timer);
	queue_redraw(ps);
}

static void handle_pending_updates(EV_P_ struct session *ps) {
	if (ps->pending_updates) {
		log_debug("Delayed handling of events, entering critical section");
		auto e = xcb_request_check(ps->c, xcb_grab_server_checked(ps->c));
		if (e) {
			log_fatal_x_error(e, "failed to grab x server");
			return quit(ps);
		}

		ps->server_grabbed = true;

		// Catching up with X server
		handle_queued_x_events(EV_A_ & ps->event_check, 0);

		// Call fill_win on new windows
		handle_new_windows(ps);

		// Handle screen changes
		// This HAS TO be called before refresh_windows, as handle_root_flags
		// could call configure_root, which will release images and mark them
		// stale.
		handle_root_flags(ps);

		// Process window flags (window mapping)
		refresh_windows(ps);

		{
			auto r = xcb_get_input_focus_reply(
			    ps->c, xcb_get_input_focus(ps->c), NULL);
			if (!ps->active_win || (r && r->focus != ps->active_win->base.id)) {
				recheck_focus(ps);
			}
			free(r);
		}

		// Process window flags (stale images)
		refresh_images(ps);

		e = xcb_request_check(ps->c, xcb_ungrab_server_checked(ps->c));
		if (e) {
			log_fatal_x_error(e, "failed to ungrab x server");
			return quit(ps);
		}

		ps->server_grabbed = false;
		ps->pending_updates = false;
		log_debug("Exited critical section");
	}
}

static void draw_callback_impl(EV_P_ session_t *ps, int revents attr_unused) {
	handle_pending_updates(EV_A_ ps);

	if (ps->first_frame) {
		// If we are still rendering the first frame, if some of the windows are
		// unmapped/destroyed during the above handle_pending_updates() call, they
		// won't have pixmap before we rendered it, causing us to crash.
		// But we will only render them if they are in fading. So we just skip
		// fading for all windows here.
		//
		// Using foreach_safe here since skipping fading can cause window to be
		// freed if it's destroyed.
		win_stack_foreach_managed_safe(w, &ps->window_stack) {
			auto _ attr_unused = win_skip_fading(ps, w);
		}
	}

	if (ps->o.benchmark) {
		if (ps->o.benchmark_wid) {
			auto w = find_managed_win(ps, ps->o.benchmark_wid);
			if (!w) {
				log_fatal("Couldn't find specified benchmark window.");
				exit(1);
			}
			add_damage_from_win(ps, w);
		} else {
			force_repaint(ps);
		}
	}

	/* TODO(yshui) Have a stripped down version of paint_preprocess that is used when
	 * screen is not redirected. its sole purpose should be to decide whether the
	 * screen should be redirected. */
	bool fade_running = false;
	bool was_redirected = ps->redirected;
	auto bottom = paint_preprocess(ps, &fade_running);
	ps->tmout_unredir_hit = false;

	if (!was_redirected && ps->redirected) {
		// paint_preprocess redirected the screen, which might change the state of
		// some of the windows (e.g. the window image might become stale).
		// so we rerun _draw_callback to make sure the rendering decision we make
		// is up-to-date, and all the new flags got handled.
		//
		// TODO(yshui) This is not ideal, we should try to avoid setting window
		// flags in paint_preprocess.
		log_debug("Re-run _draw_callback");
		return draw_callback_impl(EV_A_ ps, revents);
	}

	// Start/stop fade timer depends on whether window are fading
	if (!fade_running && ev_is_active(&ps->fade_timer)) {
		ev_timer_stop(EV_A_ & ps->fade_timer);
	} else if (fade_running && !ev_is_active(&ps->fade_timer)) {
		ev_timer_set(&ps->fade_timer, fade_timeout(ps), 0);
		ev_timer_start(EV_A_ & ps->fade_timer);
	}

	// If the screen is unredirected, free all_damage to stop painting
	if (ps->redirected && ps->o.stoppaint_force != ON) {
		static int paint = 0;

		log_trace("Render start, frame %d", paint);
		if (ps->o.experimental_backends) {
			paint_all_new(ps, bottom, false);
		} else {
			paint_all(ps, bottom, false);
		}
		log_trace("Render end");

		ps->first_frame = false;
		paint++;
		if (ps->o.benchmark && paint >= ps->o.benchmark) {
			exit(0);
		}
	}

	if (!fade_running) {
		ps->fade_time = 0L;
	}

	// TODO(yshui) Investigate how big the X critical section needs to be. There are
	// suggestions that rendering should be in the critical section as well.

	ps->redraw_needed = false;
}

static void draw_callback(EV_P_ ev_idle *w, int revents) {
	// This function is not used if we are using --swopti
	session_t *ps = session_ptr(w, draw_idle);

	draw_callback_impl(EV_A_ ps, revents);

	// Don't do painting non-stop unless we are in benchmark mode
	if (!ps->o.benchmark) {
		ev_idle_stop(EV_A_ & ps->draw_idle);
	}
}

static void delayed_draw_timer_callback(EV_P_ ev_timer *w, int revents) {
	session_t *ps = session_ptr(w, delayed_draw_timer);
	draw_callback_impl(EV_A_ ps, revents);

	// We might have stopped the ev_idle in delayed_draw_callback,
	// so we restart it if we are in benchmark mode
	if (ps->o.benchmark)
		ev_idle_start(EV_A_ & ps->draw_idle);
}

static void delayed_draw_callback(EV_P_ ev_idle *w, int revents) {
	// This function is only used if we are using --swopti
	session_t *ps = session_ptr(w, draw_idle);
	assert(ps->redraw_needed);
	assert(!ev_is_active(&ps->delayed_draw_timer));

	double delay = swopti_handle_timeout(ps);
	if (delay < 1e-6) {
		if (!ps->o.benchmark) {
			ev_idle_stop(EV_A_ & ps->draw_idle);
		}
		return draw_callback_impl(EV_A_ ps, revents);
	}

	// This is a little bit hacky. When we get to this point in code, we need
	// to update the screen , but we will only be updating after a delay, So
	// we want to stop the ev_idle, so this callback doesn't get call repeatedly
	// during the delay, we also want queue_redraw to not restart the ev_idle.
	// So we stop ev_idle and leave ps->redraw_needed to be true. (effectively,
	// ps->redraw_needed means if redraw is needed or if draw is in progress).
	//
	// We do this anyway even if we are in benchmark mode. That means we will
	// have to restart draw_idle after the draw actually happened when we are in
	// benchmark mode.
	ev_idle_stop(EV_A_ & ps->draw_idle);

	ev_timer_set(&ps->delayed_draw_timer, delay, 0);
	ev_timer_start(EV_A_ & ps->delayed_draw_timer);
}

static void x_event_callback(EV_P attr_unused, ev_io *w, int revents attr_unused) {
	session_t *ps = (session_t *)w;
	xcb_generic_event_t *ev = xcb_poll_for_event(ps->c);
	if (ev) {
		ev_handle(ps, ev);
		free(ev);
	}
}

/**
 * Turn on the program reset flag.
 *
 * This will result in the compostior resetting itself after next paint.
 */
static void reset_enable(EV_P_ ev_signal *w attr_unused, int revents attr_unused) {
	log_info("picom is resetting...");
	ev_break(EV_A_ EVBREAK_ALL);
}

static void exit_enable(EV_P attr_unused, ev_signal *w, int revents attr_unused) {
	session_t *ps = session_ptr(w, int_signal);
	log_info("picom is quitting...");
	quit(ps);
}

static void config_file_change_cb(void *_ps) {
	auto ps = (struct session *)_ps;
	reset_enable(ps->loop, NULL, 0);
}

/**
 * Initialize a session.
 *
 * @param argc number of commandline arguments
 * @param argv commandline arguments
 * @param dpy  the X Display
 * @param config_file the path to the config file
 * @param all_xerros whether we should report all X errors
 * @param fork whether we will fork after initialization
 */
static session_t *session_init(int argc, char **argv, Display *dpy,
                               const char *config_file, bool all_xerrors, bool fork) {
	static const session_t s_def = {
	    .backend_data = NULL,
	    .dpy = NULL,
	    .scr = 0,
	    .c = NULL,
	    .vis = 0,
	    .depth = 0,
	    .root = XCB_NONE,
	    .root_height = 0,
	    .root_width = 0,
	    // .root_damage = XCB_NONE,
	    .overlay = XCB_NONE,
	    .root_tile_fill = false,
	    .root_tile_paint = PAINT_INIT,
	    .tgt_picture = XCB_NONE,
	    .tgt_buffer = PAINT_INIT,
	    .reg_win = XCB_NONE,
#ifdef CONFIG_OPENGL
	    .glx_prog_win = GLX_PROG_MAIN_INIT,
#endif
	    .redirected = false,
	    .alpha_picts = NULL,
	    .fade_time = 0L,
	    .ignore_head = NULL,
	    .ignore_tail = NULL,
	    .quit = false,

	    .expose_rects = NULL,
	    .size_expose = 0,
	    .n_expose = 0,

	    .windows = NULL,
	    .active_win = NULL,
	    .active_leader = XCB_NONE,

	    .black_picture = XCB_NONE,
	    .cshadow_picture = XCB_NONE,
	    .white_picture = XCB_NONE,
	    .gaussian_map = NULL,

	    .refresh_rate = 0,
	    .refresh_intv = 0UL,
	    .paint_tm_offset = 0L,

#ifdef CONFIG_VSYNC_DRM
	    .drm_fd = -1,
#endif

	    .xfixes_event = 0,
	    .xfixes_error = 0,
	    .damage_event = 0,
	    .damage_error = 0,
	    .render_event = 0,
	    .render_error = 0,
	    .composite_event = 0,
	    .composite_error = 0,
	    .composite_opcode = 0,
	    .shape_exists = false,
	    .shape_event = 0,
	    .shape_error = 0,
	    .randr_exists = 0,
	    .randr_event = 0,
	    .randr_error = 0,
	    .glx_exists = false,
	    .glx_event = 0,
	    .glx_error = 0,
	    .xrfilter_convolution_exists = false,

	    .atoms_wintypes = {0},
	    .track_atom_lst = NULL,

#ifdef CONFIG_DBUS
	    .dbus_data = NULL,
#endif
	};

	auto stderr_logger = stderr_logger_new();
	if (stderr_logger) {
		// stderr logger might fail to create if we are already
		// daemonized.
		log_add_target_tls(stderr_logger);
	}

	// Allocate a session and copy default values into it
	session_t *ps = cmalloc(session_t);
	*ps = s_def;
	list_init_head(&ps->window_stack);
	ps->loop = EV_DEFAULT;
	pixman_region32_init(&ps->screen_reg);

	ps->ignore_tail = &ps->ignore_head;

	ps->o.show_all_xerrors = all_xerrors;

	// Use the same Display across reset, primarily for resource leak checking
	ps->dpy = dpy;
	ps->c = XGetXCBConnection(ps->dpy);

	const xcb_query_extension_reply_t *ext_info;

	ps->previous_xerror_handler = XSetErrorHandler(xerror);

	ps->scr = DefaultScreen(ps->dpy);

	auto screen = x_screen_of_display(ps->c, ps->scr);
	ps->vis = screen->root_visual;
	ps->depth = screen->root_depth;
	ps->root = screen->root;
	ps->root_width = screen->width_in_pixels;
	ps->root_height = screen->height_in_pixels;

	// Start listening to events on root earlier to catch all possible
	// root geometry changes
	auto e = xcb_request_check(
	    ps->c, xcb_change_window_attributes_checked(
	               ps->c, ps->root, XCB_CW_EVENT_MASK,
	               (const uint32_t[]){XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
	                                  XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
	                                  XCB_EVENT_MASK_PROPERTY_CHANGE}));
	if (e) {
		log_error_x_error(e, "Failed to setup root window event mask");
	}

	xcb_prefetch_extension_data(ps->c, &xcb_render_id);
	xcb_prefetch_extension_data(ps->c, &xcb_composite_id);
	xcb_prefetch_extension_data(ps->c, &xcb_damage_id);
	xcb_prefetch_extension_data(ps->c, &xcb_shape_id);
	xcb_prefetch_extension_data(ps->c, &xcb_xfixes_id);
	xcb_prefetch_extension_data(ps->c, &xcb_randr_id);
	xcb_prefetch_extension_data(ps->c, &xcb_xinerama_id);
	xcb_prefetch_extension_data(ps->c, &xcb_present_id);
	xcb_prefetch_extension_data(ps->c, &xcb_sync_id);
	xcb_prefetch_extension_data(ps->c, &xcb_glx_id);

	ext_info = xcb_get_extension_data(ps->c, &xcb_render_id);
	if (!ext_info || !ext_info->present) {
		log_fatal("No render extension");
		exit(1);
	}
	ps->render_event = ext_info->first_event;
	ps->render_error = ext_info->first_error;

	ext_info = xcb_get_extension_data(ps->c, &xcb_composite_id);
	if (!ext_info || !ext_info->present) {
		log_fatal("No composite extension");
		exit(1);
	}
	ps->composite_opcode = ext_info->major_opcode;
	ps->composite_event = ext_info->first_event;
	ps->composite_error = ext_info->first_error;

	{
		xcb_composite_query_version_reply_t *reply = xcb_composite_query_version_reply(
		    ps->c,
		    xcb_composite_query_version(ps->c, XCB_COMPOSITE_MAJOR_VERSION,
		                                XCB_COMPOSITE_MINOR_VERSION),
		    NULL);

		if (!reply || (reply->major_version == 0 && reply->minor_version < 2)) {
			log_fatal("Your X server doesn't have Composite >= 0.2 support, "
			          "we cannot proceed.");
			exit(1);
		}
		free(reply);
	}

	ext_info = xcb_get_extension_data(ps->c, &xcb_damage_id);
	if (!ext_info || !ext_info->present) {
		log_fatal("No damage extension");
		exit(1);
	}
	ps->damage_event = ext_info->first_event;
	ps->damage_error = ext_info->first_error;
	xcb_discard_reply(ps->c, xcb_damage_query_version(ps->c, XCB_DAMAGE_MAJOR_VERSION,
	                                                  XCB_DAMAGE_MINOR_VERSION)
	                             .sequence);

	ext_info = xcb_get_extension_data(ps->c, &xcb_xfixes_id);
	if (!ext_info || !ext_info->present) {
		log_fatal("No XFixes extension");
		exit(1);
	}
	ps->xfixes_event = ext_info->first_event;
	ps->xfixes_error = ext_info->first_error;
	xcb_discard_reply(ps->c, xcb_xfixes_query_version(ps->c, XCB_XFIXES_MAJOR_VERSION,
	                                                  XCB_XFIXES_MINOR_VERSION)
	                             .sequence);

	ps->damaged_region = x_new_id(ps->c);
	if (!XCB_AWAIT_VOID(xcb_xfixes_create_region, ps->c, ps->damaged_region, 0, NULL)) {
		log_fatal("Failed to create a XFixes region");
		goto err;
	}

	ext_info = xcb_get_extension_data(ps->c, &xcb_glx_id);
	if (ext_info && ext_info->present) {
		ps->glx_exists = true;
		ps->glx_error = ext_info->first_error;
		ps->glx_event = ext_info->first_event;
	}

	// Parse configuration file
	win_option_mask_t winopt_mask[NUM_WINTYPES] = {{0}};
	bool shadow_enabled = false, fading_enable = false, hasneg = false;
	char *config_file_to_free = NULL;
	config_file = config_file_to_free = parse_config(
	    &ps->o, config_file, &shadow_enabled, &fading_enable, &hasneg, winopt_mask);

	if (IS_ERR(config_file_to_free)) {
		return NULL;
	}

	// Parse all of the rest command line options
	if (!get_cfg(&ps->o, argc, argv, shadow_enabled, fading_enable, hasneg, winopt_mask)) {
		log_fatal("Failed to get configuration, usually mean you have specified "
		          "invalid options.");
		return NULL;
	}

	if (ps->o.logpath) {
		auto l = file_logger_new(ps->o.logpath);
		if (l) {
			log_info("Switching to log file: %s", ps->o.logpath);
			if (stderr_logger) {
				log_remove_target_tls(stderr_logger);
				stderr_logger = NULL;
			}
			log_add_target_tls(l);
			stderr_logger = NULL;
		} else {
			log_error("Failed to setup log file %s, I will keep using stderr",
			          ps->o.logpath);
		}
	}

	if (strstr(argv[0], "compton")) {
		log_warn("This compositor has been renamed to \"picom\", the \"compton\" "
		         "binary will not be installed in the future.");
	}

	ps->atoms = init_atoms(ps->c);
	ps->atoms_wintypes[WINTYPE_UNKNOWN] = 0;
#define SET_WM_TYPE_ATOM(x)                                                              \
	ps->atoms_wintypes[WINTYPE_##x] = ps->atoms->a_NET_WM_WINDOW_TYPE_##x
	SET_WM_TYPE_ATOM(DESKTOP);
	SET_WM_TYPE_ATOM(DOCK);
	SET_WM_TYPE_ATOM(TOOLBAR);
	SET_WM_TYPE_ATOM(MENU);
	SET_WM_TYPE_ATOM(UTILITY);
	SET_WM_TYPE_ATOM(SPLASH);
	SET_WM_TYPE_ATOM(DIALOG);
	SET_WM_TYPE_ATOM(NORMAL);
	SET_WM_TYPE_ATOM(DROPDOWN_MENU);
	SET_WM_TYPE_ATOM(POPUP_MENU);
	SET_WM_TYPE_ATOM(TOOLTIP);
	SET_WM_TYPE_ATOM(NOTIFICATION);
	SET_WM_TYPE_ATOM(COMBO);
	SET_WM_TYPE_ATOM(DND);
#undef SET_WM_TYPE_ATOM

	// Get needed atoms for c2 condition lists
	if (!(c2_list_postprocess(ps, ps->o.unredir_if_possible_blacklist) &&
	      c2_list_postprocess(ps, ps->o.paint_blacklist) &&
	      c2_list_postprocess(ps, ps->o.shadow_blacklist) &&
	      c2_list_postprocess(ps, ps->o.shadow_clip_list) &&
	      c2_list_postprocess(ps, ps->o.fade_blacklist) &&
	      c2_list_postprocess(ps, ps->o.blur_background_blacklist) &&
	      c2_list_postprocess(ps, ps->o.invert_color_list) &&
	      c2_list_postprocess(ps, ps->o.opacity_rules) &&
		  c2_list_postprocess(ps, ps->o.transition_rules) &&
	      c2_list_postprocess(ps, ps->o.rounded_corners_blacklist) &&
	      c2_list_postprocess(ps, ps->o.focus_blacklist))) {
		log_error("Post-processing of conditionals failed, some of your rules "
		          "might not work");
	}

	ps->gaussian_map = gaussian_kernel_autodetect_deviation(ps->o.shadow_radius);
	sum_kernel_preprocess(ps->gaussian_map);

	rebuild_shadow_exclude_reg(ps);

	// Query X Shape
	ext_info = xcb_get_extension_data(ps->c, &xcb_shape_id);
	if (ext_info && ext_info->present) {
		ps->shape_event = ext_info->first_event;
		ps->shape_error = ext_info->first_error;
		ps->shape_exists = true;
	}

	ext_info = xcb_get_extension_data(ps->c, &xcb_randr_id);
	if (ext_info && ext_info->present) {
		ps->randr_exists = true;
		ps->randr_event = ext_info->first_event;
		ps->randr_error = ext_info->first_error;
	}

	ext_info = xcb_get_extension_data(ps->c, &xcb_present_id);
	if (ext_info && ext_info->present) {
		auto r = xcb_present_query_version_reply(
		    ps->c,
		    xcb_present_query_version(ps->c, XCB_PRESENT_MAJOR_VERSION,
		                              XCB_PRESENT_MINOR_VERSION),
		    NULL);
		if (r) {
			ps->present_exists = true;
			free(r);
		}
	}

	// Query X Sync
	ext_info = xcb_get_extension_data(ps->c, &xcb_sync_id);
	if (ext_info && ext_info->present) {
		ps->xsync_error = ext_info->first_error;
		ps->xsync_event = ext_info->first_event;
		// Need X Sync 3.1 for fences
		auto r = xcb_sync_initialize_reply(
		    ps->c,
		    xcb_sync_initialize(ps->c, XCB_SYNC_MAJOR_VERSION, XCB_SYNC_MINOR_VERSION),
		    NULL);
		if (r && (r->major_version > 3 ||
		          (r->major_version == 3 && r->minor_version >= 1))) {
			ps->xsync_exists = true;
			free(r);
		}
	}

	ps->sync_fence = XCB_NONE;
	if (ps->xsync_exists) {
		ps->sync_fence = x_new_id(ps->c);
		e = xcb_request_check(
		    ps->c, xcb_sync_create_fence(ps->c, ps->root, ps->sync_fence, 0));
		if (e) {
			if (ps->o.xrender_sync_fence) {
				log_error_x_error(e, "Failed to create a XSync fence. "
				                     "xrender-sync-fence will be "
				                     "disabled");
				ps->o.xrender_sync_fence = false;
			}
			ps->sync_fence = XCB_NONE;
			free(e);
		}
	} else if (ps->o.xrender_sync_fence) {
		log_error("XSync extension not found. No XSync fence sync is "
		          "possible. (xrender-sync-fence can't be enabled)");
		ps->o.xrender_sync_fence = false;
	}

	// Query X RandR
	if ((ps->o.sw_opti && !ps->o.refresh_rate) || ps->o.xinerama_shadow_crop) {
		if (!ps->randr_exists) {
			log_fatal("No XRandR extension. sw-opti, refresh-rate or "
			          "xinerama-shadow-crop "
			          "cannot be enabled.");
			goto err;
		}
	}

	// Query X Xinerama extension
	if (ps->o.xinerama_shadow_crop) {
		ext_info = xcb_get_extension_data(ps->c, &xcb_xinerama_id);
		ps->xinerama_exists = ext_info && ext_info->present;
	}

	rebuild_screen_reg(ps);

	bool compositor_running = false;
	if (session_redirection_mode(ps) == XCB_COMPOSITE_REDIRECT_MANUAL) {
		// We are running in the manual redirection mode, meaning we are running
		// as a proper compositor. So we need to register us as a compositor, etc.

		// We are also here when --diagnostics is set. We want to be here because
		// that gives us more diagnostic information.

		// Create registration window
		int ret = register_cm(ps);
		if (ret == -1) {
			exit(1);
		}

		compositor_running = ret == 1;
		if (compositor_running) {
			// Don't take the overlay when there is another compositor
			// running, so we don't disrupt it.

			// If we are printing diagnostic, we will continue a bit further
			// to get more diagnostic information, otherwise we will exit.
			if (!ps->o.print_diagnostics) {
				log_fatal("Another composite manager is already running");
				exit(1);
			}
		} else {
			if (!init_overlay(ps)) {
				goto err;
			}
		}
	} else {
		// We are here if we don't really function as a compositor, so we are not
		// taking over the screen, and we don't need to register as a compositor

		// If we are in debug mode, we need to create a window for rendering if
		// the backend supports presenting.

		// The old backends doesn't have a automatic redirection mode
		log_info("The compositor is started in automatic redirection mode.");
		assert(ps->o.experimental_backends);

		if (backend_list[ps->o.backend]->present) {
			// If the backend has `present`, we couldn't be in automatic
			// redirection mode unless we are in debug mode.
			assert(ps->o.debug_mode);
			if (!init_debug_window(ps)) {
				goto err;
			}
		}
	}

	ps->drivers = detect_driver(ps->c, ps->backend_data, ps->root);
	apply_driver_workarounds(ps, ps->drivers);

	// Initialize filters, must be preceded by OpenGL context creation
	if (!ps->o.experimental_backends && !init_render(ps)) {
		log_fatal("Failed to initialize the backend");
		exit(1);
	}

	if (ps->o.print_diagnostics) {
		print_diagnostics(ps, config_file, compositor_running);
		free(config_file_to_free);
		exit(0);
	}

	ps->file_watch_handle = file_watch_init(ps->loop);
	if (ps->file_watch_handle && config_file) {
		file_watch_add(ps->file_watch_handle, config_file, config_file_change_cb, ps);
	}

	free(config_file_to_free);

	if (bkend_use_glx(ps) && !ps->o.experimental_backends) {
		auto gl_logger = gl_string_marker_logger_new();
		if (gl_logger) {
			log_info("Enabling gl string marker");
			log_add_target_tls(gl_logger);
		}
	}

	if (ps->o.experimental_backends) {
		if (ps->o.monitor_repaint && !backend_list[ps->o.backend]->fill) {
			log_warn("--monitor-repaint is not supported by the backend, "
			         "disabling");
			ps->o.monitor_repaint = false;
		}
	}

	// Initialize software optimization
	if (ps->o.sw_opti)
		ps->o.sw_opti = swopti_init(ps);

	// Monitor screen changes if vsync_sw is enabled and we are using
	// an auto-detected refresh rate, or when Xinerama features are enabled
	if (ps->randr_exists &&
	    ((ps->o.sw_opti && !ps->o.refresh_rate) || ps->o.xinerama_shadow_crop))
		xcb_randr_select_input(ps->c, ps->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);

	cxinerama_upd_scrs(ps);

	{
		xcb_render_create_picture_value_list_t pa = {
		    .subwindowmode = IncludeInferiors,
		};

		ps->root_picture = x_create_picture_with_visual_and_pixmap(
		    ps->c, ps->vis, ps->root, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
		if (ps->overlay != XCB_NONE) {
			ps->tgt_picture = x_create_picture_with_visual_and_pixmap(
			    ps->c, ps->vis, ps->overlay, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
		} else
			ps->tgt_picture = ps->root_picture;
	}

	ev_io_init(&ps->xiow, x_event_callback, ConnectionNumber(ps->dpy), EV_READ);
	ev_io_start(ps->loop, &ps->xiow);
	ev_init(&ps->unredir_timer, tmout_unredir_callback);
	if (ps->o.sw_opti)
		ev_idle_init(&ps->draw_idle, delayed_draw_callback);
	else
		ev_idle_init(&ps->draw_idle, draw_callback);

	ev_init(&ps->fade_timer, fade_timer_callback);
	ev_init(&ps->delayed_draw_timer, delayed_draw_timer_callback);

	// Set up SIGUSR1 signal handler to reset program
	ev_signal_init(&ps->usr1_signal, reset_enable, SIGUSR1);
	ev_signal_init(&ps->int_signal, exit_enable, SIGINT);
	ev_signal_start(ps->loop, &ps->usr1_signal);
	ev_signal_start(ps->loop, &ps->int_signal);

	// xcb can read multiple events from the socket when a request with reply is
	// made.
	//
	// Use an ev_prepare to make sure we cannot accidentally forget to handle them
	// before we go to sleep.
	//
	// If we don't drain the queue before goes to sleep (i.e. blocking on socket
	// input), we will be sleeping with events available in queue. Which might
	// cause us to block indefinitely because arrival of new events could be
	// dependent on processing of existing events (e.g. if we don't process damage
	// event and do damage subtract, new damage event won't be generated).
	//
	// So we make use of a ev_prepare handle, which is called right before libev
	// goes into sleep, to handle all the queued X events.
	ev_prepare_init(&ps->event_check, handle_queued_x_events);
	// Make sure nothing can cause xcb to read from the X socket after events are
	// handled and before we going to sleep.
	ev_set_priority(&ps->event_check, EV_MINPRI);
	ev_prepare_start(ps->loop, &ps->event_check);

	// Initialize DBus. We need to do this early, because add_win might call dbus
	// functions
	if (ps->o.dbus) {
#ifdef CONFIG_DBUS
		cdbus_init(ps, DisplayString(ps->dpy));
		if (!ps->dbus_data) {
			ps->o.dbus = false;
		}
#else
		log_fatal("DBus support not compiled in!");
		exit(1);
#endif
	}

	e = xcb_request_check(ps->c, xcb_grab_server_checked(ps->c));
	if (e) {
		log_fatal_x_error(e, "Failed to grab X server");
		free(e);
		goto err;
	}

	ps->server_grabbed = true;

	// We are going to pull latest information from X server now, events sent by X
	// earlier is irrelavant at this point.
	// A better solution is probably grabbing the server from the very start. But I
	// think there still could be race condition that mandates discarding the events.
	x_discard_events(ps->c);

	xcb_query_tree_reply_t *query_tree_reply =
	    xcb_query_tree_reply(ps->c, xcb_query_tree(ps->c, ps->root), NULL);

	e = xcb_request_check(ps->c, xcb_ungrab_server(ps->c));
	if (e) {
		log_fatal_x_error(e, "Failed to ungrab server");
		free(e);
		goto err;
	}

	ps->server_grabbed = false;

	if (query_tree_reply) {
		xcb_window_t *children;
		int nchildren;

		children = xcb_query_tree_children(query_tree_reply);
		nchildren = xcb_query_tree_children_length(query_tree_reply);

		for (int i = 0; i < nchildren; i++) {
			add_win_above(ps, children[i], i ? children[i - 1] : XCB_NONE);
		}
		free(query_tree_reply);
	}

	log_debug("Initial stack:");
	list_foreach(struct win, w, &ps->window_stack, stack_neighbour) {
		log_debug("%#010x", w->id);
	}

	ps->pending_updates = true;

	write_pid(ps);

	if (fork && stderr_logger) {
		// Remove the stderr logger if we will fork
		log_remove_target_tls(stderr_logger);
	}
	return ps;
err:
	free(ps);
	return NULL;
}

/**
 * Destroy a session.
 *
 * Does not close the X connection or free the <code>session_t</code>
 * structure, though.
 *
 * @param ps session to destroy
 */
static void session_destroy(session_t *ps) {
	if (ps->redirected) {
		unredirect(ps);
	}

#ifdef CONFIG_OPENGL
	free(ps->argb_fbconfig);
	ps->argb_fbconfig = NULL;
#endif

	file_watch_destroy(ps->loop, ps->file_watch_handle);
	ps->file_watch_handle = NULL;

	// Stop listening to events on root window
	xcb_change_window_attributes(ps->c, ps->root, XCB_CW_EVENT_MASK,
	                             (const uint32_t[]){0});

#ifdef CONFIG_DBUS
	// Kill DBus connection
	if (ps->o.dbus) {
		assert(ps->dbus_data);
		cdbus_destroy(ps);
	}
#endif

	// Free window linked list

	list_foreach_safe(struct win, w, &ps->window_stack, stack_neighbour) {
		if (!w->destroyed) {
			win_ev_stop(ps, w);
			HASH_DEL(ps->windows, w);
		}

		if (w->managed) {
			auto mw = (struct managed_win *)w;
			free_win_res(ps, mw);
		}
		free(w);
	}
	list_init_head(&ps->window_stack);

	// Free blacklists
	free_wincondlst(&ps->o.shadow_blacklist);
	free_wincondlst(&ps->o.shadow_clip_list);
	free_wincondlst(&ps->o.fade_blacklist);
	free_wincondlst(&ps->o.focus_blacklist);
	free_wincondlst(&ps->o.invert_color_list);
	free_wincondlst(&ps->o.blur_background_blacklist);
	free_wincondlst(&ps->o.opacity_rules);
	free_wincondlst(&ps->o.transition_rules);
	free_wincondlst(&ps->o.paint_blacklist);
	free_wincondlst(&ps->o.unredir_if_possible_blacklist);
	free_wincondlst(&ps->o.rounded_corners_blacklist);

	// Free tracked atom list
	{
		latom_t *next = NULL;
		for (latom_t *this = ps->track_atom_lst; this; this = next) {
			next = this->next;
			free(this);
		}

		ps->track_atom_lst = NULL;
	}

	// Free ignore linked list
	{
		ignore_t *next = NULL;
		for (ignore_t *ign = ps->ignore_head; ign; ign = next) {
			next = ign->next;

			free(ign);
		}

		// Reset head and tail
		ps->ignore_head = NULL;
		ps->ignore_tail = &ps->ignore_head;
	}

	// Free tgt_{buffer,picture} and root_picture
	if (ps->tgt_buffer.pict == ps->tgt_picture)
		ps->tgt_buffer.pict = XCB_NONE;

	if (ps->tgt_picture == ps->root_picture)
		ps->tgt_picture = XCB_NONE;
	else
		free_picture(ps->c, &ps->tgt_picture);

	free_picture(ps->c, &ps->root_picture);
	free_paint(ps, &ps->tgt_buffer);

	pixman_region32_fini(&ps->screen_reg);
	free(ps->expose_rects);

	free(ps->o.write_pid_path);
	free(ps->o.logpath);
	for (int i = 0; i < ps->o.blur_kernel_count; ++i) {
		free(ps->o.blur_kerns[i]);
	}
	free(ps->o.blur_kerns);
	free(ps->o.glx_fshader_win_str);
	free_xinerama_info(ps);

#ifdef CONFIG_VSYNC_DRM
	// Close file opened for DRM VSync
	if (ps->drm_fd >= 0) {
		close(ps->drm_fd);
		ps->drm_fd = -1;
	}
#endif

	// Release overlay window
	if (ps->overlay) {
		xcb_composite_release_overlay_window(ps->c, ps->overlay);
		ps->overlay = XCB_NONE;
	}

	if (ps->sync_fence != XCB_NONE) {
		xcb_sync_destroy_fence(ps->c, ps->sync_fence);
		ps->sync_fence = XCB_NONE;
	}

	// Free reg_win
	if (ps->reg_win != XCB_NONE) {
		xcb_destroy_window(ps->c, ps->reg_win);
		ps->reg_win = XCB_NONE;
	}

	if (ps->debug_window != XCB_NONE) {
		xcb_destroy_window(ps->c, ps->debug_window);
		ps->debug_window = XCB_NONE;
	}

	if (ps->damaged_region != XCB_NONE) {
		xcb_xfixes_destroy_region(ps->c, ps->damaged_region);
		ps->damaged_region = XCB_NONE;
	}

	if (ps->o.experimental_backends) {
		// backend is deinitialized in unredirect()
		assert(ps->backend_data == NULL);
	} else {
		deinit_render(ps);
	}

#if CONFIG_OPENGL
	if (glx_has_context(ps)) {
		// GLX context created, but not for rendering
		glx_destroy(ps);
	}
#endif

	// Flush all events
	x_sync(ps->c);
	ev_io_stop(ps->loop, &ps->xiow);
	free_conv(ps->gaussian_map);
	destroy_atoms(ps->atoms);

#ifdef DEBUG_XRC
	// Report about resource leakage
	xrc_report_xid();
#endif

	XSetErrorHandler(ps->previous_xerror_handler);

	// Stop libev event handlers
	ev_timer_stop(ps->loop, &ps->unredir_timer);
	ev_timer_stop(ps->loop, &ps->fade_timer);
	ev_idle_stop(ps->loop, &ps->draw_idle);
	ev_prepare_stop(ps->loop, &ps->event_check);
	ev_signal_stop(ps->loop, &ps->usr1_signal);
	ev_signal_stop(ps->loop, &ps->int_signal);
}

/**
 * Do the actual work.
 *
 * @param ps current session
 */
static void session_run(session_t *ps) {
	if (ps->o.sw_opti)
		ps->paint_tm_offset = get_time_timeval().tv_usec;

	// In benchmark mode, we want draw_idle handler to always be active
	if (ps->o.benchmark) {
		ev_idle_start(ps->loop, &ps->draw_idle);
	} else {
		// Let's draw our first frame!
		queue_redraw(ps);
	}
	ev_run(ps->loop, 0);
}

/**
 * The function that everybody knows.
 */
int main(int argc, char **argv) {
	// Set locale so window names with special characters are interpreted
	// correctly
	setlocale(LC_ALL, "");

	// Initialize logging system for early logging
	log_init_tls();

	{
		auto stderr_logger = stderr_logger_new();
		if (stderr_logger) {
			log_add_target_tls(stderr_logger);
		}
	}

	int exit_code;
	char *config_file = NULL;
	bool all_xerrors = false, need_fork = false;
	if (get_early_config(argc, argv, &config_file, &all_xerrors, &need_fork, &exit_code)) {
		return exit_code;
	}

	int pfds[2];
	if (need_fork) {
		if (pipe2(pfds, O_CLOEXEC)) {
			perror("pipe2");
			return 1;
		}
		auto pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid > 0) {
			// We are the parent
			close(pfds[1]);
			// We wait for the child to tell us it has finished initialization
			// by sending us something via the pipe.
			int tmp;
			if (read(pfds[0], &tmp, sizeof tmp) <= 0) {
				// Failed to read, the child has most likely died
				// We can probably waitpid() here.
				return 1;
			} else {
				// We are done
				return 0;
			}
		}
		// We are the child
		close(pfds[0]);
	}

	// Main loop
	bool quit = false;
	int ret_code = 0;
	char *pid_file = NULL;

	do {
		Display *dpy = XOpenDisplay(NULL);
		if (!dpy) {
			log_fatal("Can't open display.");
			ret_code = 1;
			break;
		}
		XSetEventQueueOwner(dpy, XCBOwnsEventQueue);

		// Reinit logging system so we don't get leftovers from previous sessions
		// or early logging.
		log_deinit_tls();
		log_init_tls();

		ps_g = session_init(argc, argv, dpy, config_file, all_xerrors, need_fork);
		if (!ps_g) {
			log_fatal("Failed to create new session.");
			ret_code = 1;
			break;
		}
		if (need_fork) {
			// Finishing up daemonization
			// Close files
			if (fclose(stdout) || fclose(stderr) || fclose(stdin)) {
				log_fatal("Failed to close standard input/output");
				ret_code = 1;
				break;
			}
			// Make us the session and process group leader so we don't get
			// killed when our parent die.
			setsid();
			// Notify the parent that we are done. This might cause the parent
			// to quit, so only do this after setsid()
			int tmp = 1;
			write(pfds[1], &tmp, sizeof tmp);
			close(pfds[1]);
			// We only do this once
			need_fork = false;
		}
		session_run(ps_g);
		quit = ps_g->quit;
		if (quit && ps_g->o.write_pid_path) {
			pid_file = strdup(ps_g->o.write_pid_path);
		}
		session_destroy(ps_g);
		free(ps_g);
		ps_g = NULL;
		if (dpy) {
			XCloseDisplay(dpy);
		}
	} while (!quit);

	free(config_file);
	if (pid_file) {
		log_trace("remove pid file %s", pid_file);
		unlink(pid_file);
		free(pid_file);
	}

	log_deinit_tls();

	return ret_code;
}
