// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>
#pragma once
#include <stdbool.h>
#include <xcb/damage.h>

// FIXME shouldn't need this
#ifdef CONFIG_OPENGL
#include <GL/glx.h>
#endif

#include "x.h"
#include "types.h"
#include "c2.h"

typedef struct session session_t;
typedef struct _glx_texture glx_texture_t;
typedef enum {
  WINTYPE_UNKNOWN,
  WINTYPE_DESKTOP,
  WINTYPE_DOCK,
  WINTYPE_TOOLBAR,
  WINTYPE_MENU,
  WINTYPE_UTILITY,
  WINTYPE_SPLASH,
  WINTYPE_DIALOG,
  WINTYPE_NORMAL,
  WINTYPE_DROPDOWN_MENU,
  WINTYPE_POPUP_MENU,
  WINTYPE_TOOLTIP,
  WINTYPE_NOTIFY,
  WINTYPE_COMBO,
  WINTYPE_DND,
  NUM_WINTYPES
} wintype_t;

/// Enumeration type of window painting mode.
typedef enum {
  WMODE_TRANS, // The window body is (potentially) transparent
  WMODE_FRAME_TRANS, // The window body is opaque, but the frame is not
  WMODE_SOLID, // The window is opaque including the frame
} winmode_t;

typedef enum {
  // The window is been unmapped, meaning unmap_win is called, but
  // the window might need fading. This also implies this window
  // was in mapped state.
  WSTATE_UNMAPPING,
  // The window is mapped
  WSTATE_MAPPED,
  // The window is unmapped
  WSTATE_UNMAPPED,
} winstate_t;

/**
 * About coordinate systems
 *
 * In general, X is the horizontal axis, Y is the vertical axis.
 * X goes from left to right, Y goes downwards.
 *
 * Global: the origin is the top left corner of the Xorg screen.
 * Local: the origin is the top left corner of the window, including border.
 */

/// Structure representing a top-level window compton manages.
typedef struct win win;
struct win {
  /// backend data attached to this window. Only available when
  /// `state` is not UNMAPPED
  void *win_data;
  /// Pointer to the next lower window in window stack.
  win *next;
  /// Pointer to the next higher window to paint.
  win *prev_trans;

  // Core members
  /// ID of the top-level frame window.
  xcb_window_t id;
  /// The "mapped state" of this window, doesn't necessary
  /// match X mapped state, because of fading.
  winstate_t state;
  /// Window attributes.
  xcb_get_window_attributes_reply_t a;
  xcb_get_geometry_reply_t g;
#ifdef CONFIG_XINERAMA
  /// Xinerama screen this window is on.
  int xinerama_scr;
#endif
  /// Window visual pict format;
  xcb_render_pictforminfo_t *pictfmt;
  /// Window painting mode.
  winmode_t mode;
  /// Whether the window has been damaged at least once.
  bool ever_damaged;
  /// Whether the window was damaged after last paint.
  bool pixmap_damaged;
  /// Damage of the window.
  xcb_damage_damage_t damage;

  /// Bounding shape of the window. In local coordinates.
  /// See above about coordinate systems.
  region_t bounding_shape;
  /// Window flags. Definitions above.
  int_fast16_t flags;
  /// Whether there's a pending <code>ConfigureNotify</code> happening
  /// when the window is unmapped.
  bool need_configure;
  /// Queued <code>ConfigureNotify</code> when the window is unmapped.
  xcb_configure_notify_event_t queue_configure;
  /// The region of screen that will be obscured when windows above is painted,
  /// in global coordinates.
  /// We use this to reduce the pixels that needed to be paint when painting
  /// this window and anything underneath. Depends on window frame
  /// opacity state, window geometry, window mapped/unmapped state,
  /// window mode of the windows above. DOES NOT INCLUDE the body of THIS WINDOW.
  /// NULL means reg_ignore has not been calculated for this window.
  rc_region_t *reg_ignore;
  /// Whether the reg_ignore of all windows beneath this window are valid
  bool reg_ignore_valid;
  /// Cached width/height of the window including border.
  int widthb, heightb;
  /// Whether the window is being destroyed. This being true means destroy_win
  /// is called, but window might still need to be faded out
  bool destroying;
  /// Whether the window is bounding-shaped.
  bool bounding_shaped;
  /// Whether the window just have rounded corners.
  bool rounded_corners;
  /// Whether this window is to be painted.
  bool to_paint;
  /// Whether the window is painting excluded.
  bool paint_excluded;
  /// Whether the window is unredirect-if-possible excluded.
  bool unredir_if_possible_excluded;
  /// Whether this window is in open/close state.
  bool in_openclose;

  // Client window related members
  /// ID of the top-level client window of the window.
  xcb_window_t client_win;
  /// Type of the window.
  wintype_t window_type;
  /// Whether it looks like a WM window. We consider a window WM window if
  /// it does not have a decedent with WM_STATE and it is not override-
  /// redirected itself.
  bool wmwin;
  /// Leader window ID of the window.
  xcb_window_t leader;
  /// Cached topmost window ID of the window.
  xcb_window_t cache_leader;

  // Focus-related members
  /// Whether the window is to be considered focused.
  bool focused;
  /// Override value of window focus state. Set by D-Bus method calls.
  switch_t focused_force;

  // Blacklist related members
  /// Name of the window.
  char *name;
  /// Window instance class of the window.
  char *class_instance;
  /// Window general class of the window.
  char *class_general;
  /// <code>WM_WINDOW_ROLE</code> value of the window.
  char *role;
  const c2_lptr_t *cache_sblst;
  const c2_lptr_t *cache_fblst;
  const c2_lptr_t *cache_fcblst;
  const c2_lptr_t *cache_ivclst;
  const c2_lptr_t *cache_bbblst;
  const c2_lptr_t *cache_oparule;
  const c2_lptr_t *cache_pblst;
  const c2_lptr_t *cache_uipblst;

  // Opacity-related members
  /// Current window opacity.
  opacity_t opacity;
  /// Target window opacity.
  opacity_t opacity_tgt;
  /// true if window (or client window, for broken window managers
  /// not transferring client window's _NET_WM_OPACITY value) has opacity prop
  bool has_opacity_prop;
  /// Cached value of opacity window attribute.
  opacity_t opacity_prop;
  /// true if opacity is set by some rules
  bool opacity_is_set;
  /// Last window opacity value we set.
  opacity_t opacity_set;

  // Fading-related members
  /// Do not fade if it's false. Change on window type change.
  /// Used by fading blacklist in the future.
  bool fade;
  /// Fade state on last paint.
  bool fade_last;
  /// Override value of window fade state. Set by D-Bus method calls.
  switch_t fade_force;
  /// Callback to be called after fading completed.
  void (*fade_callback) (session_t *ps, win **w);

  // Frame-opacity-related members
  /// Current window frame opacity. Affected by window opacity.
  double frame_opacity;
  /// Frame extents. Acquired from _NET_FRAME_EXTENTS.
  margin_t frame_extents;

  // Shadow-related members
  /// Whether a window has shadow. Calculated.
  bool shadow;
  /// Shadow state on last paint.
  bool shadow_last;
  /// Override value of window shadow state. Set by D-Bus method calls.
  switch_t shadow_force;
  /// Opacity of the shadow. Affected by window opacity and frame opacity.
  double shadow_opacity;
  /// X offset of shadow. Affected by commandline argument.
  int shadow_dx;
  /// Y offset of shadow. Affected by commandline argument.
  int shadow_dy;
  /// Width of shadow. Affected by window size and commandline argument.
  int shadow_width;
  /// Height of shadow. Affected by window size and commandline argument.
  int shadow_height;
  /// The value of _COMPTON_SHADOW attribute of the window. Below 0 for
  /// none.
  long prop_shadow;

  // Dim-related members
  /// Whether the window is to be dimmed.
  bool dim;

  /// Whether to invert window color.
  bool invert_color;
  /// Color inversion state on last paint.
  bool invert_color_last;
  /// Override value of window color inversion state. Set by D-Bus method
  /// calls.
  switch_t invert_color_force;

  /// Whether to blur window background.
  bool blur_background;
  /// Background state on last paint.
  bool blur_background_last;

#ifdef CONFIG_OPENGL
  /// Textures and FBO background blur use.
  glx_blur_cache_t glx_blur_cache;
#endif
};

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
void win_mark_client(session_t *ps, win *w, xcb_window_t client);
void win_unmark_client(session_t *ps, win *w);
void win_recheck_client(session_t *ps, win *w);
xcb_window_t win_get_leader_raw(session_t *ps, win *w, int recursions);
bool win_get_class(session_t *ps, win *w);
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
 * Get a rectangular region in global coordinates a window (and possibly
 * its shadow) occupies.
 *
 * Note w->shadow and shadow geometry must be correct before calling this
 * function.
 */
void win_extents(win *w, region_t *res);
region_t win_extents_by_val(win *w);
/**
 * Add a window to damaged area.
 *
 * @param ps current session
 * @param w struct _win element representing the window
 */
void add_damage_from_win(session_t *ps, win *w);
/**
 * Get a rectangular region a window occupies, excluding frame and shadow.
 *
 * Return region in global coordinates.
 */
void win_get_region_noframe_local(win *w, region_t *);
region_t win_get_region_noframe_local_by_val(win *w);
/**
 * Retrieve frame extents from a window.
 */
void
win_update_frame_extents(session_t *ps, win *w, xcb_window_t client);
bool add_win(session_t *ps, xcb_window_t id, xcb_window_t prev);

/**
 * Set fade callback of a window, and possibly execute the previous
 * callback.
 *
 * If a callback can cause rendering result to change, it should call
 * `queue_redraw`.
 *
 * @param exec_callback whether the previous callback is to be executed
 */
void win_set_fade_callback(session_t *ps, win **_w,
    void (*callback) (session_t *ps, win **w), bool exec_callback);

/**
 * Execute fade callback of a window if fading finished.
 */
void
win_check_fade_finished(session_t *ps, win **_w);

// Stop receiving events (except ConfigureNotify, XXX why?) from a window
void win_ev_stop(session_t *ps, win *w);

/**
 * Get the leader of a window.
 *
 * This function updates w->cache_leader if necessary.
 */
static inline xcb_window_t
win_get_leader(session_t *ps, win *w) {
  return win_get_leader_raw(ps, w, 0);
}

/// check if window has ARGB visual
bool win_has_alpha(win *w);

/// check if reg_ignore_valid is true for all windows above us
bool win_is_region_ignore_valid(session_t *ps, win *w);

/// Free a struct win
/// prev = pointer to the `next` field of the previous
///        win in the list
void free_win(session_t *ps, win *w);

static inline region_t
win_get_bounding_shape_global_by_val(win *w) {
  region_t ret;
  pixman_region32_init(&ret);
  pixman_region32_copy(&ret, &w->bounding_shape);
  pixman_region32_translate(&ret, w->g.x, w->g.y);
  return ret;
}

/**
 * Calculate the extents of the frame of the given window based on EWMH
 * _NET_FRAME_EXTENTS and the X window border width.
 */
static inline margin_t attr_pure
win_calc_frame_extents(const win *w) {
  margin_t result = w->frame_extents;
  result.top = max_i(result.top, w->g.border_width);
  result.left = max_i(result.left, w->g.border_width);
  result.bottom = max_i(result.bottom, w->g.border_width);
  result.right = max_i(result.right, w->g.border_width);
  return result;
}

/**
 * Check whether a window has WM frames.
 */
static inline bool attr_pure
win_has_frame(const win *w) {
  return w->g.border_width
    || w->frame_extents.top || w->frame_extents.left
    || w->frame_extents.right || w->frame_extents.bottom;
}
