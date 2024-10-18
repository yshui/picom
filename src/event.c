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
		if (ps->c.e.damage_event + XCB_DAMAGE_NOTIFY == ev->response_type) {
			return ((xcb_damage_notify_event_t *)ev)->drawable;
		}

		if (ps->c.e.has_shape && ev->response_type == ps->c.e.shape_event) {
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

	if (ps->c.e.damage_event + XCB_DAMAGE_NOTIFY == ev->response_type) {
		return "DAMAGE_NOTIFY";
	}

	if (ps->c.e.has_shape && ev->response_type == ps->c.e.shape_event) {
		return "SHAPE_NOTIFY";
	}

	if (ps->c.e.has_sync) {
		int o = ev->response_type - ps->c.e.sync_event;
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

struct ev_ewmh_active_win_request {
	struct x_async_request_base base;
	session_t *ps;
};

/// Update current active window based on EWMH _NET_ACTIVE_WIN.
///
/// Does not change anything if we fail to get the attribute or the window
/// returned could not be found.
static void
update_ewmh_active_win(struct x_connection *c, struct x_async_request_base *req_base,
                       const xcb_raw_generic_event_t *reply_or_error) {
	auto ps = ((struct ev_ewmh_active_win_request *)req_base)->ps;
	free(req_base);

	ps->pending_focus_check = false;

	if (reply_or_error == NULL) {
		// Connection shutdown
		return;
	}

	if (reply_or_error->response_type == 0) {
		log_error("Failed to get _NET_ACTIVE_WINDOW: %s",
		          x_strerror(c, (xcb_generic_error_t *)reply_or_error));
		return;
	}

	// Search for the window
	auto reply = (const xcb_get_property_reply_t *)reply_or_error;
	if (reply->type == XCB_NONE || xcb_get_property_value_length(reply) < 4) {
		log_debug("EWMH _NET_ACTIVE_WINDOW not set.");
		return;
	}

	auto wid = *(xcb_window_t *)xcb_get_property_value(reply);
	log_debug("EWMH _NET_ACTIVE_WINDOW is %#010x", wid);

	// Mark the window focused. No need to unfocus the previous one.
	auto cursor = wm_find_by_client(ps->wm, wid);
	wm_ref_set_focused(ps->wm, cursor);
	ps->pending_updates = true;
	log_debug("%#010x (%s) focused.", wid, win_wm_ref_name(cursor));
}

struct ev_recheck_focus_request {
	struct x_async_request_base base;
	session_t *ps;
};

/**
 * Recheck currently focused window and set its <code>w->focused</code>
 * to true.
 *
 * @param ps current session
 * @return struct _win of currently focused window, NULL if not found
 */
static void recheck_focus(struct x_connection *c, struct x_async_request_base *req_base,
                          const xcb_raw_generic_event_t *reply_or_error) {
	auto ps = ((struct ev_ewmh_active_win_request *)req_base)->ps;
	free(req_base);

	ps->pending_focus_check = false;

	if (reply_or_error == NULL) {
		// Connection shutdown
		return;
	}

	// Determine the currently focused window so we can apply appropriate
	// opacity on it
	if (reply_or_error->response_type == 0) {
		// Not able to get input focus means very not good things...
		auto e = (xcb_generic_error_t *)reply_or_error;
		log_error_x_error(c, e, "Failed to get focused window.");
		return;
	}

	auto reply = (const xcb_get_input_focus_reply_t *)reply_or_error;
	xcb_window_t wid = reply->focus;
	log_debug("Current focused window is %#010x", wid);
	if (wid == XCB_NONE || wid == XCB_INPUT_FOCUS_POINTER_ROOT ||
	    wid == ps->c.screen_info->root) {
		// Focus is not on a toplevel.
		return;
	}

	auto cursor = wm_find(ps->wm, wid);
	assert(cursor != NULL || !wm_is_consistent(ps->wm));

	if (cursor != NULL) {
		cursor = wm_ref_toplevel_of(ps->wm, cursor);
		assert(cursor != NULL || !wm_is_consistent(ps->wm));
	}

	// And we set the focus state here
	wm_ref_set_focused(ps->wm, cursor);
	ps->pending_updates = true;
	log_debug("%#010x (%s) focused.", wid, win_wm_ref_name(cursor));
}

void ev_update_focused(struct session *ps) {
	if (ps->pending_focus_check) {
		return;
	}

	if (ps->o.use_ewmh_active_win) {
		auto req = ccalloc(1, struct ev_ewmh_active_win_request);
		req->base.sequence =
		    xcb_get_property(ps->c.c, 0, ps->c.screen_info->root,
		                     ps->atoms->a_NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW, 0, 1)
		        .sequence;
		req->base.callback = update_ewmh_active_win;
		req->ps = ps;
		x_await_request(&ps->c, &req->base);
		log_debug("Started async request to get _NET_ACTIVE_WINDOW");
	} else {
		auto req = ccalloc(1, struct ev_recheck_focus_request);
		req->base.sequence = xcb_get_input_focus(ps->c.c).sequence;
		req->base.callback = recheck_focus;
		req->ps = ps;
		x_await_request(&ps->c, &req->base);
		log_debug("Started async request to recheck focus");
	}

	ps->pending_focus_check = true;
}

static inline void ev_focus_change(session_t *ps) {
	if (ps->o.use_ewmh_active_win) {
		// Not using focus_in/focus_out events.
		return;
	}
	ev_update_focused(ps);
}

static inline void ev_focus_in(session_t *ps, xcb_focus_in_event_t *ev) {
	log_debug("{ mode: %s, detail: %s }", ev_focus_mode_name(ev),
	          ev_focus_detail_name(ev));
	ev_focus_change(ps);
}

static inline void ev_focus_out(session_t *ps, xcb_focus_out_event_t *ev) {
	log_debug("{ mode: %s, detail: %s }", ev_focus_mode_name(ev),
	          ev_focus_detail_name(ev));
	ev_focus_change(ps);
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

	if (!cursor) {
		if (wm_is_consistent(ps->wm)) {
			log_error("Configure event received for unknown window %#010x",
			          ce->window);
			assert(false);
		}
		return;
	}

	wm_stack_move_to_above(ps->wm, cursor, ce->above_sibling);

	auto w = wm_ref_deref(cursor);
	if (!w) {
		log_debug("Window %#010x is unmanaged.", ce->window);
		return;
	}

	bool changed = win_set_pending_geometry(w, win_geometry_from_configure_notify(ce)) |
	               (w->a.override_redirect != ce->override_redirect);
	w->a.override_redirect = ce->override_redirect;

	if (w->state == WSTATE_MAPPED) {
		ps->pending_updates |= changed;
	}
}

static inline void ev_configure_notify(session_t *ps, xcb_configure_notify_event_t *ev) {
	log_debug("{ event: %#010x, id: %#010x, above: %#010x, override_redirect: %d }",
	          ev->event, ev->window, ev->above_sibling, ev->override_redirect);

	if (ps->overlay && ev->window == ps->overlay) {
		return;
	}

	if (ev->window == ps->c.screen_info->root) {
		configure_root(ps);
	} else {
		if (ev->window == ev->event) {
			return;
		}

		configure_win(ps, ev);
	}
}

static inline void ev_destroy_notify(session_t *ps, xcb_destroy_notify_event_t *ev) {
	log_debug("{ event: %#010x, id: %#010x }", ev->event, ev->window);
	// If we hit an ABA problem, it is possible to get a DestroyNotify event from a
	// parent for its child, but not from the child for itself.
	if (ev->event != ev->window) {
		wm_disconnect(ps->wm, ev->window, ev->event, XCB_NONE);
	} else {
		wm_destroy(ps->wm, ev->window);
	}
}

static inline void ev_map_notify(session_t *ps, xcb_map_notify_event_t *ev) {
	if (ev->window == ev->event) {
		return;
	}

	// Unmap overlay window if it got mapped but we are currently not
	// in redirected state.
	if (ps->overlay && ev->window == ps->overlay) {
		if (!ps->redirected) {
			log_debug("Overlay is mapped while we are not redirected");
			auto succeeded =
			    XCB_AWAIT_VOID(xcb_unmap_window, &ps->c, ps->overlay);
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

	if (ev->event == ev->window) {
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
	if (ev->event == ev->window) {
		return;
	}
	if (ev->parent != ev->event) {
		wm_disconnect(ps->wm, ev->window, ev->event, ev->parent);
	} else {
		wm_reparent(ps->wm, &ps->c, ps->atoms, ev->window, ev->parent);
	}
}

static inline void ev_circulate_notify(session_t *ps, xcb_circulate_notify_event_t *ev) {
	if (ev->event == ev->window) {
		return;
	}

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
}

static inline void ev_expose(session_t *ps, xcb_expose_event_t *ev) {
	if (ev->window != ps->c.screen_info->root &&
	    (ps->overlay == XCB_NONE || ev->window != ps->overlay)) {
		return;
	}

	if (ev->count == 0) {
		force_repaint(ps);
	}
}

static inline void ev_property_notify(session_t *ps, xcb_property_notify_event_t *ev) {
	log_debug("{ atom = %#010x, window = %#010x, state = %d }", ev->atom, ev->window,
	          ev->state);
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
			ev_update_focused(ps);
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
				x_print_error(&ps->c, e->sequence, e->major_code,
				              e->minor_code, e->error_code);
			}
			free(e);
		}
		win_extents(w, &parts);
		log_debug("Window %#010x (%s) has been damaged the first time", win_id(w),
		          w->name);
	} else {
		auto cookie = xcb_damage_subtract(ps->c.c, w->damage, XCB_NONE, ps->x_region);
		if (!ps->o.show_all_xerrors) {
			x_set_error_action_ignore(&ps->c, cookie);
		}
		x_fetch_region(&ps->c, ps->x_region, &parts);
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
	if (ev->response_type != ps->c.e.damage_event + XCB_DAMAGE_NOTIFY) {
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
		if (ps->c.e.has_shape && ev->response_type == ps->c.e.shape_event) {
			ev_shape_notify(ps, (xcb_shape_notify_event_t *)ev);
			break;
		}
		if (ps->c.e.has_randr &&
		    ev->response_type ==
		        (ps->c.e.randr_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY)) {
			x_update_monitors_async(&ps->c, &ps->monitors);
			break;
		}
		if (ps->c.e.damage_event + XCB_DAMAGE_NOTIFY == ev->response_type) {
			ev_damage_notify(ps, (xcb_damage_notify_event_t *)ev);
			break;
		}
	}
}
