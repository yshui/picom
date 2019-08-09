// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2019, Yuxuan Shui <yshuiv7@gmail.com>

#include <stdio.h>

#include <X11/Xlibint.h>
#include <X11/extensions/sync.h>
#include <xcb/randr.h>
#include <xcb/damage.h>

#include "atom.h"
#include "common.h"
#include "compiler.h"
#include "compton.h"
#include "event.h"
#include "utils.h"
#include "region.h"
#include "config.h"
#include "x.h"
#include "win.h"
#include "log.h"

/// Event handling with X is complicated. Handling events with other events possibly
/// in-flight is no good. Because your internal state won't be up to date. Also, querying
/// the server while events are in-flight is not good. Because events later in the queue
/// might container information you are querying. Thus those events will cause you to do
/// unnecessary updates even when you already have the latest information (remember, you
/// made the query when those events were already in the queue. so the reply you got is
/// more up-to-date than the events). Also, handling events when other client are making
/// concurrent requests is not good. Because the server states are changing without you
/// knowning them. This is super racy, and can cause lots of potential problems.
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
/// TODO the things described above

/**
 * Get a window's name from window ID.
 */
static inline const char *ev_window_name(session_t *ps, xcb_window_t wid) {
	char *name = "";
	if (wid) {
		name = "(Failed to get title)";
		if (ps->root == wid) {
			name = "(Root window)";
		} else if (ps->overlay == wid) {
			name = "(Overlay)";
		} else {
			auto w = find_managed_win(ps, wid);
			if (!w) {
				w = find_toplevel(ps, wid);
			}

			if (w) {
				win_get_name(ps, w);
				if (w->name) {
					name = w->name;
				}
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

static inline const char *ev_name(session_t *ps, xcb_generic_event_t *ev) {
	static char buf[128];
	switch (ev->response_type & 0x7f) {
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

	if (ps->damage_event + XCB_DAMAGE_NOTIFY == ev->response_type)
		return "Damage";

	if (ps->shape_exists && ev->response_type == ps->shape_event)
		return "ShapeNotify";

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
	assert(ev->parent == ps->root);
	add_win_top(ps, ev->window);
}

/// Handle configure event of a regular window
static void configure_win(session_t *ps, xcb_configure_notify_event_t *ce) {
	auto w = find_win(ps, ce->window);
	region_t damage;
	pixman_region32_init(&damage);

	if (!w) {
		return;
	}

	if (!w->managed) {
		restack_above(ps, w, ce->above_sibling);
		return;
	}

	auto mw = (struct managed_win *)w;

	if (mw->state == WSTATE_UNMAPPED || mw->state == WSTATE_UNMAPPING ||
	    mw->state == WSTATE_DESTROYING) {
		// Only restack the window to make sure we can handle future restack
		// notification correctly
		restack_above(ps, w, ce->above_sibling);
	} else {
		restack_above(ps, w, ce->above_sibling);
		bool factor_change = false;
		win_extents(mw, &damage);

		// If window geometry change, free old extents
		if (mw->g.x != ce->x || mw->g.y != ce->y || mw->g.width != ce->width ||
		    mw->g.height != ce->height || mw->g.border_width != ce->border_width) {
			factor_change = true;
		}

		mw->g.x = ce->x;
		mw->g.y = ce->y;

		if (mw->g.width != ce->width || mw->g.height != ce->height ||
		    mw->g.border_width != ce->border_width) {
			log_trace("Window size changed, %dx%d -> %dx%d", mw->g.width,
			          mw->g.height, ce->width, ce->height);
			mw->g.width = ce->width;
			mw->g.height = ce->height;
			mw->g.border_width = ce->border_width;
			win_on_win_size_change(ps, mw);
			win_update_bounding_shape(ps, mw);
		}

		region_t new_extents;
		pixman_region32_init(&new_extents);
		win_extents(mw, &new_extents);
		pixman_region32_union(&damage, &damage, &new_extents);
		pixman_region32_fini(&new_extents);

		if (factor_change) {
			win_on_factor_change(ps, mw);
			add_damage(ps, &damage);
			win_update_screen(ps, mw);
		}
	}

	pixman_region32_fini(&damage);

	// override_redirect flag cannot be changed after window creation, as far
	// as I know, so there's no point to re-match windows here.
	mw->a.override_redirect = ce->override_redirect;
}

static inline void ev_configure_notify(session_t *ps, xcb_configure_notify_event_t *ev) {
	log_debug("{ send_event: %d, id: %#010x, above: %#010x, override_redirect: %d }",
	          ev->event, ev->window, ev->above_sibling, ev->override_redirect);
	if (ev->window == ps->root) {
		configure_root(ps, ev->width, ev->height);
	} else {
		configure_win(ps, ev);
	}
}

static inline void ev_destroy_notify(session_t *ps, xcb_destroy_notify_event_t *ev) {
	auto w = find_win(ps, ev->window);
	if (w) {
		if (w->managed) {
			unmap_win(ps, (struct managed_win **)&w, true);
		} else {
			destroy_unmanaged_win(ps, &w);
		}
	}
}

static inline void ev_map_notify(session_t *ps, xcb_map_notify_event_t *ev) {
	map_win_by_id(ps, ev->window);
	// FocusIn/Out may be ignored when the window is unmapped, so we must
	// recheck focus here
	ps->pending_updates = true; // to update focus
}

static inline void ev_unmap_notify(session_t *ps, xcb_unmap_notify_event_t *ev) {
	auto w = find_managed_win(ps, ev->window);
	if (w) {
		unmap_win(ps, &w, false);
	}
}

static inline void ev_reparent_notify(session_t *ps, xcb_reparent_notify_event_t *ev) {
	log_debug("{ new_parent: %#010x, override_redirect: %d }", ev->parent,
	          ev->override_redirect);

	if (ev->parent == ps->root) {
		// X will generate reparent notifiy even if the parent didn't actually
		// change (i.e. reparent again to current parent). So we check if that's
		// the case
		auto w = find_win(ps, ev->window);
		if (w) {
			// This window has already been reparented to root before,
			// so we don't need to create a new window for it, we just need to
			// move it to the top
			restack_top(ps, w);
		} else {
			add_win_top(ps, ev->window);
		}
	} else {
		// otherwise, find and destroy the window first
		auto w = find_win(ps, ev->window);
		if (w) {
			if (w->managed) {
				unmap_win(ps, (struct managed_win **)&w, true);
			} else {
				destroy_unmanaged_win(ps, &w);
			}
		}

		// Reset event mask in case something wrong happens
		xcb_change_window_attributes(
		    ps->c, ev->window, XCB_CW_EVENT_MASK,
		    (const uint32_t[]){determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN)});

		// Check if the window is an undetected client window
		// Firstly, check if it's a known client window
		if (!find_toplevel(ps, ev->window)) {
			// If not, look for its frame window
			auto w_top = find_toplevel2(ps, ev->parent);
			// If found, and the client window has not been determined, or its
			// frame may not have a correct client, continue
			if (w_top &&
			    (!w_top->client_win || w_top->client_win == w_top->base.id)) {
				// If it has WM_STATE, mark it the client window
				if (wid_has_prop(ps, ev->window, ps->atoms->aWM_STATE)) {
					w_top->wmwin = false;
					win_unmark_client(ps, w_top);
					win_mark_client(ps, w_top, ev->window);
				}
				// Otherwise, watch for WM_STATE on it
				else {
					xcb_change_window_attributes(
					    ps->c, ev->window, XCB_CW_EVENT_MASK,
					    (const uint32_t[]){
					        determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN) |
					        XCB_EVENT_MASK_PROPERTY_CHANGE});
				}
			}
		}
	}
}

static inline void ev_circulate_notify(session_t *ps, xcb_circulate_notify_event_t *ev) {
	auto w = find_win(ps, ev->window);

	if (!w)
		return;

	if (ev->place == PlaceOnTop) {
		restack_top(ps, w);
	} else {
		restack_bottom(ps, w);
	}
}

static inline void expose_root(session_t *ps, const rect_t *rects, int nrects) {
	region_t region;
	pixman_region32_init_rects(&region, rects, nrects);
	add_damage(ps, &region);
}

static inline void ev_expose(session_t *ps, xcb_expose_event_t *ev) {
	if (ev->window == ps->root || (ps->overlay && ev->window == ps->overlay)) {
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

static inline void ev_property_notify(session_t *ps, xcb_property_notify_event_t *ev) {
	if (unlikely(log_get_level_tls() <= LOG_LEVEL_TRACE)) {
		// Print out changed atom
		xcb_get_atom_name_reply_t *reply =
		    xcb_get_atom_name_reply(ps->c, xcb_get_atom_name(ps->c, ev->atom), NULL);
		const char *name = "?";
		int name_len = 1;
		if (reply) {
			name = xcb_get_atom_name_name(reply);
			name_len = xcb_get_atom_name_name_length(reply);
		}

		log_debug("{ atom = %.*s }", name_len, name);
		free(reply);
	}

	if (ps->root == ev->window) {
		if (ps->o.use_ewmh_active_win &&
		    ps->atoms->a_NET_ACTIVE_WINDOW == ev->atom) {
			// to update focus
			ps->pending_updates = true;
		} else {
			// Destroy the root "image" if the wallpaper probably changed
			if (x_is_root_back_pixmap_atom(ps, ev->atom)) {
				root_damaged(ps);
			}
		}

		// Unconcerned about any other proprties on root window
		return;
	}

	// If WM_STATE changes
	if (ev->atom == ps->atoms->aWM_STATE) {
		// Check whether it could be a client window
		if (!find_toplevel(ps, ev->window)) {
			// Reset event mask anyway
			xcb_change_window_attributes(ps->c, ev->window, XCB_CW_EVENT_MASK,
			                             (const uint32_t[]){determine_evmask(
			                                 ps, ev->window, WIN_EVMODE_UNKNOWN)});

			auto w_top = find_toplevel2(ps, ev->window);
			// Initialize client_win as early as possible
			if (w_top &&
			    (!w_top->client_win || w_top->client_win == w_top->base.id) &&
			    wid_has_prop(ps, ev->window, ps->atoms->aWM_STATE)) {
				w_top->wmwin = false;
				win_unmark_client(ps, w_top);
				win_mark_client(ps, w_top, ev->window);
			}
		}
	}

	// If _NET_WM_WINDOW_TYPE changes... God knows why this would happen, but
	// there are always some stupid applications. (#144)
	if (ev->atom == ps->atoms->a_NET_WM_WINDOW_TYPE) {
		struct managed_win *w = NULL;
		if ((w = find_toplevel(ps, ev->window)))
			win_update_wintype(ps, w);
	}

	// If _NET_WM_OPACITY changes
	if (ev->atom == ps->atoms->a_NET_WM_WINDOW_OPACITY) {
		auto w = find_managed_win(ps, ev->window) ?: find_toplevel(ps, ev->window);
		if (w) {
			win_update_opacity_prop(ps, w);
			// we cannot receive OPACITY change when window is destroyed
			assert(w->state != WSTATE_DESTROYING);
			if (w->state == WSTATE_MAPPED) {
				// See the winstate_t transition table
				w->state = WSTATE_FADING;
			}
			w->opacity_target = win_calc_opacity_target(ps, w, false);
		}
	}

	// If frame extents property changes
	if (ps->o.frame_opacity > 0 && ev->atom == ps->atoms->a_NET_FRAME_EXTENTS) {
		auto w = find_toplevel(ps, ev->window);
		if (w) {
			win_update_frame_extents(ps, w, ev->window);
			// If frame extents change, the window needs repaint
			add_damage_from_win(ps, w);
		}
	}

	// If name changes
	if (ps->o.track_wdata &&
	    (ps->atoms->aWM_NAME == ev->atom || ps->atoms->a_NET_WM_NAME == ev->atom)) {
		auto w = find_toplevel(ps, ev->window);
		if (w && 1 == win_get_name(ps, w)) {
			win_on_factor_change(ps, w);
		}
	}

	// If class changes
	if (ps->o.track_wdata && ps->atoms->aWM_CLASS == ev->atom) {
		auto w = find_toplevel(ps, ev->window);
		if (w) {
			win_get_class(ps, w);
			win_on_factor_change(ps, w);
		}
	}

	// If role changes
	if (ps->o.track_wdata && ps->atoms->aWM_WINDOW_ROLE== ev->atom) {
		auto w = find_toplevel(ps, ev->window);
		if (w && 1 == win_get_role(ps, w)) {
			win_on_factor_change(ps, w);
		}
	}

	// If _COMPTON_SHADOW changes
	if (ps->o.respect_prop_shadow && ps->atoms->a_COMPTON_SHADOW == ev->atom) {
		auto w = find_managed_win(ps, ev->window);
		if (w) {
			win_update_prop_shadow(ps, w);
		}
	}

	// If a leader property changes
	if ((ps->o.detect_transient && ps->atoms->aWM_TRANSIENT_FOR == ev->atom) ||
	    (ps->o.detect_client_leader && ps->atoms->aWM_CLIENT_LEADER == ev->atom)) {
		auto w = find_toplevel(ps, ev->window);
		if (w) {
			win_update_leader(ps, w);
		}
	}

	// Check for other atoms we are tracking
	for (latom_t *platom = ps->track_atom_lst; platom; platom = platom->next) {
		if (platom->atom == ev->atom) {
			auto w = find_managed_win(ps, ev->window);
			if (!w)
				w = find_toplevel(ps, ev->window);
			if (w)
				win_on_factor_change(ps, w);
			break;
		}
	}
}

static inline void repair_win(session_t *ps, struct managed_win *w) {
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE)
		return;

	region_t parts;
	pixman_region32_init(&parts);

	if (!w->ever_damaged) {
		win_extents(w, &parts);
		set_ignore_cookie(
		    ps, xcb_damage_subtract(ps->c, w->damage, XCB_NONE, XCB_NONE));
	} else {
		xcb_xfixes_region_t tmp = x_new_id(ps->c);
		xcb_xfixes_create_region(ps->c, tmp, 0, NULL);
		set_ignore_cookie(ps, xcb_damage_subtract(ps->c, w->damage, XCB_NONE, tmp));
		x_fetch_region(ps->c, tmp, &parts);
		xcb_xfixes_destroy_region(ps->c, tmp);
		pixman_region32_translate(&parts, w->g.x + w->g.border_width,
		                          w->g.y + w->g.border_width);
	}

	w->ever_damaged = true;
	w->pixmap_damaged = true;

	// Why care about damage when screen is unredirected?
	// We will force full-screen repaint on redirection.
	if (!ps->redirected) {
		pixman_region32_fini(&parts);
		return;
	}

	// Remove the part in the damage area that could be ignored
	if (w->reg_ignore && win_is_region_ignore_valid(ps, w))
		pixman_region32_subtract(&parts, &parts, w->reg_ignore);

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
	// Mark the old border_size as damaged
	region_t tmp = win_get_bounding_shape_global_by_val(w);
	add_damage(ps, &tmp);
	pixman_region32_fini(&tmp);

	win_update_bounding_shape(ps, w);

	// Mark the new border_size as damaged
	tmp = win_get_bounding_shape_global_by_val(w);
	add_damage(ps, &tmp);
	pixman_region32_fini(&tmp);

	w->reg_ignore_valid = false;
}

static inline void
ev_selection_clear(session_t *ps, xcb_selection_clear_event_t attr_unused *ev) {
	// The only selection we own is the _NET_WM_CM_Sn selection.
	// If we lose that one, we should exit.
	log_fatal("Another composite manager started and took the _NET_WM_CM_Sn "
	          "selection.");
	quit_compton(ps);
}

void ev_handle(session_t *ps, xcb_generic_event_t *ev) {
	if ((ev->response_type & 0x7f) != KeymapNotify) {
		discard_ignore(ps, ev->full_sequence);
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
	auto proc = XESetWireToEvent(ps->dpy, ev->response_type, 0);
	if (proc) {
		XESetWireToEvent(ps->dpy, ev->response_type, proc);
		XEvent dummy;

		// Stop Xlib from complaining about lost sequence numbers.
		// proc might also just be Xlib internal event processing functions, and
		// because they probably won't see all X replies, they will complain about
		// missing sequence numbers.
		//
		// We only need the low 16 bits
		ev->sequence = (uint16_t)(LastKnownRequestProcessed(ps->dpy) & 0xffff);
		proc(ps->dpy, &dummy, (xEvent *)ev);
	}

	// XXX redraw needs to be more fine grained
	queue_redraw(ps);

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
	case 0: ev_xcb_error(ps, (xcb_generic_error_t *)ev); break;
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
