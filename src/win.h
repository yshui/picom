#pragma once
#include <stdbool.h>
#include <X11/Xlib.h>

#include "x.h"

typedef struct session session_t;
typedef struct win win;


int win_get_name(session_t *ps, win *w);
int win_get_role(session_t *ps, win *w);
void win_determine_mode(session_t *ps, win *w);
/**
 * Set real focused state of a window.
 */
void win_set_focused(session_t *ps, win *w, bool focused);
void win_determine_fade(session_t *ps, win *w);
void win_update_prop_shadow_raw(session_t *ps, win *w);
void win_update_prop_shadow(session_t *ps, win *w);
void win_set_shadow(session_t *ps, win *w, bool shadow_new);
void win_determine_shadow(session_t *ps, win *w);
void win_set_invert_color(session_t *ps, win *w, bool invert_color_new);
void win_determine_invert_color(session_t *ps, win *w);
void win_set_blur_background(session_t *ps, win *w, bool blur_background_new);
void win_determine_blur_background(session_t *ps, win *w);
void win_on_wtype_change(session_t *ps, win *w);
void win_on_factor_change(session_t *ps, win *w);
void calc_win_size(session_t *ps, win *w);
void calc_shadow_geometry(session_t *ps, win *w);
void win_upd_wintype(session_t *ps, win *w);
void win_mark_client(session_t *ps, win *w, Window client);
void win_unmark_client(session_t *ps, win *w);
void win_recheck_client(session_t *ps, win *w);
Window win_get_leader_raw(session_t *ps, win *w, int recursions);
bool win_get_class(session_t *ps, win *w);
void win_rounded_corners(session_t *ps, win *w);
void win_calc_opacity(session_t *ps, win *w);
void win_calc_dim(session_t *ps, win *w);
/**
 * Reread opacity property of a window.
 */
void win_update_opacity_prop(session_t *ps, win *w);
/**
 * Update leader of a window.
 */
void win_update_leader(session_t *ps, win *w);
/**
 * Update focused state of a window.
 */
void win_update_focused(session_t *ps, win *w);
/**
 * Retrieve the bounding shape of a window.
 */
// XXX was win_border_size
void win_update_bounding_shape(session_t *ps, win *w);
/**
 * Get a rectangular region a window (and possibly its shadow) occupies.
 *
 * Note w->shadow and shadow geometry must be correct before calling this
 * function.
 */
void win_extents(win *w, region_t *res);
/**
 * Add a window to damaged area.
 *
 * @param ps current session
 * @param w struct _win element representing the window
 */
void add_damage_from_win(session_t *ps, win *w);
/**
 * Get a rectangular region a window occupies, excluding shadow.
 *
 * global = use global coordinates
 */
void win_get_region(session_t *ps, win *w, bool global, region_t *);
/**
 * Get a rectangular region a window occupies, excluding frame and shadow.
 */
void win_get_region_noframe(session_t *ps, win *w, bool global, region_t *);
/**
 * Retrieve frame extents from a window.
 */
void
win_update_frame_extents(session_t *ps, win *w, Window client);
bool add_win(session_t *ps, Window id, Window prev);

/**
 * Get the leader of a window.
 *
 * This function updates w->cache_leader if necessary.
 */
static inline Window
win_get_leader(session_t *ps, win *w) {
  return win_get_leader_raw(ps, w, 0);
}

/// check if window has ARGB visual
bool win_has_alpha(win *w);

/// check if reg_ignore_valid is true for all windows above us
bool win_is_region_ignore_valid(session_t *ps, win *w);
