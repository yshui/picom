// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2019, Yuxuan Shui <yshuiv7@gmail.com>

#include <stdint.h>
#include <stdio.h>

#include <X11/Xlibint.h>
#include <X11/extensions/sync.h>
#include <xcb/damage.h>
#include <xcb/randr.h>
#include <xcb/xcb_event.h>
#include <xcb/xproto.h>

#include "atom.h"
#include "c2.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "event.h"
#include "log.h"
#include "picom.h"
#include "region.h"
#include "types.h"
#include "utils.h"
#include "win.h"
#include "win_defs.h"
#include "wm.h"
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
			auto w = wm_find_managed(ps->wm, wid);
			if (!w) {
				w = wm_find_by_client(ps->wm, wid);
			}

			if (w && w->name) {
				name = w->name;
			}
		}
	}
	return name;
}

static inline xcb_window_t attr_pure ev_window(session_t *ps, xcb_generic_event_t *ev) {
	switch (ev->response_type) {
	case FocusIn:
	case FocusOut: return ((xcb_focus_in_event_t *)ev)->event;
	case CreateNotify: return ((xcb_create_notify_event_t *)ev)->window;
	case ConfigureNotify: return ((xcb_configure_notify_event_t *)ev)->window;
	case DestroyNotify: return ((xcb_destroy_notify_event_t *)ev)->window;
	case MapNotify: return ((xcb_map_notify_event_t *)ev)->window;
	case UnmapNotify: return ((xcb_unmap_notify_event_t *)ev)->window;
	case ReparentNotify: return ((xcb_reparent_notify_event_t *)ev)->window;
	case CirculateNotify: return ((xcb_circulate_notify_event_t *)ev)->window;
	case Expose: return ((xcb_expose_event_t *)ev)->window;
	case PropertyNotify: return ((xcb_property_notify_event_t *)ev)->window;
	case ClientMessage: return ((xcb_client_message_event_t *)ev)->window;
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
	case s: return #s;

static inline const char *ev_name(session_t *ps, xcb_generic_event_t *ev) {
	static char buf[128];
	switch (XCB_EVENT_RESPONSE_TYPE(ev)) {
		CASESTRRET(FocusIn);
		CASESTRRET(FocusOut);
		CASESTRRET(CreateNotify);
		CASESTRRET(ConfigureNotify);
		CASESTRRET(DestroyNotify);
		CASESTRRET(MapNotify);
		CASESTRRET(UnmapNotify);
		CASESTRRET(ReparentNotify);
		CASESTRRET(CirculateNotify);
		CASESTRRET(Expose);
		CASESTRRET(PropertyNotify);
		CASESTRRET(ClientMessage);
	}

	if (ps->damage_event + XCB_DAMAGE_NOTIFY == ev->response_type) {
		return "Damage";
	}

	if (ps->shape_exists && ev->response_type == ps->shape_event) {
		return "ShapeNotify";
	}

	if (ps->xsync_exists) {
		int o = ev->response_type - ps->xsync_event;
		switch (o) {
			CASESTRRET(XSyncCounterNotify);
			CASESTRRET(XSyncAlarmNotify);
		}
	}

	sprintf(buf, "Event %d", ev->response_type);

	return buf;
}

static inline const char *attr_pure ev_focus_mode_name(xcb_focus_in_event_t *ev) {
	switch (ev->mode) {
		CASESTRRET(NotifyNormal);
		CASESTRRET(NotifyWhileGrabbed);
		CASESTRRET(NotifyGrab);
		CASESTRRET(NotifyUngrab);
	}

	return "Unknown";
}

static inline const char *attr_pure ev_focus_detail_name(xcb_focus_in_event_t *ev) {
	switch (ev->detail) {
		CASESTRRET(NotifyAncestor);
		CASESTRRET(NotifyVirtual);
		CASESTRRET(NotifyInferior);
		CASESTRRET(NotifyNonlinear);
		CASESTRRET(NotifyNonlinearVirtual);
		CASESTRRET(NotifyPointer);
		CASESTRRET(NotifyPointerRoot);
		CASESTRRET(NotifyDetailNone);
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
	if (ev->parent == ps->c.screen_info->root) {
		wm_stack_add_top(ps->wm, ev->window);
		ps->pending_updates = true;
		return;
	}

	auto w = wm_find_managed(ps->wm, ev->parent);
	if (w == NULL) {
		// The parent window is not a toplevel window, we don't care about it.
		// This can happen if a toplevel is reparented somewhere, and more events
		// were generated from it before we can unsubscribe.
		return;
	}
	// A direct child of a toplevel, subscribe to property changes so we can
	// know if WM_STATE is set on this window.
	wm_subwin_add_and_subscribe(ps->wm, &ps->c, ev->window, ev->parent);
	if (w->client_win == XCB_NONE || w->client_win == w->base.id) {
		win_set_flags(w, WIN_FLAGS_CLIENT_STALE);
		ps->pending_updates = true;
	}
}

/// Handle configure event of a regular window
static void configure_win(session_t *ps, xcb_configure_notify_event_t *ce) {
	auto w = wm_find(ps->wm, ce->window);

	if (!w) {
		return;
	}

	if (!w->managed) {
		wm_stack_move_above(ps->wm, w, ce->above_sibling);
		return;
	}

	auto mw = (struct managed_win *)w;

	wm_stack_move_above(ps->wm, w, ce->above_sibling);

	// We check against pending_g here, because there might have been multiple
	// configure notifies in this cycle, or the window could receive multiple updates
	// while it's unmapped.
	bool position_changed = mw->pending_g.x != ce->x || mw->pending_g.y != ce->y;
	bool size_changed = mw->pending_g.width != ce->width ||
	                    mw->pending_g.height != ce->height ||
	                    mw->pending_g.border_width != ce->border_width;
	if (position_changed || size_changed) {
		// Queue pending updates
		win_set_flags(mw, WIN_FLAGS_FACTOR_CHANGED);
		// TODO(yshui) don't set pending_updates if the window is not
		// visible/mapped
		ps->pending_updates = true;

		// At least one of the following if's is true
		if (position_changed) {
			log_trace("Window position changed, %dx%d -> %dx%d", mw->g.x,
			          mw->g.y, ce->x, ce->y);
			mw->pending_g.x = ce->x;
			mw->pending_g.y = ce->y;
			win_set_flags(mw, WIN_FLAGS_POSITION_STALE);
		}

		if (size_changed) {
			log_trace("Window size changed, %dx%d -> %dx%d", mw->g.width,
			          mw->g.height, ce->width, ce->height);
			mw->pending_g.width = ce->width;
			mw->pending_g.height = ce->height;
			mw->pending_g.border_width = ce->border_width;
			win_set_flags(mw, WIN_FLAGS_SIZE_STALE);
		}

		// Recalculate which monitor this window is on
		win_update_monitor(&ps->monitors, mw);
	}

	// override_redirect flag cannot be changed after window creation, as far
	// as I know, so there's no point to re-match windows here.
	mw->a.override_redirect = ce->override_redirect;
}

static inline void ev_configure_notify(session_t *ps, xcb_configure_notify_event_t *ev) {
	log_debug("{ send_event: %d, id: %#010x, above: %#010x, override_redirect: %d }",
	          ev->event, ev->window, ev->above_sibling, ev->override_redirect);
	if (ev->window == ps->c.screen_info->root) {
		set_root_flags(ps, ROOT_FLAGS_CONFIGURED);
	} else {
		configure_win(ps, ev);
	}
}

static inline void ev_destroy_notify(session_t *ps, xcb_destroy_notify_event_t *ev) {
	auto subwin = wm_subwin_find(ps->wm, ev->window);
	if (subwin) {
		wm_subwin_remove(ps->wm, subwin);
	}

	auto w = wm_find(ps->wm, ev->window);
	auto mw = wm_find_by_client(ps->wm, ev->window);
	if (mw && mw->client_win == mw->base.id) {
		// We only want _real_ frame window
		assert(&mw->base == w);
		mw = NULL;
	}

	// A window can't be a client window and a top-level window at the same time,
	// so only one of `w` and `mw` can be non-NULL
	assert(w == NULL || mw == NULL);

	if (w != NULL) {
		destroy_win_start(ps, w);
		if (!w->managed || !win_as_managed(w)->to_paint) {
			// If the window wasn't managed, or was already not rendered,
			// we don't need to fade it out.
			if (w->managed) {
				win_skip_fading(win_as_managed(w));
			}
			destroy_win_finish(ps, w);
		}
		return;
	}
	if (mw != NULL) {
		win_unmark_client(mw);
		win_set_flags(mw, WIN_FLAGS_CLIENT_STALE);
		ps->pending_updates = true;
		return;
	}
	log_debug("Received a destroy notify from an unknown window, %#010x", ev->window);
}

static inline void ev_map_notify(session_t *ps, xcb_map_notify_event_t *ev) {
	// Unmap overlay window if it got mapped but we are currently not
	// in redirected state.
	if (ps->overlay && ev->window == ps->overlay && !ps->redirected) {
		log_debug("Overlay is mapped while we are not redirected");
		auto e = xcb_request_check(
		    ps->c.c, xcb_unmap_window_checked(ps->c.c, ps->overlay));
		if (e) {
			log_error("Failed to unmap the overlay window");
			free(e);
		}
		// We don't track the overlay window, so we can return
		return;
	}

	auto w = wm_find_managed(ps->wm, ev->window);
	if (!w) {
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
	auto w = wm_find_managed(ps->wm, ev->window);
	if (w) {
		unmap_win_start(ps, w);
	}
}

static inline void ev_reparent_notify(session_t *ps, xcb_reparent_notify_event_t *ev) {
	log_debug("Window %#010x has new parent: %#010x, override_redirect: %d",
	          ev->window, ev->parent, ev->override_redirect);

	auto old_toplevel = wm_find_by_client(ps->wm, ev->window);
	auto old_w = wm_find(ps->wm, ev->window);
	auto old_subwin = wm_subwin_find(ps->wm, ev->window);
	auto new_toplevel = wm_find_managed(ps->wm, ev->parent);
	xcb_window_t old_parent = XCB_NONE;
	if (old_subwin) {
		old_parent = old_subwin->toplevel;
	} else if (old_w) {
		old_parent = ps->c.screen_info->root;
	}
	if (old_toplevel && old_toplevel->client_win == old_toplevel->base.id) {
		// We only want _real_ frame window, meaning old_toplevel must be
		// a parent of `ev->window`.
		old_toplevel = NULL;
	}
	// A window can't be a toplevel and a subwin at the same time
	assert(old_w == NULL || old_subwin == NULL);

	log_debug("old toplevel: %p, old subwin: %p, new toplevel: %p, old parent: "
	          "%#010x, new parent: %#010x, root window: %#010x",
	          old_w, old_subwin, new_toplevel, old_parent, ev->parent,
	          ps->c.screen_info->root);

	if (old_w == NULL && old_subwin == NULL && new_toplevel == NULL &&
	    ev->parent != ps->c.screen_info->root) {
		// The window is neither a toplevel nor a subwin, and the new parent is
		// neither a root nor a toplevel, we don't care about this window.
		// This can happen if a window is reparented to somewhere irrelevant, but
		// more events from it are generated before we can unsubscribe.
		return;
	}

	if (ev->parent == old_parent) {
		// Parent unchanged, but if the parent is root, we need to move the window
		// to the top of the window stack
		if (old_w) {
			// root -> root reparent, we just need to move it to the top
			log_debug("Restack %#010x (%s) to top", old_w->id,
			          win_get_name_if_managed(old_w));
			wm_stack_move_to_top(ps->wm, old_w);
		}
		return;
	}

	if (old_toplevel) {
		assert(old_subwin != NULL);
		assert(old_subwin->toplevel == old_toplevel->base.id);
		win_unmark_client(old_toplevel);
		win_set_flags(old_toplevel, WIN_FLAGS_CLIENT_STALE);
		ps->pending_updates = true;
	}

	if (old_w) {
		// A toplevel is reparented, so it is no longer a toplevel. We need to
		// destroy the existing toplevel.
		if (old_w->managed) {
			auto mw = (struct managed_win *)old_w;
			// Usually, damage for unmapped windows are added in
			// `paint_preprocess`, when a window was painted before and isn't
			// anymore. But since we are reparenting the window here, we would
			// lose track of the `to_paint` information. So we just add damage
			// here.
			if (mw->to_paint) {
				add_damage_from_win(ps, mw);
			}
			// Emulating what X server does: a destroyed
			// window is always unmapped first.
			if (mw->state == WSTATE_MAPPED) {
				unmap_win_start(ps, mw);
			}
		}
		destroy_win_start(ps, old_w);
		if (old_w->managed) {
			win_skip_fading(win_as_managed(old_w));
		}
		destroy_win_finish(ps, old_w);
	}

	// We need to guarantee a subwin exists iff it has a valid toplevel.
	auto new_subwin = old_subwin;
	if (new_subwin != NULL && new_toplevel == NULL) {
		wm_subwin_remove_and_unsubscribe(ps->wm, &ps->c, new_subwin);
		new_subwin = NULL;
	} else if (new_subwin == NULL && new_toplevel != NULL) {
		new_subwin =
		    wm_subwin_add_and_subscribe(ps->wm, &ps->c, ev->window, ev->parent);
	}
	if (new_subwin) {
		new_subwin->toplevel = new_toplevel->base.id;
		if (new_toplevel->client_win == XCB_NONE ||
		    new_toplevel->client_win == new_toplevel->base.id) {
			win_set_flags(new_toplevel, WIN_FLAGS_CLIENT_STALE);
			ps->pending_updates = true;
		}
	}

	if (ev->parent == ps->c.screen_info->root) {
		// New parent is root, add a toplevel;
		assert(old_w == NULL);
		assert(new_toplevel == NULL);
		wm_stack_add_top(ps->wm, ev->window);
		ps->pending_updates = true;
	}
}

static inline void ev_circulate_notify(session_t *ps, xcb_circulate_notify_event_t *ev) {
	auto w = wm_find(ps->wm, ev->window);

	if (!w) {
		return;
	}

	log_debug("Moving window %#010x (%s) to the %s", w->id,
	          win_get_name_if_managed(w), ev->place == PlaceOnTop ? "top" : "bottom");
	if (ev->place == PlaceOnTop) {
		wm_stack_move_to_top(ps->wm, w);
	} else {
		wm_stack_move_to_bottom(ps->wm, w);
	}
}

static inline void expose_root(session_t *ps, const rect_t *rects, int nrects) {
	region_t region;
	pixman_region32_init_rects(&region, rects, nrects);
	add_damage(ps, &region);
	pixman_region32_fini(&region);
}

static inline void ev_expose(session_t *ps, xcb_expose_event_t *ev) {
	if (ev->window == ps->c.screen_info->root ||
	    (ps->overlay && ev->window == ps->overlay)) {
		int more = ev->count + 1;
		if (ps->n_expose == ps->size_expose) {
			if (ps->expose_rects) {
				ps->expose_rects =
				    crealloc(ps->expose_rects, ps->size_expose + more);
				ps->size_expose += more;
			} else {
				ps->expose_rects = ccalloc(more, rect_t);
				ps->size_expose = more;
			}
		}

		ps->expose_rects[ps->n_expose].x1 = ev->x;
		ps->expose_rects[ps->n_expose].y1 = ev->y;
		ps->expose_rects[ps->n_expose].x2 = ev->x + ev->width;
		ps->expose_rects[ps->n_expose].y2 = ev->y + ev->height;
		ps->n_expose++;

		if (ev->count == 0) {
			expose_root(ps, ps->expose_rects, ps->n_expose);
			ps->n_expose = 0;
		}
	}
}

static inline void ev_subwin_wm_state_changed(session_t *ps, xcb_property_notify_event_t *ev) {
	auto subwin = wm_subwin_find(ps->wm, ev->window);
	if (!subwin) {
		// We only care if a direct child of a toplevel gained/lost WM_STATE
		return;
	}

	enum tristate old_has_wm_state = subwin->has_wm_state;
	subwin->has_wm_state = ev->state == XCB_PROPERTY_DELETE ? TRI_FALSE : TRI_TRUE;
	if (old_has_wm_state == subwin->has_wm_state) {
		if (subwin->has_wm_state == TRI_FALSE) {
			log_warn("Child window %#010x of window %#010x lost WM_STATE a "
			         "second time?",
			         ev->window, subwin->toplevel);
		}
		return;
	}

	auto toplevel = wm_find(ps->wm, subwin->toplevel);
	BUG_ON(toplevel == NULL);
	if (!toplevel->managed) {
		return;
	}

	auto managed = (struct managed_win *)toplevel;
	if (managed->client_win == subwin->id) {
		// 1. This window is the client window of its toplevel, and now it lost
		//    its WM_STATE (implies it must have it before)
		assert(subwin->has_wm_state == TRI_FALSE);
		win_set_flags(managed, WIN_FLAGS_CLIENT_STALE);
	} else if (subwin->has_wm_state == TRI_TRUE) {
		// 2. This window is not the client window of its toplevel, and
		//    now it gained WM_STATE
		if (managed->client_win != XCB_NONE && managed->client_win != toplevel->id) {
			log_warn("Toplevel %#010x already has a client window %#010x, "
			         "but another of its child windows %#010x gained "
			         "WM_STATE.",
			         toplevel->id, managed->client_win, subwin->id);
		} else {
			win_set_flags(managed, WIN_FLAGS_CLIENT_STALE);
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
	auto w = wm_find_by_client(ps->wm, ev->window);
	if (ev->atom == ps->atoms->aWM_STATE) {
		ev_subwin_wm_state_changed(ps, ev);
	}

	if (ev->atom == ps->atoms->a_NET_WM_BYPASS_COMPOSITOR) {
		// Unnecessary until we remove the queue_redraw in ev_handle
		queue_redraw(ps);
	}

	if (w) {
		win_set_property_stale(w, ev->atom);
	}

	if (ev->atom == ps->atoms->a_NET_WM_WINDOW_OPACITY) {
		// We already handle if this is set on the client window, check
		// if this is set on the frame window as well.
		// TODO(yshui) do we really need this?
		auto toplevel = wm_find_managed(ps->wm, ev->window);
		if (toplevel) {
			win_set_property_stale(toplevel, ev->atom);
		}
	}

	// Check for other atoms we are tracking
	if (c2_state_is_property_tracked(ps->c2_state, ev->atom)) {
		bool change_is_on_client = false;
		w = wm_find_managed(ps->wm, ev->window);
		if (!w) {
			w = wm_find_by_client(ps->wm, ev->window);
			change_is_on_client = true;
		}
		if (w) {
			c2_window_state_mark_dirty(ps->c2_state, &w->c2_state, ev->atom,
			                           change_is_on_client);
			// Set FACTOR_CHANGED so rules based on properties will be
			// re-evaluated.
			// Don't need to set property stale here, since that only
			// concerns properties we explicitly check.
			win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		}
	}
}

static inline void repair_win(session_t *ps, struct managed_win *w) {
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

		// We only binds the window pixmap once the window is damaged.
		win_set_flags(w, WIN_FLAGS_PIXMAP_STALE);
		ps->pending_updates = true;
	} else {
		auto cookie = xcb_damage_subtract(ps->c.c, w->damage, XCB_NONE,
		                                  ps->damage_ring.x_region);
		if (!ps->o.show_all_xerrors) {
			set_ignore_cookie(&ps->c, cookie);
		}
		x_fetch_region(&ps->c, ps->damage_ring.x_region, &parts);
		pixman_region32_translate(&parts, w->g.x + w->g.border_width,
		                          w->g.y + w->g.border_width);
	}

	log_trace("Mark window %#010x (%s) as having received damage", w->base.id, w->name);
	w->ever_damaged = true;
	w->pixmap_damaged = true;

	// Why care about damage when screen is unredirected?
	// We will force full-screen repaint on redirection.
	if (!ps->redirected) {
		pixman_region32_fini(&parts);
		return;
	}

	// Remove the part in the damage area that could be ignored
	if (w->reg_ignore && win_is_region_ignore_valid(ps, w)) {
		pixman_region32_subtract(&parts, &parts, w->reg_ignore);
	}

	add_damage(ps, &parts);
	pixman_region32_fini(&parts);
}

static inline void ev_damage_notify(session_t *ps, xcb_damage_notify_event_t *de) {
	/*
	if (ps->root == de->drawable) {
	  root_damaged();
	  return;
	} */

	auto w = wm_find_managed(ps->wm, de->drawable);

	if (!w) {
		return;
	}

	repair_win(ps, w);
}

static inline void ev_shape_notify(session_t *ps, xcb_shape_notify_event_t *ev) {
	auto w = wm_find_managed(ps->wm, ev->affected_window);
	if (!w || w->a.map_state == XCB_MAP_STATE_UNMAPPED) {
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
	if (XCB_EVENT_RESPONSE_TYPE(ev) != KeymapNotify) {
		x_discard_pending(&ps->c, ev->full_sequence);
	}

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
	case 0: x_handle_error(&ps->c, (xcb_generic_error_t *)ev); break;
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
