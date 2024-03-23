// SPDX-License-Identifier: MIT
/*
 * picom - a compositor for X11
 *
 * Based on `compton` - Copyright (c) 2011-2013, Christopher Jeffrey
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2019-2023, Yuxuan Shui
 *
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
#include <libgen.h>
#include <math.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/dpms.h>
#include <xcb/glx.h>
#include <xcb/present.h>
#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xcb_aux.h>
#include <xcb/xfixes.h>

#include <ev.h>
#include <test.h>

#include "common.h"
#include "compiler.h"
#include "config.h"
#include "err.h"
#include "inspect.h"
#include "kernel.h"
#include "picom.h"
#include "transition.h"
#include "win_defs.h"
#ifdef CONFIG_OPENGL
#include "opengl.h"
#endif
#include "atom.h"
#include "backend/backend.h"
#include "c2.h"
#include "dbus.h"
#include "diagnostic.h"
#include "event.h"
#include "file_watch.h"
#include "list.h"
#include "log.h"
#include "options.h"
#include "region.h"
#include "render.h"
#include "statistics.h"
#include "types.h"
#include "uthash_extra.h"
#include "utils.h"
#include "vblank.h"
#include "win.h"
#include "x.h"

/// Get session_t pointer from a pointer to a member of session_t
#define session_ptr(ptr, member)                                                         \
	({                                                                               \
		const __typeof__(((session_t *)0)->member) *__mptr = (ptr);              \
		(session_t *)((char *)__mptr - offsetof(session_t, member));             \
	})

static bool must_use redirect_start(session_t *ps);

static void unredirect(session_t *ps);

// === Global constants ===

/// Name strings for window types.
const struct wintype_info WINTYPES[] = {
    [WINTYPE_UNKNOWN] = {"unknown", NULL},
#define X(name, type) [WINTYPE_##type] = {#name, "_NET_WM_WINDOW_TYPE_" #type}
    X(desktop, DESKTOP),
    X(dock, DOCK),
    X(toolbar, TOOLBAR),
    X(menu, MENU),
    X(utility, UTILITY),
    X(splash, SPLASH),
    X(dialog, DIALOG),
    X(normal, NORMAL),
    X(dropdown_menu, DROPDOWN_MENU),
    X(popup_menu, POPUP_MENU),
    X(tooltip, TOOLTIP),
    X(notification, NOTIFICATION),
    X(combo, COMBO),
    X(dnd, DND),
#undef X
};

// clang-format off
/// Names of backends.
const char *const BACKEND_STRS[] = {[BKEND_XRENDER] = "xrender",
                                    [BKEND_GLX] = "glx",
                                    [BKEND_XR_GLX_HYBRID] = "xr_glx_hybrid",
                                    [BKEND_DUMMY] = "dummy",
                                    [BKEND_EGL] = "egl",
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
 * Get current system clock in milliseconds.
 */
static inline int64_t get_time_ms(void) {
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (int64_t)tp.tv_sec * 1000 + (int64_t)tp.tv_nsec / 1000000;
}

/**
 * Find matched window.
 *
 * XXX move to win.c
 */
static inline struct managed_win *find_win_all(session_t *ps, const xcb_window_t wid) {
	if (!wid || PointerRoot == wid || wid == ps->c.screen_info->root || wid == ps->overlay) {
		return NULL;
	}

	auto w = find_managed_win(ps, wid);
	if (!w) {
		w = find_toplevel(ps, wid);
	}
	if (!w) {
		w = find_managed_window_or_parent(ps, wid);
	}
	return w;
}

enum vblank_callback_action check_render_finish(struct vblank_event *e attr_unused, void *ud) {
	auto ps = (session_t *)ud;
	if (!ps->backend_busy) {
		return VBLANK_CALLBACK_DONE;
	}

	struct timespec render_time;
	bool completed =
	    ps->backend_data->ops->last_render_time(ps->backend_data, &render_time);
	if (!completed) {
		// Render hasn't completed yet, we can't start another render.
		// Check again at the next vblank.
		log_debug("Last render did not complete during vblank, msc: "
		          "%" PRIu64,
		          ps->last_msc);
		return VBLANK_CALLBACK_AGAIN;
	}

	// The frame has been finished and presented, record its render time.
	if (ps->o.debug_options.smart_frame_pacing) {
		int render_time_us =
		    (int)(render_time.tv_sec * 1000000L + render_time.tv_nsec / 1000L);
		render_statistics_add_render_time_sample(
		    &ps->render_stats, render_time_us + (int)ps->last_schedule_delay);
		log_verbose("Last render call took: %d (gpu) + %d (cpu) us, "
		            "last_msc: %" PRIu64,
		            render_time_us, (int)ps->last_schedule_delay, ps->last_msc);
	}
	ps->backend_busy = false;
	return VBLANK_CALLBACK_DONE;
}

enum vblank_callback_action
collect_vblank_interval_statistics(struct vblank_event *e, void *ud) {
	auto ps = (session_t *)ud;
	double vblank_interval = NAN;
	assert(ps->frame_pacing);
	assert(ps->vblank_scheduler);

	if (!ps->o.debug_options.smart_frame_pacing) {
		// We don't need to collect statistics if we are not doing smart frame
		// pacing.
		return VBLANK_CALLBACK_DONE;
	}

	// TODO(yshui): this naive method of estimating vblank interval does not handle
	//              the variable refresh rate case very well. This includes the case
	//              of a VRR enabled monitor; or a monitor that's turned off, in which
	//              case the vblank events might slow down or stop all together.
	//              I tried using DPMS to detect monitor power state, and stop adding
	//              samples when the monitor is off, but I had a hard time to get it
	//              working reliably, there are just too many corner cases.

	// Don't add sample again if we already collected statistics for this vblank
	if (ps->last_msc < e->msc) {
		if (ps->last_msc_instant != 0) {
			auto frame_count = e->msc - ps->last_msc;
			auto frame_time =
			    (int)((e->ust - ps->last_msc_instant) / frame_count);
			if (frame_count == 1) {
				render_statistics_add_vblank_time_sample(
				    &ps->render_stats, frame_time);
				log_trace("Frame count %" PRIu64 ", frame time: %d us, "
				          "ust: "
				          "%" PRIu64,
				          frame_count, frame_time, e->ust);
			} else {
				log_trace("Frame count %" PRIu64 ", frame time: %d us, "
				          "msc: "
				          "%" PRIu64 ", not adding sample.",
				          frame_count, frame_time, e->ust);
			}
		}
		ps->last_msc_instant = e->ust;
		ps->last_msc = e->msc;
	} else if (ps->last_msc > e->msc) {
		log_warn("PresentCompleteNotify msc is going backwards, last_msc: "
		         "%" PRIu64 ", current msc: %" PRIu64,
		         ps->last_msc, e->msc);
		ps->last_msc_instant = 0;
		ps->last_msc = 0;
	}

	vblank_interval = render_statistics_get_vblank_time(&ps->render_stats);
	log_trace("Vblank interval estimate: %f us", vblank_interval);
	if (vblank_interval == 0) {
		// We don't have enough data for vblank interval estimate, schedule
		// another vblank event.
		return VBLANK_CALLBACK_AGAIN;
	}
	return VBLANK_CALLBACK_DONE;
}

void schedule_render(session_t *ps, bool triggered_by_vblank);

/// vblank callback scheduled by schedule_render, when a render is ongoing.
///
/// Check if previously queued render has finished, and reschedule render if it has.
enum vblank_callback_action reschedule_render_at_vblank(struct vblank_event *e, void *ud) {
	auto ps = (session_t *)ud;
	assert(ps->frame_pacing);
	assert(ps->render_queued);
	assert(ps->vblank_scheduler);

	log_verbose("Rescheduling render at vblank, msc: %" PRIu64, e->msc);

	collect_vblank_interval_statistics(e, ud);
	check_render_finish(e, ud);

	if (ps->backend_busy) {
		return VBLANK_CALLBACK_AGAIN;
	}

	schedule_render(ps, false);
	return VBLANK_CALLBACK_DONE;
}

/// How many seconds into the future should we start rendering the next frame.
///
/// Renders are scheduled like this:
///
/// 1. queue_redraw() queues a new render by calling schedule_render, if there
///    is no render currently scheduled. i.e. render_queued == false.
/// 2. then, we need to figure out the best time to start rendering. we need to
///    at least know when the next vblank will start, as we can't start render
///    before the current rendered frame is displayed on screen. we have this
///    information from the vblank scheduler, it will notify us when that happens.
///    we might also want to delay the rendering even further to reduce latency,
///    this is discussed below, in FUTURE WORKS.
/// 3. we schedule a render for that target point in time.
/// 4. draw_callback() is called at the schedule time (i.e. when scheduled
///    vblank event is delivered). Backend APIs are called to issue render
///    commands. render_queued is set to false, and backend_busy is set to true.
///
/// There are some considerations in step 2:
///
/// First of all, a vblank event being delivered
/// doesn't necessarily mean the frame has been displayed on screen. If a frame
/// takes too long to render, it might miss the current vblank, and will be
/// displayed on screen during one of the subsequent vblanks. So in
/// schedule_render_at_vblank, we ask the backend to see if it has finished
/// rendering. if not, render_queued is unchanged, and another vblank is
/// scheduled; otherwise, draw_callback_impl will be scheduled to be call at
/// an appropriate time. Second, we might not have rendered for the previous vblank,
/// in which case the last vblank event we received could be many frames in the past,
/// so we can't make scheduling decisions based on that. So we always schedule
/// a vblank event when render is queued, and make scheduling decisions when the
/// event is delivered.
///
/// All of the above is what happens when frame_pacing is true. Otherwise
/// render_in_progress is either QUEUED or IDLE, and queue_redraw will always
/// schedule a render to be started immediately. PresentCompleteNotify will not
/// be received, and handle_end_of_vblank will not be called.
///
/// The `triggered_by_timer` parameter is used to indicate whether this function
/// is triggered by a steady timer, i.e. we are rendering for each vblank. The
/// other case is when we stop rendering for a while because there is no changes
/// on screen, then something changed and schedule_render is triggered by a
/// DamageNotify. The idea is that when the schedule is triggered by a steady
/// timer, schedule_render will be called at a predictable offset into each
/// vblank.
///
/// # FUTURE WORKS
///
/// As discussed in step 2 above, we might want to delay the rendering even
/// further. If we know the time it takes to render a frame, and the interval
/// between vblanks, we can try to schedule the render to start at a point in
/// time that's closer to the next vblank. We should be able to get this
/// information by doing statistics on the render time of previous frames, which
/// is available from the backends; and the interval between vblank events,
/// which is available from the vblank scheduler.
///
/// The code that does this is already implemented below, but disabled by
/// default. There are several problems with it, see bug #1072.
void schedule_render(session_t *ps, bool triggered_by_vblank attr_unused) {
	// If the backend is busy, we will try again at the next vblank.
	if (ps->backend_busy) {
		// We should never have set backend_busy to true unless frame_pacing is
		// enabled.
		assert(ps->vblank_scheduler);
		assert(ps->frame_pacing);
		log_verbose("Backend busy, will reschedule render at next vblank.");
		if (!vblank_scheduler_schedule(ps->vblank_scheduler,
		                               reschedule_render_at_vblank, ps)) {
			// TODO(yshui): handle error here
			abort();
		}
		return;
	}

	// By default, we want to schedule render immediately, later in this function we
	// might adjust that and move the render later, based on render timing statistics.
	double delay_s = 0;
	unsigned int divisor = 0;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	auto now_us = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;

	ps->next_render = now_us;

	if (!ps->frame_pacing || !ps->redirected) {
		// If not doing frame pacing, schedule a render immediately; if
		// not redirected, we schedule immediately to have a chance to
		// redirect. We won't have frame or render timing information
		// anyway.
		assert(!ev_is_active(&ps->draw_timer));
		goto schedule;
	}

	// if ps->o.debug_options.smart_frame_pacing is false, we won't have any render
	// time or vblank interval estimates, so we would naturally fallback to schedule
	// render immediately.
	auto render_budget = render_statistics_get_budget(&ps->render_stats);
	auto frame_time = render_statistics_get_vblank_time(&ps->render_stats);
	if (frame_time == 0) {
		// We don't have enough data for render time estimates, maybe there's
		// no frame rendered yet, or the backend doesn't support render timing
		// information, schedule render immediately.
		log_verbose("Not enough data for render time estimates.");
		goto schedule;
	}

	if (render_budget >= frame_time) {
		// If the estimated render time is already longer than the estimated
		// vblank interval, there is no way we can make it. Instead of always
		// dropping frames, we try desperately to catch up and schedule a
		// render immediately.
		log_verbose("Render budget: %u us >= frame time: %" PRIu32 " us",
		            render_budget, frame_time);
		goto schedule;
	}

	auto target_frame = (now_us + render_budget - ps->last_msc_instant) / frame_time + 1;
	auto const deadline = ps->last_msc_instant + target_frame * frame_time;
	unsigned int available = 0;
	if (deadline > now_us) {
		available = (unsigned int)(deadline - now_us);
	}

	if (available > render_budget) {
		delay_s = (double)(available - render_budget) / 1000000.0;
		ps->next_render = deadline - render_budget;
	}

	if (delay_s > 1) {
		log_warn("Delay too long: %f s, render_budget: %d us, frame_time: "
		         "%" PRIu32 " us, now_us: %" PRIu64 " us, next_msc: %" PRIu64 " u"
		         "s",
		         delay_s, render_budget, frame_time, now_us, deadline);
	}

	log_verbose("Delay: %.6lf s, last_msc: %" PRIu64 ", render_budget: %d, "
	            "frame_time: %" PRIu32 ", now_us: %" PRIu64 ", next_render: %" PRIu64
	            ", next_msc: %" PRIu64 ", divisor: "
	            "%d",
	            delay_s, ps->last_msc_instant, render_budget, frame_time, now_us,
	            ps->next_render, deadline, divisor);

schedule:
	// If the backend is not busy, we just need to schedule the render at the
	// specified time; otherwise we need to wait for the next vblank event and
	// reschedule.
	ps->last_schedule_delay = 0;
	assert(!ev_is_active(&ps->draw_timer));
	ev_timer_set(&ps->draw_timer, delay_s, 0);
	ev_timer_start(ps->loop, &ps->draw_timer);
}

void queue_redraw(session_t *ps) {
	log_verbose("Queue redraw, render_queued: %d, backend_busy: %d",
	            ps->render_queued, ps->backend_busy);

	if (ps->render_queued) {
		return;
	}
	ps->render_queued = true;
	schedule_render(ps, false);
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

	auto cursor = &ps->damage_ring.damages[ps->damage_ring.cursor];
	pixman_region32_union(cursor, cursor, (region_t *)damage);
}

// === Fading ===

/**
 * Get the time left before next fading point.
 *
 * In milliseconds.
 */
static double fade_timeout(session_t *ps) {
	auto now = get_time_ms();
	if (ps->o.fade_delta + ps->fade_time < now) {
		return 0;
	}

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
static bool run_fade(struct managed_win **_w, unsigned int steps) {
	auto w = *_w;
	log_trace("Process fading for window %s (%#010x), steps: %u", w->name, w->base.id,
	          steps);
	if (w->number_of_animations == 0) {
		// We have reached target opacity.
		// We don't call win_check_fade_finished here because that could destroy
		// the window, but we still need the damage info from this window
		log_trace("|- was fading but finished");
		return false;
	}

	log_trace("|- fading, opacity: %lf", animatable_get(&w->opacity));
	animatable_step(&w->opacity, steps);
	animatable_step(&w->blur_opacity, steps);
	log_trace("|- opacity updated: %lf (%u steps)", animatable_get(&w->opacity), steps);

	// Note even if the animatable is not animating anymore at this point, we still
	// want to run preprocess one last time to finish state transition. So return true
	// in that case too.
	return true;
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
	xcb_window_t wid = wid_get_prop_window(&ps->c, ps->c.screen_info->root,
	                                       ps->atoms->a_NET_ACTIVE_WINDOW);
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
	    xcb_get_input_focus_reply(ps->c.c, xcb_get_input_focus(ps->c.c), NULL);

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
	if (!ret) {
		exit(1);
	}
}

/// Free up all the images and deinit the backend
static void destroy_backend(session_t *ps) {
	win_stack_foreach_managed_safe(w, &ps->window_stack) {
		// Wrapping up fading in progress
		win_skip_fading(w);

		if (ps->backend_data) {
			// Unmapped windows could still have shadow images, but not pixmap
			// images
			assert(!w->win_image || w->state != WSTATE_UNMAPPED);
			// In some cases, the window might have PIXMAP_STALE flag set:
			//   1. If the window is unmapped. Their stale flags won't be
			//      handled until they are mapped.
			//   2. If we haven't had chance to handle the stale flags. This
			//      could happen if we received a root ConfigureNotify
			//      _immidiately_ after we redirected.
			win_clear_flags(w, WIN_FLAGS_PIXMAP_STALE);
			win_release_images(ps->backend_data, w);
		}
		free_paint(ps, &w->paint);

		if (w->state == WSTATE_DESTROYED) {
			destroy_win_finish(ps, &w->base);
		}
	}

	HASH_ITER2(ps->shaders, shader) {
		if (shader->backend_shader != NULL) {
			ps->backend_data->ops->destroy_shader(ps->backend_data,
			                                      shader->backend_shader);
			shader->backend_shader = NULL;
		}
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
		if (ps->shadow_context) {
			ps->backend_data->ops->destroy_shadow_context(ps->backend_data,
			                                              ps->shadow_context);
			ps->shadow_context = NULL;
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
	if (!ps->o.legacy_backends) {
		assert(!ps->backend_data);
		// Reinitialize win_data
		assert(backend_list[ps->o.backend]);
		ps->backend_data =
		    backend_list[ps->o.backend]->init(ps, session_get_target_window(ps));
		if (!ps->backend_data) {
			log_fatal("Failed to initialize backend, aborting...");
			quit(ps);
			return false;
		}
		ps->backend_data->ops = backend_list[ps->o.backend];
		ps->shadow_context = ps->backend_data->ops->create_shadow_context(
		    ps->backend_data, ps->o.shadow_radius);
		if (!ps->shadow_context) {
			log_fatal("Failed to initialize shadow context, aborting...");
			goto err;
		}

		if (!initialize_blur(ps)) {
			log_fatal("Failed to prepare for background blur, aborting...");
			goto err;
		}

		// Create shaders
		HASH_ITER2(ps->shaders, shader) {
			assert(shader->backend_shader == NULL);
			shader->backend_shader = ps->backend_data->ops->create_shader(
			    ps->backend_data, shader->source);
			if (shader->backend_shader == NULL) {
				log_warn("Failed to create shader for shader file %s, "
				         "this shader will not be used",
				         shader->key);
			} else {
				if (ps->backend_data->ops->get_shader_attributes) {
					shader->attributes =
					    ps->backend_data->ops->get_shader_attributes(
					        ps->backend_data, shader->backend_shader);
				} else {
					shader->attributes = 0;
				}
				log_debug("Shader %s has attributes %" PRIu64,
				          shader->key, shader->attributes);
			}
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
			win_set_flags(w, WIN_FLAGS_PIXMAP_STALE);
			ps->pending_updates = true;
		}
	}

	// The old backends binds pixmap lazily, nothing to do here
	return true;
err:
	if (ps->shadow_context) {
		ps->backend_data->ops->destroy_shadow_context(ps->backend_data,
		                                              ps->shadow_context);
		ps->shadow_context = NULL;
	}
	ps->backend_data->ops->deinit(ps->backend_data);
	ps->backend_data = NULL;
	quit(ps);
	return false;
}

/// Handle configure event of the root window
static void configure_root(session_t *ps) {
	// TODO(yshui) re-initializing backend should be done outside of the
	// critical section. Probably set a flag and do it in draw_callback_impl.
	auto r = XCB_AWAIT(xcb_get_geometry, ps->c.c, ps->c.screen_info->root);
	if (!r) {
		log_fatal("Failed to fetch root geometry");
		abort();
	}

	log_info("Root configuration changed, new geometry: %dx%d", r->width, r->height);
	bool has_root_change = false;
	if (ps->redirected) {
		// On root window changes
		if (!ps->o.legacy_backends) {
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

	// Whether a window is fullscreen depends on the new screen
	// size. So we need to refresh the fullscreen state of all
	// windows.
	win_stack_foreach_managed(w, &ps->window_stack) {
		win_update_is_fullscreen(ps, w);
	}

	if (ps->redirected) {
		for (int i = 0; i < ps->damage_ring.count; i++) {
			pixman_region32_clear(&ps->damage_ring.damages[i]);
		}
		ps->damage_ring.cursor = ps->damage_ring.count - 1;
#ifdef CONFIG_OPENGL
		// GLX root change callback
		if (BKEND_GLX == ps->o.backend && ps->o.legacy_backends) {
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
}

static void handle_root_flags(session_t *ps) {
	if ((ps->root_flags & ROOT_FLAGS_SCREEN_CHANGE) != 0) {
		if (ps->o.crop_shadow_to_monitor && ps->randr_exists) {
			x_update_monitors(&ps->c, &ps->monitors);
		}
		ps->root_flags &= ~(uint64_t)ROOT_FLAGS_SCREEN_CHANGE;
	}

	if ((ps->root_flags & ROOT_FLAGS_CONFIGURED) != 0) {
		configure_root(ps);
		ps->root_flags &= ~(uint64_t)ROOT_FLAGS_CONFIGURED;
	}
}

/**
 * Go through the window stack and calculate some parameters for rendering.
 *
 * @return whether the operation succeeded
 */
static bool paint_preprocess(session_t *ps, bool *fade_running, bool *animation,
                             struct managed_win **out_bottom) {
	// XXX need better, more general name for `fade_running`. It really
	// means if fade is still ongoing after the current frame is rendered
	struct managed_win *bottom = NULL;
	*fade_running = false;
	*animation = false;
	*out_bottom = NULL;

	// Fading step calculation
	unsigned int steps = 0L;
	auto now = get_time_ms();
	if (ps->fade_time) {
		assert(now >= ps->fade_time);
		auto raw_steps = (now - ps->fade_time) / ps->o.fade_delta;
		assert(raw_steps <= UINT_MAX);
		steps = (unsigned int)raw_steps;
		ps->fade_time += raw_steps * ps->o.fade_delta;
	} else {
		// Reset fade_time if unset
		ps->fade_time = get_time_ms();
		steps = 0L;
	}

	// First, let's process fading, and animated shaders
	// TODO(yshui) check if a window is fully obscured, and if we don't need to
	//             process fading or animation for it.
	win_stack_foreach_managed_safe(w, &ps->window_stack) {
		const winmode_t mode_old = w->mode;
		const bool was_painted = w->to_paint;

		if (win_should_dim(ps, w) != w->dim) {
			w->dim = win_should_dim(ps, w);
			add_damage_from_win(ps, w);
		}

		if (w->fg_shader && (w->fg_shader->attributes & SHADER_ATTRIBUTE_ANIMATED)) {
			add_damage_from_win(ps, w);
			*animation = true;
		}

		// Add window to damaged area if its opacity changes
		// If was_painted == false, and to_paint is also false, we don't care
		// If was_painted == false, but to_paint is true, damage will be added in
		// the loop below
		if (was_painted && w->number_of_animations != 0) {
			add_damage_from_win(ps, w);
		}

		// Run fading
		if (run_fade(&w, steps)) {
			*fade_running = true;
		}

		if (w->state == WSTATE_DESTROYED && w->number_of_animations == 0) {
			// the window should be destroyed because it was destroyed
			// by X server and now its animations are finished
			destroy_win_finish(ps, &w->base);
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
		const double window_opacity = animatable_get(&w->opacity);
		const double blur_opacity = animatable_get(&w->blur_opacity);

		// Destroy reg_ignore if some window above us invalidated it
		if (!reg_ignore_valid) {
			rc_region_unref(&w->reg_ignore);
		}

		// log_trace("%d %d %s", w->a.map_state, w->ever_damaged, w->name);
		log_trace("Checking whether window %#010x (%s) should be painted",
		          w->base.id, w->name);

		// Give up if it's not damaged or invisible, or it's unmapped and its
		// pixmap is gone (for example due to a ConfigureNotify), or when it's
		// excluded
		if (w->state == WSTATE_UNMAPPED && w->number_of_animations == 0) {
			if (window_opacity != 0 || blur_opacity != 0) {
				log_warn("Window %#010x (%s) is unmapped but still has "
				         "opacity",
				         w->base.id, w->name);
			}
			log_trace("|- is unmapped");
			to_paint = false;
		} else if (unlikely(ps->debug_window != XCB_NONE) &&
		           (w->base.id == ps->debug_window ||
		            w->client_win == ps->debug_window)) {
			log_trace("|- is the debug window");
			to_paint = false;
		} else if (!w->ever_damaged) {
			log_trace("|- has not received any damages");
			to_paint = false;
		} else if (unlikely(w->g.x + w->g.width < 1 || w->g.y + w->g.height < 1 ||
		                    w->g.x >= ps->root_width || w->g.y >= ps->root_height)) {
			log_trace("|- is positioned outside of the screen");
			to_paint = false;
		} else if (unlikely(window_opacity * MAX_ALPHA < 1 &&
		                    (!w->blur_background || blur_opacity * MAX_ALPHA < 1))) {
			// For consistency, even a window has 0 opacity, we would still
			// blur its background. (unless it's background is not blurred, or
			// the blur opacity is 0)
			log_trace("|- has 0 opacity");
			to_paint = false;
		} else if (w->paint_excluded) {
			log_trace("|- is excluded from painting");
			to_paint = false;
		} else if (unlikely((w->flags & WIN_FLAGS_IMAGE_ERROR) != 0)) {
			log_trace("|- has image errors");
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
			log_trace("|- will not be painted");
			goto skip_window;
		}

		log_trace("|- will be painted");
		log_verbose("Window %#010x (%s) will be painted", w->base.id, w->name);

		// Calculate shadow opacity
		w->shadow_opacity = ps->o.shadow_opacity * window_opacity * ps->o.frame_opacity;

		// Generate ignore region for painting to reduce GPU load
		if (!w->reg_ignore) {
			w->reg_ignore = rc_region_ref(last_reg_ignore);
		}

		// If the window is solid, or we enabled clipping for transparent windows,
		// we add the window region to the ignored region
		// Otherwise last_reg_ignore shouldn't change
		if ((w->mode != WMODE_TRANS && !ps->o.force_win_blend) ||
		    (ps->o.transparent_clipping && !w->transparent_clipping_excluded)) {
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
			    w->is_fullscreen && !w->unredir_if_possible_excluded) {
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
				return false;
			}
		}
	}

	*out_bottom = bottom;
	return true;
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
			ps->root_image = NULL;
		}
		auto pixmap = x_get_root_back_pixmap(&ps->c, ps->atoms);
		if (pixmap != XCB_NONE) {
			xcb_get_geometry_reply_t *r = xcb_get_geometry_reply(
			    ps->c.c, xcb_get_geometry(ps->c.c, pixmap), NULL);
			if (!r) {
				goto err;
			}

			// We used to assume that pixmaps pointed by the root background
			// pixmap atoms are owned by the root window and have the same
			// depth and hence the same visual that we can use to bind them.
			// However, some applications break this assumption, e.g. the
			// Xfce's desktop manager xfdesktop that sets the _XROOTPMAP_ID
			// atom to a pixmap owned by it that seems to always have 32 bpp
			// depth when the common root window's depth is 24 bpp. So use the
			// root window's visual only if the root background pixmap's depth
			// matches the root window's depth. Otherwise, find a suitable
			// visual for the root background pixmap's depth and use it.
			//
			// We can't obtain a suitable visual for the root background
			// pixmap the same way as the win_bind_pixmap function because it
			// requires a window and we have only a pixmap. We also can't not
			// bind the root background pixmap in case of depth mismatch
			// because some options rely on it's content, e.g.
			// transparent-clipping.
			xcb_visualid_t visual =
			    r->depth == ps->c.screen_info->root_depth
			        ? ps->c.screen_info->root_visual
			        : x_get_visual_for_depth(ps->c.screen_info, r->depth);
			free(r);

			ps->root_image = ps->backend_data->ops->bind_pixmap(
			    ps->backend_data, pixmap, x_get_visual_info(&ps->c, visual), false);
			if (ps->root_image) {
				ps->backend_data->ops->set_image_property(
				    ps->backend_data, IMAGE_PROPERTY_EFFECTIVE_SIZE,
				    ps->root_image, (int[]){ps->root_width, ps->root_height});
			} else {
			err:
				log_error("Failed to bind root back pixmap");
			}
		}
	}

	// Mark screen damaged
	force_repaint(ps);
}

/**
 * Force a full-screen repaint.
 */
void force_repaint(session_t *ps) {
	assert(pixman_region32_not_empty(&ps->screen_reg));
	queue_redraw(ps);
	add_damage(ps, &ps->screen_reg);
}

/**
 * Setup window properties, then register us with the compositor selection (_NET_WM_CM_S)
 *
 * @return 0 if success, 1 if compositor already running, -1 if error.
 */
static int register_cm(session_t *ps) {
	assert(!ps->reg_win);

	ps->reg_win = x_new_id(&ps->c);
	auto e = xcb_request_check(
	    ps->c.c, xcb_create_window_checked(ps->c.c, XCB_COPY_FROM_PARENT, ps->reg_win,
	                                       ps->c.screen_info->root, 0, 0, 1, 1, 0, XCB_NONE,
	                                       ps->c.screen_info->root_visual, 0, NULL));

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
		    ps->c.c, xcb_change_property_checked(
		                 ps->c.c, XCB_PROP_MODE_REPLACE, ps->reg_win, prop_atoms[i],
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
	    ps->c.c, xcb_change_property_checked(ps->c.c, XCB_PROP_MODE_REPLACE, ps->reg_win,
	                                         ps->atoms->aWM_CLASS, XCB_ATOM_STRING, 8,
	                                         ARR_SIZE(picom_class), picom_class));
	if (e) {
		log_error_x_error(e, "Failed to set the WM_CLASS property");
		free(e);
	}

	// Set WM_CLIENT_MACHINE. As per EWMH, because we set _NET_WM_PID, we must also
	// set WM_CLIENT_MACHINE.
	{
		auto const hostname_max = (unsigned long)sysconf(_SC_HOST_NAME_MAX);
		char *hostname = malloc(hostname_max);

		if (gethostname(hostname, hostname_max) == 0) {
			e = xcb_request_check(
			    ps->c.c, xcb_change_property_checked(
			                 ps->c.c, XCB_PROP_MODE_REPLACE, ps->reg_win,
			                 ps->atoms->aWM_CLIENT_MACHINE, XCB_ATOM_STRING,
			                 8, (uint32_t)strlen(hostname), hostname));
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
		xcb_change_property(ps->c.c, XCB_PROP_MODE_REPLACE, ps->reg_win,
		                    ps->atoms->a_NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);
	}

	// Set COMPTON_VERSION
	e = xcb_request_check(ps->c.c, xcb_change_property_checked(
	                                   ps->c.c, XCB_PROP_MODE_REPLACE, ps->reg_win,
	                                   ps->atoms->aCOMPTON_VERSION, XCB_ATOM_STRING, 8,
	                                   (uint32_t)strlen(PICOM_VERSION), PICOM_VERSION));
	if (e) {
		log_error_x_error(e, "Failed to set COMPTON_VERSION.");
		free(e);
	}

	// Acquire X Selection _NET_WM_CM_S?
	if (!ps->o.no_x_selection) {
		const char register_prop[] = "_NET_WM_CM_S";
		xcb_atom_t atom;

		char *buf = NULL;
		if (asprintf(&buf, "%s%d", register_prop, ps->c.screen) < 0) {
			log_fatal("Failed to allocate memory");
			return -1;
		}
		atom = get_atom_with_nul(ps->atoms, buf, ps->c.c);
		free(buf);

		xcb_get_selection_owner_reply_t *reply = xcb_get_selection_owner_reply(
		    ps->c.c, xcb_get_selection_owner(ps->c.c, atom), NULL);

		if (reply && reply->owner != XCB_NONE) {
			// Another compositor already running
			free(reply);
			return 1;
		}
		free(reply);
		xcb_set_selection_owner(ps->c.c, ps->reg_win, atom, 0);
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
 * Initialize X composite overlay window.
 */
static bool init_overlay(session_t *ps) {
	xcb_composite_get_overlay_window_reply_t *reply = xcb_composite_get_overlay_window_reply(
	    ps->c.c, xcb_composite_get_overlay_window(ps->c.c, ps->c.screen_info->root), NULL);
	if (reply) {
		ps->overlay = reply->overlay_win;
		free(reply);
	} else {
		ps->overlay = XCB_NONE;
	}
	if (ps->overlay != XCB_NONE) {
		// Set window region of the overlay window, code stolen from
		// compiz-0.8.8
		if (!XCB_AWAIT_VOID(xcb_shape_mask, ps->c.c, XCB_SHAPE_SO_SET,
		                    XCB_SHAPE_SK_BOUNDING, ps->overlay, 0, 0, 0)) {
			log_fatal("Failed to set the bounding shape of overlay, giving "
			          "up.");
			return false;
		}
		if (!XCB_AWAIT_VOID(xcb_shape_rectangles, ps->c.c, XCB_SHAPE_SO_SET,
		                    XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
		                    ps->overlay, 0, 0, 0, NULL)) {
			log_fatal("Failed to set the input shape of overlay, giving up.");
			return false;
		}

		// Listen to Expose events on the overlay
		xcb_change_window_attributes(ps->c.c, ps->overlay, XCB_CW_EVENT_MASK,
		                             (const uint32_t[]){XCB_EVENT_MASK_EXPOSURE});

		// Retrieve DamageNotify on root window if we are painting on an
		// overlay
		// root_damage = XDamageCreate(ps->dpy, root, XDamageReportNonEmpty);

		// Unmap the overlay, we will map it when needed in redirect_start
		XCB_AWAIT_VOID(xcb_unmap_window, ps->c.c, ps->overlay);
	} else {
		log_error("Cannot get X Composite overlay window. Falling "
		          "back to painting on root window.");
	}
	log_debug("overlay = %#010x", ps->overlay);

	return true;
}

static bool init_debug_window(session_t *ps) {
	xcb_colormap_t colormap = x_new_id(&ps->c);
	ps->debug_window = x_new_id(&ps->c);

	auto err = xcb_request_check(
	    ps->c.c, xcb_create_colormap_checked(ps->c.c, XCB_COLORMAP_ALLOC_NONE,
	                                         colormap, ps->c.screen_info->root,
	                                         ps->c.screen_info->root_visual));
	if (err) {
		goto err_out;
	}

	err = xcb_request_check(
	    ps->c.c, xcb_create_window_checked(
	                 ps->c.c, (uint8_t)ps->c.screen_info->root_depth,
	                 ps->debug_window, ps->c.screen_info->root, 0, 0,
	                 to_u16_checked(ps->root_width), to_u16_checked(ps->root_height),
	                 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, ps->c.screen_info->root_visual,
	                 XCB_CW_COLORMAP, (uint32_t[]){colormap, 0}));
	if (err) {
		goto err_out;
	}

	err = xcb_request_check(ps->c.c, xcb_map_window_checked(ps->c.c, ps->debug_window));
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
	return ps->overlay != XCB_NONE ? ps->overlay : ps->c.screen_info->root;
}

struct cdbus_data *session_get_cdbus(struct session *ps) {
	return ps->dbus_data;
}

uint8_t session_redirection_mode(session_t *ps) {
	if (ps->o.debug_mode) {
		// If the backend is not rendering to the screen, we don't need to
		// take over the screen.
		assert(!ps->o.legacy_backends);
		return XCB_COMPOSITE_REDIRECT_AUTOMATIC;
	}
	if (!ps->o.legacy_backends && !backend_list[ps->o.backend]->present) {
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
		xcb_map_window(ps->c.c, ps->overlay);
	}

	bool success = XCB_AWAIT_VOID(xcb_composite_redirect_subwindows, ps->c.c,
	                              ps->c.screen_info->root, session_redirection_mode(ps));
	if (!success) {
		log_fatal("Another composite manager is already running "
		          "(and does not handle _NET_WM_CM_Sn correctly)");
		return false;
	}

	xcb_aux_sync(ps->c.c);

	if (!initialize_backend(ps)) {
		return false;
	}

	if (!ps->o.legacy_backends) {
		assert(ps->backend_data);
		ps->damage_ring.count = ps->backend_data->ops->max_buffer_age;
	} else {
		ps->damage_ring.count = maximum_buffer_age(ps);
	}
	ps->damage_ring.damages = ccalloc(ps->damage_ring.count, region_t);
	ps->damage_ring.cursor = ps->damage_ring.count - 1;

	for (int i = 0; i < ps->damage_ring.count; i++) {
		pixman_region32_init(&ps->damage_ring.damages[i]);
	}

	ps->frame_pacing = !ps->o.no_frame_pacing && ps->o.vsync;
	if ((ps->o.legacy_backends || ps->o.benchmark || !ps->backend_data->ops->last_render_time) &&
	    ps->frame_pacing) {
		// Disable frame pacing if we are using a legacy backend or if we are in
		// benchmark mode, or if the backend doesn't report render time
		log_info("Disabling frame pacing.");
		ps->frame_pacing = false;
	}

	// Re-detect driver since we now have a backend
	ps->drivers = detect_driver(ps->c.c, ps->backend_data, ps->c.screen_info->root);
	apply_driver_workarounds(ps, ps->drivers);

	if (ps->present_exists && ps->frame_pacing) {
		// Initialize rendering and frame timing statistics, and frame pacing
		// states.
		ps->last_msc_instant = 0;
		ps->last_msc = 0;
		ps->last_schedule_delay = 0;
		render_statistics_reset(&ps->render_stats);
		enum vblank_scheduler_type scheduler_type =
		    choose_vblank_scheduler(ps->drivers);
		if (ps->o.debug_options.force_vblank_scheduler != LAST_VBLANK_SCHEDULER) {
			scheduler_type =
			    (enum vblank_scheduler_type)ps->o.debug_options.force_vblank_scheduler;
		}
		log_info("Using vblank scheduler: %s.", vblank_scheduler_str[scheduler_type]);
		ps->vblank_scheduler =
		    vblank_scheduler_new(ps->loop, &ps->c, session_get_target_window(ps),
		                         scheduler_type, ps->o.use_realtime_scheduling);
		if (!ps->vblank_scheduler) {
			return false;
		}
		vblank_scheduler_schedule(ps->vblank_scheduler,
		                          collect_vblank_interval_statistics, ps);
	} else if (ps->frame_pacing) {
		log_error("Present extension is not supported, frame pacing disabled.");
		ps->frame_pacing = false;
	}

	// Must call XSync() here
	xcb_aux_sync(ps->c.c);

	ps->redirected = true;
	ps->first_frame = true;

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

	xcb_composite_unredirect_subwindows(ps->c.c, ps->c.screen_info->root,
	                                    session_redirection_mode(ps));
	// Unmap overlay window
	if (ps->overlay != XCB_NONE) {
		xcb_unmap_window(ps->c.c, ps->overlay);
	}

	// Free the damage ring
	for (int i = 0; i < ps->damage_ring.count; ++i) {
		pixman_region32_fini(&ps->damage_ring.damages[i]);
	}
	ps->damage_ring.count = 0;
	free(ps->damage_ring.damages);
	ps->damage_ring.cursor = 0;
	ps->damage_ring.damages = NULL;

	if (ps->vblank_scheduler) {
		vblank_scheduler_free(ps->vblank_scheduler);
		ps->vblank_scheduler = NULL;
	}

	// Must call XSync() here
	xcb_aux_sync(ps->c.c);

	ps->redirected = false;
	log_debug("Screen unredirected.");
}

/// Handle queued events before we go to sleep.
///
/// This function is called by ev_prepare watcher, which is called just before
/// the event loop goes to sleep. X damage events are incremental, which means
/// if we don't handle the ones X server already sent us, we won't get new ones.
/// And if we don't get new ones, we won't render, i.e. we would freeze. libxcb
/// keeps an internal queue of events, so we have to be 100% sure no events are
/// left in that queue before we go to sleep.
static void handle_queued_x_events(EV_P attr_unused, ev_prepare *w, int revents attr_unused) {
	session_t *ps = session_ptr(w, event_check);
	// Flush because if we go into sleep when there is still requests in the
	// outgoing buffer, they will not be sent for an indefinite amount of
	// time. Use XFlush here too, we might still use some Xlib functions
	// because OpenGL.
	//
	// Also note, after we have flushed here, we won't flush again in this
	// function before going into sleep. This is because `xcb_flush`/`XFlush`
	// may _read_ more events from the server (yes, this is ridiculous, I
	// know). And we can't have that, see the comments above this function.
	//
	// This means if functions called ev_handle need to send some events,
	// they need to carefully make sure those events are flushed, one way or
	// another.
	XFlush(ps->c.dpy);
	xcb_flush(ps->c.c);

	if (ps->vblank_scheduler) {
		vblank_handle_x_events(ps->vblank_scheduler);
	}

	xcb_generic_event_t *ev;
	while ((ev = xcb_poll_for_queued_event(ps->c.c))) {
		ev_handle(ps, ev);
		free(ev);
	};
	int err = xcb_connection_has_error(ps->c.c);
	if (err) {
		log_fatal("X11 server connection broke (error %d)", err);
		exit(1);
	}
}

static void handle_new_windows(session_t *ps) {
	list_foreach_safe(struct win, w, &ps->window_stack, stack_neighbour) {
		if (w->is_new) {
			auto new_w = maybe_allocate_managed_win(ps, w);
			if (new_w == w) {
				continue;
			}

			assert(new_w->managed);
			list_replace(&w->stack_neighbour, &new_w->stack_neighbour);
			struct win *replaced = NULL;
			HASH_REPLACE_INT(ps->windows, id, new_w, replaced);
			assert(replaced == w);
			free(w);

			auto mw = (struct managed_win *)new_w;
			if (mw->a.map_state == XCB_MAP_STATE_VIEWABLE) {
				win_set_flags(mw, WIN_FLAGS_MAPPED);

				// This window might be damaged before we called fill_win
				// and created the damage handle. And there is no way for
				// us to find out. So just blindly mark it damaged
				mw->ever_damaged = true;
			}
			// Send D-Bus signal
			if (ps->o.dbus) {
				cdbus_ev_win_added(ps->dbus_data, new_w);
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
	// TODO(yshui): do we still need the fade timer? we queue redraw automatically in
	// draw_callback_impl if animation is running.
	session_t *ps = session_ptr(w, fade_timer);
	queue_redraw(ps);
}

static void handle_pending_updates(EV_P_ struct session *ps) {
	if (ps->pending_updates) {
		log_debug("Delayed handling of events, entering critical section");
		auto e = xcb_request_check(ps->c.c, xcb_grab_server_checked(ps->c.c));
		if (e) {
			log_fatal_x_error(e, "failed to grab x server");
			free(e);
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
			    ps->c.c, xcb_get_input_focus(ps->c.c), NULL);
			if (!ps->active_win || (r && r->focus != ps->active_win->base.id)) {
				recheck_focus(ps);
			}
			free(r);
		}

		// Process window flags (stale images)
		refresh_images(ps);

		e = xcb_request_check(ps->c.c, xcb_ungrab_server_checked(ps->c.c));
		if (e) {
			log_fatal_x_error(e, "failed to ungrab x server");
			free(e);
			return quit(ps);
		}

		ps->server_grabbed = false;
		ps->pending_updates = false;
		log_debug("Exited critical section");
	}
}

static void draw_callback_impl(EV_P_ session_t *ps, int revents attr_unused) {
	assert(!ps->backend_busy);
	assert(ps->render_queued);

	struct timespec now;
	int64_t draw_callback_enter_us;
	clock_gettime(CLOCK_MONOTONIC, &now);

	draw_callback_enter_us = (now.tv_sec * 1000000LL + now.tv_nsec / 1000);
	if (ps->next_render != 0) {
		log_trace("Schedule delay: %" PRIi64 " us",
		          draw_callback_enter_us - (int64_t)ps->next_render);
	}

	handle_pending_updates(EV_A_ ps);

	int64_t after_handle_pending_updates_us;
	clock_gettime(CLOCK_MONOTONIC, &now);
	after_handle_pending_updates_us = (now.tv_sec * 1000000LL + now.tv_nsec / 1000);
	log_trace("handle_pending_updates took: %" PRIi64 " us",
	          after_handle_pending_updates_us - draw_callback_enter_us);

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
			win_skip_fading(w);
			if (w->state == WSTATE_DESTROYED) {
				destroy_win_finish(ps, &w->base);
			}
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
	bool animation = false;
	bool was_redirected = ps->redirected;
	struct managed_win *bottom = NULL;
	if (!paint_preprocess(ps, &fade_running, &animation, &bottom)) {
		log_fatal("Pre-render preparation has failed, exiting...");
		exit(1);
	}

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

	int64_t after_preprocess_us;
	clock_gettime(CLOCK_MONOTONIC, &now);
	after_preprocess_us = (now.tv_sec * 1000000LL + now.tv_nsec / 1000);
	log_trace("paint_preprocess took: %" PRIi64 " us",
	          after_preprocess_us - after_handle_pending_updates_us);

	// If the screen is unredirected, we don't render anything.
	bool did_render = false;
	if (ps->redirected && ps->o.stoppaint_force != ON) {
		static int paint = 0;

		log_verbose("Render start, frame %d", paint);
		if (!ps->o.legacy_backends) {
			did_render = paint_all_new(ps, bottom);
		} else {
			paint_all(ps, bottom);
		}
		log_verbose("Render end");

		ps->first_frame = false;
		paint++;
		if (ps->o.benchmark && paint >= ps->o.benchmark) {
			exit(0);
		}
	}

	// With frame pacing, we set backend_busy to true after the end of
	// vblank. Without frame pacing, we won't be receiving vblank events, so
	// we set backend_busy to false here, right after we issue the render
	// commands.
	// The other case is if we decided there is no change to render, in that
	// case no render command is issued, so we also set backend_busy to
	// false.
	ps->backend_busy = (ps->frame_pacing && did_render);
	ps->next_render = 0;

	if (!fade_running) {
		ps->fade_time = 0L;
	}

	ps->render_queued = false;

	// TODO(yshui) Investigate how big the X critical section needs to be. There are
	// suggestions that rendering should be in the critical section as well.

	// Queue redraw if animation is running. This should be picked up by next present
	// event.
	if (animation) {
		queue_redraw(ps);
	}
	if (ps->vblank_scheduler) {
		// Even if we might not want to render during next vblank, we want to keep
		// `backend_busy` up to date, so when the next render comes, we can
		// immediately know if we can render.
		vblank_scheduler_schedule(ps->vblank_scheduler, check_render_finish, ps);
	}
}

static void draw_callback(EV_P_ ev_timer *w, int revents) {
	session_t *ps = session_ptr(w, draw_timer);

	draw_callback_impl(EV_A_ ps, revents);
	ev_timer_stop(EV_A_ w);

	// Immediately start next frame if we are in benchmark mode.
	if (ps->o.benchmark) {
		ev_timer_set(w, 0, 0);
		ev_timer_start(EV_A_ w);
	}
}

static void x_event_callback(EV_P attr_unused, ev_io *w, int revents attr_unused) {
	session_t *ps = (session_t *)w;
	xcb_generic_event_t *ev = xcb_poll_for_event(ps->c.c);
	if (ev) {
		ev_handle(ps, ev);
		free(ev);
	}
}

/**
 * Turn on the program reset flag.
 *
 * This will result in the compositor resetting itself after next paint.
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

static bool load_shader_source(session_t *ps, const char *path) {
	if (!path) {
		// Using the default shader.
		return false;
	}

	log_info("Loading shader source from %s", path);

	struct shader_info *shader = NULL;
	HASH_FIND_STR(ps->shaders, path, shader);
	if (shader) {
		log_debug("Shader already loaded, reusing");
		return false;
	}

	shader = ccalloc(1, struct shader_info);
	shader->key = strdup(path);
	HASH_ADD_KEYPTR(hh, ps->shaders, shader->key, strlen(shader->key), shader);

	FILE *f = fopen(path, "r");
	if (!f) {
		log_error("Failed to open custom shader file: %s", path);
		goto err;
	}
	struct stat statbuf;
	if (fstat(fileno(f), &statbuf) < 0) {
		log_error("Failed to access custom shader file: %s", path);
		goto err;
	}

	auto num_bytes = (size_t)statbuf.st_size;
	shader->source = ccalloc(num_bytes + 1, char);
	auto read_bytes = fread(shader->source, sizeof(char), num_bytes, f);
	if (read_bytes < num_bytes || ferror(f)) {
		// This is a difficult to hit error case, review thoroughly.
		log_error("Failed to read custom shader at %s. (read %lu bytes, expected "
		          "%lu bytes)",
		          path, read_bytes, num_bytes);
		goto err;
	}
	return false;
err:
	HASH_DEL(ps->shaders, shader);
	if (f) {
		fclose(f);
	}
	free(shader->source);
	free(shader->key);
	free(shader);
	return true;
}

static bool load_shader_source_for_condition(const c2_lptr_t *cond, void *data) {
	return load_shader_source(data, c2_list_get_data(cond));
}

/**
 * Initialize a session.
 *
 * @param argc number of command line arguments
 * @param argv command line arguments
 * @param dpy  the X Display
 * @param config_file the path to the config file
 * @param all_xerrors whether we should report all X errors
 * @param fork whether we will fork after initialization
 */
static session_t *session_init(int argc, char **argv, Display *dpy,
                               const char *config_file, bool all_xerrors, bool fork) {
	static const session_t s_def = {
	    .backend_data = NULL,
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
	    .shadow_context = NULL,

	    .last_msc = 0,

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

	// TODO(yshui) investigate what's the best window size
	render_statistics_init(&ps->render_stats, 128);

	ps->o.show_all_xerrors = all_xerrors;

	// Use the same Display across reset, primarily for resource leak checking
	x_connection_init(&ps->c, dpy);
	// We store width/height from screen_info instead using them directly because they
	// can change, see configure_root().
	ps->root_width = ps->c.screen_info->width_in_pixels;
	ps->root_height = ps->c.screen_info->height_in_pixels;

	const xcb_query_extension_reply_t *ext_info;

	// Start listening to events on root earlier to catch all possible
	// root geometry changes
	auto e = xcb_request_check(
	    ps->c.c, xcb_change_window_attributes_checked(
	                 ps->c.c, ps->c.screen_info->root, XCB_CW_EVENT_MASK,
	                 (const uint32_t[]){XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
	                                    XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
	                                    XCB_EVENT_MASK_PROPERTY_CHANGE}));
	if (e) {
		log_error_x_error(e, "Failed to setup root window event mask");
		free(e);
	}

	xcb_prefetch_extension_data(ps->c.c, &xcb_render_id);
	xcb_prefetch_extension_data(ps->c.c, &xcb_composite_id);
	xcb_prefetch_extension_data(ps->c.c, &xcb_damage_id);
	xcb_prefetch_extension_data(ps->c.c, &xcb_shape_id);
	xcb_prefetch_extension_data(ps->c.c, &xcb_xfixes_id);
	xcb_prefetch_extension_data(ps->c.c, &xcb_randr_id);
	xcb_prefetch_extension_data(ps->c.c, &xcb_present_id);
	xcb_prefetch_extension_data(ps->c.c, &xcb_sync_id);
	xcb_prefetch_extension_data(ps->c.c, &xcb_glx_id);
	xcb_prefetch_extension_data(ps->c.c, &xcb_dpms_id);

	ext_info = xcb_get_extension_data(ps->c.c, &xcb_render_id);
	if (!ext_info || !ext_info->present) {
		log_fatal("No render extension");
		exit(1);
	}
	ps->render_event = ext_info->first_event;
	ps->render_error = ext_info->first_error;

	ext_info = xcb_get_extension_data(ps->c.c, &xcb_composite_id);
	if (!ext_info || !ext_info->present) {
		log_fatal("No composite extension");
		exit(1);
	}
	ps->composite_opcode = ext_info->major_opcode;
	ps->composite_event = ext_info->first_event;
	ps->composite_error = ext_info->first_error;

	{
		xcb_composite_query_version_reply_t *reply = xcb_composite_query_version_reply(
		    ps->c.c,
		    xcb_composite_query_version(ps->c.c, XCB_COMPOSITE_MAJOR_VERSION,
		                                XCB_COMPOSITE_MINOR_VERSION),
		    NULL);

		if (!reply || (reply->major_version == 0 && reply->minor_version < 2)) {
			log_fatal("Your X server doesn't have Composite >= 0.2 support, "
			          "we cannot proceed.");
			exit(1);
		}
		free(reply);
	}

	ext_info = xcb_get_extension_data(ps->c.c, &xcb_damage_id);
	if (!ext_info || !ext_info->present) {
		log_fatal("No damage extension");
		exit(1);
	}
	ps->damage_event = ext_info->first_event;
	ps->damage_error = ext_info->first_error;
	xcb_discard_reply(ps->c.c, xcb_damage_query_version(ps->c.c, XCB_DAMAGE_MAJOR_VERSION,
	                                                    XCB_DAMAGE_MINOR_VERSION)
	                               .sequence);

	ext_info = xcb_get_extension_data(ps->c.c, &xcb_xfixes_id);
	if (!ext_info || !ext_info->present) {
		log_fatal("No XFixes extension");
		exit(1);
	}
	ps->xfixes_event = ext_info->first_event;
	ps->xfixes_error = ext_info->first_error;
	xcb_discard_reply(ps->c.c, xcb_xfixes_query_version(ps->c.c, XCB_XFIXES_MAJOR_VERSION,
	                                                    XCB_XFIXES_MINOR_VERSION)
	                               .sequence);

	ps->damage_ring.x_region = x_new_id(&ps->c);
	if (!XCB_AWAIT_VOID(xcb_xfixes_create_region, ps->c.c, ps->damage_ring.x_region,
	                    0, NULL)) {
		log_fatal("Failed to create a XFixes region");
		goto err;
	}

	ext_info = xcb_get_extension_data(ps->c.c, &xcb_glx_id);
	if (ext_info && ext_info->present) {
		ps->glx_exists = true;
		ps->glx_error = ext_info->first_error;
		ps->glx_event = ext_info->first_event;
	}

	ext_info = xcb_get_extension_data(ps->c.c, &xcb_dpms_id);
	ps->dpms_exists = ext_info && ext_info->present;
	if (!ps->dpms_exists) {
		log_warn("No DPMS extension");
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

	if (ps->o.window_shader_fg) {
		log_debug("Default window shader: \"%s\"", ps->o.window_shader_fg);
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

	ps->atoms = init_atoms(ps->c.c);
	ps->c2_state = c2_state_new(ps->atoms);

	// Get needed atoms for c2 condition lists
	options_postprocess_c2_lists(ps->c2_state, &ps->c, &ps->o);

	// Load shader source file specified in the shader rules
	if (c2_list_foreach(ps->o.window_shader_fg_rules, load_shader_source_for_condition, ps)) {
		log_error("Failed to load shader source file for some of the window "
		          "shader rules");
	}
	if (load_shader_source(ps, ps->o.window_shader_fg)) {
		log_error("Failed to load window shader source file");
	}

	if (log_get_level_tls() <= LOG_LEVEL_DEBUG) {
		HASH_ITER2(ps->shaders, shader) {
			log_debug("Shader %s:", shader->key);
			log_debug("%s", shader->source);
		}
	}

	if (ps->o.legacy_backends) {
		ps->shadow_context =
		    (void *)gaussian_kernel_autodetect_deviation(ps->o.shadow_radius);
		sum_kernel_preprocess((conv *)ps->shadow_context);
	}

	rebuild_shadow_exclude_reg(ps);

	// Query X Shape
	ext_info = xcb_get_extension_data(ps->c.c, &xcb_shape_id);
	if (ext_info && ext_info->present) {
		ps->shape_event = ext_info->first_event;
		ps->shape_error = ext_info->first_error;
		ps->shape_exists = true;
	}

	ext_info = xcb_get_extension_data(ps->c.c, &xcb_randr_id);
	if (ext_info && ext_info->present) {
		ps->randr_exists = true;
		ps->randr_event = ext_info->first_event;
		ps->randr_error = ext_info->first_error;
	}

	ext_info = xcb_get_extension_data(ps->c.c, &xcb_present_id);
	if (ext_info && ext_info->present) {
		auto r = xcb_present_query_version_reply(
		    ps->c.c,
		    xcb_present_query_version(ps->c.c, XCB_PRESENT_MAJOR_VERSION,
		                              XCB_PRESENT_MINOR_VERSION),
		    NULL);
		if (r) {
			ps->present_exists = true;
			free(r);
		}
	}

	// Query X Sync
	ext_info = xcb_get_extension_data(ps->c.c, &xcb_sync_id);
	if (ext_info && ext_info->present) {
		ps->xsync_error = ext_info->first_error;
		ps->xsync_event = ext_info->first_event;
		// Need X Sync 3.1 for fences
		auto r = xcb_sync_initialize_reply(
		    ps->c.c,
		    xcb_sync_initialize(ps->c.c, XCB_SYNC_MAJOR_VERSION, XCB_SYNC_MINOR_VERSION),
		    NULL);
		if (r && (r->major_version > 3 ||
		          (r->major_version == 3 && r->minor_version >= 1))) {
			ps->xsync_exists = true;
			free(r);
		}
	}

	ps->sync_fence = XCB_NONE;
	if (ps->xsync_exists) {
		ps->sync_fence = x_new_id(&ps->c);
		e = xcb_request_check(
		    ps->c.c, xcb_sync_create_fence_checked(
		                 ps->c.c, ps->c.screen_info->root, ps->sync_fence, 0));
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
	if (ps->o.crop_shadow_to_monitor && !ps->randr_exists) {
		log_fatal("No X RandR extension. crop-shadow-to-monitor cannot be "
		          "enabled.");
		goto err;
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
		assert(!ps->o.legacy_backends);

		if (backend_list[ps->o.backend]->present) {
			// If the backend has `present`, we couldn't be in automatic
			// redirection mode unless we are in debug mode.
			assert(ps->o.debug_mode);
			if (!init_debug_window(ps)) {
				goto err;
			}
		}
	}

	ps->drivers = detect_driver(ps->c.c, ps->backend_data, ps->c.screen_info->root);
	apply_driver_workarounds(ps, ps->drivers);

	// Initialize filters, must be preceded by OpenGL context creation
	if (ps->o.legacy_backends && !init_render(ps)) {
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

	if (bkend_use_glx(ps) && ps->o.legacy_backends) {
		auto gl_logger = gl_string_marker_logger_new();
		if (gl_logger) {
			log_info("Enabling gl string marker");
			log_add_target_tls(gl_logger);
		}
	}

	if (!ps->o.legacy_backends) {
		if (ps->o.monitor_repaint && !backend_list[ps->o.backend]->fill) {
			log_warn("--monitor-repaint is not supported by the backend, "
			         "disabling");
			ps->o.monitor_repaint = false;
		}
	}

	// Monitor screen changes if vsync_sw is enabled and we are using
	// an auto-detected refresh rate, or when X RandR features are enabled
	if (ps->randr_exists && ps->o.crop_shadow_to_monitor) {
		xcb_randr_select_input(ps->c.c, ps->c.screen_info->root,
		                       XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
		x_update_monitors(&ps->c, &ps->monitors);
	}

	{
		xcb_render_create_picture_value_list_t pa = {
		    .subwindowmode = IncludeInferiors,
		};

		ps->root_picture = x_create_picture_with_visual_and_pixmap(
		    &ps->c, ps->c.screen_info->root_visual, ps->c.screen_info->root,
		    XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
		if (ps->overlay != XCB_NONE) {
			ps->tgt_picture = x_create_picture_with_visual_and_pixmap(
			    &ps->c, ps->c.screen_info->root_visual, ps->overlay,
			    XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
		} else {
			ps->tgt_picture = ps->root_picture;
		}
	}

	ev_io_init(&ps->xiow, x_event_callback, ConnectionNumber(ps->c.dpy), EV_READ);
	ev_io_start(ps->loop, &ps->xiow);
	ev_init(&ps->unredir_timer, tmout_unredir_callback);
	ev_init(&ps->draw_timer, draw_callback);

	ev_init(&ps->fade_timer, fade_timer_callback);

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
		ps->dbus_data = cdbus_init(ps, DisplayString(ps->c.dpy));
		if (!ps->dbus_data) {
			ps->o.dbus = false;
		}
#else
		log_fatal("DBus support not compiled in!");
		exit(1);
#endif
	}

	e = xcb_request_check(ps->c.c, xcb_grab_server_checked(ps->c.c));
	if (e) {
		log_fatal_x_error(e, "Failed to grab X server");
		free(e);
		goto err;
	}

	ps->server_grabbed = true;

	// We are going to pull latest information from X server now, events sent by X
	// earlier is irrelevant at this point.
	// A better solution is probably grabbing the server from the very start. But I
	// think there still could be race condition that mandates discarding the events.
	x_discard_events(&ps->c);

	xcb_query_tree_reply_t *query_tree_reply = xcb_query_tree_reply(
	    ps->c.c, xcb_query_tree(ps->c.c, ps->c.screen_info->root), NULL);

	e = xcb_request_check(ps->c.c, xcb_ungrab_server_checked(ps->c.c));
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

	file_watch_destroy(ps->loop, ps->file_watch_handle);
	ps->file_watch_handle = NULL;

	// Stop listening to events on root window
	xcb_change_window_attributes(ps->c.c, ps->c.screen_info->root, XCB_CW_EVENT_MASK,
	                             (const uint32_t[]){0});

#ifdef CONFIG_DBUS
	// Kill DBus connection
	if (ps->o.dbus) {
		assert(ps->dbus_data);
		cdbus_destroy(ps->dbus_data);
		ps->dbus_data = NULL;
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
	options_destroy(&ps->o);
	c2_state_free(ps->c2_state);

	// Free tgt_{buffer,picture} and root_picture
	if (ps->tgt_buffer.pict == ps->tgt_picture) {
		ps->tgt_buffer.pict = XCB_NONE;
	}

	if (ps->tgt_picture != ps->root_picture) {
		x_free_picture(&ps->c, ps->tgt_picture);
	}
	x_free_picture(&ps->c, ps->root_picture);
	ps->tgt_picture = ps->root_picture = XCB_NONE;

	free_paint(ps, &ps->tgt_buffer);

	pixman_region32_fini(&ps->screen_reg);
	free(ps->expose_rects);

	x_free_monitor_info(&ps->monitors);

	render_statistics_destroy(&ps->render_stats);

	// Release custom window shaders
	free(ps->o.window_shader_fg);
	struct shader_info *shader, *tmp;
	HASH_ITER(hh, ps->shaders, shader, tmp) {
		HASH_DEL(ps->shaders, shader);
		assert(shader->backend_shader == NULL);
		free(shader->source);
		free(shader->key);
		free(shader);
	}

#ifdef CONFIG_VSYNC_DRM
	// Close file opened for DRM VSync
	if (ps->drm_fd >= 0) {
		close(ps->drm_fd);
		ps->drm_fd = -1;
	}
#endif

	// Release overlay window
	if (ps->overlay) {
		xcb_composite_release_overlay_window(ps->c.c, ps->overlay);
		ps->overlay = XCB_NONE;
	}

	if (ps->sync_fence != XCB_NONE) {
		xcb_sync_destroy_fence(ps->c.c, ps->sync_fence);
		ps->sync_fence = XCB_NONE;
	}

	// Free reg_win
	if (ps->reg_win != XCB_NONE) {
		xcb_destroy_window(ps->c.c, ps->reg_win);
		ps->reg_win = XCB_NONE;
	}

	if (ps->debug_window != XCB_NONE) {
		xcb_destroy_window(ps->c.c, ps->debug_window);
		ps->debug_window = XCB_NONE;
	}

	if (ps->damage_ring.x_region != XCB_NONE) {
		xcb_xfixes_destroy_region(ps->c.c, ps->damage_ring.x_region);
		ps->damage_ring.x_region = XCB_NONE;
	}

	if (!ps->o.legacy_backends) {
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
	xcb_aux_sync(ps->c.c);
	ev_io_stop(ps->loop, &ps->xiow);
	if (ps->o.legacy_backends) {
		free_conv((conv *)ps->shadow_context);
	}
	destroy_atoms(ps->atoms);

#ifdef DEBUG_XRC
	// Report about resource leakage
	xrc_report_xid();
#endif

	// Stop libev event handlers
	ev_timer_stop(ps->loop, &ps->unredir_timer);
	ev_timer_stop(ps->loop, &ps->fade_timer);
	ev_timer_stop(ps->loop, &ps->draw_timer);
	ev_prepare_stop(ps->loop, &ps->event_check);
	ev_signal_stop(ps->loop, &ps->usr1_signal);
	ev_signal_stop(ps->loop, &ps->int_signal);

	free_x_connection(&ps->c);
}

/**
 * Do the actual work.
 *
 * @param ps current session
 */
static void session_run(session_t *ps) {
	if (ps->o.use_realtime_scheduling) {
		set_rr_scheduling();
	}

	// In benchmark mode, we want draw_timer handler to always be active
	if (ps->o.benchmark) {
		ev_timer_set(&ps->draw_timer, 0, 0);
		ev_timer_start(ps->loop, &ps->draw_timer);
	} else {
		// Let's draw our first frame!
		queue_redraw(ps);
	}
	ev_run(ps->loop, 0);
}

#ifdef CONFIG_FUZZER
#define PICOM_MAIN(...) no_main(__VA_ARGS__)
#else
#define PICOM_MAIN(...) main(__VA_ARGS__)
#endif

/**
 * The function that everybody knows.
 */
int PICOM_MAIN(int argc, char **argv) {
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

	char *exe_name = basename(argv[0]);
	if (strcmp(exe_name, "picom-inspect") == 0) {
		return inspect_main(argc, argv, config_file);
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
			}
			// We are done
			return 0;
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
			if (write(pfds[1], &tmp, sizeof tmp) != sizeof tmp) {
				log_fatal("Failed to notify parent process");
				ret_code = 1;
				break;
			}
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
