// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2019, Yuxuan Shui <yshuiv7@gmail.com>

#include <stdint.h>
#include <stdio.h>

#include <X11/Xlibint.h>
#include <xcb/damage.h>
#include <xcb/randr.h>
#include <xcb/xcb_event.h>
#include <xcb/xproto.h>

#include <picom/types.h>

#include "atom.h"
#include "c2.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "event.h"
#include "log.h"
#include "picom.h"
#include "region.h"
#include "utils/dynarr.h"
#include "wm/defs.h"
#include "wm/wm.h"
#include "x.h"

/// Event handling with X is complicated. Handling events with other events possibly
/// in-flight is no good. Because your internal state won't be up to date. Also, querying
/// the server while events are in-flight is not good. Because events later in the queue
/// might container information you are querying. Thus those events will cause you to do
/// unnecessary updates even when you already have the latest information (remember, you
/// made the query when those events were already in the queue. so the reply you got is
/// more up-to-date than the events). Also, handling events when other client are making
/// concurrent requests is not good. Because the server states are changing without you
/// knowing them. This is super racy, and can cause lots of potential problems.
///
/// All of above mandates we do these things:
///    1. Grab server when handling events
///    2. Make sure the event queue is empty before we make any query to the server
///
/// Notice (2) has a dependency circle. To handle events, you sometimes need to make
/// queries. But to make queries you have to first handle events.
///
/// To break that circle, we split all event handling into top and bottom halves. The
/// bottom half will just look at the event itself, update as much state as they can
/// without making queries, then queue up necessary works need to be done by the top half.
/// The top half will do all the other necessary updates. Before entering the top half, we
/// grab the server and make sure the event queue is empty.
///
/// When top half finished, we enter the render stage, where no server state should be
/// queried. All rendering should be done with our internal knowledge of the server state.
///
/// P.S. There is another reason to avoid sending any request to the server as much as
/// possible. To make sure requests are sent, flushes are needed. And `xcb_flush`/`XFlush`
/// functions may read more events from the server into their queues. This is
/// undesirable, see the comments on `handle_queued_x_events` in picom.c for more details.

// TODO(yshui) the things described above. This is mostly done, maybe some of
//             the functions here is still making unnecessary queries, we need
//             to do some auditing to be sure.

/**
 * Get a window's name from window ID.
 */
static inline const char *ev_window_name(session_t *ps, xcb_window_t wid) {
	char *name = "";
	if (wid) {
		name = "(Failed to get title)";
		if (ps->c.screen_info->root == wid) {
			name = "(Root window)";
		} else if (ps->overlay == wid) {
			name = "(Overlay)";
		} else {
			auto cursor = wm_find(ps->wm, wid);
			if (!cursor || !wm_ref_deref(cursor)) {
				cursor = wm_find_by_client(ps->wm, wid);
			}

			auto w = cursor ? wm_ref_deref(cursor) : NULL;
			if (w && w->name) {
				name = w->name;
			}
		}
	}
	return name;
}

static inline xcb_window_t attr_pure ev_window(session_t *ps, xcb_generic_event_t *ev) {
	switch (ev->response_type) {
	case XCB_FOCUS_IN:
	case XCB_FOCUS_OUT: return ((xcb_focus_in_event_t *)ev)->event;
	case XCB_CREATE_NOTIFY: return ((xcb_create_notify_event_t *)ev)->window;
	case XCB_CONFIGURE_NOTIFY: return ((xcb_configure_notify_event_t *)ev)->window;
	case XCB_DESTROY_NOTIFY: return ((xcb_destroy_notify_event_t *)ev)->window;
	case XCB_MAP_NOTIFY: return ((xcb_map_notify_event_t *)ev)->window;
	case XCB_UNMAP_NOTIFY: return ((xcb_unmap_notify_event_t *)ev)->window;
	case XCB_REPARENT_NOTIFY: return ((xcb_reparent_notify_event_t *)ev)->window;
	case XCB_CIRCULATE_NOTIFY: return ((xcb_circulate_notify_event_t *)ev)->window;
	case XCB_EXPOSE: return ((xcb_expose_event_t *)ev)->window;
	case XCB_PROPERTY_NOTIFY: return ((xcb_property_notify_event_t *)ev)->window;
	case XCB_CLIENT_MESSAGE: return ((xcb_client_message_event_t *)ev)->window;
	default:
		if (ps->damage_event + XCB_DAMAGE_NOTIFY == ev->response_type) {
			return ((xcb_damage_notify_event_t *)ev)->drawable;
		}

		if (ps->shape_exists && ev->response_type == ps->shape_event) {
			return ((xcb_shape_notify_event_t *)ev)->affected_window;
		}

		return 0;
	}
}

#define CASESTRRET(s)                                                                    \
	case XCB_##s: return #s;

static inline const char *ev_name(session_t *ps, xcb_generic_event_t *ev) {
	static char buf[128];
	switch (XCB_EVENT_RESPONSE_TYPE(ev)) {
		CASESTRRET(FOCUS_IN);
		CASESTRRET(FOCUS_OUT);
		CASESTRRET(CREATE_NOTIFY);
		CASESTRRET(CONFIGURE_NOTIFY);
		CASESTRRET(DESTROY_NOTIFY);
		CASESTRRET(MAP_NOTIFY);
		CASESTRRET(UNMAP_NOTIFY);
		CASESTRRET(REPARENT_NOTIFY);
		CASESTRRET(CIRCULATE_NOTIFY);
		CASESTRRET(EXPOSE);
		CASESTRRET(PROPERTY_NOTIFY);
		CASESTRRET(CLIENT_MESSAGE);
	}

	if (ps->damage_event + XCB_DAMAGE_NOTIFY == ev->response_type) {
		return "DAMAGE_NOTIFY";
	}

	if (ps->shape_exists && ev->response_type == ps->shape_event) {
		return "SHAPE_NOTIFY";
	}

	if (ps->xsync_exists) {
		int o = ev->response_type - ps->xsync_event;
		switch (o) {
			CASESTRRET(SYNC_COUNTER_NOTIFY);
			CASESTRRET(SYNC_ALARM_NOTIFY);
		}
	}

	sprintf(buf, "Event %d", ev->response_type);

	return buf;
}

static inline const char *attr_pure ev_focus_mode_name(xcb_focus_in_event_t *ev) {
#undef CASESTRRET
#define CASESTRRET(s)                                                                    \
	case XCB_NOTIFY_MODE_##s: return #s

	switch (ev->mode) {
		CASESTRRET(NORMAL);
		CASESTRRET(WHILE_GRABBED);
		CASESTRRET(GRAB);
		CASESTRRET(UNGRAB);
	}

	return "Unknown";
}

static inline const char *attr_pure ev_focus_detail_name(xcb_focus_in_event_t *ev) {
#undef CASESTRRET
#define CASESTRRET(s)                                                                    \
	case XCB_NOTIFY_DETAIL_##s: return #s
	switch (ev->detail) {
		CASESTRRET(ANCESTOR);
		CASESTRRET(VIRTUAL);
		CASESTRRET(INFERIOR);
		CASESTRRET(NONLINEAR);
		CASESTRRET(NONLINEAR_VIRTUAL);
		CASESTRRET(POINTER);
		CASESTRRET(POINTER_ROOT);
		CASESTRRET(NONE);
	}

	return "Unknown";
}

#undef CASESTRRET

static inline void ev_focus_in(session_t *ps, xcb_focus_in_event_t *ev) {
	log_debug("{ mode: %s, detail: %s }\n", ev_focus_mode_name(ev),
	          ev_focus_detail_name(ev));
	ps->pending_updates = true;
}

static inline void ev_focus_out(session_t *ps, xcb_focus_out_event_t *ev) {
	log_debug("{ mode: %s, detail: %s }\n", ev_focus_mode_name(ev),
	          ev_focus_detail_name(ev));
	ps->pending_updates = true;
}

static inline void ev_create_notify(session_t *ps, xcb_create_notify_event_t *ev) {
	auto parent = wm_find(ps->wm, ev->parent);
	if (parent == NULL) {
		log_error("Create notify received for window %#010x, but its parent "
		          "window %#010x is not in our tree. Expect malfunction.",
		          ev->window, ev->parent);
		assert(false);
	}
	wm_import_start(ps->wm, &ps->c, ps->atoms, ev->window, parent);
}

/// Handle configure event of a regular window
static void configure_win(session_t *ps, xcb_configure_notify_event_t *ce) {
	auto cursor = wm_find(ps->wm, ce->window);
	auto below = wm_find(ps->wm, ce->above_sibling);

	if (!cursor) {
		if (wm_is_consistent(ps->wm)) {
			log_error("Configure event received for unknown window %#010x",
			          ce->window);
			assert(false);
		}
		return;
	}

	if (below == NULL && ce->above_sibling != XCB_NONE) {
		log_error("Configure event received for window %#010x, but its sibling "
		          "window %#010x is not in our tree. Expect malfunction.",
		          ce->window, ce->above_sibling);
		assert(false);
	} else if (below != NULL) {
		wm_stack_move_to_above(ps->wm, cursor, below);
	} else {
		// above_sibling being XCB_NONE means the window is put at the bottom.
		wm_stack_move_to_end(ps->wm, cursor, true);
	}

	auto w = wm_ref_deref(cursor);
	if (!w) {
		return;
	}

	add_damage_from_win(ps, w);

	// We check against pending_g here, because there might have been multiple
	// configure notifies in this cycle, or the window could receive multiple updates
	// while it's unmapped.
	bool position_changed = w->pending_g.x != ce->x || w->pending_g.y != ce->y;
	bool size_changed = w->pending_g.width != ce->width ||
	                    w->pending_g.height != ce->height ||
	                    w->pending_g.border_width != ce->border_width;
	if (position_changed || size_changed) {
		// Queue pending updates
		win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		// TODO(yshui) don't set pending_updates if the window is not
		// visible/mapped
		ps->pending_updates = true;

		// At least one of the following if's is true
		if (position_changed) {
			log_trace("Window position changed, %dx%d -> %dx%d", w->g.x,
			          w->g.y, ce->x, ce->y);
			w->pending_g.x = ce->x;
			w->pending_g.y = ce->y;
			win_set_flags(w, WIN_FLAGS_POSITION_STALE);
		}

		if (size_changed) {
			log_trace("Window size changed, %dx%d -> %dx%d", w->g.width,
			          w->g.height, ce->width, ce->height);
			w->pending_g.width = ce->width;
			w->pending_g.height = ce->height;
			w->pending_g.border_width = ce->border_width;
			win_set_flags(w, WIN_FLAGS_SIZE_STALE);
		}

		// Recalculate which monitor this window is on
		win_update_monitor(&ps->monitors, w);
	}

	// override_redirect flag cannot be changed after window creation, as far
	// as I know, so there's no point to re-match windows here.
	w->a.override_redirect = ce->override_redirect;
}

static inline void ev_configure_notify(session_t *ps, xcb_configure_notify_event_t *ev) {
	log_debug("{ event: %#010x, id: %#010x, above: %#010x, override_redirect: %d }",
	          ev->event, ev->window, ev->above_sibling, ev->override_redirect);

	if (ps->overlay && ev->window == ps->overlay) {
		return;
	}

	if (ev->window == ps->c.screen_info->root) {
		set_root_flags(ps, ROOT_FLAGS_CONFIGURED);
	} else {
		configure_win(ps, ev);
	}
}

static inline void ev_destroy_notify(session_t *ps, xcb_destroy_notify_event_t *ev) {
	log_debug("{ event: %#010x, id: %#010x }", ev->event, ev->window);
	wm_destroy(ps->wm, ev->window);
}

static inline void ev_map_notify(session_t *ps, xcb_map_notify_event_t *ev) {
	// Unmap overlay window if it got mapped but we are currently not
	// in redirected state.
	if (ps->overlay && ev->window == ps->overlay) {
		if (!ps->redirected) {
			log_debug("Overlay is mapped while we are not redirected");
			auto succeeded =
			    XCB_AWAIT_VOID(xcb_unmap_window, ps->c.c, ps->overlay);
			if (!succeeded) {
				log_error("Failed to unmap the overlay window");
			}
		}
		// We don't track the overlay window, so we can return
		return;
	}

	auto cursor = wm_find(ps->wm, ev->window);
	if (cursor == NULL) {
		if (wm_is_consistent(ps->wm)) {
			log_debug("Map event received for unknown window %#010x, overlay "
			          "is %#010x",
			          ev->window, ps->overlay);
			assert(false);
		}
		return;
	}

	auto w = wm_ref_deref(cursor);
	if (w == NULL) {
		return;
	}
	win_set_flags(w, WIN_FLAGS_MAPPED);
	// We set `ever_damaged` to false here, instead of in `map_win_start`,
	// because we might receive damage events before that function is called
	// (which is called when we handle the `WIN_FLAGS_MAPPED` flag), in
	// which case `repair_win` will be called, which uses `ever_damaged` so
	// it needs to be correct. This also covers the case where the window is
	// unmapped before `map_win_start` is called.
	w->ever_damaged = false;

	// FocusIn/Out may be ignored when the window is unmapped, so we must
	// recheck focus here
	ps->pending_updates = true;        // to update focus
}

static inline void ev_unmap_notify(session_t *ps, xcb_unmap_notify_event_t *ev) {
	if (ps->overlay && ev->window == ps->overlay) {
		return;
	}

	auto cursor = wm_find(ps->wm, ev->window);
	if (cursor == NULL) {
		if (wm_is_consistent(ps->wm)) {
			log_error("Unmap event received for unknown window %#010x", ev->window);
			assert(false);
		}
		return;
	}
	auto w = wm_ref_deref(cursor);
	if (w != NULL) {
		unmap_win_start(w);
	}
}

static inline void ev_reparent_notify(session_t *ps, xcb_reparent_notify_event_t *ev) {
	log_debug("Window %#010x has new parent: %#010x, override_redirect: %d, "
	          "send_event: %#010x",
	          ev->window, ev->parent, ev->override_redirect, ev->event);
	wm_reparent(ps->wm, ev->window, ev->parent);
}

static inline void ev_circulate_notify(session_t *ps, xcb_circulate_notify_event_t *ev) {
	auto cursor = wm_find(ps->wm, ev->window);

	if (cursor == NULL) {
		if (wm_is_consistent(ps->wm)) {
			log_debug("Circulate event received for unknown window %#010x",
			          ev->window);
			assert(false);
		}
		return;
	}

	log_debug("Moving window %#010x (%s) to the %s", ev->window,
	          ev_window_name(ps, ev->window), ev->place == PlaceOnTop ? "top" : "bottom");
	wm_stack_move_to_end(ps->wm, cursor, ev->place == XCB_PLACE_ON_BOTTOM);

	auto w = wm_ref_deref(cursor);
	if (w != NULL) {
		add_damage_from_win(ps, w);
	}
}

static inline void expose_root(session_t *ps, const rect_t *rects, size_t nrects) {
	region_t region;
	pixman_region32_init_rects(&region, rects, (int)nrects);
	add_damage(ps, &region);
	pixman_region32_fini(&region);
}

static inline void ev_expose(session_t *ps, xcb_expose_event_t *ev) {
	if (ev->window == ps->c.screen_info->root ||
	    (ps->overlay && ev->window == ps->overlay)) {
		dynarr_reserve(ps->expose_rects, ev->count + 1);

		rect_t new_rect = {
		    .x1 = ev->x,
		    .y1 = ev->y,
		    .x2 = ev->x + ev->width,
		    .y2 = ev->y + ev->height,
		};
		dynarr_push(ps->expose_rects, new_rect);

		if (ev->count == 0) {
			expose_root(ps, ps->expose_rects, dynarr_len(ps->expose_rects));
			dynarr_clear_pod(ps->expose_rects);
		}
	}
}

static inline void ev_property_notify(session_t *ps, xcb_property_notify_event_t *ev) {
	if (unlikely(log_get_level_tls() <= LOG_LEVEL_TRACE)) {
		// Print out changed atom
		xcb_get_atom_name_reply_t *reply = xcb_get_atom_name_reply(
		    ps->c.c, xcb_get_atom_name(ps->c.c, ev->atom), NULL);
		const char *name = "?";
		int name_len = 1;
		if (reply) {
			name = xcb_get_atom_name_name(reply);
			name_len = xcb_get_atom_name_name_length(reply);
		}

		log_debug("{ atom = %.*s }", name_len, name);
		free(reply);
	}

	if (ps->c.screen_info->root == ev->window) {
		if (ps->o.use_ewmh_active_win && ps->atoms->a_NET_ACTIVE_WINDOW == ev->atom) {
			// to update focus
			ps->pending_updates = true;
		} else {
			// Destroy the root "image" if the wallpaper probably changed
			if (x_is_root_back_pixmap_atom(ps->atoms, ev->atom)) {
				root_damaged(ps);
			}
		}

		// Unconcerned about any other properties on root window
		return;
	}

	ps->pending_updates = true;
	auto cursor = wm_find(ps->wm, ev->window);
	if (cursor == NULL) {
		if (wm_is_consistent(ps->wm)) {
			log_error("Property notify received for unknown window %#010x",
			          ev->window);
			assert(false);
		}
		return;
	}

	auto toplevel_cursor = wm_ref_toplevel_of(ps->wm, cursor);
	if (ev->atom == ps->atoms->aWM_STATE) {
		log_debug("WM_STATE changed for window %#010x (%s): %s", ev->window,
		          ev_window_name(ps, ev->window),
		          ev->state == XCB_PROPERTY_DELETE ? "deleted" : "set");
		wm_set_has_wm_state(ps->wm, cursor, ev->state != XCB_PROPERTY_DELETE);
	}

	if (toplevel_cursor == NULL) {
		assert(!wm_is_consistent(ps->wm));
		return;
	}

	// We only care if the property is set on the toplevel itself, or on its
	// client window if it has one. WM_STATE is an exception, it is handled
	// always because it is what determines if a window is a client window.
	auto client_cursor = wm_ref_client_of(toplevel_cursor) ?: toplevel_cursor;
	if (cursor != client_cursor && cursor != toplevel_cursor) {
		return;
	}

	if (ev->atom == ps->atoms->a_NET_WM_BYPASS_COMPOSITOR) {
		// Unnecessary until we remove the queue_redraw in ev_handle
		queue_redraw(ps);
	}

	auto toplevel = wm_ref_deref(toplevel_cursor);
	if (toplevel) {
		win_set_property_stale(toplevel, ev->atom);
	}

	if (ev->atom == ps->atoms->a_NET_WM_WINDOW_OPACITY && toplevel != NULL) {
		// We already handle if this is set on the client window, check
		// if this is set on the frame window as well.
		// TODO(yshui) do we really need this?
		win_set_property_stale(toplevel, ev->atom);
	}

	// Check for other atoms we are tracking
	if (c2_state_is_property_tracked(ps->c2_state, ev->atom)) {
		bool change_is_on_client = cursor == client_cursor;
		if (toplevel) {
			c2_window_state_mark_dirty(ps->c2_state, &toplevel->c2_state,
			                           ev->atom, change_is_on_client);
			// Set FACTOR_CHANGED so rules based on properties will be
			// re-evaluated.
			// Don't need to set property stale here, since that only
			// concerns properties we explicitly check.
			win_set_flags(toplevel, WIN_FLAGS_FACTOR_CHANGED);
		}
	}
}

static inline void repair_win(session_t *ps, struct win *w) {
	// Only mapped window can receive damages
	assert(w->state == WSTATE_MAPPED || win_check_flags_all(w, WIN_FLAGS_MAPPED));

	region_t parts;
	pixman_region32_init(&parts);

	// If this is the first time this window is damaged, we would redraw the
	// whole window, so we don't need to fetch the damage region. But we still need
	// to make sure the X server receives the DamageSubtract request, hence the
	// `xcb_request_check` here.
	// Otherwise, we fetch the damage regions. That means we will receive a reply
	// from the X server, which implies it has received our DamageSubtract request.
	if (!w->ever_damaged) {
		auto e = xcb_request_check(
		    ps->c.c,
		    xcb_damage_subtract_checked(ps->c.c, w->damage, XCB_NONE, XCB_NONE));
		if (e) {
			if (ps->o.show_all_xerrors) {
				x_print_error(e->sequence, e->major_code, e->minor_code,
				              e->error_code);
			}
			free(e);
		}
		win_extents(w, &parts);
		log_debug("Window %#010x (%s) has been damaged the first time", win_id(w),
		          w->name);
	} else {
		auto cookie = xcb_damage_subtract(ps->c.c, w->damage, XCB_NONE,
		                                  ps->damage_ring.x_region);
		if (!ps->o.show_all_xerrors) {
			x_set_error_action_ignore(&ps->c, cookie);
		}
		x_fetch_region(&ps->c, ps->damage_ring.x_region, &parts);
		pixman_region32_translate(&parts, w->g.x + w->g.border_width,
		                          w->g.y + w->g.border_width);
	}

	log_trace("Mark window %#010x (%s) as having received damage", win_id(w), w->name);
	w->ever_damaged = true;
	w->pixmap_damaged = true;

	// Why care about damage when screen is unredirected?
	// We will force full-screen repaint on redirection.
	if (!ps->redirected) {
		pixman_region32_fini(&parts);
		return;
	}

	// Remove the part in the damage area that could be ignored
	region_t without_ignored;
	pixman_region32_init(&without_ignored);
	if (w->reg_ignore && win_is_region_ignore_valid(ps, w)) {
		pixman_region32_subtract(&without_ignored, &parts, w->reg_ignore);
	}

	add_damage(ps, &without_ignored);
	pixman_region32_fini(&without_ignored);

	pixman_region32_translate(&parts, -w->g.x, -w->g.y);
	pixman_region32_union(&w->damaged, &w->damaged, &parts);
	pixman_region32_fini(&parts);
}

static inline void ev_damage_notify(session_t *ps, xcb_damage_notify_event_t *de) {
	auto cursor = wm_find(ps->wm, de->drawable);

	if (cursor == NULL) {
		log_error("Damage notify received for unknown window %#010x", de->drawable);
		return;
	}

	auto w = wm_ref_deref(cursor);
	if (w != NULL) {
		repair_win(ps, w);
	}
}

static inline void ev_shape_notify(session_t *ps, xcb_shape_notify_event_t *ev) {
	auto cursor = wm_find(ps->wm, ev->affected_window);
	if (cursor == NULL) {
		log_error("Shape notify received for unknown window %#010x",
		          ev->affected_window);
		return;
	}

	auto w = wm_ref_deref(cursor);
	if (w == NULL || w->a.map_state == XCB_MAP_STATE_UNMAPPED) {
		return;
	}

	/*
	 * Empty bounding_shape may indicated an
	 * unmapped/destroyed window, in which case
	 * seemingly BadRegion errors would be triggered
	 * if we attempt to rebuild border_size
	 */
	// Mark the old bounding shape as damaged
	if (!win_check_flags_any(w, WIN_FLAGS_SIZE_STALE | WIN_FLAGS_POSITION_STALE)) {
		region_t tmp = win_get_bounding_shape_global_by_val(w);
		add_damage(ps, &tmp);
		pixman_region32_fini(&tmp);
	}
	w->reg_ignore_valid = false;

	win_set_flags(w, WIN_FLAGS_SIZE_STALE);
	ps->pending_updates = true;
}

static inline void
ev_selection_clear(session_t *ps, xcb_selection_clear_event_t attr_unused *ev) {
	// The only selection we own is the _NET_WM_CM_Sn selection.
	// If we lose that one, we should exit.
	log_fatal("Another composite manager started and took the _NET_WM_CM_Sn "
	          "selection.");
	quit(ps);
}

void ev_handle(session_t *ps, xcb_generic_event_t *ev) {
	xcb_window_t wid = ev_window(ps, ev);
	if (ev->response_type != ps->damage_event + XCB_DAMAGE_NOTIFY) {
		log_debug("event %10.10s serial %#010x window %#010x \"%s\"",
		          ev_name(ps, ev), ev->full_sequence, wid, ev_window_name(ps, wid));
	} else {
		log_trace("event %10.10s serial %#010x window %#010x \"%s\"",
		          ev_name(ps, ev), ev->full_sequence, wid, ev_window_name(ps, wid));
	}

	// Check if a custom XEvent constructor was registered in xlib for this event
	// type, and call it discarding the constructed XEvent if any. XESetWireToEvent
	// might be used by libraries to intercept messages from the X server e.g. the
	// OpenGL lib waiting for DRI2 events.

	// XXX This exists to workaround compton issue #33, #34, #47
	// For even more details, see:
	// https://bugs.freedesktop.org/show_bug.cgi?id=35945
	// https://lists.freedesktop.org/archives/xcb/2011-November/007337.html
	auto response_type = XCB_EVENT_RESPONSE_TYPE(ev);
	auto proc = XESetWireToEvent(ps->c.dpy, response_type, 0);
	if (proc) {
		XESetWireToEvent(ps->c.dpy, response_type, proc);
		XEvent dummy;

		// Stop Xlib from complaining about lost sequence numbers.
		// proc might also just be Xlib internal event processing functions, and
		// because they probably won't see all X replies, they will complain about
		// missing sequence numbers.
		//
		// We only need the low 16 bits
		uint16_t seq = ev->sequence;
		ev->sequence = (uint16_t)(LastKnownRequestProcessed(ps->c.dpy) & 0xffff);
		proc(ps->c.dpy, &dummy, (xEvent *)ev);
		// Restore the sequence number
		ev->sequence = seq;
	}

	// XXX redraw needs to be more fine grained
	queue_redraw(ps);

	// We intentionally ignore events sent via SendEvent. Those events has the 8th bit
	// of response_type set, meaning they will match none of the cases below.
	switch (ev->response_type) {
	case XCB_FOCUS_IN: ev_focus_in(ps, (xcb_focus_in_event_t *)ev); break;
	case XCB_FOCUS_OUT: ev_focus_out(ps, (xcb_focus_out_event_t *)ev); break;
	case XCB_CREATE_NOTIFY:
		ev_create_notify(ps, (xcb_create_notify_event_t *)ev);
		break;
	case XCB_CONFIGURE_NOTIFY:
		ev_configure_notify(ps, (xcb_configure_notify_event_t *)ev);
		break;
	case XCB_DESTROY_NOTIFY:
		ev_destroy_notify(ps, (xcb_destroy_notify_event_t *)ev);
		break;
	case XCB_MAP_NOTIFY: ev_map_notify(ps, (xcb_map_notify_event_t *)ev); break;
	case XCB_UNMAP_NOTIFY: ev_unmap_notify(ps, (xcb_unmap_notify_event_t *)ev); break;
	case XCB_REPARENT_NOTIFY:
		ev_reparent_notify(ps, (xcb_reparent_notify_event_t *)ev);
		break;
	case XCB_CIRCULATE_NOTIFY:
		ev_circulate_notify(ps, (xcb_circulate_notify_event_t *)ev);
		break;
	case XCB_EXPOSE: ev_expose(ps, (xcb_expose_event_t *)ev); break;
	case XCB_PROPERTY_NOTIFY:
		ev_property_notify(ps, (xcb_property_notify_event_t *)ev);
		break;
	case XCB_SELECTION_CLEAR:
		ev_selection_clear(ps, (xcb_selection_clear_event_t *)ev);
		break;
	default:
		if (ps->shape_exists && ev->response_type == ps->shape_event) {
			ev_shape_notify(ps, (xcb_shape_notify_event_t *)ev);
			break;
		}
		if (ps->randr_exists &&
		    ev->response_type == (ps->randr_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY)) {
			set_root_flags(ps, ROOT_FLAGS_SCREEN_CHANGE);
			break;
		}
		if (ps->damage_event + XCB_DAMAGE_NOTIFY == ev->response_type) {
			ev_damage_notify(ps, (xcb_damage_notify_event_t *)ev);
			break;
		}
	}
}
