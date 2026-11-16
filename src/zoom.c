// SPDX-License-Identifier: MPL-2.0
// Copyright (c) picom contributors

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <ev.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include <test.h>

#include "atom.h"
#include "common.h"
#include "config.h"
#include "log.h"
#include "picom.h"
#include "renderer/renderer.h"
#include "utils/dynarr.h"
#include "utils/misc.h"
#include "wm/win.h"
#include "x.h"

#include "zoom.h"

/// How often the pointer is checked while a monitor is zoomed, in seconds.
#define ZOOM_POINTER_POLL_INTERVAL (1.0 / 60.0)

struct zoom_monitor {
	char *name;
	/// The monitor's rectangle, in root window coordinates.
	int x, y, width, height;
	double factor;
	/// Top-left corner of the view. The view's size is the monitor's size divided
	/// by `factor`.
	double view_x, view_y;
	/// The last factor above 1 this monitor had, 0 if it never had one.
	double toggle_factor;
	/// Whether the monitor currently exists. The state of monitors that went away
	/// is kept, in case they come back.
	bool present;
};

struct zoom_state {
	/// All monitors ever seen, dynarr
	struct zoom_monitor *monitors;
	/// The last toplevel window seen gaining focus.
	xcb_window_t focused_toplevel;
};

struct zoom {
	session_t *ps;
	struct zoom_state *state;

	/// Polls the pointer while a monitor is zoomed.
	ev_timer pointer_timer;
	/// Last known pointer position, in root window coordinates.
	int pointer_x, pointer_y;
	/// Whether `pointer_x` and `pointer_y` are valid.
	bool pointer_valid;

	/// Whether we asked the X server to hide its cursor.
	bool cursor_hidden;
	/// Whether the cursor image needs to be fetched again.
	bool cursor_stale;
	/// The current cursor image, `cursor.pixels` is owned by us.
	struct renderer_zoom_cursor cursor;
	uint64_t cursor_serial;

	/// Parameters handed to the renderer, dynarr
	struct renderer_zoom_monitor *render_monitors;
	struct renderer_zoom render;
	struct renderer_zoom_cursor render_cursor;
};

// === Geometry ===

static inline double zoom_monitor_view_width(const struct zoom_monitor *m) {
	return m->width / m->factor;
}

static inline double zoom_monitor_view_height(const struct zoom_monitor *m) {
	return m->height / m->factor;
}

static inline bool zoom_monitor_contains(const struct zoom_monitor *m, double x, double y) {
	return x >= m->x && x < m->x + m->width && y >= m->y && y < m->y + m->height;
}

static inline double zoom_clamp(double value, double low, double high) {
	if (value > high) {
		value = high;
	}
	if (value < low) {
		value = low;
	}
	return value;
}

/// Push the view back inside the monitor.
static void zoom_monitor_clamp_view(struct zoom_monitor *m) {
	m->view_x = zoom_clamp(m->view_x, m->x, m->x + m->width - zoom_monitor_view_width(m));
	m->view_y =
	    zoom_clamp(m->view_y, m->y, m->y + m->height - zoom_monitor_view_height(m));
}

/// Move the view so it shows as much as possible of the given range, along one axis.
/// Returns the new starting coordinate of the view.
static double zoom_push_range(double view, double view_size, double start, double size) {
	if (size > view_size) {
		return start;
	}
	if (start < view) {
		return start;
	}
	if (start + size > view + view_size) {
		return start + size - view_size;
	}
	return view;
}

/// Update the view of `m` after the pointer moved to (`x`, `y`).
static void zoom_monitor_track_pointer(struct zoom_monitor *m,
                                       enum zoom_mouse_tracking mode, double x, double y) {
	double view_w = zoom_monitor_view_width(m), view_h = zoom_monitor_view_height(m);
	switch (mode) {
	case ZOOM_MOUSE_TRACKING_PROPORTIONAL:
		m->view_x = m->x + (x - m->x) * (1.0 - 1.0 / m->factor);
		m->view_y = m->y + (y - m->y) * (1.0 - 1.0 / m->factor);
		break;
	case ZOOM_MOUSE_TRACKING_CENTERED:
		m->view_x = x - view_w / 2.0;
		m->view_y = y - view_h / 2.0;
		break;
	case ZOOM_MOUSE_TRACKING_PUSH:
		// The pointer is a point, a range of size 0.
		m->view_x = zoom_push_range(m->view_x, view_w, x, 0);
		m->view_y = zoom_push_range(m->view_y, view_h, y, 0);
		break;
	case ZOOM_MOUSE_TRACKING_NONE:
	case ZOOM_MOUSE_TRACKING_INVALID: break;
	}
	zoom_monitor_clamp_view(m);
}

/// Update the view of `m` after a window with the given rectangle gained focus.
static void zoom_monitor_track_focus(struct zoom_monitor *m, enum zoom_focus_tracking mode,
                                     double x, double y, double width, double height) {
	double view_w = zoom_monitor_view_width(m), view_h = zoom_monitor_view_height(m);
	switch (mode) {
	case ZOOM_FOCUS_TRACKING_CENTERED:
		m->view_x = x + width / 2.0 - view_w / 2.0;
		m->view_y = y + height / 2.0 - view_h / 2.0;
		break;
	case ZOOM_FOCUS_TRACKING_PUSH:
		if (width > view_w || height > view_h) {
			// The window doesn't fit, show its top-left part.
			m->view_x = x;
			m->view_y = y;
		} else {
			m->view_x = zoom_push_range(m->view_x, view_w, x, width);
			m->view_y = zoom_push_range(m->view_y, view_h, y, height);
		}
		break;
	case ZOOM_FOCUS_TRACKING_NONE:
	case ZOOM_FOCUS_TRACKING_INVALID: break;
	}
	zoom_monitor_clamp_view(m);
}

/// Change the factor of `m`. The new view is centered where the old view was centered,
/// or, coming from factor 1, on the pointer if it is on this monitor, or on the middle of
/// the monitor otherwise. If the pointer is on this monitor, the mouse tracking mode is
/// applied afterwards.
///
/// @param from_one treat the monitor as if its factor was 1
static void zoom_monitor_change_factor(struct zoom_monitor *m, double factor, bool from_one,
                                       enum zoom_mouse_tracking mode, bool have_pointer,
                                       double pointer_x, double pointer_y) {
	bool pointer_inside = have_pointer && zoom_monitor_contains(m, pointer_x, pointer_y);
	double center_x, center_y;
	if (!from_one && m->factor > 1) {
		center_x = m->view_x + zoom_monitor_view_width(m) / 2.0;
		center_y = m->view_y + zoom_monitor_view_height(m) / 2.0;
	} else if (pointer_inside) {
		center_x = pointer_x;
		center_y = pointer_y;
	} else {
		center_x = m->x + m->width / 2.0;
		center_y = m->y + m->height / 2.0;
	}

	m->factor = factor;
	if (factor > 1) {
		m->toggle_factor = factor;
	}
	m->view_x = center_x - zoom_monitor_view_width(m) / 2.0;
	m->view_y = center_y - zoom_monitor_view_height(m) / 2.0;
	zoom_monitor_clamp_view(m);
	if (pointer_inside) {
		zoom_monitor_track_pointer(m, mode, pointer_x, pointer_y);
	}
}

static inline bool zoom_factor_valid(double factor) {
	return isfinite(factor) && factor >= ZOOM_FACTOR_MIN && factor <= ZOOM_FACTOR_MAX;
}

static inline bool zoom_step_valid(double step) {
	return isfinite(step) && step > 0;
}

// === Monitors ===

static void zoom_monitor_deinit(struct zoom_monitor *m) {
	free(m->name);
	m->name = NULL;
}

static struct zoom_monitor *
zoom_find_monitor(struct zoom_state *state, const char *name, bool present_only) {
	dynarr_foreach(state->monitors, m) {
		if (strcmp(m->name, name) == 0 && (m->present || !present_only)) {
			return m;
		}
	}
	return NULL;
}

/// The current monitor containing (`x`, `y`). If `zoomed_only`, only zoomed monitors
/// are considered.
static struct zoom_monitor *
zoom_monitor_at(struct zoom_state *state, double x, double y, bool zoomed_only) {
	dynarr_foreach(state->monitors, m) {
		if (m->present && (!zoomed_only || m->factor > 1) &&
		    zoom_monitor_contains(m, x, y)) {
			return m;
		}
	}
	return NULL;
}

static bool zoom_state_active(const struct zoom_state *state) {
	for (size_t i = 0; i < dynarr_len(state->monitors); i++) {
		if (state->monitors[i].present && state->monitors[i].factor > 1) {
			return true;
		}
	}
	return false;
}

struct zoom_state *zoom_state_new(void) {
	auto state = ccalloc(1, struct zoom_state);
	state->monitors = dynarr_new(struct zoom_monitor, 4);
	state->focused_toplevel = XCB_NONE;
	return state;
}

void zoom_state_free(struct zoom_state *state) {
	if (state == NULL) {
		return;
	}
	dynarr_free(state->monitors, zoom_monitor_deinit);
	free(state);
}

// === Pointer and cursor ===

static bool zoom_query_pointer(struct zoom *z, int *x, int *y) {
	auto c = &z->ps->c;
	auto r = xcb_query_pointer_reply(
	    c->c, xcb_query_pointer(c->c, c->screen_info->root), NULL);
	if (r == NULL) {
		return false;
	}
	bool same_screen = r->same_screen;
	*x = r->root_x;
	*y = r->root_y;
	free(r);
	return same_screen;
}

/// Refresh the last known pointer position.
static void zoom_update_pointer(struct zoom *z) {
	int x, y;
	z->pointer_valid = zoom_query_pointer(z, &x, &y);
	if (z->pointer_valid) {
		z->pointer_x = x;
		z->pointer_y = y;
	}
}

/// The zoomed monitor the pointer is on, if any.
static struct zoom_monitor *zoom_pointer_monitor(struct zoom *z) {
	if (!z->pointer_valid) {
		return NULL;
	}
	return zoom_monitor_at(z->state, z->pointer_x, z->pointer_y, true);
}

/// Fetch the current cursor image from the X server.
static void zoom_fetch_cursor(struct zoom *z) {
	auto c = &z->ps->c;
	auto r =
	    xcb_xfixes_get_cursor_image_reply(c->c, xcb_xfixes_get_cursor_image(c->c), NULL);
	z->cursor_stale = false;
	if (r == NULL) {
		log_warn("Failed to get the cursor image.");
		return;
	}
	auto npixels = (size_t)r->width * r->height;
	if ((size_t)xcb_xfixes_get_cursor_image_cursor_image_length(r) < npixels) {
		log_warn("Cursor image is truncated.");
		free(r);
		return;
	}
	uint32_t *pixels = NULL;
	if (npixels > 0) {
		pixels = ccalloc(npixels, uint32_t);
		memcpy(pixels, xcb_xfixes_get_cursor_image_cursor_image(r),
		       npixels * sizeof(uint32_t));
	}
	free((void *)z->cursor.pixels);
	z->cursor.pixels = pixels;
	z->cursor.width = r->width;
	z->cursor.height = r->height;
	z->cursor.xhot = r->xhot;
	z->cursor.yhot = r->yhot;
	z->cursor.serial = ++z->cursor_serial;
	free(r);
}

/// Hide the X server's own cursor while the pointer is on a zoomed monitor, where we
/// draw a magnified cursor ourselves.
static void zoom_update_cursor_visibility(struct zoom *z) {
	auto c = &z->ps->c;
	bool hide = zoom_pointer_monitor(z) != NULL;
	if (hide == z->cursor_hidden) {
		return;
	}
	if (hide) {
		xcb_xfixes_hide_cursor(c->c, c->screen_info->root);
	} else {
		xcb_xfixes_show_cursor(c->c, c->screen_info->root);
	}
	z->cursor_hidden = hide;
}

/// Bring everything up to date after the zoom state changed.
static void zoom_state_changed(struct zoom *z, bool was_active) {
	bool active = zoom_state_active(z->state);
	if (active && !ev_is_active(&z->pointer_timer)) {
		ev_timer_again(z->ps->loop, &z->pointer_timer);
	} else if (!active && ev_is_active(&z->pointer_timer)) {
		ev_timer_stop(z->ps->loop, &z->pointer_timer);
	}
	if (active && z->cursor_stale) {
		zoom_fetch_cursor(z);
	}
	zoom_update_cursor_visibility(z);
	if (active || was_active) {
		// The whole screen changes when the zoom changes.
		force_repaint(z->ps);
	}
}

static void zoom_poll_pointer(EV_P attr_unused, ev_timer *w, int revents attr_unused) {
	auto z = container_of(w, struct zoom, pointer_timer);
	auto old_monitor = zoom_pointer_monitor(z);
	int old_x = z->pointer_x, old_y = z->pointer_y;
	bool was_valid = z->pointer_valid;
	zoom_update_pointer(z);

	bool needs_redraw = false;
	bool moved = z->pointer_valid &&
	             (!was_valid || old_x != z->pointer_x || old_y != z->pointer_y);
	if (moved) {
		auto m = zoom_pointer_monitor(z);
		if (m != NULL) {
			zoom_monitor_track_pointer(m, z->ps->o.zoom_mouse_tracking,
			                           z->pointer_x, z->pointer_y);
		}
		// The magnified cursor moved, appeared, or disappeared.
		needs_redraw = m != NULL || old_monitor != NULL;
	} else if (old_monitor != zoom_pointer_monitor(z)) {
		needs_redraw = true;
	}
	if (z->cursor_stale) {
		zoom_fetch_cursor(z);
		needs_redraw = needs_redraw || zoom_pointer_monitor(z) != NULL;
	}
	zoom_update_cursor_visibility(z);
	if (needs_redraw) {
		queue_redraw(z->ps);
	}
}

// === Monitor updates ===

void zoom_handle_monitors(struct zoom *z, const struct x_monitors *monitors) {
	if (z == NULL) {
		return;
	}
	auto state = z->state;
	bool was_active = zoom_state_active(state);
	size_t nold = dynarr_len(state->monitors);
	bool *seen = ccalloc(nold + 1, bool);
	zoom_update_pointer(z);

	for (int i = 0; i < monitors->count; i++) {
		auto e = pixman_region32_extents(&monitors->regions[i]);
		int x = e->x1, y = e->y1, width = e->x2 - e->x1, height = e->y2 - e->y1;
		const char *name =
		    get_atom_name(z->ps->atoms, monitors->names[i], z->ps->c.c);
		if (name == NULL || width <= 0 || height <= 0) {
			continue;
		}
		struct zoom_monitor *m = zoom_find_monitor(state, name, false);
		if (m != NULL && (size_t)(m - state->monitors) < nold &&
		    seen[m - state->monitors]) {
			log_warn("Duplicate monitor name %s, ignored.", name);
			continue;
		}
		if (m == NULL) {
			log_debug("New monitor %s: %dx%d+%d+%d", name, width, height, x, y);
			dynarr_push(state->monitors, ((struct zoom_monitor){
			                                 .name = strdup(name),
			                                 .x = x,
			                                 .y = y,
			                                 .width = width,
			                                 .height = height,
			                                 .factor = 1,
			                                 .view_x = x,
			                                 .view_y = y,
			                                 .toggle_factor = 0,
			                                 .present = true,
			                             }));
			continue;
		}

		auto index = (size_t)(m - state->monitors);
		if (index < nold) {
			seen[index] = true;
		}
		if (!m->present) {
			// A monitor came back, restore its zoom as if the factor was just
			// raised from 1.
			log_debug("Monitor %s is back: %dx%d+%d+%d", name, width, height, x, y);
			m->present = true;
			m->x = x;
			m->y = y;
			m->width = width;
			m->height = height;
			double factor = m->factor;
			m->factor = 1;
			m->view_x = m->x;
			m->view_y = m->y;
			zoom_monitor_change_factor(
			    m, factor, true, z->ps->o.zoom_mouse_tracking,
			    z->pointer_valid, z->pointer_x, z->pointer_y);
		} else if (m->x != x || m->y != y || m->width != width || m->height != height) {
			// A monitor moved or changed size, keep the view where it was
			// relative to the monitor, as far as possible.
			log_debug("Monitor %s changed: %dx%d+%d+%d", name, width, height, x, y);
			double center_x = m->view_x + zoom_monitor_view_width(m) / 2.0 - m->x;
			double center_y =
			    m->view_y + zoom_monitor_view_height(m) / 2.0 - m->y;
			m->x = x;
			m->y = y;
			m->width = width;
			m->height = height;
			m->view_x = m->x + center_x - zoom_monitor_view_width(m) / 2.0;
			m->view_y = m->y + center_y - zoom_monitor_view_height(m) / 2.0;
			zoom_monitor_clamp_view(m);
		}
	}

	for (size_t i = 0; i < nold; i++) {
		if (!seen[i] && state->monitors[i].present) {
			log_debug("Monitor %s is gone", state->monitors[i].name);
			state->monitors[i].present = false;
		}
	}
	free(seen);
	zoom_state_changed(z, was_active);
}

void zoom_handle_cursor_change(struct zoom *z) {
	if (z == NULL) {
		return;
	}
	z->cursor_stale = true;
	if (zoom_pointer_monitor(z) != NULL) {
		zoom_fetch_cursor(z);
		queue_redraw(z->ps);
	}
}

// === Session ===

struct zoom *zoom_new(session_t *ps, struct zoom_state *state) {
	auto z = ccalloc(1, struct zoom);
	z->ps = ps;
	z->state = state;
	z->cursor_stale = true;
	z->render_monitors = dynarr_new(struct renderer_zoom_monitor, 4);
	ev_init(&z->pointer_timer, zoom_poll_pointer);
	z->pointer_timer.repeat = ZOOM_POINTER_POLL_INTERVAL;

	xcb_xfixes_select_cursor_input(ps->c.c, ps->c.screen_info->root,
	                               XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR);
	// After a reset, monitors keep their zoom until the monitor list is updated:
	// then the ones still present keep their views, and the ones that came back
	// get their views placed anew.
	zoom_update_pointer(z);
	zoom_state_changed(z, false);
	return z;
}

void zoom_free(struct zoom *z) {
	if (z == NULL) {
		return;
	}
	ev_timer_stop(z->ps->loop, &z->pointer_timer);
	if (z->cursor_hidden) {
		xcb_xfixes_show_cursor(z->ps->c.c, z->ps->c.screen_info->root);
	}
	free((void *)z->cursor.pixels);
	dynarr_free_pod(z->render_monitors);
	free(z);
}

bool zoom_is_active(const struct zoom *z) {
	return z != NULL && zoom_state_active(z->state);
}

void zoom_handle_focus(struct zoom *z, const struct win *w) {
	if (z == NULL) {
		return;
	}
	auto toplevel = win_id(w);
	if (toplevel == z->state->focused_toplevel) {
		return;
	}
	z->state->focused_toplevel = toplevel;

	auto mode = z->ps->o.zoom_focus_tracking;
	if (mode == ZOOM_FOCUS_TRACKING_NONE || !zoom_state_active(z->state)) {
		return;
	}

	// The rectangle the window is drawn in, including its frame.
	double x = w->g.x, y = w->g.y, width = w->widthb, height = w->heightb;
	int pointer_x, pointer_y;
	if (zoom_query_pointer(z, &pointer_x, &pointer_y) && pointer_x >= x &&
	    pointer_x < x + width && pointer_y >= y && pointer_y < y + height) {
		// Focus moved to the window under the pointer, e.g. because it was
		// clicked. The pointer is already in view.
		return;
	}

	auto m = zoom_monitor_at(z->state, x + width / 2.0, y + height / 2.0, true);
	if (m == NULL) {
		return;
	}
	log_debug("Window %#010x focused, moving the view of monitor %s", toplevel, m->name);
	zoom_monitor_track_focus(m, mode, x, y, width, height);
	queue_redraw(z->ps);
}

// === Control ===

enum zoom_result zoom_set_monitor_factor(struct zoom *z, const char *name, double factor) {
	if (!zoom_factor_valid(factor)) {
		return ZOOM_ERROR_INVALID_FACTOR;
	}
	bool all = strcmp(name, "*") == 0;
	struct zoom_monitor *target = NULL;
	if (!all) {
		target = zoom_find_monitor(z->state, name, true);
		if (target == NULL) {
			return ZOOM_ERROR_UNKNOWN_MONITOR;
		}
	}

	bool was_active = zoom_state_active(z->state);
	zoom_update_pointer(z);
	dynarr_foreach(z->state->monitors, m) {
		if (m->present && (all || m == target)) {
			zoom_monitor_change_factor(
			    m, factor, false, z->ps->o.zoom_mouse_tracking,
			    z->pointer_valid, z->pointer_x, z->pointer_y);
		}
	}
	zoom_state_changed(z, was_active);
	return ZOOM_OK;
}

enum zoom_result zoom_apply_verb(struct zoom *z, enum zoom_verb verb, double arg) {
	if ((verb == ZOOM_VERB_IN || verb == ZOOM_VERB_OUT) && !zoom_step_valid(arg)) {
		return ZOOM_ERROR_INVALID_STEP;
	}
	if (verb == ZOOM_VERB_SET && !zoom_factor_valid(arg)) {
		return ZOOM_ERROR_INVALID_FACTOR;
	}

	zoom_update_pointer(z);
	if (!z->pointer_valid) {
		return ZOOM_ERROR_NO_MONITOR_UNDER_POINTER;
	}
	auto m = zoom_monitor_at(z->state, z->pointer_x, z->pointer_y, false);
	if (m == NULL) {
		return ZOOM_ERROR_NO_MONITOR_UNDER_POINTER;
	}

	double factor = m->factor;
	switch (verb) {
	case ZOOM_VERB_IN: factor = fmin(m->factor + arg, ZOOM_FACTOR_MAX); break;
	case ZOOM_VERB_OUT: factor = fmax(m->factor - arg, ZOOM_FACTOR_MIN); break;
	case ZOOM_VERB_SET: factor = arg; break;
	case ZOOM_VERB_OFF: factor = ZOOM_FACTOR_MIN; break;
	case ZOOM_VERB_TOGGLE:
		if (m->factor > 1) {
			factor = ZOOM_FACTOR_MIN;
		} else if (m->toggle_factor > 1) {
			factor = m->toggle_factor;
		} else {
			factor = ZOOM_FACTOR_TOGGLE_DEFAULT;
		}
		break;
	}

	bool was_active = zoom_state_active(z->state);
	zoom_monitor_change_factor(m, factor, false, z->ps->o.zoom_mouse_tracking, true,
	                           z->pointer_x, z->pointer_y);
	zoom_state_changed(z, was_active);
	return ZOOM_OK;
}

bool zoom_monitor_get(const struct zoom *z, size_t index, struct zoom_monitor_info *info) {
	for (size_t i = 0; i < dynarr_len(z->state->monitors); i++) {
		const struct zoom_monitor *m = &z->state->monitors[i];
		if (!m->present) {
			continue;
		}
		if (index-- == 0) {
			*info = (struct zoom_monitor_info){
			    .name = m->name,
			    .factor = m->factor,
			    .x = m->x,
			    .y = m->y,
			    .width = m->width,
			    .height = m->height,
			    .view_x = m->view_x,
			    .view_y = m->view_y,
			    .view_width = zoom_monitor_view_width(m),
			    .view_height = zoom_monitor_view_height(m),
			};
			return true;
		}
	}
	return false;
}

const char *zoom_result_to_string(enum zoom_result result) {
	switch (result) {
	case ZOOM_OK: return "Success";
	case ZOOM_ERROR_INVALID_FACTOR:
		return "Zoom factor must be a finite number from 1 to 32";
	case ZOOM_ERROR_INVALID_STEP: return "Zoom step must be a finite number above 0";
	case ZOOM_ERROR_UNKNOWN_MONITOR: return "No such monitor";
	case ZOOM_ERROR_NO_MONITOR_UNDER_POINTER:
		return "The pointer is not on a monitor";
	}
	return "Unknown error";
}

const struct renderer_zoom *zoom_render_parameters(struct zoom *z) {
	if (z == NULL || !zoom_state_active(z->state)) {
		return NULL;
	}
	dynarr_clear_pod(z->render_monitors);
	auto pointer_monitor = zoom_pointer_monitor(z);
	int cursor_monitor = -1;
	dynarr_foreach(z->state->monitors, m) {
		if (!m->present || m->factor <= 1) {
			continue;
		}
		if (m == pointer_monitor) {
			cursor_monitor = (int)dynarr_len(z->render_monitors);
		}
		dynarr_push(z->render_monitors, ((struct renderer_zoom_monitor){
		                                    .x = m->x,
		                                    .y = m->y,
		                                    .width = m->width,
		                                    .height = m->height,
		                                    .factor = m->factor,
		                                    .view_x = m->view_x,
		                                    .view_y = m->view_y,
		                                }));
	}
	z->render.monitors = z->render_monitors;
	z->render.nmonitors = (int)dynarr_len(z->render_monitors);
	z->render.cursor = NULL;
	if (cursor_monitor >= 0 && z->cursor.pixels != NULL && z->cursor.width > 0 &&
	    z->cursor.height > 0) {
		z->render_cursor = z->cursor;
		z->render_cursor.monitor = cursor_monitor;
		z->render_cursor.x = z->pointer_x;
		z->render_cursor.y = z->pointer_y;
		z->render.cursor = &z->render_cursor;
	}
	return &z->render;
}

// === Tests ===

static struct zoom_monitor test_monitor(int x, int y, int width, int height) {
	return (struct zoom_monitor){
	    .x = x,
	    .y = y,
	    .width = width,
	    .height = height,
	    .factor = 1,
	    .view_x = x,
	    .view_y = y,
	    .present = true,
	};
}

TEST_CASE(zoom_factor_change) {
	auto m = test_monitor(0, 0, 640, 480);
	// From 1 with the pointer inside: centered on the pointer, then tracked.
	zoom_monitor_change_factor(&m, 2, false, ZOOM_MOUSE_TRACKING_PROPORTIONAL, true,
	                           200, 150);
	TEST_EQUAL(m.view_x, 100);
	TEST_EQUAL(m.view_y, 75);
	TEST_EQUAL(m.toggle_factor, 2);
	// Between zoomed factors: around the old view's center.
	zoom_monitor_change_factor(&m, 4, false, ZOOM_MOUSE_TRACKING_NONE, false, 0, 0);
	TEST_EQUAL(m.view_x, 180);
	TEST_EQUAL(m.view_y, 135);
	// With the pointer inside, the tracking mode is applied once.
	zoom_monitor_change_factor(&m, 4, false, ZOOM_MOUSE_TRACKING_PROPORTIONAL, true,
	                           200, 150);
	TEST_EQUAL(m.view_x, 150);
	TEST_EQUAL(m.view_y, 112.5);
	// Back to 1: the view is the whole monitor, memory is kept.
	zoom_monitor_change_factor(&m, 1, false, ZOOM_MOUSE_TRACKING_NONE, false, 0, 0);
	TEST_EQUAL(m.view_x, 0);
	TEST_EQUAL(m.view_y, 0);
	TEST_EQUAL(m.toggle_factor, 4);

	// From 1 with the pointer elsewhere: centered on the monitor.
	auto m2 = test_monitor(640, 120, 480, 360);
	zoom_monitor_change_factor(&m2, 2, false, ZOOM_MOUSE_TRACKING_PROPORTIONAL, true,
	                           1000, 30);
	TEST_EQUAL(m2.view_x, 760);
	TEST_EQUAL(m2.view_y, 210);
}

TEST_CASE(zoom_pointer_tracking) {
	auto m = test_monitor(0, 0, 640, 480);
	m.factor = 2;
	zoom_monitor_track_pointer(&m, ZOOM_MOUSE_TRACKING_CENTERED, 10, 10);
	TEST_EQUAL(m.view_x, 0);
	TEST_EQUAL(m.view_y, 0);
	zoom_monitor_track_pointer(&m, ZOOM_MOUSE_TRACKING_CENTERED, 400, 300);
	TEST_EQUAL(m.view_x, 240);
	TEST_EQUAL(m.view_y, 180);
	// Push: inside the view, nothing moves.
	zoom_monitor_track_pointer(&m, ZOOM_MOUSE_TRACKING_PUSH, 300, 200);
	TEST_EQUAL(m.view_x, 240);
	TEST_EQUAL(m.view_y, 180);
	// Push: outside, the view moves just enough.
	zoom_monitor_track_pointer(&m, ZOOM_MOUSE_TRACKING_PUSH, 100, 430);
	TEST_EQUAL(m.view_x, 100);
	TEST_EQUAL(m.view_y, 190);
	zoom_monitor_track_pointer(&m, ZOOM_MOUSE_TRACKING_NONE, 0, 0);
	TEST_EQUAL(m.view_x, 100);
	TEST_EQUAL(m.view_y, 190);
	zoom_monitor_track_pointer(&m, ZOOM_MOUSE_TRACKING_PROPORTIONAL, 640, 480);
	TEST_EQUAL(m.view_x, 320);
	TEST_EQUAL(m.view_y, 240);
}

TEST_CASE(zoom_focus_tracking) {
	auto m = test_monitor(0, 0, 640, 480);
	m.factor = 2;
	zoom_monitor_track_focus(&m, ZOOM_FOCUS_TRACKING_CENTERED, 400, 300, 100, 80);
	TEST_EQUAL(m.view_x, 290);
	TEST_EQUAL(m.view_y, 220);
	// Already in view.
	zoom_monitor_track_focus(&m, ZOOM_FOCUS_TRACKING_PUSH, 400, 300, 100, 80);
	TEST_EQUAL(m.view_x, 290);
	TEST_EQUAL(m.view_y, 220);
	zoom_monitor_track_focus(&m, ZOOM_FOCUS_TRACKING_PUSH, 10, 20, 100, 80);
	TEST_EQUAL(m.view_x, 10);
	TEST_EQUAL(m.view_y, 20);
	// Too big to fit: top-left of the window, clamped.
	zoom_monitor_track_focus(&m, ZOOM_FOCUS_TRACKING_PUSH, 330, 50, 400, 100);
	TEST_EQUAL(m.view_x, 320);
	TEST_EQUAL(m.view_y, 50);
}
