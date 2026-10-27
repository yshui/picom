// SPDX-License-Identifier: GPL-2.0-or-later
#include <limits.h>
#include <math.h>
#include <string.h>

#include "backend/backend.h"
#include "common.h"
#include "magic_lamp.h"
#include "win.h"

bool win_magic_lamp_busy(const struct win *w) {
	return w->lamp.active || w->lamp.pending;
}

void win_magic_lamp_cancel(struct win *w) {
	w->lamp.active = w->lamp.pending = w->lamp.mesh.enabled = false;
}

static bool cardinal(session_t *ps, xcb_window_t window, xcb_atom_t atom, uint32_t *value) {
	auto prop = x_get_prop(&ps->c, window, atom, 2, XCB_ATOM_CARDINAL, 32);
	bool valid = prop.nitems == 1;
	if (valid) *value = prop.c32[0];
	free(prop.r);
	return valid;
}

static bool has_atom(session_t *ps, xcb_window_t window, xcb_atom_t property, xcb_atom_t atom) {
	auto prop = x_get_prop(&ps->c, window, property, 1024, XCB_ATOM_ATOM, 32);
	bool found = false;
	for (unsigned long i = 0; i < prop.nitems; i++) found |= prop.atom[i] == atom;
	free(prop.r);
	return found;
}

static void refresh_state(session_t *ps, struct win *w) {
	auto l = &w->lamp;
	xcb_window_t client = win_client_id(w, false);
	if (l->client != XCB_NONE && l->client != client) {
		win_magic_lamp_cancel(w);
		l->minimized = false;
	}
	l->client = client;
	l->dirty = false;
	l->initialized = true;
	l->valid = false;
	l->excluded = true;
	if (client == XCB_NONE || w->a.override_redirect) return;

	auto state = x_get_prop(&ps->c, client, ps->atoms->aWM_STATE, 2,
	                        ps->atoms->aWM_STATE, 32);
	l->wm_state = state.nitems == 2 ? state.c32[0] : 0;
	free(state.r);
	// An empty ATOM list is a valid normal state. x_get_prop deliberately
	// discards empty replies, so read this property directly.
	auto states = xcb_get_property_reply(ps->c.c,
	    xcb_get_property(ps->c.c, false, client, ps->atoms->a_NET_WM_STATE,
	                     XCB_ATOM_ATOM, 0, 1024), NULL);
	l->valid = states && states->type == XCB_ATOM_ATOM && states->format == 32 &&
	           states->bytes_after == 0 && l->wm_state != 0;
	l->hidden = l->shaded = false;
	if (l->valid) {
		const xcb_atom_t *atoms = xcb_get_property_value(states);
		for (uint32_t i = 0; i < states->value_len; i++) {
			l->hidden |= atoms[i] == ps->atoms->a_NET_WM_STATE_HIDDEN;
			l->shaded |= atoms[i] == ps->atoms->a_NET_WM_STATE_SHADED;
		}
	}
	free(states);
	auto root = ps->c.screen_info->root;
	l->supported = has_atom(ps, root, ps->atoms->a_NET_SUPPORTED,
	                        ps->atoms->a_NET_WM_STATE_HIDDEN);
	uint32_t desktop = 0, current = 0, showing = 0;
	bool have_desktop = cardinal(ps, client, ps->atoms->a_NET_WM_DESKTOP, &desktop);
	bool have_current = cardinal(ps, root, ps->atoms->a_NET_CURRENT_DESKTOP, &current);
	cardinal(ps, root, ps->atoms->a_NET_SHOWING_DESKTOP, &showing);
	l->excluded = showing != 0 || l->shaded ||
	              (have_desktop && have_current && desktop != UINT32_MAX && desktop != current);
	// Children hidden with a minimized ancestor must not get a second funnel.
	auto parent = x_get_prop(&ps->c, client, ps->atoms->aWM_TRANSIENT_FOR, 1, XCB_ATOM_WINDOW, 32);
	if (parent.nitems == 1 && parent.c32[0] != client && parent.c32[0] != root) {
		l->excluded |= has_atom(ps, parent.c32[0], ps->atoms->a_NET_WM_STATE,
		                        ps->atoms->a_NET_WM_STATE_HIDDEN);
	}
	free(parent.r);
}

static void choose_target(session_t *ps, struct win *w) {
	auto m = &w->lamp.mesh;
	m->origin = (vec2){w->g.x, w->g.y};
	m->size = (vec2){w->widthb, w->heightb};
	auto prop = x_get_prop(&ps->c, w->lamp.client, ps->atoms->a_NET_WM_ICON_GEOMETRY,
	                       5, XCB_ATOM_CARDINAL, 32);
	bool valid = prop.nitems == 4 && prop.c32[2] > 0 && prop.c32[3] > 0 &&
	             prop.c32[2] <= INT16_MAX && prop.c32[3] <= INT16_MAX;
	if (valid) {
		m->target = (vec2){(int32_t)prop.c32[0], (int32_t)prop.c32[1]};
		m->target_size = (vec2){prop.c32[2], prop.c32[3]};
		// Ignore unreasonable stale destinations without overflowing integer masks.
		valid = fabs(m->target.x) <= 65535 && fabs(m->target.y) <= 65535;
	}
	free(prop.r);
	if (valid) {
		rect_t monitor = {0, 0, ps->root_width, ps->root_height};
		for (int i = 0; i < ps->monitors.count; i++) {
			if (pixman_region32_contains_point(&ps->monitors.regions[i],
			                                  (int)m->target.x, (int)m->target.y, NULL)) {
				monitor = *pixman_region32_extents(&ps->monitors.regions[i]);
				break;
			}
		}
		double cx = m->target.x + m->target_size.x / 2;
		double cy = m->target.y + m->target_size.y / 2;
		double distances[] = {fabs(monitor.y2 - cy), fabs(cy - monitor.y1),
		                      fabs(cx - monitor.x1), fabs(monitor.x2 - cx)};
		m->direction = MAGIC_LAMP_BOTTOM;
		for (int i = 1; i < 4; i++) {
			if (distances[i] < distances[m->direction]) m->direction = (enum magic_lamp_direction)i;
		}
		// A dock containing the icon disambiguates corner icons.
		wm_stack_foreach(ps->wm, cursor) {
			auto dock = wm_ref_deref(cursor);
			if (!dock || !(dock->window_types & (1 << WINTYPE_DOCK))) continue;
			if (cx < dock->g.x || cy < dock->g.y ||
			    cx > dock->g.x + dock->widthb || cy > dock->g.y + dock->heightb) continue;
			if (dock->widthb >= dock->heightb) {
				m->direction = cy < (monitor.y1 + monitor.y2) / 2.0 ? MAGIC_LAMP_TOP : MAGIC_LAMP_BOTTOM;
			} else {
				m->direction = cx < (monitor.x1 + monitor.x2) / 2.0 ? MAGIC_LAMP_LEFT : MAGIC_LAMP_RIGHT;
			}
			break;
		}
	} else {
		auto pointer = xcb_query_pointer_reply(ps->c.c,
		    xcb_query_pointer(ps->c.c, ps->c.screen_info->root), NULL);
		m->target = pointer ? (vec2){pointer->root_x, pointer->root_y}
		                    : (vec2){m->origin.x + m->size.x / 2, m->origin.y};
		m->target_size = (vec2){};
		free(pointer);
		double x = m->target.x - m->origin.x, y = m->target.y - m->origin.y;
		if (y < 0) m->direction = MAGIC_LAMP_TOP;
		else if (x < 0) m->direction = MAGIC_LAMP_LEFT;
		else if (y > m->size.y) m->direction = MAGIC_LAMP_BOTTOM;
		else if (x > m->size.x) m->direction = MAGIC_LAMP_RIGHT;
		else {
			double distance[] = {m->size.y - y, y, x, m->size.x - x};
			m->direction = MAGIC_LAMP_TOP;
			for (int i = 0; i < 4; i++) {
				if (distance[i] < distance[m->direction]) m->direction = (enum magic_lamp_direction)i;
			}
			switch (m->direction) {
			case MAGIC_LAMP_BOTTOM: m->target.y = m->origin.y + m->size.y; break;
			case MAGIC_LAMP_TOP: m->target.y = m->origin.y; break;
			case MAGIC_LAMP_LEFT: m->target.x = m->origin.x; break;
			case MAGIC_LAMP_RIGHT: m->target.x = m->origin.x + m->size.x; break;
			}
		}
	}
}

enum lamp_result win_magic_lamp_process(session_t *ps, struct win *w, double dt) {
	auto l = &w->lamp;
	bool busy = win_magic_lamp_busy(w);
	auto options = win_options(w);
	const char *backend = ps->o.backend ? backend_name(ps->o.backend) : "";
	if (!options.magic_lamp || !ps->redirected || w->state == WSTATE_DESTROYED ||
	    (strcmp(backend, "glx") && strcmp(backend, "egl"))) {
		win_magic_lamp_cancel(w);
		l->minimized = false;
		return busy ? LAMP_FINISHED : LAMP_IGNORE;
	}
	bool changed = w->previous.state != w->state;
	if (l->dirty || !l->initialized || changed) refresh_state(ps, w);
	bool eligible = l->client && l->supported && !w->a.override_redirect &&
	                (w->window_types & ((1 << WINTYPE_NORMAL) | (1 << WINTYPE_DIALOG)));
	if (changed && w->state == WSTATE_UNMAPPED && eligible && !l->excluded &&
	    w->ever_damaged && w->win_image) {
		if (!busy) {
			l->opacity = win_animatable_get(w, WIN_SCRIPT_OPACITY);
			// unmap_win_start already set raw opacity to zero.
			if (!w->running_animation_instance) l->opacity = w->previous.opacity;
			choose_target(ps, w);
			l->mesh.progress = 0;
		}
		l->pending = true;
		l->restoring = false;
		l->wait = 0;
	} else if (changed && w->state == WSTATE_MAPPED && l->minimized && eligible && !l->excluded) {
		if (!l->active) {
			choose_target(ps, w);
			l->mesh.progress = 1;
		}
		l->pending = true;
		l->restoring = true;
		l->opacity = w->opacity;
		l->wait = 0;
	} else if (changed && w->state == WSTATE_MAPPED && (busy || l->minimized)) {
		win_magic_lamp_cancel(w);
		l->minimized = false;
		return LAMP_SHOW;
	}
	if (l->pending) {
		l->wait += changed ? 0 : fmax(0, dt);
		bool coherent = l->valid && (l->restoring ? !l->hidden && l->wm_state == 1
		                                             : l->hidden && l->wm_state == 3);
		if (!eligible || l->excluded || (!coherent && l->wait >= 0.05)) {
			bool restore = l->restoring;
			win_magic_lamp_cancel(w);
			l->minimized = false;
			log_debug("Magic Lamp fallback %s for %#010x", restore ? "show" : "hide", win_id(w));
			return restore ? LAMP_SHOW : LAMP_HIDE;
		}
		l->mesh.enabled = true;
		w->wobbly.active = false;
		if (!coherent) return LAMP_RUNNING;
		l->minimized = !l->restoring;
		auto trigger = l->restoring ? ANIMATION_TRIGGER_RESTORE : ANIMATION_TRIGGER_MINIMIZE;
		if (options.animations[trigger].script || w->animation_block[trigger]) {
			win_magic_lamp_cancel(w);
			return l->restoring ? LAMP_RESTORE_SCRIPT : LAMP_MINIMIZE_SCRIPT;
		}
		free(w->running_animation_instance);
		w->running_animation_instance = NULL;
		l->pending = false;
		l->active = true;
		l->wait = 0;
		log_debug("Magic Lamp %s for %#010x: duration=%dms", l->restoring ? "restore" : "minimize",
		          win_id(w), ps->o.magic_lamp_duration);
		return LAMP_RUNNING;
	}
	if (l->active) {
		if (l->excluded || !eligible || (w->state != WSTATE_MAPPED && !w->win_image)) {
			win_magic_lamp_cancel(w);
			return LAMP_FINISHED;
		}
		// Mapping refreshes the pixmap after animation processing. Wait for a usable
		// image before advancing restore, so the first visible frame is deformed.
		if (l->restoring && (!w->win_image || !w->ever_damaged)) {
			l->wait += fmax(0, dt);
			if (l->wait < 0.25) return LAMP_RUNNING;
			win_magic_lamp_cancel(w);
			return LAMP_FINISHED;
		}
		l->mesh.progress = magic_lamp_advance(l->mesh.progress, l->restoring, dt,
		                                      ps->o.magic_lamp_duration / 1000.0);
		if (l->mesh.progress == (l->restoring ? 0 : 1)) {
			log_debug("Magic Lamp finished for %#010x", win_id(w));
			win_magic_lamp_cancel(w);
			return LAMP_FINISHED;
		}
		return LAMP_RUNNING;
	}
	return LAMP_IGNORE;
}
