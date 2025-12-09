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
#include <ev.h>
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
#include <xcb/glx.h>
#include <xcb/present.h>
#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xcb_aux.h>
#include <xcb/xfixes.h>

#include <picom/backend.h>
#include <picom/types.h>
#include <test.h>

#include "api_internal.h"
#include "atom.h"
#include "backend/backend.h"
#include "c2.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "dbus.h"
#include "diagnostic.h"
#include "event.h"
#include "inspect.h"
#include "log.h"
#include "options.h"
#include "picom.h"
#include "region.h"
#include "renderer/command_builder.h"
#include "renderer/layout.h"
#include "renderer/renderer.h"
#include "utils/file_watch.h"
#include "utils/list.h"
#include "utils/misc.h"
#include "utils/process.h"
#include "utils/statistics.h"
#include "utils/str.h"
#include "utils/ui.h"
#include "utils/uthash_extra.h"
#include "vblank.h"
#include "wm/defs.h"
#include "wm/wm.h"
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

// === Global variables ===

/// Pointer to current session, as a global variable. Only used by
/// xerror(), which could not have a pointer to current session passed in.
/// XXX Limit what xerror can access by not having this pointer
session_t *ps_g = NULL;

void quit(session_t *ps) {
	ps->quit = true;
	ev_break(ps->loop, EVBREAK_ALL);
}

/**
 * Convert struct timespec to milliseconds.
 */
static inline int64_t timespec_ms(struct timespec ts) {
	return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

enum vblank_callback_action check_render_finish(struct vblank_event *e attr_unused, void *ud) {
	auto ps = (session_t *)ud;
	if (!ps->backend_busy) {
		return VBLANK_CALLBACK_DONE;
	}

	struct timespec render_time;
	bool completed =
	    ps->backend_data->ops.last_render_time(ps->backend_data, &render_time);
	if (!completed) {
		// Render hasn't completed yet, we can't start another render.
		// Check again at the next vblank.
		log_debug("Last render did not complete during vblank, msc: "
		          "%" PRIu64,
		          ps->last_msc);
		return VBLANK_CALLBACK_AGAIN;
	}

	// The frame has been finished and presented, record its render time.
	if (global_debug_options.smart_frame_pacing) {
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

	if (!global_debug_options.smart_frame_pacing) {
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

	// if global_debug_options.smart_frame_pacing is false, we won't have any render
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

// === Windows ===

/**
 * Rebuild cached <code>screen_reg</code>.
 */
static void rebuild_screen_reg(session_t *ps) {
	get_screen_region(ps, &ps->screen_reg);
}

/// Free up all the images and deinit the backend
static void destroy_backend(session_t *ps) {
	wm_stack_foreach_safe(ps->wm, cursor, next_cursor) {
		auto w = wm_ref_deref(cursor);
		if (w == NULL) {
			continue;
		}
		// An unmapped window shouldn't have a pixmap, unless it has animation
		// running. (`w->previous.state != w->state` means there might be
		// animation but we haven't had a chance to start it because
		// `win_process_animation_and_state_change` hasn't been called.)
		// TBH, this assertion is probably too complex than what it's worth.
		assert(!w->win_image || w->state == WSTATE_MAPPED ||
		       w->running_animation_instance != NULL || w->previous.state != w->state);
		// Wrapping up animation in progress
		free(w->running_animation_instance);
		w->running_animation_instance = NULL;

		if (ps->backend_data) {
			// Unmapped windows could still have shadow images.
			// In some cases, the window might have PIXMAP_STALE flag set:
			//   1. If the window is unmapped. Their stale flags won't be
			//      handled until they are mapped.
			//   2. If we haven't had chance to handle the stale flags. This
			//      could happen if we received a root ConfigureNotify
			//      _immediately_ after we redirected.
			win_clear_flags(w, WIN_FLAGS_PIXMAP_STALE);
			win_release_images(ps->backend_data, w);
		}

		if (w->state == WSTATE_DESTROYED) {
			win_destroy_finish(ps, w);
		}
	}

	HASH_ITER2(ps->shaders, shader) {
		if (shader->backend_shader != NULL) {
			ps->backend_data->ops.destroy_shader(ps->backend_data,
			                                     shader->backend_shader);
			shader->backend_shader = NULL;
		}
	}

	if (ps->backend_data && ps->root_image) {
		ps->backend_data->ops.release_image(ps->backend_data, ps->root_image);
		ps->root_image = NULL;
	}

	if (ps->backend_data) {
		if (ps->renderer) {
			renderer_free(ps->backend_data, ps->renderer);
			ps->renderer = NULL;
		}
		// deinit backend
		if (ps->backend_blur_context) {
			ps->backend_data->ops.destroy_blur_context(
			    ps->backend_data, ps->backend_blur_context);
			ps->backend_blur_context = NULL;
		}
		ps->backend_data->ops.deinit(ps->backend_data);
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

	enum backend_image_format format = ps->o.dithered_present
	                                       ? BACKEND_IMAGE_FORMAT_PIXMAP_HIGH
	                                       : BACKEND_IMAGE_FORMAT_PIXMAP;
	ps->backend_blur_context = ps->backend_data->ops.create_blur_context(
	    ps->backend_data, ps->o.blur_method, format, args);
	return ps->backend_blur_context != NULL;
}

/// Init the backend and bind all the window pixmap to backend images
static bool initialize_backend(session_t *ps) {
	assert(!ps->backend_data);
	// Reinitialize win_data
	ps->backend_data = backend_init(ps->o.backend, ps, session_get_target_window(ps));
	api_backend_plugins_invoke(backend_name(ps->o.backend), ps->backend_data);
	if (!ps->backend_data) {
		log_fatal("Failed to initialize backend, aborting...");
		quit(ps);
		return false;
	}

	if (!initialize_blur(ps)) {
		log_fatal("Failed to prepare for background blur, aborting...");
		goto err;
	}

	// Create shaders
	if (!ps->backend_data->ops.create_shader && ps->shaders) {
		log_warn("Shaders are not supported by selected backend %s, "
		         "they will be ignored",
		         backend_name(ps->o.backend));
	} else {
		HASH_ITER2(ps->shaders, shader) {
			assert(shader->backend_shader == NULL);
			shader->backend_shader = ps->backend_data->ops.create_shader(
			    ps->backend_data, shader->spec, shader->source);
			if (shader->backend_shader == NULL) {
				log_warn("Failed to create shader for shader "
				         "file %s, this shader will not be used",
				         shader_spec_get_path(shader->spec));
			} else {
				shader->attributes = 0;
				if (ps->backend_data->ops.get_shader_attributes) {
					shader->attributes =
					    ps->backend_data->ops.get_shader_attributes(
					        ps->backend_data, shader->backend_shader);
				}
				log_debug("Shader %s has attributes %" PRIu64,
				          shader_spec_get_path(shader->spec),
				          shader->attributes);
			}
		}
	}

	wm_stack_foreach(ps->wm, cursor) {
		auto w = wm_ref_deref(cursor);
		if (w != NULL) {
			assert(w->state != WSTATE_DESTROYED);
			// We need to reacquire image
			log_debug("Marking window %#010x (%s) for update after "
			          "redirection",
			          win_id(w), w->name);
			win_set_flags(w, WIN_FLAGS_PIXMAP_STALE);
			ps->pending_updates = true;
		}
	}
	ps->renderer =
	    renderer_new(ps->backend_data, ps->o.shadow_radius, ps->o.dithered_present);
	if (!ps->renderer) {
		log_fatal("Failed to create renderer, aborting...");
		goto err;
	}

	return true;
err:
	ps->backend_data->ops.deinit(ps->backend_data);
	ps->backend_data = NULL;
	quit(ps);
	return false;
}

/// Handle configure event of the root window
void configure_root(session_t *ps) {
	// TODO(yshui) re-initializing backend should be done outside of the
	// critical section. Probably set a flag and do it in draw_callback_impl.
	auto r = XCB_AWAIT(xcb_get_geometry, &ps->c, ps->c.screen_info->root);
	if (!r) {
		log_fatal("Failed to fetch root geometry");
		abort();
	}

	log_info("Root configuration changed, new geometry: %dx%d", r->width, r->height);
	if (ps->redirected) {
		// On root window changes
		BUG_ON_NULL(ps->backend_data);
		if (ps->backend_data->ops.root_change == NULL) {
			// deinit/reinit backend and free up resources if the backend
			// cannot handle root change
			destroy_backend(ps);
		}
	}

	ps->root_width = r->width;
	ps->root_height = r->height;
	free(r);

	rebuild_screen_reg(ps);

	// Whether a window is fullscreen depends on the new screen
	// size. So we need to refresh the fullscreen state of all
	// windows.
	wm_stack_foreach(ps->wm, cursor) {
		auto w = wm_ref_deref(cursor);
		if (w != NULL) {
			win_update_is_fullscreen(ps, w);
		}
	}

	if (!ps->redirected) {
		return;
	}

	if (ps->backend_data != NULL) {
		ps->backend_data->ops.root_change(ps->backend_data, ps);
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

/**
 * Go through the window stack and calculate some parameters for rendering.
 *
 * @return whether the operation succeeded
 */
static bool paint_preprocess(session_t *ps, bool *animation, struct win **out_bottom) {
	// XXX need better, more general name for `fade_running`. It really
	// means if fade is still ongoing after the current frame is rendered
	struct win *bottom = NULL;
	*animation = false;
	*out_bottom = NULL;

	// First, let's process fading, and animated shaders
	// TODO(yshui) check if a window is fully obscured, and if we don't need to
	//             process fading or animation for it.
	wm_stack_foreach_safe(ps->wm, cursor, tmp) {
		auto w = wm_ref_deref(cursor);
		if (w == NULL) {
			continue;
		}

		if (w->running_animation_instance != NULL) {
			*animation = true;
		}

		if (win_has_frame(w)) {
			w->frame_opacity = ps->o.frame_opacity;
		} else {
			w->frame_opacity = 1.0;
		}

		// Update window mode
		w->mode = win_calc_mode(w);
	}

	// Opacity will not change, from this point onwards.

	bool unredir_possible = false;
	// Track whether it's the highest window to paint
	bool is_highest = true;
	if (ps->root_pixmap_shader != NULL &&
	    ps->root_pixmap_shader->attributes & SHADER_ATTRIBUTE_ANIMATED) {
		*animation = true;
	}
	wm_stack_foreach_safe(ps->wm, cursor, next_cursor) {
		__label__ skip_window;
		auto w = wm_ref_deref(cursor);
		if (w == NULL) {
			continue;
		}

		bool to_paint = true;
		// w->to_paint remembers whether this window is painted last time
		const double window_opacity = win_animatable_get(w, WIN_SCRIPT_OPACITY);
		const double blur_opacity = win_animatable_get(w, WIN_SCRIPT_BLUR_OPACITY);
		auto window_options = win_options(w);
		struct shader_info *fg_shader = NULL;
		if (window_options.shader != NULL) {
			HASH_FIND(hh, ps->shaders, window_options.shader->data,
			          window_options.shader->size, fg_shader);
		}
		if (fg_shader != NULL && fg_shader->attributes & SHADER_ATTRIBUTE_ANIMATED) {
			*animation = true;
		}

		// log_trace("%d %d %s", w->a.map_state, w->ever_damaged, w->name);
		log_trace("Checking whether window %#010x (%s) should be painted",
		          win_id(w), w->name);

		// Give up if it's not damaged or invisible, or it's unmapped and its
		// pixmap is gone (for example due to a ConfigureNotify), or when it's
		// excluded
		if ((w->state == WSTATE_UNMAPPED || w->state == WSTATE_DESTROYED) &&
		    w->running_animation_instance == NULL) {
			log_trace("|- is unmapped");
			to_paint = false;
		} else if (unlikely(ps->debug_window != XCB_NONE) &&
		           (win_id(w) == ps->debug_window ||
		            win_client_id(w, /*fallback_to_self=*/false) == ps->debug_window)) {
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
		                    (!window_options.blur_background ||
		                     blur_opacity * MAX_ALPHA < 1))) {
			// For consistency, even a window has 0 opacity, we would still
			// blur its background. (unless it's background is not blurred, or
			// the blur opacity is 0)
			log_trace("|- has 0 opacity");
			to_paint = false;
		} else if (!window_options.paint) {
			log_trace("|- is excluded from painting");
			to_paint = false;
		} else if (unlikely((w->flags & WIN_FLAGS_PIXMAP_ERROR) != 0)) {
			log_trace("|- has image errors");
			to_paint = false;
		}
		// log_trace("%s %d %d %d", w->name, to_paint, w->opacity,
		// w->paint_excluded);

		// to_paint will never change after this point
		if (!to_paint) {
			log_trace("|- will not be painted");
			goto skip_window;
		}

		log_trace("|- will be painted");
		log_verbose("Window %#010x (%s) will be painted", win_id(w), w->name);

		// (Un)redirect screen
		// We could definitely unredirect the screen when there's no window to
		// paint, but this is typically unnecessary, may cause flickering when
		// fading is enabled, and could create inconsistency when the wallpaper
		// is not correctly set.
		if (ps->o.unredir_if_possible && is_highest && w->mode == WMODE_SOLID &&
		    !ps->o.force_win_blend && w->is_fullscreen &&
		    (window_options.unredir == WINDOW_UNREDIR_WHEN_POSSIBLE ||
		     window_options.unredir == WINDOW_UNREDIR_WHEN_POSSIBLE_ELSE_TERMINATE)) {
			unredir_possible = true;
		}

		// Unredirect screen if some window is forcing unredir, even when they are
		// not on the top.
		if (ps->o.unredir_if_possible &&
		    window_options.unredir == WINDOW_UNREDIR_FORCED) {
			unredir_possible = true;
		}

		bottom = w;

		// If the screen is not redirected check if the window's unredir setting
		// allows unredirection to be terminated.
		if (ps->redirected || window_options.unredir == WINDOW_UNREDIR_TERMINATE ||
		    window_options.unredir == WINDOW_UNREDIR_WHEN_POSSIBLE_ELSE_TERMINATE) {
			// Setting is_highest to false will stop all windows stacked below
			// from triggering unredirection. But if `unredir_possible` is
			// already set, this will not prevent unredirection.
			is_highest = false;
		}

	skip_window:
		if (w->state == WSTATE_DESTROYED && w->running_animation_instance == NULL) {
			// the window should be destroyed because it was destroyed
			// by X server and now its animations are finished
			win_destroy_finish(ps, w);
			w = NULL;
		}

		// Avoid setting w->to_paint if w is freed
		if (w) {
			w->to_paint = to_paint;
		}
	}

	// If possible, unredirect all windows and stop painting
	if (ps->o.redirected_force != UNSET) {
		unredir_possible = !ps->o.redirected_force;
	} else if (ps->o.unredir_if_possible && is_highest && !ps->redirected) {
		// `is_highest` being true means there's no window with a unredir setting
		// that allows unredirection to be terminated. So if screen is not
		// redirected, keep it that way.
		//
		// (might not be the best naming.)
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
	if (!ps->redirected) {
		return;
	}

	if (ps->backend_data) {
		if (ps->root_image) {
			ps->backend_data->ops.release_image(ps->backend_data, ps->root_image);
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

			ps->root_image = ps->backend_data->ops.bind_pixmap(
			    ps->backend_data, pixmap, x_get_visual_info(&ps->c, visual));
			ps->root_image_generation += 1;
			ps->root_image_extent = (rect_t){
			    .x1 = r->x, .x2 = r->x + r->width, .y1 = r->y, .y2 = r->y + r->height};
			free(r);

			if (!ps->root_image) {
			err:
				log_error("Failed to bind root back pixmap");
			}
		}
	}

	// Mark screen damaged
	force_repaint(ps);
}

/// Force a full-screen repaint.
void force_repaint(session_t *ps) {
	// `layout_manager` might be NULL if the screen is not redirected, if a render is
	// started in this case it will always be a full-screen render.
	if (ps->layout_manager != NULL) {
		layout_manager_clear(ps->layout_manager);
	}
	queue_redraw(ps);
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
			log_error_x_error(&ps->c, e, "Failed to set window property %d",
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
		log_error_x_error(&ps->c, e, "Failed to set the WM_CLASS property");
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
				log_error_x_error(&ps->c, e,
				                  "Failed to set the WM_CLIENT_MACHINE"
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
		log_error_x_error(&ps->c, e, "Failed to set COMPTON_VERSION.");
		free(e);
	}

	// Acquire X Selection _NET_WM_CM_S?
	if (!ps->o.no_x_selection) {
		const char register_prop[] = "_NET_WM_CM_S";
		xcb_atom_t atom;

		char *buf = NULL;
		casprintf(&buf, "%s%d", register_prop, ps->c.screen);
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
		if (!XCB_AWAIT_VOID(xcb_shape_mask, &ps->c, XCB_SHAPE_SO_SET,
		                    XCB_SHAPE_SK_BOUNDING, ps->overlay, 0, 0, 0)) {
			log_fatal("Failed to set the bounding shape of overlay, giving "
			          "up.");
			return false;
		}
		if (!XCB_AWAIT_VOID(xcb_shape_rectangles, &ps->c, XCB_SHAPE_SO_SET,
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
		XCB_AWAIT_VOID(xcb_unmap_window, &ps->c, ps->overlay);
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

#ifdef CONFIG_DBUS
struct cdbus_data *session_get_cdbus(struct session *ps) {
	return ps->dbus_data;
}
#endif

uint8_t session_redirection_mode(session_t *ps) {
	if (ps->o.debug_mode) {
		// If the backend is not rendering to the screen, we don't need to
		// take over the screen.
		return XCB_COMPOSITE_REDIRECT_AUTOMATIC;
	}
	if (!backend_can_present(ps->o.backend)) {
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

	bool success = XCB_AWAIT_VOID(xcb_composite_redirect_subwindows, &ps->c,
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

	assert(ps->backend_data);
	auto damage_count = ps->backend_data->ops.max_buffer_age(ps->backend_data);
	ps->layout_manager = layout_manager_new((unsigned)damage_count);

	ps->frame_pacing = ps->o.frame_pacing && ps->o.vsync;
	if ((ps->o.benchmark || !ps->backend_data->ops.last_render_time) && ps->frame_pacing) {
		// Disable frame pacing if we are in benchmark mode or if the backend
		// doesn't report render time.
		log_info("Disabling frame pacing.");
		ps->frame_pacing = false;
	}

	// Re-detect driver since we now have a backend
	ps->drivers = detect_driver(ps->c.c, ps->backend_data, ps->c.screen_info->root);
	apply_driver_workarounds(ps, ps->drivers);

	if (ps->c.e.has_present && ps->frame_pacing) {
		// Initialize rendering and frame timing statistics, and frame pacing
		// states.
		ps->last_msc_instant = 0;
		ps->last_msc = 0;
		ps->last_schedule_delay = 0;
		render_statistics_reset(&ps->render_stats);
		enum vblank_scheduler_type scheduler_type =
		    choose_vblank_scheduler(ps->drivers);
		if (global_debug_options.force_vblank_scheduler != LAST_VBLANK_SCHEDULER) {
			scheduler_type =
			    (enum vblank_scheduler_type)global_debug_options.force_vblank_scheduler;
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
	if (ps->overlay != XCB_NONE &&
	    session_redirection_mode(ps) == XCB_COMPOSITE_REDIRECT_MANUAL) {
		xcb_unmap_window(ps->c.c, ps->overlay);
	}

	if (ps->layout_manager) {
		layout_manager_free(ps->layout_manager);
		ps->layout_manager = NULL;
	}

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
static void handle_x_events(struct session *ps) {
	uint32_t latest_completed = ps->c.latest_completed_request;
	while (true) {
		if (!x_prepare_for_sleep(&ps->c)) {
			log_fatal("X connection broke.");
			exit(1);
		}

		// If we send any new requests, we should loop again to flush them. There
		// is no direct way to do this from xcb. So if we called `ev_handle`, or
		// if any pending requests were completed, we conservatively loop again.
		bool needs_flush = false;
		if (ps->vblank_scheduler) {
			vblank_handle_x_events(ps->vblank_scheduler);
		}

		xcb_generic_event_t *ev;
		while ((ev = x_poll_for_event(&ps->c, true))) {
			ev_handle(ps, (xcb_generic_event_t *)ev);
			needs_flush = true;
			free(ev);
		};

		if (ps->c.latest_completed_request != latest_completed) {
			needs_flush = true;
			latest_completed = ps->c.latest_completed_request;
		}

		if (!needs_flush) {
			break;
		}
	}

	int err = xcb_connection_has_error(ps->c.c);
	if (err) {
		log_fatal("X11 server connection broke (error %d)", err);
		exit(1);
	}

	if (wm_has_tree_changes(ps->wm)) {
		log_debug("Window tree changed, queueing redraw.");
		ps->pending_updates = true;
		queue_redraw(ps);
	}
}

static void handle_x_events_ev(EV_P attr_unused, ev_prepare *w, int revents attr_unused) {
	session_t *ps = session_ptr(w, event_check);
	handle_x_events(ps);
}

struct new_window_attributes_request {
	struct x_async_request_base base;
	struct session *ps;
	wm_treeid id;
};

static void handle_new_window_attributes_reply(struct x_connection * /*c*/,
                                               struct x_async_request_base *req_base,
                                               const xcb_raw_generic_event_t *reply_or_error) {
	auto req = (struct new_window_attributes_request *)req_base;
	auto ps = req->ps;
	auto id = req->id;
	auto new_window = wm_find(ps->wm, id.x);
	free(req);

	if (reply_or_error == NULL) {
		// Shutting down
		return;
	}

	if (reply_or_error->response_type == 0) {
		log_debug("Failed to get window attributes for newly created window "
		          "%#010x",
		          id.x);
		return;
	}

	if (new_window == NULL) {
		// Highly unlikely. This window is destroyed, then another window is
		// created with the same window ID before this request completed, and the
		// latter window isn't in our tree yet.
		if (wm_is_consistent(ps->wm)) {
			log_error("Newly created window %#010x is not in the window tree",
			          id.x);
			assert(false);
		}
		return;
	}

	auto current_id = wm_ref_treeid(new_window);
	if (!wm_treeid_eq(current_id, id)) {
		log_debug("Window %#010x was not the window we queried anymore. Current "
		          "gen %" PRIu64 ", expected %" PRIu64,
		          id.x, current_id.gen, id.gen);
		return;
	}

	auto toplevel = wm_ref_toplevel_of(ps->wm, new_window);
	if (toplevel != new_window) {
		log_debug("Newly created window %#010x was moved away from toplevel "
		          "while we were waiting for its attributes",
		          id.x);
		return;
	}
	if (wm_ref_deref(toplevel) != NULL) {
		// This is possible if a toplevel is reparented away, then reparented to
		// root so it became a toplevel again. If the GetWindowAttributes request
		// sent for the first time it became a toplevel wasn't completed for this
		// whole duration, it will create a managed window object for the
		// toplevel. But there is another get attributes request sent the
		// second time it became a toplevel. When we get the reply for the second
		// request, we will reach here.
		log_debug("Newly created window %#010x is already managed", id.x);
		return;
	}

	auto w = win_maybe_allocate(
	    ps, toplevel, (const xcb_get_window_attributes_reply_t *)reply_or_error);
	if (w != NULL && w->a.map_state == XCB_MAP_STATE_VIEWABLE) {
		win_set_flags(w, WIN_FLAGS_MAPPED);
		ps->pending_updates = true;
	}
}

static void handle_new_windows(session_t *ps) {
	// Check tree changes first, because later property updates need accurate
	// client window information
	struct win *w = NULL;
	while (true) {
		auto wm_change = wm_dequeue_change(ps->wm);
		if (wm_change.type == WM_TREE_CHANGE_NONE) {
			break;
		}
		switch (wm_change.type) {
		case WM_TREE_CHANGE_TOPLEVEL_NEW:;
			auto req = ccalloc(1, struct new_window_attributes_request);
			// We don't directly record the toplevel wm_ref here, because any
			// number of things could happen before we get the reply. The
			// window can be reparented, destroyed, then get its window ID
			// reused, etc.
			req->id = wm_ref_treeid(wm_change.toplevel);
			req->ps = ps;
			req->base.callback = handle_new_window_attributes_reply,
			req->base.sequence =
			    xcb_get_window_attributes(ps->c.c, req->id.x).sequence;
			x_await_request(&ps->c, &req->base);
			break;
		case WM_TREE_CHANGE_TOPLEVEL_KILLED:
			w = wm_ref_deref(wm_change.toplevel);
			if (w != NULL) {
				win_destroy_start(w);
			} else {
				// This window is not managed, no point keeping the zombie
				// around.
				wm_reap_zombie(wm_change.toplevel);
			}
			break;
		case WM_TREE_CHANGE_CLIENT:
			log_debug("Client window for window %#010x changed from "
			          "%#010x to %#010x",
			          wm_ref_win_id(wm_change.toplevel),
			          wm_change.client.old.x, wm_change.client.new_.x);
			w = wm_ref_deref(wm_change.toplevel);
			if (w != NULL) {
				win_set_flags(w, WIN_FLAGS_CLIENT_STALE);
			} else {
				log_debug("An unmanaged window %#010x has a new client "
				          "%#010x",
				          wm_ref_win_id(wm_change.toplevel),
				          wm_change.client.new_.x);
			}
			ev_update_focused(ps);
			break;
		case WM_TREE_CHANGE_TOPLEVEL_RESTACKED: break;
		default: unreachable();
		}
	}
}

static void refresh_windows(session_t *ps) {
	wm_stack_foreach(ps->wm, cursor) {
		auto w = wm_ref_deref(cursor);
		if (w == NULL) {
			continue;
		}
		win_process_primary_flags(ps, w);
	}
	wm_refresh_leaders(ps->wm);
	wm_stack_foreach(ps->wm, cursor) {
		auto w = wm_ref_deref(cursor);
		if (w == NULL) {
			continue;
		}
		win_process_secondary_flags(ps, w);
	}
}

static void refresh_images(session_t *ps) {
	wm_stack_foreach(ps->wm, cursor) {
		auto w = wm_ref_deref(cursor);
		if (w != NULL) {
			win_process_image_flags(ps, w);
		}
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

static void handle_pending_updates(struct session *ps, double delta_t) {
	// Process new windows, and maybe allocate struct managed_win for them
	handle_new_windows(ps);

	if (ps->pending_updates) {
		log_debug("Delayed handling of events");

		// Process window flags. This needs to happen before `refresh_images`
		// because this might set the pixmap stale window flag.
		refresh_windows(ps);
		ps->pending_updates = false;
	}

	wm_stack_foreach_safe(ps->wm, cursor, tmp) {
		auto w = wm_ref_deref(cursor);
		BUG_ON(w != NULL && w->tree_ref != cursor);
		// Window might be freed by this function, if it's destroyed and its
		// animation finished
		if (w != NULL && win_process_animation_and_state_change(ps, w, delta_t)) {
			free(w->running_animation_instance);
			w->running_animation_instance = NULL;
			w->in_openclose = false;
			if (w->saved_win_image != NULL) {
				win_release_saved_win_image(ps->backend_data, w);
			}
			if (w->state == WSTATE_UNMAPPED) {
				unmap_win_finish(ps, w);
			} else if (w->state == WSTATE_DESTROYED) {
				win_destroy_finish(ps, w);
			}
		}
	}

	// Process window flags (stale images)
	refresh_images(ps);
	assert(ps->pending_updates == false);
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

static void draw_callback_impl(EV_P_ session_t *ps, int revents attr_unused) {
	assert(!ps->backend_busy);
	assert(ps->render_queued);

	struct timespec now;
	int64_t draw_callback_enter_us;
	clock_gettime(CLOCK_MONOTONIC, &now);

	// Fading step calculation
	auto now_ms = timespec_ms(now);
	int64_t delta_ms = 0L;
	if (ps->fade_time) {
		assert(now_ms >= ps->fade_time);
		delta_ms = now_ms - ps->fade_time;
	}
	ps->fade_time = now_ms;

	draw_callback_enter_us = (now.tv_sec * 1000000LL + now.tv_nsec / 1000);
	if (ps->next_render != 0) {
		log_trace("Schedule delay: %" PRIi64 " us",
		          draw_callback_enter_us - (int64_t)ps->next_render);
	}

	handle_pending_updates(ps, (double)delta_ms / 1000.0);

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
		//
		// TODO(yshui) I think maybe we don't need this anymore, since now we
		//             immediate acquire pixmap right after `map_win_start`.
		wm_stack_foreach_safe(ps->wm, cursor, tmp) {
			auto w = wm_ref_deref(cursor);
			if (w == NULL) {
				continue;
			}
			free(w->running_animation_instance);
			w->running_animation_instance = NULL;
			if (w->state == WSTATE_DESTROYED) {
				win_destroy_finish(ps, w);
			}
		}
	}

	if (ps->o.benchmark) {
		if (ps->o.benchmark_wid) {
			auto w = wm_find(ps->wm, ps->o.benchmark_wid);
			if (w == NULL) {
				log_fatal("Couldn't find specified benchmark window.");
				exit(1);
			}
			w = wm_ref_toplevel_of(ps->wm, w);

			auto win = w == NULL ? NULL : wm_ref_deref(w);
			if (win == NULL) {
				region_t tmp;
				pixman_region32_init(&tmp);
				win_extents(win, &tmp);
				pixman_region32_translate(&tmp, -win->g.x, -win->g.y);
				pixman_region32_union(&win->damaged, &win->damaged, &tmp);
				pixman_region32_fini(&tmp);
			} else {
				force_repaint(ps);
			}
		} else {
			force_repaint(ps);
		}
	}

	/* TODO(yshui) Have a stripped down version of paint_preprocess that is used when
	 * screen is not redirected. its sole purpose should be to decide whether the
	 * screen should be redirected. */
	bool animation = false;
	bool was_redirected = ps->redirected;
	struct win *bottom = NULL;
	if (!paint_preprocess(ps, &animation, &bottom)) {
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
		uint64_t after_damage_us = 0;
		now = get_time_timespec();
		auto render_start_us =
		    (uint64_t)now.tv_sec * 1000000UL + (uint64_t)now.tv_nsec / 1000;
		if (ps->backend_data->ops.device_status &&
		    ps->backend_data->ops.device_status(ps->backend_data) !=
		        DEVICE_STATUS_NORMAL) {
			log_error("Device reset detected");
			// Wait for reset to complete
			// Although ideally the backend should return
			// DEVICE_STATUS_NORMAL after a reset is completed, it's
			// not always possible.
			//
			// According to ARB_robustness (emphasis mine):
			//
			//     "If a reset status other than NO_ERROR is returned
			//     and subsequent calls return NO_ERROR, the context
			//     reset was encountered and completed. If a reset
			//     status is repeatedly returned, the context **may**
			//     be in the process of resetting."
			//
			//  Which means it may also not be in the process of
			//  resetting. For example on AMDGPU devices, Mesa OpenGL
			//  always return CONTEXT_RESET after a reset has started,
			//  completed or not.
			//
			//  So here we blindly wait 5 seconds and hope ourselves
			//  best of the luck.
			sleep(5);
			log_info("Resetting picom after device reset");
			reset_enable(ps->loop, NULL, 0);
			return;
		}
		layout_manager_append_layout(
		    ps->layout_manager, ps->wm, ps->root_image_generation,
		    (ivec2){.width = ps->root_width, .height = ps->root_height});
		bool succeeded = renderer_render(
		    ps->renderer, ps->backend_data, ps->root_image, &ps->root_image_extent,
		    ps->layout_manager, ps->command_builder, ps->backend_blur_context,
		    render_start_us, ps->sync_fence, ps->o.use_damage, ps->o.monitor_repaint,
		    ps->o.force_win_blend, ps->o.blur_background_frame,
		    ps->o.inactive_dim_fixed, ps->o.max_brightness,
		    ps->o.crop_shadow_to_monitor ? &ps->monitors : NULL,
		    ps->root_pixmap_shader, ps->shaders, &after_damage_us);
		if (!succeeded) {
			log_fatal("Render failure");
			abort();
		}
		did_render = true;
		if (ps->next_render > 0) {
			log_verbose("Render schedule deviation: %ld us (%s) %" PRIu64
			            " %" PRIu64,
			            labs((long)after_damage_us - (long)ps->next_render),
			            after_damage_us < ps->next_render ? "early" : "late",
			            after_damage_us, ps->next_render);
			ps->last_schedule_delay = 0;
			if (after_damage_us > ps->next_render) {
				ps->last_schedule_delay = after_damage_us - ps->next_render;
			}
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
	ps->render_queued = false;

	// TODO(yshui) Investigate how big the X critical section needs to be. There are
	// suggestions that rendering should be in the critical section as well.

	// Queue redraw if animation is running. This should be picked up by next present
	// event.
	if (animation) {
		queue_redraw(ps);
	} else {
		ps->fade_time = 0L;
	}
	if (ps->vblank_scheduler && ps->backend_busy) {
		// Even if we might not want to render during next vblank, we want to keep
		// `backend_busy` up to date, so when the next render comes, we can
		// immediately know if we can render.
		vblank_scheduler_schedule(ps->vblank_scheduler, check_render_finish, ps);
	}
}

static void draw_callback(EV_P_ ev_timer *w, int revents) {
	session_t *ps = session_ptr(w, draw_timer);

	// The draw timer has to be stopped before calling the draw_callback_impl
	// function because it may be set and started there, e.g. when a custom
	// animated shader is used.
	ev_timer_stop(EV_A_ w);
	draw_callback_impl(EV_A_ ps, revents);

	// Immediately start next frame if we are in benchmark mode.
	if (ps->o.benchmark) {
		ps->render_queued = true;
		ev_timer_set(w, 0, 0);
		ev_timer_start(EV_A_ w);
	}
}

static void x_event_callback(EV_P attr_unused, ev_io *w, int revents attr_unused) {
	// Make sure the X connection is being read from at least once every time
	// we woke up because of readability of the X connection.
	//
	// `handle_x_events` is not guaranteed to read from X, so if we don't do
	// it here we could dead lock.
	struct session *ps = session_ptr(w, xiow);
	auto ev = x_poll_for_event(&ps->c, false);
	if (ev) {
		ev_handle(ps, (xcb_generic_event_t *)ev);
		free(ev);
	}
}

static void config_file_change_cb(void *_ps) {
	auto ps = (struct session *)_ps;
	reset_enable(ps->loop, NULL, 0);
}

static struct shader_info *
load_shader_source(session_t *ps, const struct shader_specification *spec) {
	const char *path = shader_spec_get_path(spec);
	log_info("Loading shader source from %s", path);

	struct shader_info *shader = NULL;
	HASH_FIND(hh, ps->shaders, spec->data, spec->size, shader);
	if (shader) {
		log_debug("Shader already loaded, reusing.");
		return shader;
	}

	struct shader_source *source = NULL;
	HASH_FIND_STR(ps->shader_sources, path, source);
	if (source) {
		log_debug("Shader source already loaded, reusing");
	} else {
		FILE *f = fopen(path, "r");
		if (!f) {
			log_error("Failed to open custom shader file: %s", path);
			return NULL;
		}

		struct stat statbuf;
		if (fstat(fileno(f), &statbuf) < 0) {
			log_error("Failed to access custom shader file: %s", path);
			fclose(f);
			return NULL;
		}

		auto num_bytes = (size_t)statbuf.st_size;
		char *source_data = ccalloc(num_bytes + 1, char);
		auto read_bytes = fread(source_data, sizeof(char), num_bytes, f);
		auto error = ferror(f);
		fclose(f);

		if (read_bytes < num_bytes || error) {
			// This is a difficult to hit error case, review thoroughly.
			log_error("Failed to read custom shader at %s. (read %zu bytes, "
			          "expected %zu bytes)",
			          path, read_bytes, num_bytes);
			free(source_data);
			return NULL;
		}

		source = ccalloc(1, struct shader_source);
		source->path = path;
		source->source = source_data;
		HASH_ADD_KEYPTR(hh, ps->shader_sources, source->path,
		                strlen(source->path), source);
	}

	shader = ccalloc(1, struct shader_info);
	shader->spec = spec;
	shader->source = source->source;
	HASH_ADD_KEYPTR(hh, ps->shaders, shader->spec->data, shader->spec->size, shader);
	return shader;
}

static struct window_options win_options_from_config(const struct options *opts) {
	struct window_options ret = {
	    .blur_background = opts->blur_method != BLUR_METHOD_NONE,
	    .full_shadow = false,
	    .shadow = opts->shadow_enable,
	    .shadow_color =
	        (struct color){
	            .red = opts->shadow_red,
	            .green = opts->shadow_green,
	            .blue = opts->shadow_blue,
	            .alpha = 1.0,
	        },
	    .corner_radius = (unsigned)opts->corner_radius,
	    .transparent_clipping = opts->transparent_clipping,
	    .dim = 0,
	    .fade = opts->fading_enable,
	    .shader = opts->window_shader_fg,
	    .invert_color = false,
	    .paint = true,
	    .clip_shadow_above = false,
	    .unredir = WINDOW_UNREDIR_WHEN_POSSIBLE_ELSE_TERMINATE,
	    .opacity = 1,
	    .blur_opacity = 1,
	};
	memcpy(ret.animations, opts->animations, sizeof(ret.animations));
	return ret;
}

static void show_config_warning_message_box(struct options *opt) {
	if (opt->problematic_options == NULL) {
		return;
	}

	struct x_connection c;
	if (spawn_picomling(&c) != 0) {
		return;
	}

	struct ui *ui = ui_new(&c);
	if (ui == NULL) {
		exit(1);
	}

	auto lines = HASH_COUNT(opt->problematic_options) + 4;
	struct ui_message_box_line normal_line_template = {
	    .text = NULL,
	    .color = UI_COLOR_WHITE,
	    .style = UI_STYLE_NORMAL,
	    .justify = UI_JUSTIFY_LEFT,
	    .pad_bottom = 3,
	};
	struct ui_message_box_content *content =
	    malloc(sizeof(*content) + lines * sizeof(struct ui_message_box_line));
	content->num_lines = lines;
	content->margin = 10;
	content->scale = 0;
	content->lines[0] = (struct ui_message_box_line){
	    .text = "picom Warning!",
	    .color = UI_COLOR_YELLOW,
	    .style = UI_STYLE_BOLD,
	    .justify = UI_JUSTIFY_CENTER,
	    .pad_bottom = 15,
	};
	content->lines[1] = normal_line_template;
	content->lines[1].text =
	    "Some of your settings have generated warnings. Check the console";
	content->lines[2] = normal_line_template;
	content->lines[2].text =
	    "output of picom for more information. Offending options are:";
	content->lines[2].pad_bottom = 10;
	struct option_name *o, *no;
	unsigned pos = 3;
	HASH_ITER(hh, opt->problematic_options, o, no) {
		char *buf = NULL;
		casprintf(&buf, "    * %s", o->name);
		content->lines[pos] = normal_line_template;
		content->lines[pos].text = buf;
		pos++;
	}
	content->lines[pos - 1].pad_bottom = 10;
	content->lines[pos] = normal_line_template;
	content->lines[pos].text = "(Press ESC to close)";
	content->lines[pos].pad_bottom = 10;
	content->lines[pos].justify = UI_JUSTIFY_CENTER;
	ui_message_box_content_plan(ui, &c, content);
	ui_message_box_show(ui, &c, content, 15);

	for (unsigned i = 3; i < lines - 1; i++) {
		free((void *)content->lines[i].text);
	}
	free(content);
	exit(0);
}

// NOLINTBEGIN(readability-function-cognitive-complexity)

/// Initialize a session.
///
/// @param argc number of command line arguments
/// @param argv command line arguments
/// @param dpy  the X Display
/// @param config_file the path to the config file
/// @param all_xerrors whether we should report all X errors
/// @param fork whether we will fork after initialization
///
static session_t *session_init(int argc, char **argv, Display *dpy,
                               const char *config_file, bool all_xerrors, bool fork) {
	static const session_t s_def = {
	    .backend_data = NULL,
	    .root_height = 0,
	    .root_width = 0,
	    // .root_damage = XCB_NONE,
	    .overlay = XCB_NONE,
	    .reg_win = XCB_NONE,
	    .redirected = false,
	    .fade_time = 0L,
	    .quit = false,

	    .last_msc = 0,

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
	xcb_generic_error_t *e = NULL;
	*ps = s_def;
	ps->loop = EV_DEFAULT;
	pixman_region32_init(&ps->screen_reg);

	// TODO(yshui) investigate what's the best window size
	render_statistics_init(&ps->render_stats, 128);

	ps->o.show_all_xerrors = all_xerrors;

	// Use the same Display across reset, primarily for resource leak checking
	x_connection_init(&ps->c, dpy);
	if (!x_extensions_init(&ps->c)) {
		goto err;
	}

	ps->x_region = x_new_id(&ps->c);
	if (!XCB_AWAIT_VOID(xcb_xfixes_create_region, &ps->c, ps->x_region, 0, NULL)) {
		log_fatal("Failed to create a XFixes region");
		goto err;
	}

	// Parse configuration file
	if (!parse_config(&ps->o, config_file)) {
		goto err;
	}

	// Parse all of the rest command line options
	if (!get_cfg(&ps->o, argc, argv)) {
		log_fatal("Failed to get configuration, usually mean you have specified "
		          "invalid options.");
		goto err;
	}

	show_config_warning_message_box(&ps->o);

	const char *basename = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];

	if (strcmp(basename, "picom-inspect") == 0) {
		ps->o.backend = backend_find("dummy");
		ps->o.print_diagnostics = false;
		ps->o.dbus = false;
		if (!ps->o.inspect_monitor) {
			ps->o.inspect_win = inspect_select_window(&ps->c);
		}
	}

	ps->window_options_default = win_options_from_config(&ps->o);

	if (ps->o.window_shader_fg) {
		log_debug("Default window shader: \"%s\"",
		          shader_spec_get_path(ps->o.window_shader_fg));
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
	c2_condition_list_foreach(&ps->o.window_shader_fg_rules, i) {
		const struct shader_specification *spec = c2_condition_get_data(i);
		if (spec != NULL && load_shader_source(ps, spec) == NULL) {
			log_error("Failed to load shader source file for some of the "
			          "window shader rules");
		}
	}
	if (ps->o.window_shader_fg != NULL &&
	    load_shader_source(ps, ps->o.window_shader_fg) == NULL) {
		log_error("Failed to load window shader source file");
	}
	if (ps->o.root_pixmap_shader != NULL) {
		ps->root_pixmap_shader = load_shader_source(ps, ps->o.root_pixmap_shader);
		if (ps->root_pixmap_shader == NULL) {
			log_error("Failed to load root pixmap shader");
		}
	}

	c2_condition_list_foreach(&ps->o.rules, i) {
		auto data = (struct window_maybe_options *)c2_condition_get_data(i);
		if (data->shader == NULL) {
			continue;
		}
		if (load_shader_source(ps, data->shader) == NULL) {
			log_error("Failed to load shader source file for window rules");
		}
	}

	if (log_get_level_tls() <= LOG_LEVEL_DEBUG) {
		HASH_ITER2(ps->shaders, shader) {
			log_debug("Shader %s:", shader_spec_get_path(shader->spec));
			log_debug("%s", shader->source);
		}
	}

	ps->sync_fence = XCB_NONE;
	if (ps->c.e.has_sync) {
		ps->sync_fence = x_new_id(&ps->c);
		e = xcb_request_check(
		    ps->c.c, xcb_sync_create_fence_checked(
		                 ps->c.c, ps->c.screen_info->root, ps->sync_fence, 0));
		if (e) {
			if (ps->o.xrender_sync_fence) {
				log_error_x_error(&ps->c, e,
				                  "Failed to create a XSync fence. "
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

	if (ps->o.crop_shadow_to_monitor && !ps->c.e.has_randr) {
		log_fatal("No X RandR extension. crop-shadow-to-monitor cannot be "
		          "enabled.");
		goto err;
	}

	bool compositor_running = false;
	if (session_redirection_mode(ps) == XCB_COMPOSITE_REDIRECT_MANUAL) {
		// We are running in the manual redirection mode, meaning we are running
		// as a proper compositor. So we need to register us as a compositor, etc.

		// We are also here when --diagnostics is set. We want to be here because
		// that gives us more diagnostic information.

		// Create registration window
		int ret = register_cm(ps);
		if (ret == -1) {
			goto err;
		}

		compositor_running = ret == 1;
	}

	ps->drivers = detect_driver(ps->c.c, ps->backend_data, ps->c.screen_info->root);
	apply_driver_workarounds(ps, ps->drivers);

	if (ps->o.print_diagnostics) {
		ps->root_width = ps->c.screen_info->width_in_pixels;
		ps->root_height = ps->c.screen_info->height_in_pixels;
		print_diagnostics(ps, config_file, compositor_running);
		exit(0);
	}

	if (ps->o.config_file_path) {
		ps->file_watch_handle = file_watch_init(ps->loop);
		if (ps->file_watch_handle) {
			file_watch_add(ps->file_watch_handle, ps->o.config_file_path,
			               config_file_change_cb, ps);
			list_foreach(struct included_config_file, i,
			             &ps->o.included_config_files, siblings) {
				file_watch_add(ps->file_watch_handle, i->path,
				               config_file_change_cb, ps);
			}
		}
	}

	// Monitor screen changes if vsync_sw is enabled and we are using
	// an auto-detected refresh rate, or when X RandR features are enabled
	if (ps->c.e.has_randr) {
		xcb_randr_select_input(ps->c.c, ps->c.screen_info->root,
		                       XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
		x_update_monitors_async(&ps->c, &ps->monitors);
	}

	ev_io_init(&ps->xiow, x_event_callback, ConnectionNumber(ps->c.dpy), EV_READ);
	ev_io_start(ps->loop, &ps->xiow);
	ev_init(&ps->unredir_timer, tmout_unredir_callback);
	ev_init(&ps->draw_timer, draw_callback);

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
	ev_prepare_init(&ps->event_check, handle_x_events_ev);
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

	ps->wm = wm_new();
	wm_import_start(ps->wm, &ps->c, ps->atoms, ps->c.screen_info->root, NULL);

	ps->command_builder = command_builder_new();

	// wm_complete_import will set event masks on the root window, but its event
	// mask is missing things we need, so we need to set it again.
	e = xcb_request_check(
	    ps->c.c, xcb_change_window_attributes_checked(
	                 ps->c.c, ps->c.screen_info->root, XCB_CW_EVENT_MASK,
	                 (const uint32_t[]){XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
	                                    XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
	                                    XCB_EVENT_MASK_PROPERTY_CHANGE}));
	if (e) {
		log_error_x_error(&ps->c, e, "Failed to setup root window event mask");
		free(e);
		goto err;
	}

	// Query the size of the root window. We need the size information before any
	// window can be managed.
	auto r = XCB_AWAIT(xcb_get_geometry, &ps->c, ps->c.screen_info->root);
	if (!r) {
		log_fatal("Failed to get geometry of the root window");
		goto err;
	}
	ps->root_width = r->width;
	ps->root_height = r->height;
	free(r);
	rebuild_screen_reg(ps);

	if (session_redirection_mode(ps) == XCB_COMPOSITE_REDIRECT_MANUAL && compositor_running) {
		// Don't take the overlay when there is another compositor
		// running, so we don't disrupt it.

		// If we are printing diagnostic, we will continue a bit further
		// to get more diagnostic information, otherwise we will exit.
		log_fatal("Another composite manager is already running");
		goto err;
	}

	if (session_redirection_mode(ps) == XCB_COMPOSITE_REDIRECT_MANUAL && !init_overlay(ps)) {
		goto err;
	}

	if (session_redirection_mode(ps) == XCB_COMPOSITE_REDIRECT_AUTOMATIC) {
		// We are here if we don't really function as a compositor, so we are not
		// taking over the screen, and we don't need to register as a compositor

		// We still need to know what the overlay window is, so we won't be
		// confused when we get events for the overlay window. Since
		// CompositeGetOverlayWindow also maps the overlay, we need to immediately
		// release it.
		auto get_overlay =
		    xcb_composite_get_overlay_window(ps->c.c, ps->c.screen_info->root);
		XCB_AWAIT_VOID(xcb_composite_release_overlay_window, &ps->c,
		               ps->c.screen_info->root);
		auto overlay_reply =
		    xcb_composite_get_overlay_window_reply(ps->c.c, get_overlay, NULL);
		if (overlay_reply) {
			ps->overlay = overlay_reply->overlay_win;
			free(overlay_reply);
		}

		// If we are in debug mode, we need to create a window for rendering if
		// the backend supports presenting.

		// The old backends doesn't have a automatic redirection mode
		log_info("The compositor is started in automatic redirection mode.");

		if (backend_can_present(ps->o.backend)) {
			// If the backend has `present`, we couldn't be in automatic
			// redirection mode unless we are in debug mode.
			assert(ps->o.debug_mode);
			if (!init_debug_window(ps)) {
				goto err;
			}
		}
	}

	ps->pending_updates = true;

	write_pid(ps);

	if (fork && stderr_logger) {
		// Remove the stderr logger if we will fork
		log_remove_target_tls(stderr_logger);
	}
	return ps;
err:
	render_statistics_destroy(&ps->render_stats);
	options_destroy(&ps->o);
	free(ps);
	return NULL;
}

// NOLINTEND(readability-function-cognitive-complexity)

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
	command_builder_free(ps->command_builder);
	ps->command_builder = NULL;

	if (ps->file_watch_handle) {
		file_watch_destroy(ps->loop, ps->file_watch_handle);
		ps->file_watch_handle = NULL;
	}

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

	wm_stack_foreach(ps->wm, cursor) {
		auto w = wm_ref_deref(cursor);
		if (w != NULL) {
			free_win_res(ps, w);
		}
	}

	// Free blacklists
	options_destroy(&ps->o);
	c2_state_free(ps->c2_state);

	pixman_region32_fini(&ps->screen_reg);

	x_free_monitor_info(&ps->monitors);

	render_statistics_destroy(&ps->render_stats);

	// Release custom window shaders
	struct shader_info *shader, *tmp;
	HASH_ITER(hh, ps->shaders, shader, tmp) {
		HASH_DEL(ps->shaders, shader);
		assert(shader->backend_shader == NULL);
		free(shader);
	}
	HASH_ITER2(ps->shader_sources, source) {
		HASH_DEL(ps->shader_sources, source);
		free((void *)source->source);
		free(source);
	}

	// Release overlay window
	if (ps->overlay && session_redirection_mode(ps) == XCB_COMPOSITE_REDIRECT_MANUAL) {
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

	if (ps->x_region != XCB_NONE) {
		xcb_xfixes_destroy_region(ps->c.c, ps->x_region);
		ps->x_region = XCB_NONE;
	}

	// backend is deinitialized in unredirect()
	assert(ps->backend_data == NULL);

	// Flush all events
	xcb_aux_sync(ps->c.c);
	ev_io_stop(ps->loop, &ps->xiow);
	destroy_atoms(ps->atoms);

	// Stop libev event handlers
	ev_timer_stop(ps->loop, &ps->unredir_timer);
	ev_timer_stop(ps->loop, &ps->draw_timer);
	ev_prepare_stop(ps->loop, &ps->event_check);
	ev_signal_stop(ps->loop, &ps->usr1_signal);
	ev_signal_stop(ps->loop, &ps->int_signal);

	// The X connection could hold references to wm if there are pending async
	// requests. Therefore the wm must be freed after the X connection.
	free_x_connection(&ps->c);
	wm_free(ps->wm);
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
		ps->render_queued = true;
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

/// Early initialization of logging system. To catch early logs, especially
/// from backend entrypoint functions.
static void __attribute__((constructor(201))) init_early_logging(void) {
	log_init_tls();
	log_set_level_tls(LOG_LEVEL_WARN);

	auto stderr_logger = stderr_logger_new();
	if (stderr_logger != NULL) {
		log_add_target_tls(stderr_logger);
	}
}

/**
 * The function that everybody knows.
 */
int PICOM_MAIN(int argc, char **argv) {
	// Set locale so window names with special characters are interpreted
	// correctly
	setlocale(LC_ALL, "");

	parse_debug_options(&global_debug_options);

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
			quit = true;
			goto end;
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
	end:
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

#ifdef UNIT_TEST
static void unittest_setup(void) {
	log_init_tls();
	// log_add_target_tls(stderr_logger_new());
}
void (*test_h_unittest_setup)(void) = unittest_setup;
#endif
