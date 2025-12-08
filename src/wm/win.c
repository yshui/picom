// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/render.h>
#include <xcb/xcb.h>
#include <xcb/xcb_renderutil.h>

#include <picom/types.h>

#include "atom.h"
#include "c2.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "dbus.h"
#include "inspect.h"
#include "log.h"
#include "picom.h"
#include "region.h"
#include "utils/console.h"
#include "utils/misc.h"
#include "x.h"

#include "defs.h"
#include "wm.h"

#include "win.h"

#define OPAQUE (0xffffffff)
static const int ROUNDED_PIXELS = 1;
static const double ROUNDED_PERCENT = 0.05;

// TODO(yshui)
//
// Right now, how window properties/states/information (let's just call them states)
// are calculated is a huge mess.
//
// We can divide a window's states (i.e. fields in struct managed_win) in to two groups:
// one is "raw" window states, those come directly from the X server; the other is
// computed window states, which is calculated based on the raw properties, and user
// configurations like rules etc.
//
// Right now what we do is when some raw states are updated, we set some flags to
// recalculate relevant computed states. This is really hard to get right, because it's
// tedious to figure out the influence a raw window state has. And it is also imprecise,
// just look at our `win_on_factor_changed` - it is so difficult to get the recalculation
// right, so we basically use "factor change" as a catch-all, basically any changes to raw
// states will cause it to be called. And we recalculate everything there, kind of
// destroying the whole point.
//
// A better way is doing this the other way around, we shouldn't need to do anything when
// updating a raw state. Instead, the computed states should declare which raw states they
// depend on, so we can go through the computed states, only recalculate the ones whose
// dependencies have changed. The c2 rules are kind of already calculated this way, we
// should unify the rest of the computed states. This would simplify the code as well.

static void
win_update_prop_shadow_raw(struct x_connection *c, struct atom *atoms, struct win *w);
static bool win_update_prop_shadow(struct x_connection *c, struct atom *atoms, struct win *w);
/**
 * Update leader of a window.
 */
static xcb_window_t
win_get_leader_property(struct x_connection *c, struct atom *atoms, xcb_window_t wid,
                        bool detect_transient, bool detect_client_leader);

/// Generate a "no corners" region function, from a function that returns the
/// region via a region_t pointer argument. Corners of the window will be removed from
/// the returned region.
/// Function signature has to be (win *, region_t *)
#define gen_without_corners(fun)                                                         \
	void fun##_without_corners(const struct win *w, region_t *res) {                 \
		fun(w, res);                                                             \
		win_region_remove_corners_local(w, res);                                 \
	}

/// Generate a "return by value" function, from a function that returns the
/// region via a region_t pointer argument.
/// Function signature has to be (win *)
#define gen_by_val(fun)                                                                  \
	region_t fun##_by_val(const struct win *w) {                                     \
		region_t ret;                                                            \
		pixman_region32_init(&ret);                                              \
		fun(w, &ret);                                                            \
		return ret;                                                              \
	}

/**
 * Update focused state of a window.
 */
static bool win_is_focused(session_t *ps, struct win *w) {
	bool is_wmwin = win_is_wmwin(w);
	if (w->a.map_state == XCB_MAP_STATE_VIEWABLE && (w->is_focused || w->is_group_focused)) {
		return true;
	}
	// Use wintype_focus, and treat WM windows and override-redirected
	// windows specially
	if (ps->o.wintype_option[index_of_lowest_one(w->window_types)].focus ||
	    (ps->o.mark_wmwin_focused && is_wmwin) ||
	    (ps->o.mark_ovredir_focused && wm_ref_client_of(w->tree_ref) == NULL && !is_wmwin) ||
	    (w->a.map_state == XCB_MAP_STATE_VIEWABLE &&
	     c2_match(ps->c2_state, w, &ps->o.focus_blacklist, NULL))) {
		return true;
	}
	return false;
}

struct group_callback_data {
	struct session *ps;
	xcb_window_t leader;
};

/**
 * Get a rectangular region a window occupies, excluding shadow.
 */
static void win_get_region_local(const struct win *w, region_t *res) {
	assert(w->widthb >= 0 && w->heightb >= 0);
	pixman_region32_fini(res);
	pixman_region32_init_rect(res, 0, 0, (uint)w->widthb, (uint)w->heightb);
}

void win_get_region_frame_local(const struct win *w, region_t *res) {
	const margin_t extents = win_calc_frame_extents(w);
	auto outer_width = w->widthb;
	auto outer_height = w->heightb;

	pixman_region32_fini(res);
	pixman_region32_init_rects(
	    res,
	    (rect_t[]){
	        // top
	        {.x1 = 0, .y1 = 0, .x2 = outer_width, .y2 = extents.top},
	        // bottom
	        {.x1 = 0, .y1 = outer_height - extents.bottom, .x2 = outer_width, .y2 = outer_height},
	        // left
	        {.x1 = 0, .y1 = 0, .x2 = extents.left, .y2 = outer_height},
	        // right
	        {.x1 = outer_width - extents.right, .y1 = 0, .x2 = outer_width, .y2 = outer_height},
	    },
	    4);

	// limit the frame region to inside the window
	region_t reg_win;
	pixman_region32_init_rects(&reg_win, (rect_t[]){{0, 0, outer_width, outer_height}}, 1);
	pixman_region32_intersect(res, &reg_win, res);
	pixman_region32_fini(&reg_win);
}

gen_by_val(win_get_region_frame_local);

static inline void release_image_and_pixmap(backend_t *base, image_handle image) {
	xcb_pixmap_t pixmap = base->ops.release_image(base, image);
	if (pixmap != XCB_NONE) {
		xcb_free_pixmap(base->c->c, pixmap);
	}
}

/// Release the images attached to this window
static inline void win_release_pixmap(backend_t *base, struct win *w) {
	log_debug("Releasing pixmap of window %#010x (%s)", win_id(w), w->name);
	if (w->win_image) {
		release_image_and_pixmap(base, w->win_image);
		w->win_image = NULL;
	}
}

static inline void win_release_shadow(backend_t *base, struct win *w) {
	log_debug("Releasing shadow of window %#010x (%s)", win_id(w), w->name);
	if (w->shadow_mask) {
		release_image_and_pixmap(base, w->shadow_mask);
		w->shadow_mask = NULL;
	}
}

static inline void win_release_mask(backend_t *base, struct win *w) {
	if (w->mask_image) {
		release_image_and_pixmap(base, w->mask_image);
		w->mask_image = NULL;
	}
}

void win_release_saved_win_image(backend_t *base, struct win *w) {
	if (w->saved_win_image) {
		release_image_and_pixmap(base, w->saved_win_image);
		w->saved_win_image = NULL;
	}
}

void win_release_images(struct backend_base *backend, struct win *w) {
	// We don't want to decide what we should do if the image we want to
	// release is stale (do we clear the stale flags or not?) But if we are
	// not releasing any images anyway, we don't care about the stale flags.
	assert(w->win_image == NULL || !win_check_flags_all(w, WIN_FLAGS_PIXMAP_STALE));

	win_release_pixmap(backend, w);
	win_release_shadow(backend, w);
	win_release_mask(backend, w);
	win_release_saved_win_image(backend, w);
}

/// Returns true if the `prop` property is stale, as well as clears the stale
/// flag.
static bool win_fetch_and_unset_property_stale(struct win *w, xcb_atom_t prop);
/// Returns true if any of the properties are stale, as well as clear all the
/// stale flags.
static void win_clear_all_properties_stale(struct win *w);

/**
 * Reread opacity property of a window.
 */
bool win_update_opacity_prop(struct x_connection *c, struct atom *atoms, struct win *w,
                             bool detect_client_opacity) {
	bool old_has_opacity_prop = w->has_opacity_prop;
	auto old_opacity = w->opacity_prop;
	// get frame opacity first
	w->has_opacity_prop =
	    wid_get_opacity_prop(c, atoms, win_id(w), OPAQUE, &w->opacity_prop);

	if (!w->has_opacity_prop && detect_client_opacity) {
		// didn't find opacity prop on the frame, try to get client opacity
		auto client_win = wm_ref_client_of(w->tree_ref);
		if (client_win != NULL) {
			w->has_opacity_prop = wid_get_opacity_prop(
			    c, atoms, wm_ref_win_id(client_win), OPAQUE, &w->opacity_prop);
		}
	}

	if (w->has_opacity_prop) {
		return !old_has_opacity_prop || w->opacity_prop != old_opacity;
	}
	return old_has_opacity_prop;
}

// TODO(yshui) make WIN_FLAGS_FACTOR_CHANGED more fine-grained, or find a better
// alternative
//             way to do all this.

/// Fetch new window properties from the X server, and run appropriate updates.
/// Might set WIN_FLAGS_FACTOR_CHANGED
static void win_update_properties(session_t *ps, struct win *w) {
	// we cannot receive property change when window has been destroyed
	assert(w->state != WSTATE_DESTROYED);

	if (win_fetch_and_unset_property_stale(w, ps->atoms->a_NET_WM_WINDOW_TYPE)) {
		if (win_update_wintype(&ps->c, ps->atoms, w)) {
			win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		}
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->a_NET_WM_WINDOW_OPACITY) &&
	    win_update_opacity_prop(&ps->c, ps->atoms, w, ps->o.detect_client_opacity)) {
		win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->a_NET_FRAME_EXTENTS)) {
		auto client_win = win_client_id(w, /*fallback_to_self=*/false);
		win_update_frame_extents(&ps->c, ps->atoms, w, client_win);
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->aWM_NAME) ||
	    win_fetch_and_unset_property_stale(w, ps->atoms->a_NET_WM_NAME)) {
		if (win_update_name(&ps->c, ps->atoms, w) == 1) {
			win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		}
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->aWM_CLASS)) {
		if (win_update_class(&ps->c, ps->atoms, w)) {
			win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		}
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->aWM_WINDOW_ROLE)) {
		if (win_update_role(&ps->c, ps->atoms, w) == 1) {
			win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		}
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->a_COMPTON_SHADOW)) {
		if (win_update_prop_shadow(&ps->c, ps->atoms, w)) {
			win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		}
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->a_NET_WM_STATE)) {
		if (win_update_prop_fullscreen(&ps->c, ps->atoms, w)) {
			win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		}
	}

	if (ps->o.track_leader &&
	    (win_fetch_and_unset_property_stale(w, ps->atoms->aWM_CLIENT_LEADER) ||
	     win_fetch_and_unset_property_stale(w, ps->atoms->aWM_TRANSIENT_FOR) ||
	     win_fetch_and_unset_property_stale(w, XCB_ATOM_WM_HINTS))) {
		auto client_win = win_client_id(w, /*fallback_to_self=*/true);
		auto new_leader = win_get_leader_property(&ps->c, ps->atoms, client_win,
		                                          ps->o.detect_transient,
		                                          ps->o.detect_client_leader);
		wm_ref_set_leader(ps->wm, w->tree_ref, new_leader);
	}

	win_clear_all_properties_stale(w);
}

/// Handle primary flags. These flags are set as direct results of raw X11 window data
/// changes.
void win_process_primary_flags(session_t *ps, struct win *w) {
	log_trace("Processing flags for window %#010x (%s), was rendered: %d, flags: "
	          "%#" PRIx64,
	          win_id(w), w->name, w->to_paint, w->flags);

	if (win_check_flags_all(w, WIN_FLAGS_MAPPED)) {
		win_map_start(ps, w);
		win_clear_flags(w, WIN_FLAGS_MAPPED);
	}

	if (w->state != WSTATE_MAPPED) {
		// Window is not mapped, so we ignore all its changes until it's mapped
		// again.
		return;
	}

	if (win_check_flags_all(w, WIN_FLAGS_CLIENT_STALE)) {
		win_on_client_update(ps, w);
		win_clear_flags(w, WIN_FLAGS_CLIENT_STALE);
	}

	if (win_check_flags_any(w, WIN_FLAGS_SIZE_STALE | WIN_FLAGS_POSITION_STALE)) {
		// For damage calculation purposes, we don't care if the window
		// is mapped in X server, we only care if we rendered it last
		// frame.
		//
		// We do not process window flags for unmapped windows even when
		// it was rendered, so an window fading out won't move even if the
		// underlying unmapped window is moved. When the window is
		// mapped again when it's still fading out, it should have the
		// same effect as a mapped window being moved, meaning we have
		// to add both the previous and the new window extents to
		// damage.
		//
		// All that is basically me saying what really matters is if the
		// window was rendered last frame, not if it's mapped in X server.

		// Update window geometry
		w->previous.g = w->g;
		w->g = w->pending_g;

		// Whether a window is fullscreen changes based on its geometry
		win_update_is_fullscreen(ps, w);

		if (win_check_flags_all(w, WIN_FLAGS_SIZE_STALE)) {
			win_on_win_size_change(w, ps->o.shadow_offset_x,
			                       ps->o.shadow_offset_y, ps->o.shadow_radius);
			win_update_bounding_shape(&ps->c, w, ps->o.detect_rounded_corners);
			win_clear_flags(w, WIN_FLAGS_SIZE_STALE);

			// Window shape/size changed, invalidate the images we built
			// log_trace("free out dated pict");
			win_set_flags(w, WIN_FLAGS_PIXMAP_STALE | WIN_FLAGS_FACTOR_CHANGED);

			win_release_mask(ps->backend_data, w);
			win_release_shadow(ps->backend_data, w);
			ps->pending_updates = true;
		}

		if (win_check_flags_all(w, WIN_FLAGS_POSITION_STALE)) {
			win_clear_flags(w, WIN_FLAGS_POSITION_STALE);
		}
	}

	if (win_check_flags_all(w, WIN_FLAGS_PROPERTY_STALE)) {
		win_update_properties(ps, w);
		win_clear_flags(w, WIN_FLAGS_PROPERTY_STALE);
	}
}

/// Handle secondary flags. These flags are set during the processing of primary flags.
/// Flags are separated into primaries and secondaries because processing of secondary
/// flags must happen after primary flags of ALL windows are processed, to make sure some
/// global states (e.g. active window group) are consistent because they will be used in
/// the processing of secondary flags.
void win_process_secondary_flags(session_t *ps, struct win *w) {
	if (w->state != WSTATE_MAPPED) {
		return;
	}

	// Handle window focus change. Set appropriate flags if focused states of
	// this window changed in the wm tree.
	bool new_focused = wm_focused_win(ps->wm) == w->tree_ref;
	bool new_group_focused = wm_focused_leader(ps->wm) == wm_ref_leader(w->tree_ref);
	if (new_focused != w->is_focused) {
		log_debug("Window %#010x (%s) focus state changed from %d to %d",
		          win_id(w), w->name, w->is_focused, new_focused);
		w->is_focused = new_focused;
		win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		// Send D-Bus signal
		if (ps->o.dbus) {
			if (new_focused) {
				cdbus_ev_win_focusin(session_get_cdbus(ps), w);
			} else {
				cdbus_ev_win_focusout(session_get_cdbus(ps), w);
			}
		}
	}
	if (new_group_focused != w->is_group_focused) {
		log_debug("Window %#010x (%s) group focus state changed from %d to %d",
		          win_id(w), w->name, w->is_group_focused, new_group_focused);
		w->is_group_focused = new_group_focused;
		win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
	}

	if (w->flags == 0) {
		return;
	}

	// Save the old window options, so if shadow changes from true to false,
	// we can release the shadow.
	auto old_options = win_options(w);

	// Factor change flags could be set by previous stages, so must be handled
	// last
	if (win_check_flags_all(w, WIN_FLAGS_FACTOR_CHANGED)) {
		win_on_factor_change(ps, w);
		win_clear_flags(w, WIN_FLAGS_FACTOR_CHANGED);
	}

	auto new_options = win_options(w);

	if (new_options.shadow != old_options.shadow && !new_options.shadow) {
		win_release_shadow(ps->backend_data, w);
	}
}

void win_process_image_flags(session_t *ps, struct win *w) {
	// Assert that the MAPPED flag is already handled.
	assert(!win_check_flags_all(w, WIN_FLAGS_MAPPED));

	if (w->state != WSTATE_MAPPED || !win_check_flags_any(w, WIN_FLAGS_PIXMAP_STALE)) {
		// 1. Flags of invisible windows are processed when they are mapped
		// 2. We don't need to update window image if pixmap is not stale
		return;
	}

	if (win_check_flags_all(w, WIN_FLAGS_PIXMAP_ERROR) || !ps->redirected) {
		// 1. We have previously failed to bind the pixmap, don't try again.
		// 2. If we aren't redirected, window images will be refreshed upon
		//    redirection anyway.
		win_clear_flags(w, WIN_FLAGS_PIXMAP_STALE);
		return;
	}

	// Image needs to be updated, update it.
	win_clear_flags(w, WIN_FLAGS_PIXMAP_STALE);

	// Check to make sure the window is still mapped, otherwise we won't be able to
	// rebind pixmap after releasing it, yet we might still need the pixmap for
	// rendering.
	auto pixmap = x_new_id(&ps->c);
	auto e = xcb_request_check(
	    ps->c.c, xcb_composite_name_window_pixmap_checked(ps->c.c, win_id(w), pixmap));
	if (e != NULL) {
		log_debug("Failed to get named pixmap for window %#010x(%s): %s. "
		          "Retaining its current window image",
		          win_id(w), w->name, x_strerror(&ps->c, e));
		free(e);
		return;
	}

	log_debug("New named pixmap for %#010x (%s) : %#010x", win_id(w), w->name, pixmap);

	// Must release images first, otherwise breaks NVIDIA driver
	win_release_pixmap(ps->backend_data, w);
	w->win_image = ps->backend_data->ops.bind_pixmap(
	    ps->backend_data, pixmap, x_get_visual_info(&ps->c, w->a.visual));
	if (!w->win_image) {
		log_error("Failed to bind pixmap");
		xcb_free_pixmap(ps->c.c, pixmap);
		win_set_flags(w, WIN_FLAGS_PIXMAP_ERROR);
	}
}

/**
 * Check if a window has rounded corners.
 * XXX This is really dumb
 */
static bool attr_pure win_has_rounded_corners(const struct win *w) {
	if (!w->bounding_shaped) {
		return false;
	}

	// Quit if border_size() returns XCB_NONE
	if (!pixman_region32_not_empty((region_t *)&w->bounding_shape)) {
		return false;
	}

	// Determine the minimum width/height of a rectangle that could mark
	// a window as having rounded corners
	auto minwidth =
	    (uint16_t)max2(w->widthb * (1 - ROUNDED_PERCENT), w->widthb - ROUNDED_PIXELS);
	auto minheight =
	    (uint16_t)max2(w->heightb * (1 - ROUNDED_PERCENT), w->heightb - ROUNDED_PIXELS);

	// Get the rectangles in the bounding region
	int nrects = 0;
	const rect_t *rects =
	    pixman_region32_rectangles((region_t *)&w->bounding_shape, &nrects);

	// Look for a rectangle large enough for this window be considered
	// having rounded corners
	for (int i = 0; i < nrects; ++i) {
		if (rects[i].x2 - rects[i].x1 >= minwidth &&
		    rects[i].y2 - rects[i].y1 >= minheight) {
			return true;
		}
	}
	return false;
}

int win_update_name(struct x_connection *c, struct atom *atoms, struct win *w) {
	char **strlst = NULL;
	int nstr = 0;
	auto client_win = win_client_id(w, /*fallback_to_self=*/true);

	if (!(wid_get_text_prop(c, atoms, client_win, atoms->a_NET_WM_NAME, &strlst, &nstr))) {
		log_debug("(%#010x): _NET_WM_NAME unset, falling back to WM_NAME.", client_win);

		if (!wid_get_text_prop(c, atoms, client_win, atoms->aWM_NAME, &strlst, &nstr)) {
			log_debug("Unsetting window name for %#010x", client_win);
			free(w->name);
			w->name = NULL;
			return -1;
		}
	}

	int ret = 0;
	if (!w->name || strcmp(w->name, strlst[0]) != 0) {
		ret = 1;
		free(w->name);
		w->name = strdup(strlst[0]);
	}

	free(strlst);

	log_debug("(%#010x): client = %#010x, name = \"%s\", ret = %d", win_id(w),
	          client_win, w->name, ret);
	return ret;
}

int win_update_role(struct x_connection *c, struct atom *atoms, struct win *w) {
	char **strlst = NULL;
	int nstr = 0;
	auto client_win = win_client_id(w, /*fallback_to_self=*/true);

	if (!wid_get_text_prop(c, atoms, client_win, atoms->aWM_WINDOW_ROLE, &strlst, &nstr)) {
		return -1;
	}

	int ret = 0;
	if (!w->role || strcmp(w->role, strlst[0]) != 0) {
		ret = 1;
		free(w->role);
		w->role = strdup(strlst[0]);
	}

	free(strlst);

	log_trace("(%#010x): client = %#010x, role = \"%s\", ret = %d", win_id(w),
	          client_win, w->role, ret);
	return ret;
}

/**
 * Check if a window is bounding-shaped.
 */
static inline bool win_bounding_shaped(struct x_connection *c, xcb_window_t wid) {
	xcb_shape_query_extents_reply_t *reply;
	bool bounding_shaped;
	reply = xcb_shape_query_extents_reply(c->c, xcb_shape_query_extents(c->c, wid), NULL);
	bounding_shaped = reply && reply->bounding_shaped;
	free(reply);

	return bounding_shaped;
}

static uint32_t
wid_get_prop_window_types(struct x_connection *c, struct atom *atoms, xcb_window_t wid) {
	winprop_t prop =
	    x_get_prop(c, wid, atoms->a_NET_WM_WINDOW_TYPE, 32L, XCB_ATOM_ATOM, 32);

	static_assert(NUM_WINTYPES <= 32, "too many window types");

	uint32_t ret = 0;
	for (unsigned i = 0; i < prop.nitems; ++i) {
		for (wintype_t j = 1; j < NUM_WINTYPES; ++j) {
			if (get_atom_with_nul(atoms, WINTYPES[j].atom, c->c) == prop.atom[i]) {
				ret |= (1U << j);
				break;
			}
		}
	}

	free_winprop(&prop);

	return ret;
}

// XXX should distinguish between frame has alpha and window body has alpha
bool win_has_alpha(const struct win *w) {
	return w->pictfmt && w->pictfmt->type == XCB_RENDER_PICT_TYPE_DIRECT &&
	       w->pictfmt->direct.alpha_mask;
}

bool win_client_has_alpha(const struct win *w) {
	return w->client_pictfmt && w->client_pictfmt->type == XCB_RENDER_PICT_TYPE_DIRECT &&
	       w->client_pictfmt->direct.alpha_mask;
}

winmode_t win_calc_mode_raw(const struct win *w) {
	if (win_has_alpha(w)) {
		if (wm_ref_client_of(w->tree_ref) == NULL) {
			// This is a window not managed by the WM, and it has
			// alpha, so it's transparent. No need to check WM frame.
			return WMODE_TRANS;
		}
		// The WM window has alpha
		if (win_client_has_alpha(w)) {
			// The client window also has alpha, the entire window is
			// transparent
			return WMODE_TRANS;
		}
		if (win_has_frame(w)) {
			// The client window doesn't have alpha, but we have a WM
			// frame window, which has alpha.
			return WMODE_FRAME_TRANS;
		}
		// Although the WM window has alpha, the frame window has 0 size,
		// so consider the window solid
	}

	if (w->frame_opacity != 1.0 && win_has_frame(w)) {
		return WMODE_FRAME_TRANS;
	}

	// log_trace("Window %#010x(%s) is solid", w->client_win, w->name);
	return WMODE_SOLID;
}

winmode_t win_calc_mode(const struct win *w) {
	if (win_animatable_get(w, WIN_SCRIPT_OPACITY) < 1.0) {
		return WMODE_TRANS;
	}
	return win_calc_mode_raw(w);
}

/**
 * Calculate and return the opacity target of a window.
 *
 * The priority of opacity settings are:
 *
 * inactive_opacity_override (if set, and unfocused) > _NET_WM_WINDOW_OPACITY (if
 * set) > opacity-rules (if matched) > window type default opacity >
 * active/inactive opacity
 *
 * @param ps           current session
 * @param w            struct _win object representing the window
 *
 * @return target opacity
 */
static double win_calc_opacity_target(session_t *ps, const struct win *w, bool focused) {
	double opacity = 1;

	if (w->state == WSTATE_UNMAPPED || w->state == WSTATE_DESTROYED) {
		// be consistent
		return 0;
	}
	// Try obeying opacity property and window type opacity firstly
	auto window_type = index_of_lowest_one(w->window_types);
	if (w->has_opacity_prop) {
		opacity = ((double)w->opacity_prop) / OPAQUE;
	} else if (!safe_isnan(w->options.opacity)) {
		opacity = w->options.opacity;
	} else if (!safe_isnan(ps->o.wintype_option[window_type].opacity)) {
		opacity = ps->o.wintype_option[window_type].opacity;
	} else {
		// Respect active_opacity only when the window is physically
		// focused
		if (w->is_focused) {
			opacity = ps->o.active_opacity;
		} else if (!focused) {
			// Respect inactive_opacity in some cases
			opacity = ps->o.inactive_opacity;
		}
	}

	// respect inactive override
	if (ps->o.inactive_opacity_override && !focused) {
		opacity = ps->o.inactive_opacity;
	}

	return opacity;
}

static inline double win_get_blur_opacity(const struct win *w) {
	auto wopts = win_options(w);
	return w->state == WSTATE_MAPPED ? wopts.blur_opacity : 0.0;
}

/// Finish the unmapping of a window (e.g. after fading has finished).
/// Doesn't free `w`
void unmap_win_finish(session_t *ps, struct win *w) {
	// We are in unmap_win, this window definitely was viewable
	if (ps->backend_data) {
		// Only the pixmap needs to be freed and reacquired when mapping.
		// Shadow image can be preserved.
		win_release_pixmap(ps->backend_data, w);
	} else {
		assert(!w->win_image);
		assert(!w->shadow_mask);
	}

	// Try again at binding images when the window is mapped next time
	if (w->state != WSTATE_DESTROYED) {
		win_clear_flags(w, WIN_FLAGS_PIXMAP_ERROR);
	}
	assert(w->running_animation_instance == NULL);
}

/**
 * Determine whether a window is to be dimmed.
 */
static void win_update_dim(session_t *ps, struct win *w, bool focused) {
	// Make sure we do nothing if the window is unmapped / being destroyed
	if (w->state == WSTATE_UNMAPPED) {
		return;
	}

	if (ps->o.inactive_dim > 0 && !focused) {
		w->options.dim = ps->o.inactive_dim;
	} else {
		w->options.dim = 0;
	}
}

/**
 * Reread _COMPTON_SHADOW property from a window.
 *
 * The property must be set on the outermost window, usually the WM frame.
 */
void win_update_prop_shadow_raw(struct x_connection *c, struct atom *atoms, struct win *w) {
	winprop_t prop =
	    x_get_prop(c, win_id(w), atoms->a_COMPTON_SHADOW, 1, XCB_ATOM_CARDINAL, 32);

	if (!prop.nitems) {
		w->prop_shadow = -1;
	} else {
		w->prop_shadow = *prop.c32;
	}

	free_winprop(&prop);
}

/**
 * Determine if a window should have shadow, and update things depending
 * on shadow state.
 */
static void win_determine_shadow(session_t *ps, struct win *w) {
	log_debug("Determining shadow of window %#010x (%s)", win_id(w), w->name);
	w->options.shadow = TRI_UNKNOWN;

	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}
	if (!ps->o.wintype_option[index_of_lowest_one(w->window_types)].shadow) {
		log_debug("Shadow disabled by wintypes");
		w->options.shadow = TRI_FALSE;
	} else if (c2_match(ps->c2_state, w, &ps->o.shadow_blacklist, NULL)) {
		log_debug("Shadow disabled by shadow-exclude");
		w->options.shadow = TRI_FALSE;
	} else if (ps->o.shadow_ignore_shaped && w->bounding_shaped && !w->rounded_corners) {
		log_debug("Shadow disabled by shadow-ignore-shaped");
		w->options.shadow = TRI_FALSE;
	} else if (w->prop_shadow == 0) {
		log_debug("Shadow disabled by shadow property");
		w->options.shadow = TRI_FALSE;
	}
}

/**
 * Reread _COMPTON_SHADOW property from a window and update related
 * things.
 */
static bool win_update_prop_shadow(struct x_connection *c, struct atom *atoms, struct win *w) {
	long long attr_shadow_old = w->prop_shadow;
	win_update_prop_shadow_raw(c, atoms, w);
	return w->prop_shadow != attr_shadow_old;
}

/**
 * Update window EWMH fullscreen state.
 */
bool win_update_prop_fullscreen(struct x_connection *c, const struct atom *atoms,
                                struct win *w) {
	auto prop = x_get_prop(c, win_client_id(w, /*fallback_to_self=*/true),
	                       atoms->a_NET_WM_STATE, 12, XCB_ATOM_ATOM, 0);
	bool is_fullscreen = false;
	for (uint32_t i = 0; i < prop.nitems; i++) {
		if (prop.atom[i] == atoms->a_NET_WM_STATE_FULLSCREEN) {
			is_fullscreen = true;
			break;
		}
	}
	free_winprop(&prop);

	bool changed = w->is_ewmh_fullscreen != is_fullscreen;
	w->is_ewmh_fullscreen = is_fullscreen;
	return changed;
}

static void win_determine_clip_shadow_above(session_t *ps, struct win *w) {
	bool should_crop =
	    (ps->o.wintype_option[index_of_lowest_one(w->window_types)].clip_shadow_above ||
	     c2_match(ps->c2_state, w, &ps->o.shadow_clip_list, NULL));
	w->options.clip_shadow_above = should_crop ? TRI_TRUE : TRI_UNKNOWN;
}

/**
 * Determine if a window should have color inverted.
 */
static void win_determine_invert_color(session_t *ps, struct win *w) {
	w->options.invert_color = TRI_UNKNOWN;
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	if (c2_match(ps->c2_state, w, &ps->o.invert_color_list, NULL)) {
		w->options.invert_color = TRI_TRUE;
	}
}

/**
 * Determine if a window should have background blurred.
 */
static void win_determine_blur_background(session_t *ps, struct win *w) {
	log_debug("Determining blur-background of window %#010x (%s)", win_id(w), w->name);
	w->options.blur_background = TRI_UNKNOWN;
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	bool blur_background_new = ps->o.blur_method != BLUR_METHOD_NONE;
	if (blur_background_new) {
		if (!ps->o.wintype_option[index_of_lowest_one(w->window_types)].blur_background) {
			log_debug("Blur background disabled by wintypes");
			w->options.blur_background = TRI_FALSE;
		} else if (c2_match(ps->c2_state, w, &ps->o.blur_background_blacklist, NULL)) {
			log_debug("Blur background disabled by blur-background-exclude");
			w->options.blur_background = TRI_FALSE;
		}
	}
}

/**
 * Determine if a window should have rounded corners.
 */
static void win_determine_rounded_corners(session_t *ps, struct win *w) {
	void *radius_override = NULL;
	bool blacklisted = c2_match(ps->c2_state, w, &ps->o.rounded_corners_blacklist, NULL);
	if (blacklisted) {
		w->options.corner_radius = 0;
		return;
	}

	bool matched =
	    c2_match(ps->c2_state, w, &ps->o.corner_radius_rules, &radius_override);
	if (matched) {
		log_debug("Window %#010x (%s) matched corner rule! %d", win_id(w),
		          w->name, (int)(long)radius_override);
	}

	// Don't round full screen windows & excluded windows,
	// unless we find a corner override in corner_radius_rules
	if (!matched && w->is_fullscreen) {
		w->options.corner_radius = 0;
		log_debug("Not rounding corners for window %#010x", win_id(w));
	} else {
		if (matched) {
			w->options.corner_radius = (int)(long)radius_override;
		} else {
			w->options.corner_radius = -1;
		}

		log_debug("Rounding corners for window %#010x", win_id(w));
		// Initialize the border color to an invalid value
		w->border_col[0] = w->border_col[1] = w->border_col[2] =
		    w->border_col[3] = -1.0F;
	}
}

/**
 * Determine custom window shader to use for a window.
 */
static void win_determine_fg_shader(session_t *ps, struct win *w) {
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	w->options.shader = NULL;

	void *val = NULL;
	if (c2_match(ps->c2_state, w, &ps->o.window_shader_fg_rules, &val)) {
		w->options.shader = val;
	}
}

/**
 * Update window opacity according to opacity rules.
 */
void win_update_opacity_rule(session_t *ps, struct win *w) {
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	double opacity = NAN;
	void *val = NULL;
	if (c2_match(ps->c2_state, w, &ps->o.opacity_rules, &val)) {
		opacity = ((double)(long)val) / 100.0;
	}

	w->options.opacity = opacity;
}

static bool
win_update_rule(struct session *ps, struct win *w, const c2_condition *rule, bool inspect) {
	void *pdata = NULL;
	if (inspect) {
		printf("    %s ... ", c2_condition_to_str(rule));
	}
	bool matched = c2_match_one(ps->c2_state, w, rule, &pdata);
	if (inspect) {
		printf("%s\n", matched ? ANSI("1;32") "matched\033[0m" : "not matched");
	}
	if (!matched) {
		return false;
	}

	auto wopts_next = (struct window_maybe_options *)pdata;
	if (inspect) {
		inspect_dump_window_maybe_options(*wopts_next);
	}
	w->options = win_maybe_options_fold(*wopts_next, w->options);
	return false;
}

/**
 * Function to be called on window data changes.
 *
 * TODO(yshui) need better name
 */
void win_on_factor_change(session_t *ps, struct win *w) {
	auto wid = win_client_id(w, /*fallback_to_self=*/true);
	bool inspect = (ps->o.inspect_win != XCB_NONE && win_id(w) == ps->o.inspect_win) ||
	               ps->o.inspect_monitor;
	log_debug("Window %#010x, client %#010x (%s) factor change", win_id(w), wid, w->name);
	c2_window_state_update(ps->c2_state, &w->c2_state, ps->c.c, wid, win_id(w));
	// Focus and is_fullscreen needs to be updated first, as other rules might depend
	// on the focused state of the window
	win_update_is_fullscreen(ps, w);

	if (ps->o.inspect_monitor) {
		printf("Window %#010x (Client %#010x):\n======\n\n", win_id(w),
		       win_client_id(w, /*fallback_to_self=*/true));
	}

	assert(w->window_types != 0);
	if (list_is_empty(&ps->o.rules)) {
		bool focused = win_is_focused(ps, w);
		auto window_type = index_of_lowest_one(w->window_types);
		// Universal rules take precedence over wintype_option and
		// other exclusion/inclusion lists. And it also supersedes
		// some of the "override" options.
		win_determine_shadow(ps, w);
		win_determine_clip_shadow_above(ps, w);
		win_determine_invert_color(ps, w);
		win_determine_blur_background(ps, w);
		win_determine_rounded_corners(ps, w);
		win_determine_fg_shader(ps, w);
		win_update_opacity_rule(ps, w);
		win_update_dim(ps, w, focused);
		w->mode = win_calc_mode(w);
		log_debug("Window mode changed to %d", w->mode);
		win_update_opacity_rule(ps, w);
		w->opacity = win_calc_opacity_target(ps, w, focused);
		w->options.paint = TRI_UNKNOWN;
		w->options.unredir = WINDOW_UNREDIR_INVALID;
		w->options.fade = TRI_UNKNOWN;
		w->options.transparent_clipping = TRI_UNKNOWN;
		if (w->a.map_state == XCB_MAP_STATE_VIEWABLE &&
		    c2_match(ps->c2_state, w, &ps->o.paint_blacklist, NULL)) {
			w->options.paint = TRI_FALSE;
		}
		if (w->a.map_state == XCB_MAP_STATE_VIEWABLE &&
		    c2_match(ps->c2_state, w, &ps->o.unredir_if_possible_blacklist, NULL)) {
			if (ps->o.wintype_option[window_type].redir_ignore) {
				w->options.unredir = WINDOW_UNREDIR_PASSIVE;
			} else {
				w->options.unredir = WINDOW_UNREDIR_TERMINATE;
			}
		} else if (win_is_bypassing_compositor(ps, w)) {
			// Here we deviate from EWMH a bit. EWMH says we must not
			// unredirect the screen if the window requesting bypassing would
			// look different after unredirecting. Instead we always follow
			// the request.
			w->options.unredir = WINDOW_UNREDIR_FORCED;
		} else if (ps->o.wintype_option[window_type].redir_ignore) {
			w->options.unredir = WINDOW_UNREDIR_WHEN_POSSIBLE;
		}

		if (c2_match(ps->c2_state, w, &ps->o.fade_blacklist, NULL)) {
			w->options.fade = TRI_FALSE;
		}
		if (c2_match(ps->c2_state, w, &ps->o.transparent_clipping_blacklist, NULL)) {
			w->options.transparent_clipping = TRI_FALSE;
		}
		w->options.full_shadow =
		    tri_from_bool(ps->o.wintype_option[window_type].full_shadow);
	} else {
		w->options = WIN_MAYBE_OPTIONS_DEFAULT;
		assert(w->state == WSTATE_MAPPED);
		if (inspect) {
			printf("Checking " BOLD("window rules") ":\n");
		}
		c2_condition_list_foreach_rev(&ps->o.rules, i) {
			win_update_rule(ps, w, i, inspect);
		}
		if (safe_isnan(w->options.opacity) && w->has_opacity_prop) {
			w->options.opacity = ((double)w->opacity_prop) / OPAQUE;
		}
		if (w->options.unredir == WINDOW_UNREDIR_INVALID &&
		    win_is_bypassing_compositor(ps, w)) {
			// If `unredir` is not set by a rule, we follow the bypassing
			// compositor property.
			w->options.unredir = WINDOW_UNREDIR_FORCED;
		}
		w->opacity = win_options(w).opacity;
	}

	w->mode = win_calc_mode(w);
	log_debug("Window mode changed to %d", w->mode);

	if (ps->debug_window != XCB_NONE &&
	    (win_id(w) == ps->debug_window ||
	     (win_client_id(w, /*fallback_to_self=*/false) == ps->debug_window))) {
		w->options.paint = TRI_FALSE;
	}

	if (inspect) {
		inspect_dump_window(ps->c2_state, &ps->o, w);
		printf("\n");
		if (!ps->o.inspect_monitor) {
			quit(ps);
		}
	}
}

/**
 * Update cache data in struct _win that depends on window size.
 */
void win_on_win_size_change(struct win *w, int shadow_offset_x, int shadow_offset_y,
                            int shadow_radius) {
	log_trace("Window %#010x (%s) size changed, was %dx%d, now %dx%d", win_id(w),
	          w->name, w->widthb, w->heightb, w->g.width + w->g.border_width * 2,
	          w->g.height + w->g.border_width * 2);

	w->widthb = w->g.width + w->g.border_width * 2;
	w->heightb = w->g.height + w->g.border_width * 2;
	w->shadow_dx = shadow_offset_x;
	w->shadow_dy = shadow_offset_y;
	w->shadow_width = w->widthb + shadow_radius * 2;
	w->shadow_height = w->heightb + shadow_radius * 2;

	// We don't handle property updates of non-visible windows until they are
	// mapped.
	assert(w->state == WSTATE_MAPPED);
}

/**
 * Update window type.
 */
bool win_update_wintype(struct x_connection *c, struct atom *atoms, struct win *w) {
	const uint32_t wtypes_old = w->window_types;
	auto wid = win_client_id(w, /*fallback_to_self=*/true);

	// Detect window type here
	w->window_types = wid_get_prop_window_types(c, atoms, wid);

	// Conform to EWMH standard, if _NET_WM_WINDOW_TYPE is not present, take
	// override-redirect windows or windows without WM_TRANSIENT_FOR as
	// _NET_WM_WINDOW_TYPE_NORMAL, otherwise as _NET_WM_WINDOW_TYPE_DIALOG.
	if (w->window_types == 0) {
		if (w->a.override_redirect ||
		    !wid_has_prop(c->c, wid, atoms->aWM_TRANSIENT_FOR)) {
			w->window_types = (1 << WINTYPE_NORMAL);
		} else {
			w->window_types = (1 << WINTYPE_DIALOG);
		}
	}

	log_debug("Window (%#010x) has type %#x", win_id(w), w->window_types);

	return w->window_types != wtypes_old;
}

/**
 * Update window after its client window changed.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 */
void win_on_client_update(session_t *ps, struct win *w) {
	// If the window isn't mapped yet, stop here, as the function will be
	// called in map_win()
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	win_update_wintype(&ps->c, ps->atoms, w);

	xcb_window_t client_win_id = win_client_id(w, /*fallback_to_self=*/true);
	// Get frame widths. The window is in damaged area already.
	win_update_frame_extents(&ps->c, ps->atoms, w, client_win_id);

	// Get window group
	if (ps->o.track_leader) {
		auto new_leader = win_get_leader_property(&ps->c, ps->atoms, client_win_id,
		                                          ps->o.detect_transient,
		                                          ps->o.detect_client_leader);
		wm_ref_set_leader(ps->wm, w->tree_ref, new_leader);
	}

	// Get window name and class if we are tracking them
	win_update_name(&ps->c, ps->atoms, w);
	win_update_class(&ps->c, ps->atoms, w);
	win_update_role(&ps->c, ps->atoms, w);
	c2_window_state_mark_dirty_for_client_change(ps->c2_state, &w->c2_state);

	// Update everything related to conditions
	win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);

	auto r = XCB_AWAIT(xcb_get_window_attributes, &ps->c, client_win_id);
	if (!r) {
		return;
	}

	w->client_pictfmt = x_get_pictform_for_visual(&ps->c, r->visual);
	free(r);
}

/**
 * Free all resources in a <code>struct _win</code>.
 */
void free_win_res(session_t *ps, struct win *w) {
	// Above should be done during unmapping
	// Except when we are called by session_destroy

	pixman_region32_fini(&w->damaged);
	pixman_region32_fini(&w->bounding_shape);
	// BadDamage may be thrown if the window is destroyed
	x_set_error_action_ignore(&ps->c, xcb_damage_destroy(ps->c.c, w->damage));
	free(w->name);
	free(w->class_instance);
	free(w->class_general);
	free(w->role);

	free(w->stale_props);
	w->stale_props = NULL;
	w->stale_props_capacity = 0;
	c2_window_state_destroy(ps->c2_state, &w->c2_state);
}

/// Query the Xorg for information about window `win`, and assign a window to `cursor` if
/// this window should be managed.
struct win *win_maybe_allocate(session_t *ps, struct wm_ref *cursor,
                               const xcb_get_window_attributes_reply_t *attrs) {
	static const struct win win_def = {
	    .frame_opacity = 1.0,
	    .in_openclose = true,        // set to false after first map is done,
	                                 // true here because window is just created
	    .flags = 0,                  // updated by
	                                 // property/attributes/etc
	                                 // change

	    .mode = WMODE_TRANS,
	    .opacity_prop = OPAQUE,
	    .opacity_set = 1,
	    .frame_extents = MARGIN_INIT,
	    .prop_shadow = -1,
	};

	// Reject overlay window
	if (wm_ref_win_id(cursor) == ps->overlay) {
		// Would anyone reparent windows to the overlay window? Doing this
		// just in case.
		return NULL;
	}

	xcb_window_t wid = wm_ref_win_id(cursor);
	log_debug("Managing window %#010x", wid);
	if (attrs->map_state == XCB_MAP_STATE_UNVIEWABLE) {
		// Failed to get window attributes or geometry probably means
		// the window is gone already. Unviewable means the window is
		// already reparented elsewhere.
		// BTW, we don't care about Input Only windows, except for
		// stacking proposes, so we need to keep track of them still.
		return NULL;
	}

	if (attrs->_class == XCB_WINDOW_CLASS_INPUT_ONLY) {
		// No need to manage this window, but we still keep it on the
		// window stack
		return NULL;
	}

	// Allocate and initialize the new win structure
	auto new = cmalloc(struct win);

	// Fill structure
	// We only need to initialize the part that are not initialized
	// by map_win
	*new = win_def;
	new->a = *attrs;
	new->shadow_opacity = ps->o.shadow_opacity;
	pixman_region32_init(&new->bounding_shape);

	xcb_generic_error_t *e;
	auto g = xcb_get_geometry_reply(ps->c.c, xcb_get_geometry(ps->c.c, wid), &e);
	if (!g) {
		log_debug("Failed to get geometry of window %#010x: %s", wid,
		          x_strerror(&ps->c, e));
		free(e);
		free(new);
		return NULL;
	}
	new->pending_g = (struct win_geometry){
	    .x = g->x,
	    .y = g->y,
	    .width = g->width,
	    .height = g->height,
	    .border_width = g->border_width,
	};

	free(g);

	// Create Damage for window (if not Input Only)
	new->damage = x_new_id(&ps->c);
	e = xcb_request_check(
	    ps->c.c, xcb_damage_create_checked(ps->c.c, new->damage, wid,
	                                       XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY));
	if (e) {
		log_debug("Failed to create damage for window %#010x: %s", wid,
		          x_strerror(&ps->c, e));
		free(e);
		free(new);
		return NULL;
	}

	// Set window event mask
	uint32_t frame_event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
	                            XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
	                            XCB_EVENT_MASK_STRUCTURE_NOTIFY;
	if (!ps->o.use_ewmh_active_win) {
		frame_event_mask |= XCB_EVENT_MASK_FOCUS_CHANGE;
	}
	x_set_error_action_ignore(
	    &ps->c, xcb_change_window_attributes(ps->c.c, wid, XCB_CW_EVENT_MASK,
	                                         (const uint32_t[]){frame_event_mask}));

	// Get notification when the shape of a window changes
	if (ps->c.e.has_shape) {
		x_set_error_action_ignore(&ps->c, xcb_shape_select_input(ps->c.c, wid, 1));
	}

	new->pictfmt = x_get_pictform_for_visual(&ps->c, new->a.visual);
	new->client_pictfmt = NULL;
	new->tree_ref = cursor;
	new->options = WIN_MAYBE_OPTIONS_DEFAULT;
	new->options_override = WIN_MAYBE_OPTIONS_DEFAULT;
	new->options_default = &ps->window_options_default;

	// Set all the stale flags on this new window, so it's properties will get
	// updated when it's mapped
	win_set_flags(new, WIN_FLAGS_SIZE_STALE | WIN_FLAGS_POSITION_STALE |
	                       WIN_FLAGS_PROPERTY_STALE | WIN_FLAGS_FACTOR_CHANGED);
	xcb_atom_t init_stale_props[] = {
	    ps->atoms->a_NET_WM_WINDOW_TYPE, ps->atoms->a_NET_WM_WINDOW_OPACITY,
	    ps->atoms->a_NET_FRAME_EXTENTS,  ps->atoms->aWM_NAME,
	    ps->atoms->a_NET_WM_NAME,        ps->atoms->aWM_CLASS,
	    ps->atoms->aWM_WINDOW_ROLE,      ps->atoms->a_COMPTON_SHADOW,
	    ps->atoms->aWM_CLIENT_LEADER,    ps->atoms->aWM_TRANSIENT_FOR,
	    ps->atoms->a_NET_WM_STATE,
	};
	win_set_properties_stale(new, init_stale_props, ARR_SIZE(init_stale_props));
	c2_window_state_init(ps->c2_state, &new->c2_state);
	pixman_region32_init(&new->damaged);

	wm_ref_set(cursor, new);

	return new;
}

/**
 * Update leader of a window.
 */
static xcb_window_t
win_get_leader_property(struct x_connection *c, struct atom *atoms, xcb_window_t wid,
                        bool detect_transient, bool detect_client_leader) {
	xcb_window_t leader = XCB_NONE;
	bool exists = false;

	// Read the leader properties
	if (detect_transient) {
		leader = wid_get_prop_window(c, wid, atoms->aWM_TRANSIENT_FOR, &exists);
		log_debug("Leader via WM_TRANSIENT_FOR of window %#010x: %#010x", wid, leader);
		if (exists && (leader == c->screen_info->root || leader == XCB_NONE)) {
			// If WM_TRANSIENT_FOR is set to NONE or the root window, use the
			// window group leader.
			//
			// Ref:
			// https://specifications.freedesktop.org/wm-spec/wm-spec-1.5.html#idm44981516332096
			auto prop = x_get_prop(c, wid, XCB_ATOM_WM_HINTS, INT_MAX,
			                       XCB_ATOM_WM_HINTS, 32);
			if (prop.nitems >= 9) {        // 9-th member is window_group
				leader = prop.c32[8];
				log_debug("Leader via WM_HINTS of window %#010x: %#010x",
				          wid, leader);
			} else {
				leader = XCB_NONE;
			}
			free_winprop(&prop);
		}
	}

	if (detect_client_leader && leader == XCB_NONE) {
		leader = wid_get_prop_window(c, wid, atoms->aWM_CLIENT_LEADER, &exists);
		log_debug("Leader via WM_CLIENT_LEADER of window %#010x: %#010x", wid, leader);
	}

	log_debug("window %#010x: leader %#010x", wid, leader);
	return leader;
}

/**
 * Retrieve the <code>WM_CLASS</code> of a window and update its
 * <code>win</code> structure.
 */
bool win_update_class(struct x_connection *c, struct atom *atoms, struct win *w) {
	char **strlst = NULL;
	int nstr = 0;
	auto client_win = win_client_id(w, /*fallback_to_self=*/true);

	// Free and reset old strings
	free(w->class_instance);
	free(w->class_general);
	w->class_instance = NULL;
	w->class_general = NULL;

	// Retrieve the property string list
	if (!wid_get_text_prop(c, atoms, client_win, atoms->aWM_CLASS, &strlst, &nstr)) {
		return false;
	}

	// Copy the strings if successful
	w->class_instance = strdup(strlst[0]);

	if (nstr > 1) {
		w->class_general = strdup(strlst[1]);
	}

	free(strlst);

	log_trace("(%#010x): client = %#010x, instance = \"%s\", general = \"%s\"",
	          win_id(w), client_win, w->class_instance, w->class_general);

	return true;
}

/**
 * Get a rectangular region a window (and possibly its shadow) occupies.
 *
 * Note w->shadow and shadow geometry must be correct before calling this
 * function.
 */
void win_extents(const struct win *w, region_t *res) {
	pixman_region32_clear(res);
	if (w->state != WSTATE_MAPPED) {
		return;
	}

	pixman_region32_union_rect(res, res, w->g.x, w->g.y, (uint)w->widthb, (uint)w->heightb);
	if (win_options(w).shadow) {
		assert(w->shadow_width >= 0 && w->shadow_height >= 0);
		pixman_region32_union_rect(res, res, w->g.x + w->shadow_dx,
		                           w->g.y + w->shadow_dy, (uint)w->shadow_width,
		                           (uint)w->shadow_height);
	}
}

gen_by_val(win_extents);

/**
 * Update the out-dated bounding shape of a window.
 *
 * Mark the window shape as updated
 */
void win_update_bounding_shape(struct x_connection *c, struct win *w,
                               bool detect_rounded_corners) {
	// We don't handle property updates of non-visible windows until they are
	// mapped.
	assert(w->state == WSTATE_MAPPED);

	pixman_region32_clear(&w->bounding_shape);
	// Start with the window rectangular region
	win_get_region_local(w, &w->bounding_shape);

	if (c->e.has_shape) {
		w->bounding_shaped = win_bounding_shaped(c, win_id(w));
	}

	// Only request for a bounding region if the window is shaped
	// (while loop is used to avoid goto, not an actual loop)
	while (w->bounding_shaped) {
		/*
		 * if window doesn't exist anymore,  this will generate an error
		 * as well as not generate a region.
		 */

		xcb_shape_get_rectangles_reply_t *r = xcb_shape_get_rectangles_reply(
		    c->c, xcb_shape_get_rectangles(c->c, win_id(w), XCB_SHAPE_SK_BOUNDING),
		    NULL);

		if (!r) {
			break;
		}

		xcb_rectangle_t *xrects = xcb_shape_get_rectangles_rectangles(r);
		int nrects = xcb_shape_get_rectangles_rectangles_length(r);
		rect_t *rects = from_x_rects(nrects, xrects);
		free(r);

		region_t br;
		pixman_region32_init_rects(&br, rects, nrects);
		free(rects);

		// Add border width because we are using a different origin.
		// X thinks the top left of the inner window is the origin
		// (for the bounding shape, although xcb_get_geometry thinks
		//  the outer top left (outer means outside of the window
		//  border) is the origin),
		// We think the top left of the border is the origin
		pixman_region32_translate(&br, w->g.border_width, w->g.border_width);

		// Intersect the bounding region we got with the window rectangle,
		// to make sure the bounding region is not bigger than the window
		// rectangle
		pixman_region32_intersect(&w->bounding_shape, &w->bounding_shape, &br);
		pixman_region32_fini(&br);
		break;
	}

	if (w->bounding_shaped && detect_rounded_corners) {
		w->rounded_corners = win_has_rounded_corners(w);
	}
}

/**
 * Retrieve frame extents from a window.
 */
void win_update_frame_extents(struct x_connection *c, struct atom *atoms, struct win *w,
                              xcb_window_t client) {
	if (client == XCB_NONE) {
		w->frame_extents = (margin_t){0};
		return;
	}

	winprop_t prop =
	    x_get_prop(c, client, atoms->a_NET_FRAME_EXTENTS, 4L, XCB_ATOM_CARDINAL, 32);

	if (prop.nitems == 4) {
		int extents[4];
		for (int i = 0; i < 4; i++) {
			if (prop.c32[i] > (uint32_t)INT_MAX) {
				log_warn("Your window manager sets a absurd "
				         "_NET_FRAME_EXTENTS value (%u), "
				         "ignoring it.",
				         prop.c32[i]);
				memset(extents, 0, sizeof(extents));
				break;
			}
			extents[i] = (int)prop.c32[i];
		}

		w->frame_extents.left = extents[0];
		w->frame_extents.right = extents[1];
		w->frame_extents.top = extents[2];
		w->frame_extents.bottom = extents[3];
	}

	log_trace("(%#010x): %d, %d, %d, %d", win_id(w), w->frame_extents.left,
	          w->frame_extents.right, w->frame_extents.top, w->frame_extents.bottom);

	free_winprop(&prop);
}

/// Finish the destruction of a window (e.g. after fading has finished).
/// Frees `w`
void win_destroy_finish(session_t *ps, struct win *w) {
	log_debug("Trying to finish destroying (%#010x)", win_id(w));

	unmap_win_finish(ps, w);

	// Unmapping might preserve the shadow image, so free it here
	win_release_shadow(ps->backend_data, w);
	win_release_mask(ps->backend_data, w);

	free_win_res(ps, w);

	wm_reap_zombie(w->tree_ref);
	free(w);
}

/// Start destroying a window. Windows cannot always be destroyed immediately
/// because of fading and such.
void win_destroy_start(struct win *w) {
	BUG_ON(w == NULL);
	log_debug("Destroying %#010x (%s)", win_id(w), w->name);

	if (w->state != WSTATE_UNMAPPED) {
		// Only UNMAPPED state has window resources freed,
		// otherwise we need to call unmap_win_finish to free
		// them.
		log_warn("Did X server not unmap window %#010x before destroying "
		         "it?",
		         win_id(w));
	}
	// Clear IMAGES_STALE flags since the window is destroyed: Clear
	// PIXMAP_STALE as there is no pixmap available anymore, so STALE
	// doesn't make sense.
	// XXX Clear SHADOW_STALE as setting/clearing flags on a destroyed
	// window doesn't work leading to an inconsistent state where the
	// shadow is refreshed but the flags are stuck in STALE. Do this
	// before changing the window state to destroying
	win_clear_flags(w, WIN_FLAGS_PIXMAP_STALE);

	// Clear some flags about stale window information. Because now
	// the window is destroyed, we can't update them anyway.
	win_clear_flags(w, WIN_FLAGS_SIZE_STALE | WIN_FLAGS_POSITION_STALE |
	                       WIN_FLAGS_PROPERTY_STALE | WIN_FLAGS_FACTOR_CHANGED);

	// Update state flags of a managed window
	w->state = WSTATE_DESTROYED;
	w->opacity = 0.0F;
	w->a.map_state = XCB_MAP_STATE_UNMAPPED;
	w->in_openclose = true;
}

void unmap_win_start(struct win *w) {
	assert(w);
	assert(w->a._class != XCB_WINDOW_CLASS_INPUT_ONLY);

	log_debug("Unmapping %#010x (%s)", win_id(w), w->name);

	assert(w->state != WSTATE_DESTROYED);

	if (unlikely(w->state == WSTATE_UNMAPPED)) {
		assert(win_check_flags_all(w, WIN_FLAGS_MAPPED));
		// Window is mapped, but we hadn't had a chance to handle the MAPPED flag.
		// Clear the pending map as this window is now unmapped
		win_clear_flags(w, WIN_FLAGS_MAPPED);
		return;
	}

	// Note we don't update focused window here. This will either be
	// triggered by subsequence Focus{In, Out} event, or by recheck_focus

	w->a.map_state = XCB_MAP_STATE_UNMAPPED;
	w->state = WSTATE_UNMAPPED;
	w->opacity = 0.0F;
}

static inline struct win_script_context
win_script_context_prepare(struct session *ps, struct win *w) {
	auto monitor_index = win_find_monitor(&ps->monitors, w);
	auto monitor =
	    monitor_index >= 0
	        ? *pixman_region32_extents(&ps->monitors.regions[monitor_index])
	        : (pixman_box32_t){
	              .x1 = 0, .y1 = 0, .x2 = ps->root_width, .y2 = ps->root_height};
	struct win_script_context ret = {
	    .x = w->g.x,
	    .y = w->g.y,
	    .width = w->widthb,
	    .height = w->heightb,
	    .opacity = w->opacity,
	    .x_before = w->previous.g.x,
	    .y_before = w->previous.g.y,
	    .width_before = w->previous.g.width + w->previous.g.border_width * 2,
	    .height_before = w->previous.g.height + w->previous.g.border_width * 2,
	    .opacity_before = w->previous.opacity,
	    .blur_opacity = win_get_blur_opacity(w),
	    .blur_opacity_before = w->previous.blur_opacity,
	    .monitor_x = monitor.x1,
	    .monitor_y = monitor.y1,
	    .monitor_width = monitor.x2 - monitor.x1,
	    .monitor_height = monitor.y2 - monitor.y1,
	    .shadow_color_before = w->previous.shadow_color,
	    .shadow_color = w->options.shadow_color,
	};
	return ret;
}

double win_animatable_get(const struct win *w, enum win_script_output output) {
	if (w->running_animation_instance && w->running_animation.output_indices[output] >= 0) {
		return w->running_animation_instance
		    ->memory[w->running_animation.output_indices[output]];
	}

	auto wopts = win_options(w);
	switch (output) {
	case WIN_SCRIPT_BLUR_OPACITY: return win_get_blur_opacity(w);
	case WIN_SCRIPT_OPACITY:
	case WIN_SCRIPT_SHADOW_OPACITY: return w->opacity;
	case WIN_SCRIPT_CROP_X:
	case WIN_SCRIPT_CROP_Y:
	case WIN_SCRIPT_OFFSET_X:
	case WIN_SCRIPT_OFFSET_Y:
	case WIN_SCRIPT_SHADOW_OFFSET_X:
	case WIN_SCRIPT_SHADOW_OFFSET_Y: return 0;
	case WIN_SCRIPT_SCALE_X:
	case WIN_SCRIPT_SCALE_Y:
	case WIN_SCRIPT_SHADOW_SCALE_X:
	case WIN_SCRIPT_SHADOW_SCALE_Y: return 1;
	case WIN_SCRIPT_CROP_WIDTH:
	case WIN_SCRIPT_CROP_HEIGHT: return INFINITY;
	case WIN_SCRIPT_SAVED_IMAGE_BLEND: return 0;
	case WIN_SCRIPT_SHADOW_RED: return wopts.shadow_color.red;
	case WIN_SCRIPT_SHADOW_GREEN: return wopts.shadow_color.green;
	case WIN_SCRIPT_SHADOW_BLUE: return wopts.shadow_color.blue;
	default: unreachable();
	}
	unreachable();
}

#define WSTATE_PAIR(a, b) ((int)(a) * NUM_OF_WSTATES + (int)(b))
/// Advance the animation of a window.
///
/// Returns true if animation was running before this function is called, and is no
/// longer running now. Returns false if animation is still running, or if there was no
/// animation running when this is called.
static bool win_advance_animation(struct win *w, double delta_t,
                                  const struct win_script_context *win_ctx) {
	// No state changes, if there's a animation running, we just continue it.
	if (w->running_animation_instance == NULL) {
		return false;
	}
	log_verbose("Advance animation for %#010x (%s) %f seconds", win_id(w), w->name, delta_t);
	if (!script_instance_is_finished(w->running_animation_instance)) {
		auto elapsed_slot =
		    script_elapsed_slot(w->running_animation_instance->script);
		w->running_animation_instance->memory[elapsed_slot] += delta_t;
		auto result = script_instance_evaluate(w->running_animation_instance,
		                                       (void *)win_ctx, false);
		if (result != SCRIPT_EVAL_OK) {
			log_error("Failed to run animation script: %d", result);
			return true;
		}
		return false;
	}
	return true;
}

bool win_process_animation_and_state_change(struct session *ps, struct win *w, double delta_t) {
	// If the window hasn't ever been damaged yet, it won't be rendered in this frame.
	// Or if it doesn't have a image bound, it won't be rendered either. (This can
	// happen is a window is destroyed during a backend reset. Backend resets releases
	// all images, and if a window is freed during that, its image cannot be
	// reacquired.)
	//
	// If the window won't be rendered, and it is also unmapped/destroyed, it won't
	// receive damage events or reacquire an image. In this case we can skip its
	// animation. For mapped windows, we need to provisionally start animation,
	// because its first damage event might come a bit late.
	bool will_never_render =
	    (!w->ever_damaged || w->win_image == NULL) && w->state != WSTATE_MAPPED;
	auto win_ctx = win_script_context_prepare(ps, w);

	bool size_changed = win_size_changed(w->previous.g, w->g);
	bool position_changed = win_position_changed(w->previous.g, w->g);

	auto old_state = w->previous.state;

	w->previous.state = w->state;
	w->previous.opacity = w->opacity;
	w->previous.g = w->g;
	w->previous.shadow_color = w->options.shadow_color;
	w->previous.blur_opacity = win_get_blur_opacity(w);

	if (!ps->redirected || will_never_render) {
		// This window won't be rendered, so we don't need to run the animations.
		bool state_changed = old_state != w->state ||
		                     win_ctx.opacity_before != win_ctx.opacity ||
		                     size_changed || position_changed;
		return state_changed || (w->running_animation_instance != NULL);
	}

	// Try to determine the right animation trigger based on state changes. Note there
	// is some complications here. X automatically unmaps windows before destroying
	// them. So a "close" trigger will also be fired from a UNMAPPED -> DESTROYED
	// transition, besides the more obvious MAPPED -> DESTROYED transition. But this
	// also means, if the user didn't configure a animation for "hide", but did
	// for "close", there is a chance this animation won't be triggered, if there is a
	// gap between the UnmapNotify and DestroyNotify. There is no way on our end of
	// fixing this without using hacks.
	enum animation_trigger trigger = ANIMATION_TRIGGER_INVALID;

	// Animation trigger priority:
	//   state > position > size > opacity > color
	if (old_state != w->state) {
		// Send D-Bus signal
		if (ps->o.dbus) {
			switch (w->state) {
			case WSTATE_UNMAPPED:
				cdbus_ev_win_unmapped(session_get_cdbus(ps), w);
				break;
			case WSTATE_MAPPED:
				cdbus_ev_win_mapped(session_get_cdbus(ps), w);
				break;
			case WSTATE_DESTROYED:
				cdbus_ev_win_destroyed(session_get_cdbus(ps), w);
				break;
			}
		}

		switch (WSTATE_PAIR(old_state, w->state)) {
		case WSTATE_PAIR(WSTATE_UNMAPPED, WSTATE_MAPPED):
			trigger = w->in_openclose ? ANIMATION_TRIGGER_OPEN
			                          : ANIMATION_TRIGGER_SHOW;
			break;
		case WSTATE_PAIR(WSTATE_UNMAPPED, WSTATE_DESTROYED):
			if ((!ps->o.no_fading_destroyed_argb || !win_has_alpha(w)) &&
			    w->running_animation_instance != NULL) {
				trigger = ANIMATION_TRIGGER_CLOSE;
			}
			break;
		case WSTATE_PAIR(WSTATE_MAPPED, WSTATE_DESTROYED):
			// TODO(yshui) we should deprecate "no-fading-destroyed-argb" and
			// ask user to write fading rules (after we have added such
			// rules). Ditto below.
			if (!ps->o.no_fading_destroyed_argb || !win_has_alpha(w)) {
				trigger = ANIMATION_TRIGGER_CLOSE;
			}
			break;
		case WSTATE_PAIR(WSTATE_MAPPED, WSTATE_UNMAPPED):
			trigger = ANIMATION_TRIGGER_HIDE;
			break;
		default:
			log_error("Impossible state transition from %d to %d", old_state,
			          w->state);
			assert(false);
			return true;
		}
	} else if (position_changed) {
		assert(w->state == WSTATE_MAPPED);
		trigger = ANIMATION_TRIGGER_POSITION;
	} else if (size_changed) {
		assert(w->state == WSTATE_MAPPED);
		trigger = ANIMATION_TRIGGER_SIZE;
	} else if (win_ctx.opacity_before != win_ctx.opacity) {
		assert(w->state == WSTATE_MAPPED);
		trigger = win_ctx.opacity > win_ctx.opacity_before
		              ? ANIMATION_TRIGGER_INCREASE_OPACITY
		              : ANIMATION_TRIGGER_DECREASE_OPACITY;
	} else if (win_ctx.blur_opacity_before != win_ctx.blur_opacity) {
		trigger = win_ctx.blur_opacity > win_ctx.blur_opacity_before
		              ? ANIMATION_TRIGGER_INCREASE_OPACITY
		              : ANIMATION_TRIGGER_DECREASE_OPACITY;
	} else if (!color_eq(win_ctx.shadow_color_before, win_ctx.shadow_color)) {
		assert(w->state == WSTATE_MAPPED);
		trigger = ANIMATION_TRIGGER_COLOR;
	}

	if (trigger == ANIMATION_TRIGGER_INVALID) {
		// No state changes, if there's a animation running, we just continue it.
		return win_advance_animation(w, delta_t, &win_ctx);
	}

	if (w->running_animation_instance &&
	    (w->running_animation.suppressions & (1 << trigger)) != 0) {
		log_debug("Not starting animation %s for window %#010x (%s) because it "
		          "is being suppressed.",
		          animation_trigger_names[trigger], win_id(w), w->name);
		return win_advance_animation(w, delta_t, &win_ctx);
	}

	if (w->animation_block[trigger] > 0) {
		log_debug("Not starting animation %s for window %#010x (%s) because it "
		          "is blocked.",
		          animation_trigger_names[trigger], win_id(w), w->name);
		return win_advance_animation(w, delta_t, &win_ctx);
	}

	auto wopts = win_options(w);
	if (wopts.animations[trigger].script == NULL) {
		return true;
	}

	if (wopts.animations[trigger].is_generated && !wopts.fade) {
		// Window's animation is fading (as signified by the fact that it's
		// generated), but the user has disabled fading for this window.
		return true;
	}

	log_debug("Starting animation %s for window %#010x (%s)",
	          animation_trigger_names[trigger], win_id(w), w->name);

	if (win_check_flags_any(w, WIN_FLAGS_PIXMAP_STALE)) {
		// Grab the old pixmap, animations might need it
		if (w->saved_win_image) {
			win_release_saved_win_image(ps->backend_data, w);
		}
		if (ps->drivers & DRIVER_NVIDIA) {
			// NVIDIA doesn't like us grabbing the new pixmap before releasing
			// the old one. So we copy the content of the old pixmap so we can
			// release it.
			if (w->win_image != NULL) {
				w->saved_win_image = ps->backend_data->ops.new_image(
				    ps->backend_data, BACKEND_IMAGE_FORMAT_PIXMAP,
				    (ivec2){
				        .width = (int)win_ctx.width_before,
				        .height = (int)win_ctx.height_before,
				    });
				region_t copy_region;
				pixman_region32_init_rect(&copy_region, 0, 0,
				                          (uint)win_ctx.width_before,
				                          (uint)win_ctx.height_before);
				ps->backend_data->ops.copy_area(
				    ps->backend_data, (ivec2){}, w->saved_win_image,
				    w->win_image, &copy_region);
				pixman_region32_fini(&copy_region);
			}
		} else {
			w->saved_win_image = w->win_image;
			w->win_image = NULL;
		}
		w->saved_win_image_scale = (vec2){
		    .x = win_ctx.width / win_ctx.width_before,
		    .y = win_ctx.height / win_ctx.height_before,
		};
	}

	auto new_animation = script_instance_new(wopts.animations[trigger].script);
	if (w->running_animation_instance) {
		// Interrupt the old animation and start the new animation from where the
		// old has left off. Note we still need to advance the old animation for
		// the last interval.
		win_advance_animation(w, delta_t, &win_ctx);
		auto memory = w->running_animation_instance->memory;
		auto output_indices = w->running_animation.output_indices;
		if (output_indices[WIN_SCRIPT_SAVED_IMAGE_BLEND] >= 0) {
			memory[output_indices[WIN_SCRIPT_SAVED_IMAGE_BLEND]] =
			    1 - memory[output_indices[WIN_SCRIPT_SAVED_IMAGE_BLEND]];
		}
		if (size_changed || position_changed) {
			// If the window has moved, we need to adjust scripts
			// outputs so that the window will stay in the same position and
			// size after applying the animation. This way the window's size
			// and position won't change discontinuously.
			struct {
				int output;
				double delta;
			} adjustments[] = {
			    {WIN_SCRIPT_OFFSET_X, win_ctx.x_before - win_ctx.x},
			    {WIN_SCRIPT_OFFSET_Y, win_ctx.y_before - win_ctx.y},
			    {WIN_SCRIPT_SHADOW_OFFSET_X, win_ctx.x_before - win_ctx.x},
			    {WIN_SCRIPT_SHADOW_OFFSET_Y, win_ctx.y_before - win_ctx.y},
			};
			for (size_t i = 0; i < ARR_SIZE(adjustments); i++) {
				if (output_indices[adjustments[i].output] >= 0) {
					memory[output_indices[adjustments[i].output]] +=
					    adjustments[i].delta;
				}
			}

			struct {
				int output;
				double factor;
			} factors[] = {
			    {WIN_SCRIPT_SCALE_X, win_ctx.width_before / win_ctx.width},
			    {WIN_SCRIPT_SCALE_Y, win_ctx.height_before / win_ctx.height},
			    {WIN_SCRIPT_SHADOW_SCALE_X, win_ctx.width_before / win_ctx.width},
			    {WIN_SCRIPT_SHADOW_SCALE_Y, win_ctx.height_before / win_ctx.height},
			};
			for (size_t i = 0; i < ARR_SIZE(factors); i++) {
				if (output_indices[factors[i].output] >= 0) {
					memory[output_indices[factors[i].output]] *=
					    factors[i].factor;
				}
			}
		}
		script_instance_resume_from(w->running_animation_instance, new_animation);
		free(w->running_animation_instance);
	}
	w->running_animation_instance = new_animation;
	w->running_animation = wopts.animations[trigger];
	script_instance_evaluate(w->running_animation_instance, &win_ctx, true);
	return script_instance_is_finished(w->running_animation_instance);
}

#undef WSTATE_PAIR

/// Find which monitor a window is on.
int win_find_monitor(const struct x_monitors *monitors, const struct win *mw) {
	int ret = -1;
	for (int i = 0; i < monitors->count; i++) {
		auto e = pixman_region32_extents(&monitors->regions[i]);
		if (e->x1 <= mw->g.x && e->y1 <= mw->g.y &&
		    e->x2 >= mw->g.x + mw->widthb && e->y2 >= mw->g.y + mw->heightb) {
			log_verbose("Window %#010x (%s), %dx%d+%dx%d, is entirely on the "
			            "monitor %d (%dx%d+%dx%d)",
			            win_id(mw), mw->name, mw->g.x, mw->g.y, mw->widthb,
			            mw->heightb, i, e->x1, e->y1, e->x2 - e->x1,
			            e->y2 - e->y1);
			return i;
		}
	}
	log_verbose("Window %#010x (%s), %dx%d+%dx%d, is not entirely on any monitor",
	            win_id(mw), mw->name, mw->g.x, mw->g.y, mw->widthb, mw->heightb);
	return ret;
}

bool win_set_pending_geometry(struct win *w, struct win_geometry g) {
	// We check against pending_g here, because there might have been multiple
	// configure notifies in this cycle, or the window could receive multiple updates
	// while it's unmapped. `pending_g` should be equal to `g` otherwise.
	bool position_changed = w->pending_g.x != g.x || w->pending_g.y != g.y;
	bool size_changed = w->pending_g.width != g.width || w->pending_g.height != g.height ||
	                    w->pending_g.border_width != g.border_width;
	if (position_changed || size_changed) {
		// Queue pending updates
		win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);

		// At least one of the following if's is true
		if (position_changed) {
			log_trace("Window %#010x position changed, %dx%d -> %dx%d",
			          win_id(w), w->g.x, w->g.y, g.x, g.y);
			w->pending_g.x = g.x;
			w->pending_g.y = g.y;
			win_set_flags(w, WIN_FLAGS_POSITION_STALE);
		}

		if (size_changed) {
			log_trace("Window %#010x size changed, %dx%d -> %dx%d", win_id(w),
			          w->g.width, w->g.height, g.width, g.height);
			w->pending_g.width = g.width;
			w->pending_g.height = g.height;
			w->pending_g.border_width = g.border_width;
			win_set_flags(w, WIN_FLAGS_SIZE_STALE);
		}
	}
	return position_changed || size_changed;
}

struct win_get_geometry_request {
	struct x_async_request_base base;
	struct session *ps;
	xcb_window_t wid;
};

static void win_handle_get_geometry_reply(struct x_connection *c,
                                          struct x_async_request_base *req_base,
                                          const xcb_raw_generic_event_t *reply_or_error) {
	auto req = (struct win_get_geometry_request *)req_base;
	auto wid = req->wid;
	auto ps = req->ps;
	free(req);

	if (reply_or_error == NULL) {
		// Shutting down
		return;
	}

	if (reply_or_error->response_type == 0) {
		log_debug("Failed to get geometry of window %#010x: %s", wid,
		          x_strerror(c, (xcb_generic_error_t *)reply_or_error));
		return;
	}

	auto cursor = wm_find(ps->wm, wid);
	if (cursor == NULL) {
		// Rare, window is destroyed then its ID is reused
		if (wm_is_consistent(ps->wm)) {
			log_error("Successfully fetched geometry of a non-existent "
			          "window %#010x",
			          wid);
			assert(false);
		}
		return;
	}

	auto w = wm_ref_deref(cursor);
	if (w == NULL) {
		// Not yet managed. Rare, window is destroyed then its ID is reused
		return;
	}

	auto r = (const xcb_get_geometry_reply_t *)reply_or_error;
	ps->pending_updates |= win_set_pending_geometry(w, win_geometry_from_get_geometry(r));
}

/// Start the mapping of a window. We cannot map immediately since we might need to fade
/// the window in.
void win_map_start(struct session *ps, struct win *w) {
	assert(w);

	// Don't care about window mapping if it's an InputOnly window
	// Also, try avoiding mapping a window twice
	if (w->a._class == XCB_WINDOW_CLASS_INPUT_ONLY) {
		return;
	}

	log_debug("Mapping (%#010x \"%s\"), old state %d", win_id(w), w->name, w->state);

	assert(w->state != WSTATE_DESTROYED);
	if (w->state == WSTATE_MAPPED) {
		log_error("Mapping an already mapped window");
		assert(false);
		return;
	}

	// Rant: window size could change after we queried its geometry here and
	// before we get its pixmap. Later, when we get back to the event
	// processing loop, we will get the notification about size change from
	// Xserver and try to refresh the pixmap, while the pixmap is actually
	// already up-to-date (i.e. the notification is stale). There is basically
	// no real way to prevent this, aside from grabbing the server.

	// XXX Can we assume map_state is always viewable?
	w->a.map_state = XCB_MAP_STATE_VIEWABLE;

	// Update window mode here to check for ARGB windows
	w->mode = win_calc_mode(w);

	w->state = WSTATE_MAPPED;
	win_set_flags(
	    w, WIN_FLAGS_PIXMAP_STALE | WIN_FLAGS_CLIENT_STALE | WIN_FLAGS_FACTOR_CHANGED);

	auto req = ccalloc(1, struct win_get_geometry_request);
	req->base = (struct x_async_request_base){
	    .callback = win_handle_get_geometry_reply,
	    .sequence = xcb_get_geometry(ps->c.c, win_id(w)).sequence,
	};
	req->wid = win_id(w);
	req->ps = ps;
	x_await_request(&ps->c, &req->base);
}

/// Set flags on a window. Some sanity checks are performed
void win_set_flags(struct win *w, uint64_t flags) {
	log_verbose("Set flags %" PRIu64 " to window %#010x (%s)", flags, win_id(w), w->name);
	if (unlikely(w->state == WSTATE_DESTROYED)) {
		log_error("Flags set on a destroyed window %#010x (%s)", win_id(w), w->name);
		return;
	}

	w->flags |= flags;
}

/// Clear flags on a window. Some sanity checks are performed
void win_clear_flags(struct win *w, uint64_t flags) {
	log_verbose("Clear flags %" PRIu64 " from window %#010x (%s)", flags, win_id(w),
	            w->name);
	if (unlikely(w->state == WSTATE_DESTROYED)) {
		log_warn("Flags %" PRIu64 " cleared on a destroyed window %#010x (%s)",
		         flags, win_id(w), w->name);
		return;
	}

	w->flags = w->flags & (~flags);
}

void win_set_properties_stale(struct win *w, const xcb_atom_t *props, int nprops) {
	auto const bits_per_element = sizeof(*w->stale_props) * 8;
	size_t new_capacity = w->stale_props_capacity;

	// Calculate the new capacity of the properties array
	for (int i = 0; i < nprops; i++) {
		if (props[i] >= new_capacity * bits_per_element) {
			new_capacity = props[i] / bits_per_element + 1;
		}
	}

	// Reallocate if necessary
	if (new_capacity > w->stale_props_capacity) {
		w->stale_props =
		    crealloc(w->stale_props, new_capacity * sizeof(*w->stale_props));

		// Clear the content of the newly allocated bytes
		memset(w->stale_props + w->stale_props_capacity, 0,
		       (new_capacity - w->stale_props_capacity) * sizeof(*w->stale_props));
		w->stale_props_capacity = new_capacity;
	}

	// Set the property bits
	for (int i = 0; i < nprops; i++) {
		w->stale_props[props[i] / bits_per_element] |=
		    1UL << (props[i] % bits_per_element);
	}
	win_set_flags(w, WIN_FLAGS_PROPERTY_STALE);
}

static void win_clear_all_properties_stale(struct win *w) {
	memset(w->stale_props, 0, w->stale_props_capacity * sizeof(*w->stale_props));
	win_clear_flags(w, WIN_FLAGS_PROPERTY_STALE);
}

static bool win_fetch_and_unset_property_stale(struct win *w, xcb_atom_t prop) {
	auto const bits_per_element = sizeof(*w->stale_props) * 8;
	if (prop >= w->stale_props_capacity * bits_per_element) {
		return false;
	}

	auto const mask = 1UL << (prop % bits_per_element);
	bool ret = w->stale_props[prop / bits_per_element] & mask;
	w->stale_props[prop / bits_per_element] &= ~mask;
	return ret;
}

bool win_check_flags_any(struct win *w, uint64_t flags) {
	return (w->flags & flags) != 0;
}

bool win_check_flags_all(struct win *w, uint64_t flags) {
	return (w->flags & flags) == flags;
}

/**
 * Check if a window is a fullscreen window.
 *
 * It's not using w->border_size for performance measures.
 */
void win_update_is_fullscreen(const session_t *ps, struct win *w) {
	if (!ps->o.no_ewmh_fullscreen && w->is_ewmh_fullscreen) {
		w->is_fullscreen = true;
		return;
	}
	w->is_fullscreen = w->g.x <= 0 && w->g.y <= 0 &&
	                   (w->g.x + w->widthb) >= ps->root_width &&
	                   (w->g.y + w->heightb) >= ps->root_height &&
	                   (!w->bounding_shaped || w->rounded_corners);
}

/**
 * Check if a window has BYPASS_COMPOSITOR property set
 *
 * TODO(yshui) cache this property
 */
bool win_is_bypassing_compositor(const session_t *ps, const struct win *w) {
	bool ret = false;
	auto wid = win_client_id(w, /*fallback_to_self=*/true);

	auto prop = x_get_prop(&ps->c, wid, ps->atoms->a_NET_WM_BYPASS_COMPOSITOR, 1L,
	                       XCB_ATOM_CARDINAL, 32);

	if (prop.nitems && *prop.c32 == 1) {
		ret = true;
	}

	free_winprop(&prop);
	return ret;
}
