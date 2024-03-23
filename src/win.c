// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>

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

#include "atom.h"
#include "backend/backend.h"
#include "c2.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "dbus.h"
#include "list.h"
#include "log.h"
#include "picom.h"
#include "region.h"
#include "render.h"
#include "string_utils.h"
#include "transition.h"
#include "types.h"
#include "uthash_extra.h"
#include "utils.h"
#include "win_defs.h"
#include "x.h"

#ifdef CONFIG_OPENGL
// TODO(yshui) Get rid of this include
#include "opengl.h"
#endif

#include "win.h"

// TODO(yshui) Make more window states internal
struct managed_win_internal {
	struct managed_win base;
};

#define OPAQUE (0xffffffff)
static const int WIN_GET_LEADER_MAX_RECURSION = 20;
static const int ROUNDED_PIXELS = 1;
static const double ROUNDED_PERCENT = 0.05;

/**
 * Reread opacity property of a window.
 */
static void win_update_opacity_prop(struct x_connection *c, struct atom *atoms,
                                    struct managed_win *w, bool detect_client_opacity);
static void win_update_opacity_target(session_t *ps, struct managed_win *w);
static void win_update_prop_shadow_raw(struct x_connection *c, struct atom *atoms,
                                       struct managed_win *w);
static bool
win_update_prop_shadow(struct x_connection *c, struct atom *atoms, struct managed_win *w);
/**
 * Update window EWMH fullscreen state.
 */
bool win_update_prop_fullscreen(struct x_connection *c, const struct atom *atoms,
                                struct managed_win *w);
/**
 * Update leader of a window.
 */
static xcb_window_t
win_get_leader_property(struct x_connection *c, struct atom *atoms, xcb_window_t wid,
                        bool detect_transient, bool detect_client_leader);
static void win_mark_client(session_t *ps, struct managed_win *w, xcb_window_t client);

/// Generate a "no corners" region function, from a function that returns the
/// region via a region_t pointer argument. Corners of the window will be removed from
/// the returned region.
/// Function signature has to be (win *, region_t *)
#define gen_without_corners(fun)                                                         \
	void fun##_without_corners(const struct managed_win *w, region_t *res) {         \
		fun(w, res);                                                             \
		win_region_remove_corners(w, res);                                       \
	}

/// Generate a "return by value" function, from a function that returns the
/// region via a region_t pointer argument.
/// Function signature has to be (win *)
#define gen_by_val(fun)                                                                  \
	region_t fun##_by_val(const struct managed_win *w) {                             \
		region_t ret;                                                            \
		pixman_region32_init(&ret);                                              \
		fun(w, &ret);                                                            \
		return ret;                                                              \
	}

/**
 * Clear leader cache of all windows.
 */
static inline void clear_cache_win_leaders(session_t *ps) {
	win_stack_foreach_managed(w, &ps->window_stack) {
		w->cache_leader = XCB_NONE;
	}
}

static xcb_window_t win_get_leader_raw(session_t *ps, struct managed_win *w, int recursions);

/**
 * Get the leader of a window.
 *
 * This function updates w->cache_leader if necessary.
 */
static inline xcb_window_t win_get_leader(session_t *ps, struct managed_win *w) {
	return win_get_leader_raw(ps, w, 0);
}

/**
 * Update focused state of a window.
 */
static void win_update_focused(session_t *ps, struct managed_win *w) {
	if (w->focused_force != UNSET) {
		w->focused = w->focused_force;
	} else {
		bool is_wmwin = win_is_wmwin(w);
		w->focused = win_is_focused_raw(w);

		// Use wintype_focus, and treat WM windows and override-redirected
		// windows specially
		if (ps->o.wintype_option[w->window_type].focus ||
		    (ps->o.mark_wmwin_focused && is_wmwin) ||
		    (ps->o.mark_ovredir_focused && w->base.id == w->client_win && !is_wmwin) ||
		    (w->a.map_state == XCB_MAP_STATE_VIEWABLE &&
		     c2_match(ps->c2_state, w, ps->o.focus_blacklist, NULL))) {
			w->focused = true;
		}

		// If window grouping detection is enabled, mark the window active if
		// its group is
		if (ps->o.track_leader && ps->active_leader &&
		    win_get_leader(ps, w) == ps->active_leader) {
			w->focused = true;
		}
	}
}

/**
 * Run win_on_factor_change() on all windows with the same leader window.
 *
 * @param leader leader window ID
 */
static inline void group_on_factor_change(session_t *ps, xcb_window_t leader) {
	if (!leader) {
		return;
	}

	HASH_ITER2(ps->windows, w) {
		assert(!w->destroyed);
		if (!w->managed) {
			continue;
		}
		auto mw = (struct managed_win *)w;
		if (win_get_leader(ps, mw) == leader) {
			win_on_factor_change(ps, mw);
		}
	}
}

static inline const char *win_get_name_if_managed(const struct win *w) {
	if (!w->managed) {
		return "(unmanaged)";
	}
	auto mw = (struct managed_win *)w;
	return mw->name;
}

/**
 * Return whether a window group is really focused.
 *
 * @param leader leader window ID
 * @return true if the window group is focused, false otherwise
 */
static inline bool group_is_focused(session_t *ps, xcb_window_t leader) {
	if (!leader) {
		return false;
	}

	HASH_ITER2(ps->windows, w) {
		assert(!w->destroyed);
		if (!w->managed) {
			continue;
		}
		auto mw = (struct managed_win *)w;
		if (win_get_leader(ps, mw) == leader && win_is_focused_raw(mw)) {
			return true;
		}
	}

	return false;
}

/**
 * Set leader of a window.
 */
static inline void win_set_leader(session_t *ps, struct managed_win *w, xcb_window_t nleader) {
	xcb_window_t cache_leader_old = win_get_leader(ps, w);

	w->leader = nleader;

	// Forcefully do this to deal with the case when a child window
	// gets mapped before parent, or when the window is a waypoint
	clear_cache_win_leaders(ps);

	// Update the old and new window group and active_leader if the
	// window could affect their state.
	xcb_window_t cache_leader = win_get_leader(ps, w);
	if (win_is_focused_raw(w) && cache_leader_old != cache_leader) {
		ps->active_leader = cache_leader;

		group_on_factor_change(ps, cache_leader_old);
		group_on_factor_change(ps, cache_leader);
	}
}

/**
 * Get a rectangular region a window occupies, excluding shadow.
 */
static void win_get_region_local(const struct managed_win *w, region_t *res) {
	assert(w->widthb >= 0 && w->heightb >= 0);
	pixman_region32_fini(res);
	pixman_region32_init_rect(res, 0, 0, (uint)w->widthb, (uint)w->heightb);
}

/**
 * Get a rectangular region a window occupies, excluding frame and shadow.
 */
void win_get_region_noframe_local(const struct managed_win *w, region_t *res) {
	const margin_t extents = win_calc_frame_extents(w);

	int x = extents.left;
	int y = extents.top;
	int width = max2(w->widthb - (extents.left + extents.right), 0);
	int height = max2(w->heightb - (extents.top + extents.bottom), 0);

	pixman_region32_fini(res);
	if (width > 0 && height > 0) {
		pixman_region32_init_rect(res, x, y, (uint)width, (uint)height);
	} else {
		pixman_region32_init(res);
	}
}

gen_without_corners(win_get_region_noframe_local);

void win_get_region_frame_local(const struct managed_win *w, region_t *res) {
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

/**
 * Add a window to damaged area.
 *
 * @param ps current session
 * @param w struct _win element representing the window
 */
void add_damage_from_win(session_t *ps, const struct managed_win *w) {
	// XXX there was a cached extents region, investigate
	//     if that's better

	// TODO(yshui) use the bounding shape when the window is shaped, otherwise the
	//             damage would be excessive
	region_t extents;
	pixman_region32_init(&extents);
	win_extents(w, &extents);
	add_damage(ps, &extents);
	pixman_region32_fini(&extents);
}

/// Release the images attached to this window
static inline void win_release_pixmap(backend_t *base, struct managed_win *w) {
	log_debug("Releasing pixmap of window %#010x (%s)", w->base.id, w->name);
	assert(w->win_image);
	if (w->win_image) {
		base->ops->release_image(base, w->win_image);
		w->win_image = NULL;
		// Bypassing win_set_flags, because `w` might have been destroyed
		w->flags |= WIN_FLAGS_PIXMAP_NONE;
	}
}
static inline void win_release_shadow(backend_t *base, struct managed_win *w) {
	log_debug("Releasing shadow of window %#010x (%s)", w->base.id, w->name);
	if (w->shadow_image) {
		assert(w->shadow);
		base->ops->release_image(base, w->shadow_image);
		w->shadow_image = NULL;
	}
}

static inline void win_release_mask(backend_t *base, struct managed_win *w) {
	if (w->mask_image) {
		base->ops->release_image(base, w->mask_image);
		w->mask_image = NULL;
	}
}

static inline bool win_bind_pixmap(struct backend_base *b, struct managed_win *w) {
	assert(!w->win_image);
	auto pixmap = x_new_id(b->c);
	auto e = xcb_request_check(
	    b->c->c, xcb_composite_name_window_pixmap_checked(b->c->c, w->base.id, pixmap));
	if (e) {
		log_error("Failed to get named pixmap for window %#010x(%s)", w->base.id,
		          w->name);
		free(e);
		return false;
	}
	log_debug("New named pixmap for %#010x (%s) : %#010x", w->base.id, w->name, pixmap);
	w->win_image =
	    b->ops->bind_pixmap(b, pixmap, x_get_visual_info(b->c, w->a.visual), true);
	if (!w->win_image) {
		log_error("Failed to bind pixmap");
		win_set_flags(w, WIN_FLAGS_IMAGE_ERROR);
		return false;
	}

	win_clear_flags(w, WIN_FLAGS_PIXMAP_NONE);
	return true;
}

bool win_bind_mask(struct backend_base *b, struct managed_win *w) {
	assert(!w->mask_image);
	auto reg_bound_local = win_get_bounding_shape_global_by_val(w);
	pixman_region32_translate(&reg_bound_local, -w->g.x, -w->g.y);
	w->mask_image = b->ops->make_mask(
	    b, (geometry_t){.width = w->widthb, .height = w->heightb}, &reg_bound_local);
	pixman_region32_fini(&reg_bound_local);

	if (!w->mask_image) {
		return false;
	}
	b->ops->set_image_property(b, IMAGE_PROPERTY_CORNER_RADIUS, w->mask_image,
	                           (double[]){w->corner_radius});
	return true;
}

bool win_bind_shadow(struct backend_base *b, struct managed_win *w, struct color c,
                     struct backend_shadow_context *sctx) {
	assert(!w->shadow_image);
	assert(w->shadow);
	if ((w->corner_radius == 0 && w->bounding_shaped == false) ||
	    b->ops->shadow_from_mask == NULL) {
		w->shadow_image = b->ops->render_shadow(b, w->widthb, w->heightb, sctx, c);
	} else {
		if (!w->mask_image) {
			// It's possible we already allocated a mask because of background
			// blur
			win_bind_mask(b, w);
		}
		w->shadow_image = b->ops->shadow_from_mask(b, w->mask_image, sctx, c);
	}
	if (!w->shadow_image) {
		log_error("Failed to bind shadow image, shadow will be disabled "
		          "for "
		          "%#010x (%s)",
		          w->base.id, w->name);
		w->shadow = false;
		return false;
	}

	log_debug("New shadow for %#010x (%s)", w->base.id, w->name);
	return true;
}

void win_release_images(struct backend_base *backend, struct managed_win *w) {
	// We don't want to decide what we should do if the image we want to
	// release is stale (do we clear the stale flags or not?) But if we are
	// not releasing any images anyway, we don't care about the stale flags.

	if (!win_check_flags_all(w, WIN_FLAGS_PIXMAP_NONE)) {
		assert(!win_check_flags_all(w, WIN_FLAGS_PIXMAP_STALE));
		win_release_pixmap(backend, w);
	}

	win_release_shadow(backend, w);
	win_release_mask(backend, w);
}

/// Returns true if the `prop` property is stale, as well as clears the stale
/// flag.
static bool win_fetch_and_unset_property_stale(struct managed_win *w, xcb_atom_t prop);
/// Returns true if any of the properties are stale, as well as clear all the
/// stale flags.
static void win_clear_all_properties_stale(struct managed_win *w);

// TODO(yshui) make WIN_FLAGS_FACTOR_CHANGED more fine-grained, or find a better
// alternative
//             way to do all this.

/// Fetch new window properties from the X server, and run appropriate updates.
/// Might set WIN_FLAGS_FACTOR_CHANGED
static void win_update_properties(session_t *ps, struct managed_win *w) {
	// we cannot receive property change when window has been destroyed
	assert(w->state != WSTATE_DESTROYED);

	if (win_fetch_and_unset_property_stale(w, ps->atoms->a_NET_WM_WINDOW_TYPE)) {
		if (win_update_wintype(&ps->c, ps->atoms, w)) {
			win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		}
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->a_NET_WM_WINDOW_OPACITY)) {
		win_update_opacity_prop(&ps->c, ps->atoms, w, ps->o.detect_client_opacity);
		win_update_opacity_target(ps, w);
	}

	if (win_fetch_and_unset_property_stale(w, ps->atoms->a_NET_FRAME_EXTENTS)) {
		win_update_frame_extents(&ps->c, ps->atoms, w, w->client_win,
		                         ps->o.frame_opacity);
		add_damage_from_win(ps, w);
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

	if (win_fetch_and_unset_property_stale(w, ps->atoms->aWM_CLIENT_LEADER) ||
	    win_fetch_and_unset_property_stale(w, ps->atoms->aWM_TRANSIENT_FOR)) {
		auto new_leader = win_get_leader_property(&ps->c, ps->atoms, w->client_win,
		                                          ps->o.detect_transient,
		                                          ps->o.detect_client_leader);
		if (w->leader != new_leader) {
			win_set_leader(ps, w, new_leader);
			win_set_flags(w, WIN_FLAGS_FACTOR_CHANGED);
		}
	}

	win_clear_all_properties_stale(w);
}

/// Handle non-image flags. This phase might set IMAGES_STALE flags
void win_process_update_flags(session_t *ps, struct managed_win *w) {
	log_trace("Processing flags for window %#010x (%s), was rendered: %d, flags: "
	          "%#" PRIx64,
	          w->base.id, w->name, w->to_paint, w->flags);

	if (win_check_flags_all(w, WIN_FLAGS_MAPPED)) {
		map_win_start(ps, w);
		win_clear_flags(w, WIN_FLAGS_MAPPED);
	}

	if (w->state != WSTATE_MAPPED) {
		// Window is not mapped, so we ignore all its changes until it's mapped
		// again.
		return;
	}

	// Check client first, because later property updates need accurate client
	// window information
	if (win_check_flags_all(w, WIN_FLAGS_CLIENT_STALE)) {
		auto client_win = win_get_client_window(&ps->c, ps->atoms, w);
		if (w->client_win && w->client_win != client_win) {
			win_unmark_client(ps, w);
		}
		win_mark_client(ps, w, client_win);
		win_clear_flags(w, WIN_FLAGS_CLIENT_STALE);
	}

	bool damaged = false;
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
		if (w->to_paint) {
			// Mark the old extents of this window as damaged. The new
			// extents will be marked damaged below, after the window
			// extents are updated.
			add_damage_from_win(ps, w);
		}

		// Update window geometry
		w->g = w->pending_g;

		// Whether a window is fullscreen changes based on its geometry
		win_update_is_fullscreen(ps, w);

		if (win_check_flags_all(w, WIN_FLAGS_SIZE_STALE)) {
			win_on_win_size_change(w, ps->o.shadow_offset_x,
			                       ps->o.shadow_offset_y, ps->o.shadow_radius);
			win_update_bounding_shape(&ps->c, w, ps->shape_exists,
			                          ps->o.detect_rounded_corners);
			damaged = true;
			win_clear_flags(w, WIN_FLAGS_SIZE_STALE);

			// Window shape/size changed, invalidate the images we built
			// log_trace("free out dated pict");
			win_set_flags(w, WIN_FLAGS_PIXMAP_STALE | WIN_FLAGS_FACTOR_CHANGED);

			win_release_mask(ps->backend_data, w);
			win_release_shadow(ps->backend_data, w);
			ps->pending_updates = true;
			free_paint(ps, &w->paint);
			free_paint(ps, &w->shadow_paint);
		}

		if (win_check_flags_all(w, WIN_FLAGS_POSITION_STALE)) {
			damaged = true;
			win_clear_flags(w, WIN_FLAGS_POSITION_STALE);
		}

		win_update_monitor(&ps->monitors, w);
	}

	if (win_check_flags_all(w, WIN_FLAGS_PROPERTY_STALE)) {
		win_update_properties(ps, w);
		win_clear_flags(w, WIN_FLAGS_PROPERTY_STALE);
	}

	// Factor change flags could be set by previous stages, so must be handled
	// last
	if (win_check_flags_all(w, WIN_FLAGS_FACTOR_CHANGED)) {
		win_on_factor_change(ps, w);
		win_clear_flags(w, WIN_FLAGS_FACTOR_CHANGED);
	}

	// Add damage, has to be done last so the window has the latest geometry
	// information.
	if (damaged) {
		add_damage_from_win(ps, w);
	}
}

void win_process_image_flags(session_t *ps, struct managed_win *w) {
	// Assert that the MAPPED flag is already handled.
	assert(!win_check_flags_all(w, WIN_FLAGS_MAPPED));

	if (w->state != WSTATE_MAPPED) {
		// Flags of invisible windows are processed when they are mapped
		return;
	}

	// Not a loop
	while (win_check_flags_any(w, WIN_FLAGS_PIXMAP_STALE) &&
	       !win_check_flags_all(w, WIN_FLAGS_IMAGE_ERROR)) {
		// Image needs to be updated, update it.
		if (!ps->backend_data) {
			// We are using legacy backend, nothing to do here.
			break;
		}

		if (win_check_flags_all(w, WIN_FLAGS_PIXMAP_STALE)) {
			// Check to make sure the window is still mapped,
			// otherwise we won't be able to rebind pixmap after
			// releasing it, yet we might still need the pixmap for
			// rendering.
			if (!win_check_flags_all(w, WIN_FLAGS_PIXMAP_NONE)) {
				// Must release images first, otherwise breaks
				// NVIDIA driver
				win_release_pixmap(ps->backend_data, w);
			}
			win_bind_pixmap(ps->backend_data, w);
		}

		// break here, loop always run only once
		break;
	}

	// Clear stale image flags
	if (win_check_flags_any(w, WIN_FLAGS_PIXMAP_STALE)) {
		win_clear_flags(w, WIN_FLAGS_PIXMAP_STALE);
	}
}

/**
 * Check if a window has rounded corners.
 * XXX This is really dumb
 */
static bool attr_pure win_has_rounded_corners(const struct managed_win *w) {
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

int win_update_name(struct x_connection *c, struct atom *atoms, struct managed_win *w) {
	char **strlst = NULL;
	int nstr = 0;

	if (!w->client_win) {
		return 0;
	}

	if (!(wid_get_text_prop(c, atoms, w->client_win, atoms->a_NET_WM_NAME, &strlst, &nstr))) {
		log_debug("(%#010x): _NET_WM_NAME unset, falling back to "
		          "WM_NAME.",
		          w->client_win);

		if (!wid_get_text_prop(c, atoms, w->client_win, atoms->aWM_NAME, &strlst,
		                       &nstr)) {
			log_debug("Unsetting window name for %#010x", w->client_win);
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

	log_debug("(%#010x): client = %#010x, name = \"%s\", "
	          "ret = %d",
	          w->base.id, w->client_win, w->name, ret);
	return ret;
}

int win_update_role(struct x_connection *c, struct atom *atoms, struct managed_win *w) {
	char **strlst = NULL;
	int nstr = 0;

	if (!wid_get_text_prop(c, atoms, w->client_win, atoms->aWM_WINDOW_ROLE, &strlst, &nstr)) {
		return -1;
	}

	int ret = 0;
	if (!w->role || strcmp(w->role, strlst[0]) != 0) {
		ret = 1;
		free(w->role);
		w->role = strdup(strlst[0]);
	}

	free(strlst);

	log_trace("(%#010x): client = %#010x, role = \"%s\", "
	          "ret = %d",
	          w->base.id, w->client_win, w->role, ret);
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

static wintype_t
wid_get_prop_wintype(struct x_connection *c, struct atom *atoms, xcb_window_t wid) {
	winprop_t prop =
	    x_get_prop(c, wid, atoms->a_NET_WM_WINDOW_TYPE, 32L, XCB_ATOM_ATOM, 32);

	for (unsigned i = 0; i < prop.nitems; ++i) {
		for (wintype_t j = 1; j < NUM_WINTYPES; ++j) {
			if (get_atom_with_nul(atoms, WINTYPES[j].atom, c->c) ==
			    (xcb_atom_t)prop.p32[i]) {
				free_winprop(&prop);
				return j;
			}
		}
	}

	free_winprop(&prop);

	return WINTYPE_UNKNOWN;
}

static bool wid_get_opacity_prop(struct x_connection *c, struct atom *atoms,
                                 xcb_window_t wid, opacity_t def, opacity_t *out) {
	bool ret = false;
	*out = def;

	winprop_t prop =
	    x_get_prop(c, wid, atoms->a_NET_WM_WINDOW_OPACITY, 1L, XCB_ATOM_CARDINAL, 32);

	if (prop.nitems) {
		*out = *prop.c32;
		ret = true;
	}

	free_winprop(&prop);

	return ret;
}

// XXX should distinguish between frame has alpha and window body has alpha
bool win_has_alpha(const struct managed_win *w) {
	return w->pictfmt && w->pictfmt->type == XCB_RENDER_PICT_TYPE_DIRECT &&
	       w->pictfmt->direct.alpha_mask;
}

bool win_client_has_alpha(const struct managed_win *w) {
	return w->client_pictfmt && w->client_pictfmt->type == XCB_RENDER_PICT_TYPE_DIRECT &&
	       w->client_pictfmt->direct.alpha_mask;
}

winmode_t win_calc_mode(const struct managed_win *w) {
	if (animatable_get(&w->opacity) < 1.0) {
		return WMODE_TRANS;
	}

	if (win_has_alpha(w)) {
		if (w->client_win == XCB_NONE) {
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
static double win_calc_opacity_target(session_t *ps, const struct managed_win *w) {
	double opacity = 1;

	if (w->state == WSTATE_UNMAPPED || w->state == WSTATE_DESTROYED) {
		// be consistent
		return 0;
	}
	// Try obeying opacity property and window type opacity firstly
	if (w->has_opacity_prop) {
		opacity = ((double)w->opacity_prop) / OPAQUE;
	} else if (w->opacity_is_set) {
		opacity = w->opacity_set;
	} else if (!safe_isnan(ps->o.wintype_option[w->window_type].opacity)) {
		opacity = ps->o.wintype_option[w->window_type].opacity;
	} else {
		// Respect active_opacity only when the window is physically
		// focused
		if (win_is_focused_raw(w)) {
			opacity = ps->o.active_opacity;
		} else if (!w->focused) {
			// Respect inactive_opacity in some cases
			opacity = ps->o.inactive_opacity;
		}
	}

	// respect inactive override
	if (ps->o.inactive_opacity_override && !w->focused) {
		opacity = ps->o.inactive_opacity;
	}

	return opacity;
}

/// Finish the unmapping of a window (e.g. after fading has finished).
/// Doesn't free `w`
static void unmap_win_finish(session_t *ps, struct managed_win *w) {
	w->reg_ignore_valid = false;
	w->state = WSTATE_UNMAPPED;

	// We are in unmap_win, this window definitely was viewable
	if (ps->backend_data) {
		// Only the pixmap needs to be freed and reacquired when mapping.
		// Shadow image can be preserved.
		if (!win_check_flags_all(w, WIN_FLAGS_PIXMAP_NONE)) {
			win_release_pixmap(ps->backend_data, w);
		}
	} else {
		assert(!w->win_image);
		assert(!w->shadow_image);
	}

	free_paint(ps, &w->paint);
	free_paint(ps, &w->shadow_paint);

	// Try again at binding images when the window is mapped next time
	win_clear_flags(w, WIN_FLAGS_IMAGE_ERROR);
}

struct window_transition_data {
	struct managed_win *w;
	session_t *ps;
	// TODO(yshui) switch to only pass backend_data after the legacy backend removal
	// struct backend_base *backend_data;
	uint64_t refcount;
};

static void win_transition_callback(enum transition_event event attr_unused, void *data_) {
	auto data = (struct window_transition_data *)data_;
	auto w = data->w;
	w->number_of_animations--;
	if (w->number_of_animations == 0) {
		if (w->state == WSTATE_DESTROYED || w->state == WSTATE_UNMAPPED) {
			if (animatable_get(&w->opacity) != 0) {
				log_warn("Window %#010x (%s) has finished fading out but "
				         "its opacity is not 0",
				         w->base.id, w->name);
			}
		}
		if (w->state == WSTATE_UNMAPPED) {
			unmap_win_finish(data->ps, data->w);
		}
		// Destroyed windows are freed in paint_preprocess, this makes managing
		// the lifetime of windows easier.
		w->in_openclose = false;
	}
	data->refcount--;
	if (data->refcount == 0) {
		free(data);
	}
}

/**
 * Determine if a window should fade on opacity change.
 */
bool win_should_fade(session_t *ps, const struct managed_win *w) {
	// To prevent it from being overwritten by last-paint value if the window
	// is
	if (w->fade_force != UNSET) {
		return w->fade_force;
	}
	if (ps->o.no_fading_openclose && w->in_openclose) {
		return false;
	}
	if (ps->o.no_fading_destroyed_argb && w->state == WSTATE_DESTROYED &&
	    win_has_alpha(w) && w->client_win && w->client_win != w->base.id) {
		// deprecated
		return false;
	}
	if (w->fade_excluded) {
		return false;
	}
	return ps->o.wintype_option[w->window_type].fade;
}

/// Call `animatable_set_target` on the opacity of a window, with appropriate
/// target opacity and duration.
static inline void
win_start_fade(session_t *ps, struct managed_win *w, double target_blur_opacity) {
	double current_opacity = animatable_get(&w->opacity),
	       target_opacity = win_calc_opacity_target(ps, w);
	double step_size =
	    target_opacity > current_opacity ? ps->o.fade_in_step : ps->o.fade_out_step;
	unsigned int duration =
	    (unsigned int)(fabs(target_opacity - current_opacity) / step_size);
	if (!win_should_fade(ps, w)) {
		duration = 0;
	}
	auto data = ccalloc(1, struct window_transition_data);
	data->ps = ps;
	data->w = w;
	data->refcount = 1;

	animatable_cancel(&w->blur_opacity);        // Cancel any ongoing blur animation
	                                            // so we can check its current value

	// We want to set the correct `number_of_animations` before calling
	// `animatable_set_target`, because it might trigger our callback which will
	// decrement `number_of_animations`
	w->number_of_animations++;
	if (target_blur_opacity != w->blur_opacity.start) {
		w->number_of_animations++;
		data->refcount++;
	}

	animatable_set_target(&w->opacity, target_opacity, duration,
	                      win_transition_callback, data);
	if (target_blur_opacity != w->blur_opacity.start) {
		animatable_set_target(&w->blur_opacity, target_blur_opacity, duration,
		                      win_transition_callback, data);
	}
}

/**
 * Determine whether a window is to be dimmed.
 */
bool win_should_dim(session_t *ps, const struct managed_win *w) {
	// Make sure we do nothing if the window is unmapped / being destroyed
	if (w->state == WSTATE_UNMAPPED) {
		return false;
	}

	if (ps->o.inactive_dim > 0 && !(w->focused)) {
		return true;
	}
	return false;
}

/**
 * Reread _COMPTON_SHADOW property from a window.
 *
 * The property must be set on the outermost window, usually the WM frame.
 */
void win_update_prop_shadow_raw(struct x_connection *c, struct atom *atoms,
                                struct managed_win *w) {
	winprop_t prop =
	    x_get_prop(c, w->base.id, atoms->a_COMPTON_SHADOW, 1, XCB_ATOM_CARDINAL, 32);

	if (!prop.nitems) {
		w->prop_shadow = -1;
	} else {
		w->prop_shadow = *prop.c32;
	}

	free_winprop(&prop);
}

static void win_set_shadow(session_t *ps, struct managed_win *w, bool shadow_new) {
	if (w->shadow == shadow_new) {
		return;
	}

	log_debug("Updating shadow property of window %#010x (%s) to %d", w->base.id,
	          w->name, shadow_new);

	// We don't handle property updates of non-visible windows until they are
	// mapped.
	assert(w->state == WSTATE_MAPPED);

	// Keep a copy of window extent before the shadow change. Will be used for
	// calculation of damaged region
	region_t extents;
	pixman_region32_init(&extents);
	win_extents(w, &extents);

	if (ps->redirected) {
		// Add damage for shadow change

		// Window extents need update on shadow state change
		// Shadow geometry currently doesn't change on shadow state change
		// calc_shadow_geometry(ps, w);
		if (shadow_new) {
			// Mark the new extents as damaged if the shadow is added
			assert(!w->shadow_image);
			pixman_region32_clear(&extents);
			win_extents(w, &extents);
			add_damage_from_win(ps, w);
		} else {
			// Mark the old extents as damaged if the shadow is
			// removed
			add_damage(ps, &extents);
			win_release_shadow(ps->backend_data, w);
		}

		// Only set pending_updates if we are redirected. Otherwise change
		// of a shadow won't have influence on whether we should redirect.
		ps->pending_updates = true;
	}

	w->shadow = shadow_new;

	pixman_region32_fini(&extents);
}

/**
 * Determine if a window should have shadow, and update things depending
 * on shadow state.
 */
static void win_determine_shadow(session_t *ps, struct managed_win *w) {
	log_debug("Determining shadow of window %#010x (%s)", w->base.id, w->name);
	bool shadow_new = w->shadow;

	if (w->shadow_force != UNSET) {
		shadow_new = w->shadow_force;
	} else if (w->a.map_state == XCB_MAP_STATE_VIEWABLE) {
		shadow_new = true;
		if (!ps->o.wintype_option[w->window_type].shadow) {
			log_debug("Shadow disabled by wintypes");
			shadow_new = false;
		} else if (c2_match(ps->c2_state, w, ps->o.shadow_blacklist, NULL)) {
			log_debug("Shadow disabled by shadow-exclude");
			shadow_new = false;
		} else if (ps->o.shadow_ignore_shaped && w->bounding_shaped &&
		           !w->rounded_corners) {
			log_debug("Shadow disabled by shadow-ignore-shaped");
			shadow_new = false;
		} else if (w->prop_shadow == 0) {
			log_debug("Shadow disabled by shadow property");
			shadow_new = false;
		}
	}

	win_set_shadow(ps, w, shadow_new);
}

/**
 * Reread _COMPTON_SHADOW property from a window and update related
 * things.
 */
static bool
win_update_prop_shadow(struct x_connection *c, struct atom *atoms, struct managed_win *w) {
	long long attr_shadow_old = w->prop_shadow;
	win_update_prop_shadow_raw(c, atoms, w);
	return w->prop_shadow != attr_shadow_old;
}

/**
 * Update window EWMH fullscreen state.
 */
bool win_update_prop_fullscreen(struct x_connection *c, const struct atom *atoms,
                                struct managed_win *w) {
	auto prop = x_get_prop(c, w->client_win, atoms->a_NET_WM_STATE, 12, XCB_ATOM_ATOM, 0);
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

static void win_determine_clip_shadow_above(session_t *ps, struct managed_win *w) {
	bool should_crop = (ps->o.wintype_option[w->window_type].clip_shadow_above ||
	                    c2_match(ps->c2_state, w, ps->o.shadow_clip_list, NULL));
	w->clip_shadow_above = should_crop;
}

static void win_set_invert_color(session_t *ps, struct managed_win *w, bool invert_color_new) {
	if (w->invert_color == invert_color_new) {
		return;
	}

	w->invert_color = invert_color_new;

	add_damage_from_win(ps, w);
}

/**
 * Determine if a window should have color inverted.
 */
static void win_determine_invert_color(session_t *ps, struct managed_win *w) {
	bool invert_color_new = w->invert_color;

	if (UNSET != w->invert_color_force) {
		invert_color_new = w->invert_color_force;
	} else if (w->a.map_state == XCB_MAP_STATE_VIEWABLE) {
		invert_color_new = c2_match(ps->c2_state, w, ps->o.invert_color_list, NULL);
	}

	win_set_invert_color(ps, w, invert_color_new);
}

/**
 * Set w->invert_color_force of a window.
 */
void win_set_invert_color_force(session_t *ps, struct managed_win *w, switch_t val) {
	if (val != w->invert_color_force) {
		w->invert_color_force = val;
		win_determine_invert_color(ps, w);
		queue_redraw(ps);
	}
}

/**
 * Set w->fade_force of a window.
 *
 * Doesn't affect fading already in progress
 */
void win_set_fade_force(struct managed_win *w, switch_t val) {
	w->fade_force = val;
}

/**
 * Set w->focused_force of a window.
 */
void win_set_focused_force(session_t *ps, struct managed_win *w, switch_t val) {
	if (val != w->focused_force) {
		w->focused_force = val;
		win_on_factor_change(ps, w);
		queue_redraw(ps);
	}
}

/**
 * Set w->shadow_force of a window.
 */
void win_set_shadow_force(session_t *ps, struct managed_win *w, switch_t val) {
	if (val != w->shadow_force) {
		w->shadow_force = val;
		win_determine_shadow(ps, w);
		queue_redraw(ps);
	}
}

static void
win_set_blur_background(session_t *ps, struct managed_win *w, bool blur_background_new) {
	if (w->blur_background == blur_background_new) {
		return;
	}

	w->blur_background = blur_background_new;

	// This damage might not be absolutely necessary (e.g. when the window is
	// opaque), but blur_background changes should be rare, so this should be
	// fine.
	add_damage_from_win(ps, w);
}

static void
win_set_fg_shader(session_t *ps, struct managed_win *w, struct shader_info *shader_new) {
	if (w->fg_shader == shader_new) {
		return;
	}

	w->fg_shader = shader_new;

	// A different shader might change how the window is drawn, these changes
	// should be rare however, so this should be fine.
	add_damage_from_win(ps, w);
}

/**
 * Determine if a window should have background blurred.
 */
static void win_determine_blur_background(session_t *ps, struct managed_win *w) {
	log_debug("Determining blur-background of window %#010x (%s)", w->base.id, w->name);
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	bool blur_background_new = ps->o.blur_method != BLUR_METHOD_NONE;
	if (blur_background_new) {
		if (!ps->o.wintype_option[w->window_type].blur_background) {
			log_debug("Blur background disabled by wintypes");
			blur_background_new = false;
		} else if (c2_match(ps->c2_state, w, ps->o.blur_background_blacklist, NULL)) {
			log_debug("Blur background disabled by "
			          "blur-background-exclude");
			blur_background_new = false;
		}
	}

	win_set_blur_background(ps, w, blur_background_new);
}

/**
 * Determine if a window should have rounded corners.
 */
static void win_determine_rounded_corners(session_t *ps, struct managed_win *w) {
	void *radius_override = NULL;
	if (c2_match(ps->c2_state, w, ps->o.corner_radius_rules, &radius_override)) {
		log_debug("Matched corner rule! %d", w->corner_radius);
	}

	if (ps->o.corner_radius == 0 && !radius_override) {
		w->corner_radius = 0;
		return;
	}

	// Don't round full screen windows & excluded windows,
	// unless we find a corner override in corner_radius_rules
	if (!radius_override &&
	    ((w && w->is_fullscreen) ||
	     c2_match(ps->c2_state, w, ps->o.rounded_corners_blacklist, NULL))) {
		w->corner_radius = 0;
		log_debug("Not rounding corners for window %#010x", w->base.id);
	} else {
		if (radius_override) {
			w->corner_radius = (int)(long)radius_override;
		} else {
			w->corner_radius = ps->o.corner_radius;
		}

		log_debug("Rounding corners for window %#010x", w->base.id);
		// Initialize the border color to an invalid value
		w->border_col[0] = w->border_col[1] = w->border_col[2] =
		    w->border_col[3] = -1.0F;
	}
}

/**
 * Determine custom window shader to use for a window.
 */
static void win_determine_fg_shader(session_t *ps, struct managed_win *w) {
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	auto shader_new = ps->o.window_shader_fg;
	void *val = NULL;
	if (c2_match(ps->c2_state, w, ps->o.window_shader_fg_rules, &val)) {
		shader_new = val;
	}

	struct shader_info *shader = NULL;
	if (shader_new) {
		HASH_FIND_STR(ps->shaders, shader_new, shader);
	}

	win_set_fg_shader(ps, w, shader);
}

/**
 * Update window opacity according to opacity rules.
 */
void win_update_opacity_rule(session_t *ps, struct managed_win *w) {
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	double opacity = 1.0;
	bool is_set = false;
	void *val = NULL;
	if (c2_match(ps->c2_state, w, ps->o.opacity_rules, &val)) {
		opacity = ((double)(long)val) / 100.0;
		is_set = true;
	}

	w->opacity_set = opacity;
	w->opacity_is_set = is_set;
}

/**
 * Function to be called on window data changes.
 *
 * TODO(yshui) need better name
 */
void win_on_factor_change(session_t *ps, struct managed_win *w) {
	log_debug("Window %#010x (%s) factor change", w->base.id, w->name);
	c2_window_state_update(ps->c2_state, &w->c2_state, ps->c.c, w->client_win, w->base.id);
	// Focus and is_fullscreen needs to be updated first, as other rules might depend
	// on the focused state of the window
	win_update_focused(ps, w);
	win_update_is_fullscreen(ps, w);

	win_determine_shadow(ps, w);
	win_determine_clip_shadow_above(ps, w);
	win_determine_invert_color(ps, w);
	win_determine_blur_background(ps, w);
	win_determine_rounded_corners(ps, w);
	win_determine_fg_shader(ps, w);
	w->mode = win_calc_mode(w);
	log_debug("Window mode changed to %d", w->mode);
	win_update_opacity_rule(ps, w);
	if (w->a.map_state == XCB_MAP_STATE_VIEWABLE) {
		w->paint_excluded = c2_match(ps->c2_state, w, ps->o.paint_blacklist, NULL);
	}
	if (w->a.map_state == XCB_MAP_STATE_VIEWABLE) {
		w->unredir_if_possible_excluded =
		    c2_match(ps->c2_state, w, ps->o.unredir_if_possible_blacklist, NULL);
	}

	w->fade_excluded = c2_match(ps->c2_state, w, ps->o.fade_blacklist, NULL);

	w->transparent_clipping_excluded =
	    c2_match(ps->c2_state, w, ps->o.transparent_clipping_blacklist, NULL);

	win_update_opacity_target(ps, w);

	w->reg_ignore_valid = false;
}

/**
 * Update cache data in struct _win that depends on window size.
 */
void win_on_win_size_change(struct managed_win *w, int shadow_offset_x,
                            int shadow_offset_y, int shadow_radius) {
	log_trace("Window %#010x (%s) size changed, was %dx%d, now %dx%d", w->base.id,
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
bool win_update_wintype(struct x_connection *c, struct atom *atoms, struct managed_win *w) {
	const wintype_t wtype_old = w->window_type;

	// Detect window type here
	w->window_type = wid_get_prop_wintype(c, atoms, w->client_win);

	// Conform to EWMH standard, if _NET_WM_WINDOW_TYPE is not present, take
	// override-redirect windows or windows without WM_TRANSIENT_FOR as
	// _NET_WM_WINDOW_TYPE_NORMAL, otherwise as _NET_WM_WINDOW_TYPE_DIALOG.
	if (WINTYPE_UNKNOWN == w->window_type) {
		if (w->a.override_redirect ||
		    !wid_has_prop(c->c, w->client_win, atoms->aWM_TRANSIENT_FOR)) {
			w->window_type = WINTYPE_NORMAL;
		} else {
			w->window_type = WINTYPE_DIALOG;
		}
	}

	return w->window_type != wtype_old;
}

/**
 * Mark a window as the client window of another.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 * @param client window ID of the client window
 */
static void win_mark_client(session_t *ps, struct managed_win *w, xcb_window_t client) {
	w->client_win = client;

	// If the window isn't mapped yet, stop here, as the function will be
	// called in map_win()
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	auto e = xcb_request_check(
	    ps->c.c, xcb_change_window_attributes_checked(
	                 ps->c.c, client, XCB_CW_EVENT_MASK,
	                 (const uint32_t[]){determine_evmask(ps, client, WIN_EVMODE_CLIENT)}));
	if (e) {
		log_error("Failed to change event mask of window %#010x", client);
		free(e);
	}

	win_update_wintype(&ps->c, ps->atoms, w);

	// Get frame widths. The window is in damaged area already.
	win_update_frame_extents(&ps->c, ps->atoms, w, client, ps->o.frame_opacity);

	// Get window group
	if (ps->o.track_leader) {
		auto new_leader = win_get_leader_property(&ps->c, ps->atoms, w->client_win,
		                                          ps->o.detect_transient,
		                                          ps->o.detect_client_leader);
		if (w->leader != new_leader) {
			win_set_leader(ps, w, new_leader);
		}
	}

	// Get window name and class if we are tracking them
	win_update_name(&ps->c, ps->atoms, w);
	win_update_class(&ps->c, ps->atoms, w);
	win_update_role(&ps->c, ps->atoms, w);

	// Update everything related to conditions
	win_on_factor_change(ps, w);

	auto r = xcb_get_window_attributes_reply(
	    ps->c.c, xcb_get_window_attributes(ps->c.c, w->client_win), &e);
	if (!r) {
		log_error_x_error(e, "Failed to get client window attributes");
		return;
	}

	w->client_pictfmt = x_get_pictform_for_visual(&ps->c, r->visual);
	free(r);
}

/**
 * Unmark current client window of a window.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 */
void win_unmark_client(session_t *ps, struct managed_win *w) {
	xcb_window_t client = w->client_win;
	log_debug("Detaching client window %#010x from frame %#010x (%s)", client,
	          w->base.id, w->name);

	w->client_win = XCB_NONE;

	// Recheck event mask
	xcb_change_window_attributes(
	    ps->c.c, client, XCB_CW_EVENT_MASK,
	    (const uint32_t[]){determine_evmask(ps, client, WIN_EVMODE_UNKNOWN)});
}

/**
 * Look for the client window of a particular window.
 */
static xcb_window_t
find_client_win(struct x_connection *c, struct atom *atoms, xcb_window_t w) {
	if (wid_has_prop(c->c, w, atoms->aWM_STATE)) {
		return w;
	}

	xcb_query_tree_reply_t *reply =
	    xcb_query_tree_reply(c->c, xcb_query_tree(c->c, w), NULL);
	if (!reply) {
		return 0;
	}

	xcb_window_t *children = xcb_query_tree_children(reply);
	int nchildren = xcb_query_tree_children_length(reply);
	int i;
	xcb_window_t ret = 0;

	for (i = 0; i < nchildren; ++i) {
		ret = find_client_win(c, atoms, children[i]);
		if (ret) {
			break;
		}
	}

	free(reply);
	return ret;
}

/**
 * Get client window of a window.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 */
xcb_window_t win_get_client_window(struct x_connection *c, struct atom *atoms,
                                   const struct managed_win *w) {
	// Always recursively look for a window with WM_STATE, as Fluxbox
	// sets override-redirect flags on all frame windows.
	xcb_window_t cw = find_client_win(c, atoms, w->base.id);
	if (cw) {
		log_debug("(%#010x): client %#010x", w->base.id, cw);
	} else {
		// Set a window's client window to itself if we couldn't find a
		// client window
		cw = w->base.id;
		log_debug("(%#010x): client self (%s)", w->base.id,
		          (w->a.override_redirect ? "override-redirected" : "wmwin"));
	}

	return cw;
}

/**
 * Free all resources in a <code>struct _win</code>.
 */
void free_win_res(session_t *ps, struct managed_win *w) {
	// No need to call backend release_image here because
	// finish_unmap_win should've done that for us.
	// XXX unless we are called by session_destroy
	// assert(w->win_data == NULL);
	free_win_res_glx(ps, w);
	free_paint(ps, &w->paint);
	free_paint(ps, &w->shadow_paint);
	// Above should be done during unmapping
	// Except when we are called by session_destroy

	pixman_region32_fini(&w->bounding_shape);
	// BadDamage may be thrown if the window is destroyed
	set_ignore_cookie(&ps->c, xcb_damage_destroy(ps->c.c, w->damage));
	rc_region_unref(&w->reg_ignore);
	free(w->name);
	free(w->class_instance);
	free(w->class_general);
	free(w->role);

	free(w->stale_props);
	w->stale_props = NULL;
	w->stale_props_capacity = 0;
	c2_window_state_destroy(ps->c2_state, &w->c2_state);
}

/// Insert a new window after list_node `prev`
/// New window will be in unmapped state
static struct win *add_win(session_t *ps, xcb_window_t id, struct list_node *prev) {
	log_debug("Adding window %#010x", id);
	struct win *old_w = NULL;
	HASH_FIND_INT(ps->windows, &id, old_w);
	assert(old_w == NULL);

	auto new_w = cmalloc(struct win);
	list_insert_after(prev, &new_w->stack_neighbour);
	new_w->id = id;
	new_w->managed = false;
	new_w->is_new = true;
	new_w->destroyed = false;

	HASH_ADD_INT(ps->windows, id, new_w);
	ps->pending_updates = true;
	return new_w;
}

/// Insert a new win entry at the top of the stack
struct win *add_win_top(session_t *ps, xcb_window_t id) {
	return add_win(ps, id, &ps->window_stack);
}

/// Insert a new window above window with id `below`, if there is no window, add
/// to top New window will be in unmapped state
struct win *add_win_above(session_t *ps, xcb_window_t id, xcb_window_t below) {
	struct win *w = NULL;
	HASH_FIND_INT(ps->windows, &below, w);
	if (!w) {
		if (!list_is_empty(&ps->window_stack)) {
			// `below` window is not found even if the window stack is
			// not empty
			return NULL;
		}
		return add_win_top(ps, id);
	}
	// we found something from the hash table, so if the stack is
	// empty, we are in an inconsistent state.
	assert(!list_is_empty(&ps->window_stack));
	return add_win(ps, id, w->stack_neighbour.prev);
}

/// Query the Xorg for information about window `win`, and return that
/// information in a new managed_win object. However, if the window does
/// not need to be managed, the original `win` object is returned.
struct win *attr_ret_nonnull maybe_allocate_managed_win(session_t *ps, struct win *w) {
	static const struct managed_win win_def = {
	    // No need to initialize. (or, you can think that
	    // they are initialized right here).
	    // The following ones are updated during paint or paint preprocess
	    .shadow_opacity = 0.0,
	    .to_paint = false,
	    .frame_opacity = 1.0,
	    .dim = false,
	    .invert_color = false,
	    .blur_background = false,
	    .reg_ignore = NULL,
	    // The following ones are updated for other reasons
	    .pixmap_damaged = false,          // updated by damage events
	    .state = WSTATE_UNMAPPED,         // updated by window state changes
	    .in_openclose = true,             // set to false after first map is done,
	                                      // true here because window is just created
	    .reg_ignore_valid = false,        // set to true when damaged
	    .flags = WIN_FLAGS_PIXMAP_NONE,        // updated by
	                                           // property/attributes/etc
	                                           // change
	    .stale_props = NULL,
	    .stale_props_capacity = 0,

	    // Runtime variables, updated by dbus
	    .fade_force = UNSET,
	    .shadow_force = UNSET,
	    .focused_force = UNSET,
	    .invert_color_force = UNSET,

	    // Initialized in this function
	    .a = {0},
	    .pictfmt = NULL,
	    .client_pictfmt = NULL,
	    .widthb = 0,
	    .heightb = 0,
	    .shadow_dx = 0,
	    .shadow_dy = 0,
	    .shadow_width = 0,
	    .shadow_height = 0,
	    .damage = XCB_NONE,

	    // Not initialized until mapped, this variables
	    // have no meaning or have no use until the window
	    // is mapped
	    .win_image = NULL,
	    .shadow_image = NULL,
	    .mask_image = NULL,
	    .prev_trans = NULL,
	    .shadow = false,
	    .clip_shadow_above = false,
	    .fg_shader = NULL,
	    .randr_monitor = -1,
	    .mode = WMODE_TRANS,
	    .ever_damaged = false,
	    .client_win = XCB_NONE,
	    .leader = XCB_NONE,
	    .cache_leader = XCB_NONE,
	    .window_type = WINTYPE_UNKNOWN,
	    .focused = false,
	    .has_opacity_prop = false,
	    .opacity_prop = OPAQUE,
	    .opacity_is_set = false,
	    .opacity_set = 1,
	    .frame_extents = MARGIN_INIT,        // in win_mark_client
	    .bounding_shaped = false,
	    .bounding_shape = {0},
	    .rounded_corners = false,
	    .paint_excluded = false,
	    .fade_excluded = false,
	    .transparent_clipping_excluded = false,
	    .unredir_if_possible_excluded = false,
	    .prop_shadow = -1,
	    // following 4 are set in win_mark_client
	    .name = NULL,
	    .class_instance = NULL,
	    .class_general = NULL,
	    .role = NULL,

	    // Initialized during paint
	    .paint = PAINT_INIT,
	    .shadow_paint = PAINT_INIT,

	    .corner_radius = 0,
	};

	assert(!w->destroyed);
	assert(w->is_new);

	w->is_new = false;

	// Reject overlay window and already added windows
	if (w->id == ps->overlay) {
		return w;
	}

	auto duplicated_win = find_managed_win(ps, w->id);
	if (duplicated_win) {
		log_debug("Window %#010x (recorded name: %s) added multiple "
		          "times",
		          w->id, duplicated_win->name);
		return &duplicated_win->base;
	}

	log_debug("Managing window %#010x", w->id);
	xcb_get_window_attributes_cookie_t acookie =
	    xcb_get_window_attributes(ps->c.c, w->id);
	xcb_get_window_attributes_reply_t *a =
	    xcb_get_window_attributes_reply(ps->c.c, acookie, NULL);
	if (!a || a->map_state == XCB_MAP_STATE_UNVIEWABLE) {
		// Failed to get window attributes or geometry probably means
		// the window is gone already. Unviewable means the window is
		// already reparented elsewhere.
		// BTW, we don't care about Input Only windows, except for
		// stacking proposes, so we need to keep track of them still.
		free(a);
		return w;
	}

	if (a->_class == XCB_WINDOW_CLASS_INPUT_ONLY) {
		// No need to manage this window, but we still keep it on the
		// window stack
		w->managed = false;
		free(a);
		return w;
	}

	// Allocate and initialize the new win structure
	auto new_internal = cmalloc(struct managed_win_internal);
	auto new = (struct managed_win *)new_internal;

	// Fill structure
	// We only need to initialize the part that are not initialized
	// by map_win
	*new = win_def;
	new->base = *w;
	new->base.managed = true;
	new->a = *a;
	new->opacity = animatable_new(0, linear_interpolator, NULL);
	new->blur_opacity = animatable_new(0, linear_interpolator, NULL);
	pixman_region32_init(&new->bounding_shape);

	free(a);

	xcb_generic_error_t *e;
	auto g = xcb_get_geometry_reply(ps->c.c, xcb_get_geometry(ps->c.c, w->id), &e);
	if (!g) {
		log_error_x_error(e, "Failed to get geometry of window %#010x", w->id);
		free(e);
		free(new);
		return w;
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
	    ps->c.c, xcb_damage_create_checked(ps->c.c, new->damage, w->id,
	                                       XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY));
	if (e) {
		log_error_x_error(e, "Failed to create damage");
		free(e);
		free(new);
		return w;
	}

	// Set window event mask
	xcb_change_window_attributes(
	    ps->c.c, new->base.id, XCB_CW_EVENT_MASK,
	    (const uint32_t[]){determine_evmask(ps, new->base.id, WIN_EVMODE_FRAME)});

	// Get notification when the shape of a window changes
	if (ps->shape_exists) {
		xcb_shape_select_input(ps->c.c, new->base.id, 1);
	}

	new->pictfmt = x_get_pictform_for_visual(&ps->c, new->a.visual);
	new->client_pictfmt = NULL;

	// Set all the stale flags on this new window, so it's properties will get
	// updated when it's mapped
	win_set_flags(new, WIN_FLAGS_CLIENT_STALE | WIN_FLAGS_SIZE_STALE |
	                       WIN_FLAGS_POSITION_STALE | WIN_FLAGS_PROPERTY_STALE |
	                       WIN_FLAGS_FACTOR_CHANGED);
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

	return &new->base;
}

/**
 * Update leader of a window.
 */
static xcb_window_t
win_get_leader_property(struct x_connection *c, struct atom *atoms, xcb_window_t wid,
                        bool detect_transient, bool detect_client_leader) {
	xcb_window_t leader = XCB_NONE;

	// Read the leader properties
	if (detect_transient) {
		leader = wid_get_prop_window(c, wid, atoms->aWM_TRANSIENT_FOR);
	}

	if (detect_client_leader && leader == XCB_NONE) {
		leader = wid_get_prop_window(c, wid, atoms->aWM_CLIENT_LEADER);
	}

	log_trace("window %#010x: leader %#010x", wid, leader);
	return leader;
}

/**
 * Internal function of win_get_leader().
 */
static xcb_window_t win_get_leader_raw(session_t *ps, struct managed_win *w, int recursions) {
	// Rebuild the cache if needed
	if (!w->cache_leader && (w->client_win || w->leader)) {
		// Leader defaults to client window
		if (!(w->cache_leader = w->leader)) {
			w->cache_leader = w->client_win;
		}

		// If the leader of this window isn't itself, look for its
		// ancestors
		if (w->cache_leader && w->cache_leader != w->client_win) {
			auto wp = find_toplevel(ps, w->cache_leader);
			if (wp) {
				// Dead loop?
				if (recursions > WIN_GET_LEADER_MAX_RECURSION) {
					return XCB_NONE;
				}

				w->cache_leader = win_get_leader_raw(ps, wp, recursions + 1);
			}
		}
	}

	return w->cache_leader;
}

/**
 * Retrieve the <code>WM_CLASS</code> of a window and update its
 * <code>win</code> structure.
 */
bool win_update_class(struct x_connection *c, struct atom *atoms, struct managed_win *w) {
	char **strlst = NULL;
	int nstr = 0;

	// Can't do anything if there's no client window
	if (!w->client_win) {
		return false;
	}

	// Free and reset old strings
	free(w->class_instance);
	free(w->class_general);
	w->class_instance = NULL;
	w->class_general = NULL;

	// Retrieve the property string list
	if (!wid_get_text_prop(c, atoms, w->client_win, atoms->aWM_CLASS, &strlst, &nstr)) {
		return false;
	}

	// Copy the strings if successful
	w->class_instance = strdup(strlst[0]);

	if (nstr > 1) {
		w->class_general = strdup(strlst[1]);
	}

	free(strlst);

	log_trace("(%#010x): client = %#010x, "
	          "instance = \"%s\", general = \"%s\"",
	          w->base.id, w->client_win, w->class_instance, w->class_general);

	return true;
}

/**
 * Handle window focus change.
 */
static void win_on_focus_change(session_t *ps, struct managed_win *w) {
	// If window grouping detection is enabled
	if (ps->o.track_leader) {
		xcb_window_t leader = win_get_leader(ps, w);

		// If the window gets focused, replace the old active_leader
		if (win_is_focused_raw(w) && leader != ps->active_leader) {
			xcb_window_t active_leader_old = ps->active_leader;

			ps->active_leader = leader;

			group_on_factor_change(ps, active_leader_old);
			group_on_factor_change(ps, leader);
		}
		// If the group get unfocused, remove it from active_leader
		else if (!win_is_focused_raw(w) && leader &&
		         leader == ps->active_leader && !group_is_focused(ps, leader)) {
			ps->active_leader = XCB_NONE;
			group_on_factor_change(ps, leader);
		}
	}

	// Update everything related to conditions
	win_on_factor_change(ps, w);

	// Send D-Bus signal
	if (ps->o.dbus) {
		if (win_is_focused_raw(w)) {
			cdbus_ev_win_focusin(ps, &w->base);
		} else {
			cdbus_ev_win_focusout(ps, &w->base);
		}
	}
}

/**
 * Set real focused state of a window.
 */
void win_set_focused(session_t *ps, struct managed_win *w) {
	// Unmapped windows will have their focused state reset on map
	if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	if (w->is_ewmh_focused) {
		assert(ps->active_win == w);
		return;
	}

	auto old_active_win = ps->active_win;
	ps->active_win = w;
	w->is_ewmh_focused = true;

	if (old_active_win) {
		assert(old_active_win->is_ewmh_focused);
		old_active_win->is_ewmh_focused = false;
		win_on_focus_change(ps, old_active_win);
	}
	win_on_focus_change(ps, w);
}

/**
 * Get a rectangular region a window (and possibly its shadow) occupies.
 *
 * Note w->shadow and shadow geometry must be correct before calling this
 * function.
 */
void win_extents(const struct managed_win *w, region_t *res) {
	pixman_region32_clear(res);
	pixman_region32_union_rect(res, res, w->g.x, w->g.y, (uint)w->widthb, (uint)w->heightb);

	if (w->shadow) {
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
void win_update_bounding_shape(struct x_connection *c, struct managed_win *w,
                               bool shape_exists, bool detect_rounded_corners) {
	// We don't handle property updates of non-visible windows until they are
	// mapped.
	assert(w->state == WSTATE_MAPPED);

	pixman_region32_clear(&w->bounding_shape);
	// Start with the window rectangular region
	win_get_region_local(w, &w->bounding_shape);

	if (shape_exists) {
		w->bounding_shaped = win_bounding_shaped(c, w->base.id);
	}

	// Only request for a bounding region if the window is shaped
	// (while loop is used to avoid goto, not an actual loop)
	while (w->bounding_shaped) {
		/*
		 * if window doesn't exist anymore,  this will generate an error
		 * as well as not generate a region.
		 */

		xcb_shape_get_rectangles_reply_t *r = xcb_shape_get_rectangles_reply(
		    c->c, xcb_shape_get_rectangles(c->c, w->base.id, XCB_SHAPE_SK_BOUNDING),
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
 * Reread opacity property of a window.
 */
void win_update_opacity_prop(struct x_connection *c, struct atom *atoms,
                             struct managed_win *w, bool detect_client_opacity) {
	// get frame opacity first
	w->has_opacity_prop =
	    wid_get_opacity_prop(c, atoms, w->base.id, OPAQUE, &w->opacity_prop);

	if (w->has_opacity_prop) {
		// opacity found
		return;
	}

	if (detect_client_opacity && w->client_win && w->base.id == w->client_win) {
		// checking client opacity not allowed
		return;
	}

	// get client opacity
	w->has_opacity_prop =
	    wid_get_opacity_prop(c, atoms, w->client_win, OPAQUE, &w->opacity_prop);
}

/**
 * Retrieve frame extents from a window.
 */
void win_update_frame_extents(struct x_connection *c, struct atom *atoms,
                              struct managed_win *w, xcb_window_t client,
                              double frame_opacity) {
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

		const bool changed = w->frame_extents.left != extents[0] ||
		                     w->frame_extents.right != extents[1] ||
		                     w->frame_extents.top != extents[2] ||
		                     w->frame_extents.bottom != extents[3];
		w->frame_extents.left = extents[0];
		w->frame_extents.right = extents[1];
		w->frame_extents.top = extents[2];
		w->frame_extents.bottom = extents[3];

		// If frame_opacity != 1, then frame of this window
		// is not included in reg_ignore of underneath windows
		if (frame_opacity == 1 && changed) {
			w->reg_ignore_valid = false;
		}
	}

	log_trace("(%#010x): %d, %d, %d, %d", w->base.id, w->frame_extents.left,
	          w->frame_extents.right, w->frame_extents.top, w->frame_extents.bottom);

	free_winprop(&prop);
}

bool win_is_region_ignore_valid(session_t *ps, const struct managed_win *w) {
	win_stack_foreach_managed(i, &ps->window_stack) {
		if (i == w) {
			break;
		}
		if (!i->reg_ignore_valid) {
			return false;
		}
	}
	return true;
}

/**
 * Stop listening for events on a particular window.
 */
void win_ev_stop(session_t *ps, const struct win *w) {
	xcb_change_window_attributes(ps->c.c, w->id, XCB_CW_EVENT_MASK, (const uint32_t[]){0});

	if (!w->managed) {
		return;
	}

	auto mw = (struct managed_win *)w;
	if (mw->client_win) {
		xcb_change_window_attributes(ps->c.c, mw->client_win, XCB_CW_EVENT_MASK,
		                             (const uint32_t[]){0});
	}

	if (ps->shape_exists) {
		xcb_shape_select_input(ps->c.c, w->id, 0);
	}
}

/// Finish the destruction of a window (e.g. after fading has finished).
/// Frees `w`
void destroy_win_finish(session_t *ps, struct win *w) {
	log_verbose("Trying to finish destroying (%#010x)", w->id);

	auto next_w = win_stack_find_next_managed(ps, &w->stack_neighbour);
	list_remove(&w->stack_neighbour);

	if (w->managed) {
		auto mw = (struct managed_win *)w;
		unmap_win_finish(ps, mw);

		// Unmapping might preserve the shadow image, so free it here
		win_release_shadow(ps->backend_data, mw);
		win_release_mask(ps->backend_data, mw);

		// Invalidate reg_ignore of windows below this one
		// TODO(yshui) what if next_w is not mapped??
		/* TODO(yshui) seriously figure out how reg_ignore behaves.
		 * I think if `w` is unmapped, and destroyed after
		 * paint happened at least once, w->reg_ignore_valid would
		 * be true, and there is no need to invalid w->next->reg_ignore
		 * when w is destroyed. */
		if (next_w) {
			rc_region_unref(&next_w->reg_ignore);
			next_w->reg_ignore_valid = false;
		}

		if (mw == ps->active_win) {
			// Usually, the window cannot be the focused at
			// destruction. FocusOut should be generated before the
			// window is destroyed. We do this check just to be
			// completely sure we don't have dangling references.
			log_debug("window %#010x (%s) is destroyed while being "
			          "focused",
			          w->id, mw->name);
			ps->active_win = NULL;
		}

		free_win_res(ps, mw);

		// Drop w from all prev_trans to avoid accessing freed memory in
		// repair_win()
		// TODO(yshui) there can only be one prev_trans pointing to w
		win_stack_foreach_managed(w2, &ps->window_stack) {
			if (mw == w2->prev_trans) {
				w2->prev_trans = NULL;
			}
		}
	}

	free(w);
}

/// Move window `w` so it's before `next` in the list
static inline void restack_win(session_t *ps, struct win *w, struct list_node *next) {
	struct managed_win *mw = NULL;
	if (w->managed) {
		mw = (struct managed_win *)w;
	}

	if (mw) {
		// This invalidates all reg_ignore below the new stack position of
		// `w`
		mw->reg_ignore_valid = false;
		rc_region_unref(&mw->reg_ignore);

		// This invalidates all reg_ignore below the old stack position of
		// `w`
		auto next_w = win_stack_find_next_managed(ps, &w->stack_neighbour);
		if (next_w) {
			next_w->reg_ignore_valid = false;
			rc_region_unref(&next_w->reg_ignore);
		}
	}

	list_move_before(&w->stack_neighbour, next);

	// add damage for this window
	if (mw) {
		add_damage_from_win(ps, mw);
	}

#ifdef DEBUG_RESTACK
	log_trace("Window stack modified. Current stack:");
	for (auto c = ps->list; c; c = c->next) {
		const char *desc = "";
		if (c->state == WSTATE_DESTROYING) {
			desc = "(D) ";
		}
		log_trace("%#010x \"%s\" %s", c->id, c->name, desc);
	}
#endif
}

/// Move window `w` so it's right above `below`
void restack_above(session_t *ps, struct win *w, xcb_window_t below) {
	xcb_window_t old_below;

	if (!list_node_is_last(&ps->window_stack, &w->stack_neighbour)) {
		old_below = list_next_entry(w, stack_neighbour)->id;
	} else {
		old_below = XCB_NONE;
	}
	log_debug("Restack %#010x (%s), old_below: %#010x, new_below: %#010x", w->id,
	          win_get_name_if_managed(w), old_below, below);

	if (old_below != below) {
		struct list_node *new_next;
		if (!below) {
			new_next = &ps->window_stack;
		} else {
			struct win *tmp_w = NULL;
			HASH_FIND_INT(ps->windows, &below, tmp_w);

			if (!tmp_w) {
				log_error("Failed to found new below window %#010x.", below);
				return;
			}

			new_next = &tmp_w->stack_neighbour;
		}
		restack_win(ps, w, new_next);
	}
}

void restack_bottom(session_t *ps, struct win *w) {
	restack_above(ps, w, 0);
}

void restack_top(session_t *ps, struct win *w) {
	log_debug("Restack %#010x (%s) to top", w->id, win_get_name_if_managed(w));
	if (&w->stack_neighbour == ps->window_stack.next) {
		// already at top
		return;
	}
	restack_win(ps, w, ps->window_stack.next);
}

/// Start destroying a window. Windows cannot always be destroyed immediately
/// because of fading and such.
void destroy_win_start(session_t *ps, struct win *w) {
	auto mw = (struct managed_win *)w;
	assert(w);

	log_debug("Destroying %#010x \"%s\", managed = %d", w->id,
	          (w->managed ? mw->name : NULL), w->managed);

	// Delete destroyed window from the hash table, even though the window
	// might still be rendered for a while. We need to make sure future window
	// with the same window id won't confuse us. Keep the window in the window
	// stack if it's managed and mapped, since we might still need to render
	// it (e.g. fading out). Window will be removed from the stack when it
	// finishes destroying.
	HASH_DEL(ps->windows, w);

	if (w->managed) {
		if (mw->state != WSTATE_UNMAPPED) {
			// Only UNMAPPED state has window resources freed,
			// otherwise we need to call unmap_win_finish to free
			// them.
			log_warn("Did X server not unmap window %#010x before destroying "
			         "it?",
			         w->id);
		}
		// Clear IMAGES_STALE flags since the window is destroyed: Clear
		// PIXMAP_STALE as there is no pixmap available anymore, so STALE
		// doesn't make sense.
		// XXX Clear SHADOW_STALE as setting/clearing flags on a destroyed
		// window doesn't work leading to an inconsistent state where the
		// shadow is refreshed but the flags are stuck in STALE. Do this
		// before changing the window state to destroying
		win_clear_flags(mw, WIN_FLAGS_PIXMAP_STALE);

		// If size/shape/position information is stale,
		// win_process_update_flags will update them and add the new
		// window extents to damage. Since the window has been destroyed,
		// we cannot get the complete information at this point, so we
		// just add what we currently have to the damage.
		if (win_check_flags_any(mw, WIN_FLAGS_SIZE_STALE | WIN_FLAGS_POSITION_STALE)) {
			add_damage_from_win(ps, mw);
		}

		if (win_check_flags_all(mw, WIN_FLAGS_CLIENT_STALE)) {
			mw->client_win = mw->base.id;
			log_debug("(%#010x): client self (%s)", mw->base.id,
			          (mw->a.override_redirect ? "override-redirected" : "wmwin"));
		}

		// Clear some flags about stale window information. Because now
		// the window is destroyed, we can't update them anyway.
		win_clear_flags(mw, WIN_FLAGS_SIZE_STALE | WIN_FLAGS_POSITION_STALE |
		                        WIN_FLAGS_PROPERTY_STALE |
		                        WIN_FLAGS_FACTOR_CHANGED | WIN_FLAGS_CLIENT_STALE);

		// Update state flags of a managed window
		mw->state = WSTATE_DESTROYED;
		mw->a.map_state = XCB_MAP_STATE_UNMAPPED;
		mw->in_openclose = true;

		// We don't initiate animation here, because it should already have been
		// started by unmap_win_start, because X automatically unmaps windows
		// before destroying them. But we do need to stop animation if
		// no_fading_destroyed_windows, or no_fading_openclose is enabled.
		if (!win_should_fade(ps, mw)) {
			win_skip_fading(mw);
		}
	}

	// don't need win_ev_stop because the window is gone anyway
	// Send D-Bus signal
	if (ps->o.dbus) {
		cdbus_ev_win_destroyed(ps, w);
	}

	if (!ps->redirected && w->managed) {
		// Skip transition if we are not rendering
		win_skip_fading(mw);
	}
}

void unmap_win_start(session_t *ps, struct managed_win *w) {
	assert(w);
	assert(w->base.managed);
	assert(w->a._class != XCB_WINDOW_CLASS_INPUT_ONLY);

	log_debug("Unmapping %#010x \"%s\"", w->base.id, w->name);

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
	win_start_fade(ps, w, 0);

	// Send D-Bus signal
	if (ps->o.dbus) {
		cdbus_ev_win_unmapped(ps, &w->base);
	}

	if (!ps->redirected || !w->ever_damaged) {
		// If we are not redirected, we skip fading because we aren't
		// rendering anything anyway. If the window wasn't ever damaged,
		// it shouldn't be painted either. But a fading out window is
		// always painted, so we have to skip fading here.
		win_skip_fading(w);
	}
}

/// Skip the current in progress fading of window,
/// transition the window straight to its end state
void win_skip_fading(struct managed_win *w) {
	if (w->number_of_animations == 0) {
		return;
	}
	log_debug("Skipping fading process of window %#010x (%s)", w->base.id, w->name);
	animatable_early_stop(&w->opacity);
	animatable_early_stop(&w->blur_opacity);
}

// TODO(absolutelynothelix): rename to x_update_win_(randr_?)monitor and move to
// the x.c.
void win_update_monitor(struct x_monitors *monitors, struct managed_win *mw) {
	mw->randr_monitor = -1;
	for (int i = 0; i < monitors->count; i++) {
		auto e = pixman_region32_extents(&monitors->regions[i]);
		if (e->x1 <= mw->g.x && e->y1 <= mw->g.y &&
		    e->x2 >= mw->g.x + mw->widthb && e->y2 >= mw->g.y + mw->heightb) {
			mw->randr_monitor = i;
			log_debug("Window %#010x (%s), %dx%d+%dx%d, is entirely on the "
			          "monitor %d (%dx%d+%dx%d)",
			          mw->base.id, mw->name, mw->g.x, mw->g.y, mw->widthb,
			          mw->heightb, i, e->x1, e->y1, e->x2 - e->x1, e->y2 - e->y1);
			return;
		}
	}
	log_debug("Window %#010x (%s), %dx%d+%dx%d, is not entirely on any monitor",
	          mw->base.id, mw->name, mw->g.x, mw->g.y, mw->widthb, mw->heightb);
}

/// Map an already registered window
void map_win_start(session_t *ps, struct managed_win *w) {
	assert(ps->server_grabbed);
	assert(w);

	// Don't care about window mapping if it's an InputOnly window
	// Also, try avoiding mapping a window twice
	if (w->a._class == XCB_WINDOW_CLASS_INPUT_ONLY) {
		return;
	}

	log_debug("Mapping (%#010x \"%s\")", w->base.id, w->name);

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

	log_debug("Window (%#010x) has type %s", w->base.id, WINTYPES[w->window_type].name);

	w->state = WSTATE_MAPPED;
	win_start_fade(ps, w, 1);

	log_debug("Window %#010x has opacity %f, opacity target is %f", w->base.id,
	          animatable_get(&w->opacity), w->opacity.target);

	// Send D-Bus signal
	if (ps->o.dbus) {
		cdbus_ev_win_mapped(ps, &w->base);
	}

	if (!ps->redirected) {
		win_skip_fading(w);
	}
}

/**
 * Update target window opacity depending on the current state.
 */
void win_update_opacity_target(session_t *ps, struct managed_win *w) {
	win_start_fade(ps, w, w->blur_opacity.target);        // We don't want to change
	                                                      // blur_opacity target

	if (w->number_of_animations == 0) {
		return;
	}

	log_debug("Window %#010x (%s) opacity %f, opacity target %f, start %f", w->base.id,
	          w->name, animatable_get(&w->opacity), w->opacity.target, w->opacity.start);

	if (!ps->redirected) {
		win_skip_fading(w);
	}
}

/**
 * Find a managed window from window id in window linked list of the session.
 */
struct win *find_win(session_t *ps, xcb_window_t id) {
	if (!id) {
		return NULL;
	}

	struct win *w = NULL;
	HASH_FIND_INT(ps->windows, &id, w);
	assert(w == NULL || !w->destroyed);
	return w;
}

/**
 * Find a managed window from window id in window linked list of the session.
 */
struct managed_win *find_managed_win(session_t *ps, xcb_window_t id) {
	struct win *w = find_win(ps, id);
	if (!w || !w->managed) {
		return NULL;
	}

	auto mw = (struct managed_win *)w;
	assert(mw->state != WSTATE_DESTROYED);
	return mw;
}

/**
 * Find out the WM frame of a client window using existing data.
 *
 * @param id window ID
 * @return struct win object of the found window, NULL if not found
 */
struct managed_win *find_toplevel(session_t *ps, xcb_window_t id) {
	if (!id) {
		return NULL;
	}

	HASH_ITER2(ps->windows, w) {
		assert(!w->destroyed);
		if (!w->managed) {
			continue;
		}

		auto mw = (struct managed_win *)w;
		if (mw->client_win == id) {
			return mw;
		}
	}

	return NULL;
}

/**
 * Find a managed window that is, or is a parent of `wid`.
 *
 * @param ps current session
 * @param wid window ID
 * @return struct _win object of the found window, NULL if not found
 */
struct managed_win *find_managed_window_or_parent(session_t *ps, xcb_window_t wid) {
	// TODO(yshui) this should probably be an "update tree", then
	// find_toplevel. current approach is a bit more "racy", as the server
	// state might be ahead of our state
	struct win *w = NULL;

	// We traverse through its ancestors to find out the frame
	// Using find_win here because if we found a unmanaged window we know
	// about, we can stop early.
	while (wid && wid != ps->c.screen_info->root && !(w = find_win(ps, wid))) {
		// xcb_query_tree probably fails if you run picom when X is
		// somehow initializing (like add it in .xinitrc). In this case
		// just leave it alone.
		auto reply =
		    xcb_query_tree_reply(ps->c.c, xcb_query_tree(ps->c.c, wid), NULL);
		if (reply == NULL) {
			break;
		}

		wid = reply->parent;
		free(reply);
	}

	if (w == NULL || !w->managed) {
		return NULL;
	}

	return (struct managed_win *)w;
}

/// Set flags on a window. Some sanity checks are performed
void win_set_flags(struct managed_win *w, uint64_t flags) {
	log_debug("Set flags %" PRIu64 " to window %#010x (%s)", flags, w->base.id, w->name);
	if (unlikely(w->state == WSTATE_DESTROYED)) {
		log_error("Flags set on a destroyed window %#010x (%s)", w->base.id, w->name);
		return;
	}

	w->flags |= flags;
}

/// Clear flags on a window. Some sanity checks are performed
void win_clear_flags(struct managed_win *w, uint64_t flags) {
	log_debug("Clear flags %" PRIu64 " from window %#010x (%s)", flags, w->base.id,
	          w->name);
	if (unlikely(w->state == WSTATE_DESTROYED)) {
		log_warn("Flags cleared on a destroyed window %#010x (%s)", w->base.id,
		         w->name);
		return;
	}

	w->flags = w->flags & (~flags);
}

void win_set_properties_stale(struct managed_win *w, const xcb_atom_t *props, int nprops) {
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
		    realloc(w->stale_props, new_capacity * sizeof(*w->stale_props));

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

static void win_clear_all_properties_stale(struct managed_win *w) {
	memset(w->stale_props, 0, w->stale_props_capacity * sizeof(*w->stale_props));
	win_clear_flags(w, WIN_FLAGS_PROPERTY_STALE);
}

static bool win_fetch_and_unset_property_stale(struct managed_win *w, xcb_atom_t prop) {
	auto const bits_per_element = sizeof(*w->stale_props) * 8;
	if (prop >= w->stale_props_capacity * bits_per_element) {
		return false;
	}

	auto const mask = 1UL << (prop % bits_per_element);
	bool ret = w->stale_props[prop / bits_per_element] & mask;
	w->stale_props[prop / bits_per_element] &= ~mask;
	return ret;
}

bool win_check_flags_any(struct managed_win *w, uint64_t flags) {
	return (w->flags & flags) != 0;
}

bool win_check_flags_all(struct managed_win *w, uint64_t flags) {
	return (w->flags & flags) == flags;
}

/**
 * Check if a window is a fullscreen window.
 *
 * It's not using w->border_size for performance measures.
 */
void win_update_is_fullscreen(const session_t *ps, struct managed_win *w) {
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
bool win_is_bypassing_compositor(const session_t *ps, const struct managed_win *w) {
	bool ret = false;

	auto prop = x_get_prop(&ps->c, w->client_win, ps->atoms->a_NET_WM_BYPASS_COMPOSITOR,
	                       1L, XCB_ATOM_CARDINAL, 32);

	if (prop.nitems && *prop.c32 == 1) {
		ret = true;
	}

	free_winprop(&prop);
	return ret;
}

/**
 * Check if a window is focused, without using any focus rules or forced focus
 * settings
 */
bool win_is_focused_raw(const struct managed_win *w) {
	return w->a.map_state == XCB_MAP_STATE_VIEWABLE && w->is_ewmh_focused;
}

// Find the managed window immediately below `i` in the window stack
struct managed_win *
win_stack_find_next_managed(const session_t *ps, const struct list_node *w) {
	while (!list_node_is_last(&ps->window_stack, w)) {
		auto next = list_entry(w->next, struct win, stack_neighbour);
		if (next->managed) {
			return (struct managed_win *)next;
		}
		w = &next->stack_neighbour;
	}
	return NULL;
}
