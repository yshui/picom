// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2019, Yuxuan Shui <yshuiv7@gmail.com>

#include <stdint.h>
#include <stdio.h>

#include <X11/Xlibint.h>
#include <X11/extensions/sync.h>
#include <xcb/damage.h>
#include <xcb/randr.h>
#include <xcb/xcb_event.h>

#include "atom.h"
#include "c2.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "event.h"
#include "log.h"
#include "picom.h"
#include "region.h"
#include "utils.h"
#include "win.h"
#include "win_defs.h"
#include "x.h"
#include "xcb/xfixes.h"

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
		if (session_get_x_connection(ps)->screen_info->root == wid) {
			name = "(Root window)";
		} else if (session_get_overlay(ps) == wid) {
			name = "(Overlay)";
		} else {
			auto w = find_managed_win(ps, wid);
			if (!w) {
				w = session_find_toplevel(ps, wid);
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
		if (session_get_damage_extention_event(ps) + XCB_DAMAGE_NOTIFY ==
		    ev->response_type) {
			return ((xcb_damage_notify_event_t *)ev)->drawable;
		}

		if (session_has_shape_extension(ps) &&
		    ev->response_type == session_get_shape_extention_event(ps)) {
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

	if (session_get_damage_extention_event(ps) + XCB_DAMAGE_NOTIFY == ev->response_type) {
		return "Damage";
	}

	if (session_has_shape_extension(ps) &&
	    ev->response_type == session_get_shape_extention_event(ps)) {
		return "ShapeNotify";
	}

	if (session_has_xsync_extension(ps)) {
		int o = ev->response_type - session_get_xsync_extention_event(ps);
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
	session_mark_updates_pending(ps);
}

static inline void ev_focus_out(session_t *ps, xcb_focus_out_event_t *ev) {
	log_debug("{ mode: %s, detail: %s }\n", ev_focus_mode_name(ev),
	          ev_focus_detail_name(ev));
	session_mark_updates_pending(ps);
}

static inline void ev_create_notify(session_t *ps, xcb_create_notify_event_t *ev) {
	if (ev->parent == session_get_x_connection(ps)->screen_info->root) {
		session_add_win_top(ps, ev->window);
	}
}

/// Handle configure event of a regular window
static void configure_win(session_t *ps, xcb_configure_notify_event_t *ce) {
	auto w = session_find_win(ps, ce->window);

	if (!w) {
		return;
	}

	if (!w->managed) {
		session_restack_above(ps, w, ce->above_sibling);
		return;
	}

	auto mw = (struct managed_win *)w;

	session_restack_above(ps, w, ce->above_sibling);

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
		session_mark_updates_pending(ps);

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
		win_update_monitor(session_get_monitors(ps), mw);
	}

	// override_redirect flag cannot be changed after window creation, as far
	// as I know, so there's no point to re-match windows here.
	mw->a.override_redirect = ce->override_redirect;
}

static inline void ev_configure_notify(session_t *ps, xcb_configure_notify_event_t *ev) {
	log_debug("{ send_event: %d, id: %#010x, above: %#010x, override_redirect: %d }",
	          ev->event, ev->window, ev->above_sibling, ev->override_redirect);
	if (ev->window == session_get_x_connection(ps)->screen_info->root) {
		set_root_flags(ps, ROOT_FLAGS_CONFIGURED);
	} else {
		configure_win(ps, ev);
	}
}

static inline void ev_destroy_notify(session_t *ps, xcb_destroy_notify_event_t *ev) {
	auto w = session_find_win(ps, ev->window);
	auto mw = session_find_toplevel(ps, ev->window);
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
		if (!w->managed || !((struct managed_win *)w)->to_paint) {
			// If the window wasn't managed, or was already not rendered,
			// we don't need to fade it out.
			destroy_win_finish(ps, w);
		}
		return;
	}
	if (mw != NULL) {
		win_unmark_client(ps, mw);
		win_set_flags(mw, WIN_FLAGS_CLIENT_STALE);
		session_mark_updates_pending(ps);
		return;
	}
	log_debug("Received a destroy notify from an unknown window, %#010x", ev->window);
}

static inline void ev_map_notify(session_t *ps, xcb_map_notify_event_t *ev) {
	// Unmap overlay window if it got mapped but we are currently not
	// in redirected state.
	auto overlay = session_get_overlay(ps);
	auto c = session_get_x_connection(ps);
	if (overlay && ev->window == overlay && !session_is_redirected(ps)) {
		log_debug("Overlay is mapped while we are not redirected");
		auto e = xcb_request_check(c->c, xcb_unmap_window_checked(c->c, overlay));
		if (e) {
			log_error("Failed to unmap the overlay window");
			free(e);
		}
		// We don't track the overlay window, so we can return
		return;
	}

	auto w = find_managed_win(ps, ev->window);
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
	session_mark_updates_pending(ps);        // to update focus
}

static inline void ev_unmap_notify(session_t *ps, xcb_unmap_notify_event_t *ev) {
	auto w = find_managed_win(ps, ev->window);
	if (w) {
		unmap_win_start(ps, w);
	}
}

static inline void ev_reparent_notify(session_t *ps, xcb_reparent_notify_event_t *ev) {
	log_debug("Window %#010x has new parent: %#010x, override_redirect: %d",
	          ev->window, ev->parent, ev->override_redirect);
	auto w_top = session_find_toplevel(ps, ev->window);
	if (w_top) {
		win_unmark_client(ps, w_top);
		win_set_flags(w_top, WIN_FLAGS_CLIENT_STALE);
		session_mark_updates_pending(ps);
	}

	auto c = session_get_x_connection(ps);
	if (ev->parent == c->screen_info->root) {
		// X will generate reparent notify even if the parent didn't actually
		// change (i.e. reparent again to current parent). So we check if that's
		// the case
		auto w = session_find_win(ps, ev->window);
		if (w) {
			// This window has already been reparented to root before,
			// so we don't need to create a new window for it, we just need to
			// move it to the top
			session_restack_top(ps, w);
		} else {
			session_add_win_top(ps, ev->window);
		}
	} else {
		// otherwise, find and destroy the window first
		{
			auto w = session_find_win(ps, ev->window);
			if (w) {
				if (w->managed) {
					auto mw = (struct managed_win *)w;
					// Usually, damage for unmapped windows
					// are added in `paint_preprocess`, when
					// a window was painted before and isn't
					// anymore. But since we are reparenting
					// the window here, we would lose track
					// of the `to_paint` information. So we
					// just add damage here.
					if (mw->to_paint) {
						add_damage_from_win(ps, mw);
					}
					// Emulating what X server does: a destroyed
					// window is always unmapped first.
					if (mw->state == WSTATE_MAPPED) {
						unmap_win_start(ps, mw);
					}
				}
				// Window reparenting is unlike normal window destruction,
				// This window is going to be rendered under another
				// parent, so we don't fade here.
				destroy_win_start(ps, w);
				destroy_win_finish(ps, w);
			}
		}

		// Reset event mask in case something wrong happens
		uint32_t evmask = determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN);

		auto atoms = session_get_atoms(ps);
		if (!wid_has_prop(c->c, ev->window, atoms->aWM_STATE)) {
			log_debug("Window %#010x doesn't have WM_STATE property, it is "
			          "probably not a client window. But we will listen for "
			          "property change in case it gains one.",
			          ev->window);
			evmask |= XCB_EVENT_MASK_PROPERTY_CHANGE;
		} else {
			auto w_real_top = find_managed_window_or_parent(ps, ev->parent);
			if (w_real_top) {
				log_debug("Mark window %#010x (%s) as having a stale "
				          "client",
				          w_real_top->base.id, w_real_top->name);
				win_set_flags(w_real_top, WIN_FLAGS_CLIENT_STALE);
				session_mark_updates_pending(ps);
			} else {
				log_debug("parent %#010x not found", ev->parent);
			}
		}
		XCB_AWAIT_VOID(xcb_change_window_attributes, c->c, ev->window,
		               XCB_CW_EVENT_MASK, (const uint32_t[]){evmask});
	}
}

static inline void ev_circulate_notify(session_t *ps, xcb_circulate_notify_event_t *ev) {
	auto w = session_find_win(ps, ev->window);

	if (!w) {
		return;
	}

	if (ev->place == PlaceOnTop) {
		session_restack_top(ps, w);
	} else {
		session_restack_bottom(ps, w);
	}
}

static inline void ev_expose(session_t *ps, xcb_expose_event_t *ev) {
	auto overlay = session_get_overlay(ps);
	if (ev->window == session_get_x_connection(ps)->screen_info->root ||
	    (overlay && ev->window == overlay)) {
		region_t region;
		pixman_region32_init_rect(&region, ev->x, ev->y, ev->width, ev->height);
		add_damage(ps, &region);
		pixman_region32_fini(&region);
	}
}

static inline void ev_property_notify(session_t *ps, xcb_property_notify_event_t *ev) {
	auto options = session_get_options(ps);
	auto atoms = session_get_atoms(ps);
	auto c = session_get_x_connection(ps);
	auto c2_state = session_get_c2(ps);
	if (unlikely(log_get_level_tls() <= LOG_LEVEL_TRACE)) {
		// Print out changed atom
		xcb_get_atom_name_reply_t *reply =
		    xcb_get_atom_name_reply(c->c, xcb_get_atom_name(c->c, ev->atom), NULL);
		const char *name = "?";
		int name_len = 1;
		if (reply) {
			name = xcb_get_atom_name_name(reply);
			name_len = xcb_get_atom_name_name_length(reply);
		}

		log_debug("{ atom = %.*s }", name_len, name);
		free(reply);
	}

	if (c->screen_info->root == ev->window) {
		if (options->use_ewmh_active_win && atoms->a_NET_ACTIVE_WINDOW == ev->atom) {
			// to update focus
			session_mark_updates_pending(ps);
		} else {
			// Destroy the root "image" if the wallpaper probably changed
			if (x_is_root_back_pixmap_atom(atoms, ev->atom)) {
				root_damaged(ps);
			}
		}

		// Unconcerned about any other properties on root window
		return;
	}

	session_mark_updates_pending(ps);
	// If WM_STATE changes
	if (ev->atom == atoms->aWM_STATE) {
		// Check whether it could be a client window
		if (!session_find_toplevel(ps, ev->window)) {
			// Reset event mask anyway
			const uint32_t evmask =
			    determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN);
			XCB_AWAIT_VOID(xcb_change_window_attributes, c->c, ev->window,
			               XCB_CW_EVENT_MASK, (const uint32_t[]){evmask});

			auto w_top = find_managed_window_or_parent(ps, ev->window);
			// ev->window might have not been managed yet, in that case w_top
			// would be NULL.
			if (w_top) {
				win_set_flags(w_top, WIN_FLAGS_CLIENT_STALE);
			}
		}
		return;
	}

	// If _NET_WM_WINDOW_TYPE changes... God knows why this would happen, but
	// there are always some stupid applications. (#144)
	if (ev->atom == atoms->a_NET_WM_WINDOW_TYPE) {
		struct managed_win *w = session_find_toplevel(ps, ev->window);
		if (w) {
			win_set_property_stale(w, ev->atom);
		}
	}

	if (ev->atom == atoms->a_NET_WM_BYPASS_COMPOSITOR) {
		// Unnecessary until we remove the queue_redraw in ev_handle
		queue_redraw(ps);
	}

	// If _NET_WM_WINDOW_OPACITY changes
	if (ev->atom == atoms->a_NET_WM_WINDOW_OPACITY) {
		auto w = find_managed_win(ps, ev->window)
		             ?: session_find_toplevel(ps, ev->window);
		if (w) {
			win_set_property_stale(w, ev->atom);
		}
	}

	// If frame extents property changes
	if (ev->atom == atoms->a_NET_FRAME_EXTENTS) {
		auto w = session_find_toplevel(ps, ev->window);
		if (w) {
			win_set_property_stale(w, ev->atom);
		}
	}

	// If name changes
	if (atoms->aWM_NAME == ev->atom || atoms->a_NET_WM_NAME == ev->atom) {
		auto w = session_find_toplevel(ps, ev->window);
		if (w) {
			win_set_property_stale(w, ev->atom);
		}
	}

	// If class changes
	if (atoms->aWM_CLASS == ev->atom) {
		auto w = session_find_toplevel(ps, ev->window);
		if (w) {
			win_set_property_stale(w, ev->atom);
		}
	}

	// If role changes
	if (atoms->aWM_WINDOW_ROLE == ev->atom) {
		auto w = session_find_toplevel(ps, ev->window);
		if (w) {
			win_set_property_stale(w, ev->atom);
		}
	}

	// If _COMPTON_SHADOW changes
	if (atoms->a_COMPTON_SHADOW == ev->atom) {
		auto w = find_managed_win(ps, ev->window);
		if (w) {
			win_set_property_stale(w, ev->atom);
		}
	}

	// If a leader property changes
	if ((options->detect_transient && atoms->aWM_TRANSIENT_FOR == ev->atom) ||
	    (options->detect_client_leader && atoms->aWM_CLIENT_LEADER == ev->atom)) {
		auto w = session_find_toplevel(ps, ev->window);
		if (w) {
			win_set_property_stale(w, ev->atom);
		}
	}

	if (!options->no_ewmh_fullscreen && ev->atom == atoms->a_NET_WM_STATE) {
		auto w = session_find_toplevel(ps, ev->window);
		if (w) {
			win_set_property_stale(w, ev->atom);
		}
	}

	// Check for other atoms we are tracking
	if (c2_state_is_property_tracked(c2_state, ev->atom)) {
		bool change_is_on_client = false;
		auto w = find_managed_win(ps, ev->window);
		if (!w) {
			w = session_find_toplevel(ps, ev->window);
			change_is_on_client = true;
		}
		if (w) {
			c2_window_state_mark_dirty(c2_state, &w->c2_state, ev->atom,
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

	auto options = session_get_options(ps);
	auto c = session_get_x_connection(ps);
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
		    c->c, xcb_damage_subtract_checked(c->c, w->damage, XCB_NONE, XCB_NONE));
		if (e) {
			if (options->show_all_xerrors) {
				x_print_error(e->sequence, e->major_code, e->minor_code,
				              e->error_code);
			}
			free(e);
		}
		win_extents(w, &parts);

		// We only binds the window pixmap once the window is damaged.
		win_set_flags(w, WIN_FLAGS_PIXMAP_STALE);
		session_mark_updates_pending(ps);
	} else {
		xcb_xfixes_region_t damage_region = session_get_damage_ring(ps)->x_region;
		auto cookie = xcb_damage_subtract(c->c, w->damage, XCB_NONE, damage_region);
		if (!options->show_all_xerrors) {
			set_ignore_cookie(c, cookie);
		}
		x_fetch_region(c, damage_region, &parts);
		pixman_region32_translate(&parts, w->g.x + w->g.border_width,
		                          w->g.y + w->g.border_width);
	}

	log_trace("Mark window %#010x (%s) as having received damage", w->base.id, w->name);
	w->ever_damaged = true;
	w->pixmap_damaged = true;

	// Why care about damage when screen is unredirected?
	// We will force full-screen repaint on redirection.
	if (!session_is_redirected(ps)) {
		pixman_region32_fini(&parts);
		return;
	}

	// Remove the part in the damage area that could be ignored
	if (w->reg_ignore && session_is_win_region_ignore_valid(ps, w)) {
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

	auto w = find_managed_win(ps, de->drawable);

	if (!w) {
		return;
	}

	repair_win(ps, w);
}

static inline void ev_shape_notify(session_t *ps, xcb_shape_notify_event_t *ev) {
	auto w = find_managed_win(ps, ev->affected_window);
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
	session_mark_updates_pending(ps);
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
	auto c = session_get_x_connection(ps);
	if (XCB_EVENT_RESPONSE_TYPE(ev) != KeymapNotify) {
		x_discard_pending(c, ev->full_sequence);
	}

	xcb_window_t wid = ev_window(ps, ev);
	if (ev->response_type != session_get_damage_extention_event(ps) + XCB_DAMAGE_NOTIFY) {
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
	auto proc = XESetWireToEvent(c->dpy, response_type, 0);
	if (proc) {
		XESetWireToEvent(c->dpy, response_type, proc);
		XEvent dummy;

		// Stop Xlib from complaining about lost sequence numbers.
		// proc might also just be Xlib internal event processing functions, and
		// because they probably won't see all X replies, they will complain about
		// missing sequence numbers.
		//
		// We only need the low 16 bits
		uint16_t seq = ev->sequence;
		ev->sequence = (uint16_t)(LastKnownRequestProcessed(c->dpy) & 0xffff);
		proc(c->dpy, &dummy, (xEvent *)ev);
		// Restore the sequence number
		ev->sequence = seq;
	}

	// XXX redraw needs to be more fine grained
	queue_redraw(ps);

	// We intentionally ignore events sent via SendEvent. Those events has the 8th bit
	// of response_type set, meaning they will match none of the cases below.
	switch (ev->response_type) {
	case FocusIn: ev_focus_in(ps, (xcb_focus_in_event_t *)ev); break;
	case FocusOut: ev_focus_out(ps, (xcb_focus_out_event_t *)ev); break;
	case CreateNotify: ev_create_notify(ps, (xcb_create_notify_event_t *)ev); break;
	case ConfigureNotify:
		ev_configure_notify(ps, (xcb_configure_notify_event_t *)ev);
		break;
	case DestroyNotify:
		ev_destroy_notify(ps, (xcb_destroy_notify_event_t *)ev);
		break;
	case MapNotify: ev_map_notify(ps, (xcb_map_notify_event_t *)ev); break;
	case UnmapNotify: ev_unmap_notify(ps, (xcb_unmap_notify_event_t *)ev); break;
	case ReparentNotify:
		ev_reparent_notify(ps, (xcb_reparent_notify_event_t *)ev);
		break;
	case CirculateNotify:
		ev_circulate_notify(ps, (xcb_circulate_notify_event_t *)ev);
		break;
	case Expose: ev_expose(ps, (xcb_expose_event_t *)ev); break;
	case PropertyNotify:
		ev_property_notify(ps, (xcb_property_notify_event_t *)ev);
		break;
	case SelectionClear:
		ev_selection_clear(ps, (xcb_selection_clear_event_t *)ev);
		break;
	case 0: x_handle_error(c, (xcb_generic_error_t *)ev); break;
	default:
		if (session_has_shape_extension(ps) &&
		    ev->response_type == session_get_shape_extention_event(ps)) {
			ev_shape_notify(ps, (xcb_shape_notify_event_t *)ev);
			break;
		}
		if (session_has_randr_extension(ps) &&
		    ev->response_type == (session_get_randr_extention_event(ps) +
		                          XCB_RANDR_SCREEN_CHANGE_NOTIFY)) {
			set_root_flags(ps, ROOT_FLAGS_SCREEN_CHANGE);
			break;
		}
		if (session_get_damage_extention_event(ps) + XCB_DAMAGE_NOTIFY ==
		    ev->response_type) {
			ev_damage_notify(ps, (xcb_damage_notify_event_t *)ev);
			break;
		}
	}
}
