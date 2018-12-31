// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>

#include <xcb/render.h>
#include <xcb/damage.h>
#include <xcb/xcb_renderutil.h>
#include <stdbool.h>
#include <math.h>

#include "compiler.h"
#include "common.h"
#include "compton.h"
#include "c2.h"
#include "x.h"
#include "string_utils.h"
#include "utils.h"
#include "log.h"

#ifdef CONFIG_DBUS
#include "dbus.h"
#endif

#include "win.h"

/// Generate a "return by value" function, from a function that returns the
/// region via a region_t pointer argument.
/// Function signature has to be (win *, region_t *)
#define gen_by_val(fun) region_t fun##_by_val(win *w) { \
  region_t ret; \
  pixman_region32_init(&ret); \
  fun(w, &ret); \
  return ret; \
}

/**
 * Clear leader cache of all windows.
 */
static inline void
clear_cache_win_leaders(session_t *ps) {
  for (win *w = ps->list; w; w = w->next)
    w->cache_leader = XCB_NONE;
}

static inline void
wid_set_opacity_prop(session_t *ps, xcb_window_t wid, opacity_t val) {
  const uint32_t v = val;
  xcb_change_property(ps->c, XCB_PROP_MODE_REPLACE, wid, ps->atom_opacity,
      XCB_ATOM_CARDINAL, 32, 1, &v);
}

static inline void
wid_rm_opacity_prop(session_t *ps, xcb_window_t wid) {
  xcb_delete_property(ps->c, wid, ps->atom_opacity);
}

/**
 * Run win_update_focused() on all windows with the same leader window.
 *
 * @param leader leader window ID
 */
static inline void
group_update_focused(session_t *ps, xcb_window_t leader) {
  if (!leader)
    return;

  for (win *w = ps->list; w; w = w->next) {
    if (win_get_leader(ps, w) == leader && !w->destroying)
      win_update_focused(ps, w);
  }

  return;
}

/**
 * Return whether a window group is really focused.
 *
 * @param leader leader window ID
 * @return true if the window group is focused, false otherwise
 */
static inline bool
group_is_focused(session_t *ps, xcb_window_t leader) {
  if (!leader)
    return false;

  for (win *w = ps->list; w; w = w->next) {
    if (win_get_leader(ps, w) == leader && !w->destroying
        && win_is_focused_real(ps, w))
      return true;
  }

  return false;
}

/**
 * Get a rectangular region a window occupies, excluding shadow.
 */
static void win_get_region_local(session_t *ps, win *w, region_t *res) {
  pixman_region32_fini(res);
  pixman_region32_init_rect(res, 0, 0, w->widthb, w->heightb);
}


/**
 * Get a rectangular region a window occupies, excluding frame and shadow.
 */
void win_get_region_noframe_local(win *w, region_t *res) {
  const margin_t extents = win_calc_frame_extents(w);

  int x = extents.left;
  int y = extents.top;
  int width = max_i(w->g.width - extents.left - extents.right, 0);
  int height = max_i(w->g.height - extents.top - extents.bottom, 0);

  pixman_region32_fini(res);
  if (width > 0 && height > 0)
    pixman_region32_init_rect(res, x, y, width, height);
}

gen_by_val(win_get_region_noframe_local)

/**
 * Add a window to damaged area.
 *
 * @param ps current session
 * @param w struct _win element representing the window
 */
void add_damage_from_win(session_t *ps, win *w) {
  // XXX there was a cached extents region, investigate
  //     if that's better
  region_t extents;
  pixman_region32_init(&extents);
  win_extents(w, &extents);
  add_damage(ps, &extents);
  pixman_region32_fini(&extents);
}

/**
 * Check if a window has rounded corners.
 * XXX This is really dumb
 */
void win_rounded_corners(session_t *ps, win *w) {
  w->rounded_corners = false;

  if (!w->bounding_shaped)
    return;

  // Quit if border_size() returns XCB_NONE
  if (!pixman_region32_not_empty(&w->bounding_shape))
    return;

  // Determine the minimum width/height of a rectangle that could mark
  // a window as having rounded corners
  unsigned short minwidth = max_i(w->widthb * (1 - ROUNDED_PERCENT),
      w->widthb - ROUNDED_PIXELS);
  unsigned short minheight = max_i(w->heightb * (1 - ROUNDED_PERCENT),
      w->heightb - ROUNDED_PIXELS);

  // Get the rectangles in the bounding region
  int nrects = 0;
  const rect_t *rects = pixman_region32_rectangles(&w->bounding_shape, &nrects);

  // Look for a rectangle large enough for this window be considered
  // having rounded corners
  for (int i = 0; i < nrects; ++i)
    if (rects[i].x2 - rects[i].x1 >= minwidth && rects[i].y2 - rects[i].y1 >= minheight) {
      w->rounded_corners = true;
      break;
    }
}

int win_get_name(session_t *ps, win *w) {
  XTextProperty text_prop = { NULL, XCB_NONE, 0, 0 };
  char **strlst = NULL;
  int nstr = 0;

  if (!w->client_win)
    return 0;

  if (!(wid_get_text_prop(ps, w->client_win, ps->atom_name_ewmh, &strlst, &nstr))) {
    log_trace("(%#010x): _NET_WM_NAME unset, falling back to WM_NAME.", w->client_win);

    if (!(XGetWMName(ps->dpy, w->client_win, &text_prop) && text_prop.value)) {
      return -1;
    }
    if (Success !=
        XmbTextPropertyToTextList(ps->dpy, &text_prop, &strlst, &nstr)
        || !nstr || !strlst) {
      if (strlst)
        XFreeStringList(strlst);
      cxfree(text_prop.value);
      return -1;
    }
    cxfree(text_prop.value);
  }

  int ret = 0;
  if (!w->name || strcmp(w->name, strlst[0]) != 0) {
    ret = 1;
    free(w->name);
    w->name = strdup(strlst[0]);
  }

  XFreeStringList(strlst);

  log_trace("(%#010x): client = %#010x, name = \"%s\", "
            "ret = %d", w->id, w->client_win, w->name, ret);
  return ret;
}

int win_get_role(session_t *ps, win *w) {
  char **strlst = NULL;
  int nstr = 0;

  if (!wid_get_text_prop(ps, w->client_win, ps->atom_role, &strlst, &nstr))
    return -1;

  int ret = 0;
  if (!w->role || strcmp(w->role, strlst[0]) != 0) {
    ret = 1;
    free(w->role);
    w->role = strdup(strlst[0]);
  }

  XFreeStringList(strlst);

  log_trace("(%#010x): client = %#010x, role = \"%s\", "
            "ret = %d", w->id, w->client_win, w->role, ret);
  return ret;
}

/**
 * Check if a window is bounding-shaped.
 */
static inline bool win_bounding_shaped(const session_t *ps, xcb_window_t wid) {
  if (ps->shape_exists) {
    xcb_shape_query_extents_reply_t *reply;
    Bool bounding_shaped;

    reply = xcb_shape_query_extents_reply(ps->c,
        xcb_shape_query_extents(ps->c, wid), NULL);
    bounding_shaped = reply && reply->bounding_shaped;
    free(reply);

    return bounding_shaped;
  }

  return false;
}

wintype_t wid_get_prop_wintype(session_t *ps, xcb_window_t wid) {
  winprop_t prop = wid_get_prop(ps, wid, ps->atom_win_type, 32L, XCB_ATOM_ATOM, 32);

  for (unsigned i = 0; i < prop.nitems; ++i) {
    for (wintype_t j = 1; j < NUM_WINTYPES; ++j) {
      if (ps->atoms_wintypes[j] == (xcb_atom_t)prop.p32[i]) {
        free_winprop(&prop);
        return j;
      }
    }
  }

  free_winprop(&prop);

  return WINTYPE_UNKNOWN;
}

bool wid_get_opacity_prop(session_t *ps, xcb_window_t wid, opacity_t def,
                          opacity_t *out) {
  bool ret = false;
  *out = def;

  winprop_t prop = wid_get_prop(ps, wid, ps->atom_opacity, 1L, XCB_ATOM_CARDINAL, 32);

  if (prop.nitems) {
    *out = *prop.c32;
    ret = true;
  }

  free_winprop(&prop);

  return ret;
}

// XXX should distinguish between frame has alpha and window body has alpha
bool win_has_alpha(win *w) {
  return w->pictfmt &&
    w->pictfmt->type == XCB_RENDER_PICT_TYPE_DIRECT &&
    w->pictfmt->direct.alpha_mask;
}

void win_determine_mode(session_t *ps, win *w) {
  if (win_has_alpha(w) || w->opacity != OPAQUE) {
    w->mode = WMODE_TRANS;
  } else if (w->frame_opacity != 1.0) {
    w->mode = WMODE_FRAME_TRANS;
  } else {
    w->mode = WMODE_SOLID;
  }
}

/**
 * Calculate and set the opacity of a window.
 *
 * If window is inactive and inactive_opacity_override is set, the
 * priority is: (Simulates the old behavior)
 *
 * inactive_opacity > _NET_WM_WINDOW_OPACITY (if not opaque)
 * > window type default opacity
 *
 * Otherwise:
 *
 * _NET_WM_WINDOW_OPACITY (if not opaque)
 * > window type default opacity (if not opaque)
 * > inactive_opacity
 *
 * @param ps current session
 * @param w struct _win object representing the window
 */
void win_calc_opacity(session_t *ps, win *w) {
  opacity_t opacity = OPAQUE;

  if (w->destroying || w->a.map_state != XCB_MAP_STATE_VIEWABLE)
    opacity = 0;
  else {
    // Try obeying opacity property and window type opacity firstly
    if (w->has_opacity_prop)
      opacity = w->opacity_prop;
    else if (!safe_isnan(ps->o.wintype_option[w->window_type].opacity))
      opacity = ps->o.wintype_option[w->window_type].opacity * OPAQUE;
    else {
      // Respect active_opacity only when the window is physically focused
      if (win_is_focused_real(ps, w))
        opacity = ps->o.active_opacity;
      else if (false == w->focused)
        // Respect inactive_opacity in some cases
        opacity = ps->o.inactive_opacity;
    }

    // respect inactive override
    if (ps->o.inactive_opacity_override && false == w->focused)
      opacity = ps->o.inactive_opacity;
  }

  w->opacity_tgt = opacity;
}

/**
 * Determine whether a window is to be dimmed.
 */
void win_calc_dim(session_t *ps, win *w) {
  bool dim;

  // Make sure we do nothing if the window is unmapped / being destroyed
  if (w->destroying || w->a.map_state != XCB_MAP_STATE_VIEWABLE)
    return;

  if (ps->o.inactive_dim && !(w->focused)) {
    dim = true;
  } else {
    dim = false;
  }

  if (dim != w->dim) {
    w->dim = dim;
    add_damage_from_win(ps, w);
  }
}

/**
 * Determine if a window should fade on opacity change.
 */
void win_determine_fade(session_t *ps, win *w) {
  // To prevent it from being overwritten by last-paint value if the window is
  // unmapped on next frame, write w->fade_last as well
  if (UNSET != w->fade_force)
    w->fade_last = w->fade = w->fade_force;
  else if (ps->o.no_fading_openclose && w->in_openclose)
    w->fade_last = w->fade = false;
  else if (ps->o.no_fading_destroyed_argb && w->destroying &&
           win_has_alpha(w) && w->client_win && w->client_win != w->id) {
    w->fade_last = w->fade = false;
  }
  // Ignore other possible causes of fading state changes after window
  // gets unmapped
  else if (w->a.map_state != XCB_MAP_STATE_VIEWABLE) {
  } else if (c2_match(ps, w, ps->o.fade_blacklist, &w->cache_fblst, NULL))
    w->fade = false;
  else
    w->fade = ps->o.wintype_option[w->window_type].fade;
}

/**
 * Reread _COMPTON_SHADOW property from a window.
 *
 * The property must be set on the outermost window, usually the WM frame.
 */
void win_update_prop_shadow_raw(session_t *ps, win *w) {
  winprop_t prop =
      wid_get_prop(ps, w->id, ps->atom_compton_shadow, 1, XCB_ATOM_CARDINAL, 32);

  if (!prop.nitems) {
    w->prop_shadow = -1;
  } else {
    w->prop_shadow = *prop.c32;
  }

  free_winprop(&prop);
}

/**
 * Reread _COMPTON_SHADOW property from a window and update related
 * things.
 */
void win_update_prop_shadow(session_t *ps, win *w) {
  long attr_shadow_old = w->prop_shadow;

  win_update_prop_shadow_raw(ps, w);

  if (w->prop_shadow != attr_shadow_old)
    win_determine_shadow(ps, w);
}

void win_set_shadow(session_t *ps, win *w, bool shadow_new) {
  if (w->shadow == shadow_new)
    return;

  region_t extents;
  pixman_region32_init(&extents);
  win_extents(w, &extents);

  w->shadow = shadow_new;

  // Window extents need update on shadow state change
  // Shadow geometry currently doesn't change on shadow state change
  // calc_shadow_geometry(ps, w);
  // Mark the old extents as damaged if the shadow is removed
  if (!w->shadow)
    add_damage(ps, &extents);

  pixman_region32_clear(&extents);
  // Mark the new extents as damaged if the shadow is added
  if (w->shadow) {
    win_extents(w, &extents);
    add_damage_from_win(ps, w);
  }
  pixman_region32_fini(&extents);
}

/**
 * Determine if a window should have shadow, and update things depending
 * on shadow state.
 */
void win_determine_shadow(session_t *ps, win *w) {
  bool shadow_new = w->shadow;

  if (UNSET != w->shadow_force)
    shadow_new = w->shadow_force;
  else if (w->a.map_state == XCB_MAP_STATE_VIEWABLE)
    shadow_new = (ps->o.wintype_option[w->window_type].shadow &&
                  !c2_match(ps, w, ps->o.shadow_blacklist, &w->cache_sblst, NULL) &&
                  !(ps->o.shadow_ignore_shaped && w->bounding_shaped &&
                    !w->rounded_corners) &&
                  !(ps->o.respect_prop_shadow && 0 == w->prop_shadow));

  win_set_shadow(ps, w, shadow_new);
}

void win_set_invert_color(session_t *ps, win *w, bool invert_color_new) {
  if (w->invert_color == invert_color_new)
    return;

  w->invert_color = invert_color_new;

  add_damage_from_win(ps, w);
}

/**
 * Determine if a window should have color inverted.
 */
void win_determine_invert_color(session_t *ps, win *w) {
  bool invert_color_new = w->invert_color;

  if (UNSET != w->invert_color_force)
    invert_color_new = w->invert_color_force;
  else if (w->a.map_state == XCB_MAP_STATE_VIEWABLE)
    invert_color_new =
        c2_match(ps, w, ps->o.invert_color_list, &w->cache_ivclst, NULL);

  win_set_invert_color(ps, w, invert_color_new);
}

void win_set_blur_background(session_t *ps, win *w, bool blur_background_new) {
  if (w->blur_background == blur_background_new)
    return;

  w->blur_background = blur_background_new;

  // Only consider window damaged if it's previously painted with background
  // blurred
  if (!win_is_solid(ps, w) || (ps->o.blur_background_frame && w->frame_opacity != 1))
    add_damage_from_win(ps, w);
}

/**
 * Determine if a window should have background blurred.
 */
void win_determine_blur_background(session_t *ps, win *w) {
  if (w->a.map_state != XCB_MAP_STATE_VIEWABLE)
    return;

  bool blur_background_new =
      ps->o.blur_background &&
      !c2_match(ps, w, ps->o.blur_background_blacklist, &w->cache_bbblst, NULL);

  win_set_blur_background(ps, w, blur_background_new);
}

/**
 * Update window opacity according to opacity rules.
 */
void win_update_opacity_rule(session_t *ps, win *w) {
  if (w->a.map_state != XCB_MAP_STATE_VIEWABLE)
    return;

  opacity_t opacity = OPAQUE;
  bool is_set = false;
  void *val = NULL;
  if (c2_match(ps, w, ps->o.opacity_rules, &w->cache_oparule, &val)) {
    opacity = ((double)(long)val) / 100.0 * OPAQUE;
    is_set = true;
  }

  if (is_set == w->opacity_is_set && opacity == w->opacity_set)
    return;

  w->opacity_set = opacity;
  w->opacity_is_set = is_set;
  if (!is_set)
    wid_rm_opacity_prop(ps, w->id);
  else
    wid_set_opacity_prop(ps, w->id, opacity);
}

/**
 * Function to be called on window type changes.
 */
void win_on_wtype_change(session_t *ps, win *w) {
  win_determine_shadow(ps, w);
  win_determine_fade(ps, w);
  win_update_focused(ps, w);
  if (ps->o.invert_color_list)
    win_determine_invert_color(ps, w);
  if (ps->o.opacity_rules)
    win_update_opacity_rule(ps, w);
}

/**
 * Function to be called on window data changes.
 */
void win_on_factor_change(session_t *ps, win *w) {
  if (ps->o.shadow_blacklist)
    win_determine_shadow(ps, w);
  if (ps->o.fade_blacklist)
    win_determine_fade(ps, w);
  if (ps->o.invert_color_list)
    win_determine_invert_color(ps, w);
  if (ps->o.focus_blacklist)
    win_update_focused(ps, w);
  if (ps->o.blur_background_blacklist)
    win_determine_blur_background(ps, w);
  if (ps->o.opacity_rules)
    win_update_opacity_rule(ps, w);
  if (w->a.map_state == XCB_MAP_STATE_VIEWABLE && ps->o.paint_blacklist)
    w->paint_excluded =
        c2_match(ps, w, ps->o.paint_blacklist, &w->cache_pblst, NULL);
  if (w->a.map_state == XCB_MAP_STATE_VIEWABLE && ps->o.unredir_if_possible_blacklist)
    w->unredir_if_possible_excluded = c2_match(
        ps, w, ps->o.unredir_if_possible_blacklist, &w->cache_uipblst, NULL);
  w->reg_ignore_valid = false;
}

/**
 * Update cache data in struct _win that depends on window size.
 */
void calc_win_size(session_t *ps, win *w) {
  w->widthb = w->g.width + w->g.border_width * 2;
  w->heightb = w->g.height + w->g.border_width * 2;
  calc_shadow_geometry(ps, w);
  w->flags |= WFLAG_SIZE_CHANGE;
  // Invalidate the shadow we built
  //free_paint(ps, &w->shadow_paint);
}

/**
 * Calculate and update geometry of the shadow of a window.
 */
void calc_shadow_geometry(session_t *ps, win *w) {
  w->shadow_dx = ps->o.shadow_offset_x;
  w->shadow_dy = ps->o.shadow_offset_y;
  w->shadow_width = w->widthb + ps->o.shadow_radius * 2;
  w->shadow_height = w->heightb + ps->o.shadow_radius * 2;
}

/**
 * Update window type.
 */
void win_upd_wintype(session_t *ps, win *w) {
  const wintype_t wtype_old = w->window_type;

  // Detect window type here
  w->window_type = wid_get_prop_wintype(ps, w->client_win);

  // Conform to EWMH standard, if _NET_WM_WINDOW_TYPE is not present, take
  // override-redirect windows or windows without WM_TRANSIENT_FOR as
  // _NET_WM_WINDOW_TYPE_NORMAL, otherwise as _NET_WM_WINDOW_TYPE_DIALOG.
  if (WINTYPE_UNKNOWN == w->window_type) {
    if (w->a.override_redirect ||
        !wid_has_prop(ps, w->client_win, ps->atom_transient))
      w->window_type = WINTYPE_NORMAL;
    else
      w->window_type = WINTYPE_DIALOG;
  }

  if (w->window_type != wtype_old)
    win_on_wtype_change(ps, w);
}

/**
 * Mark a window as the client window of another.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 * @param client window ID of the client window
 */
void win_mark_client(session_t *ps, win *w, xcb_window_t client) {
  w->client_win = client;

  // If the window isn't mapped yet, stop here, as the function will be
  // called in map_win()
  if (w->a.map_state != XCB_MAP_STATE_VIEWABLE)
    return;

  xcb_change_window_attributes(ps->c, client, XCB_CW_EVENT_MASK,
      (const uint32_t[]) { determine_evmask(ps, client, WIN_EVMODE_CLIENT) });

  // Make sure the XSelectInput() requests are sent
  XFlush(ps->dpy);

  win_upd_wintype(ps, w);

  // Get frame widths. The window is in damaged area already.
  if (ps->o.frame_opacity != 1)
    win_update_frame_extents(ps, w, client);

  // Get window group
  if (ps->o.track_leader)
    win_update_leader(ps, w);

  // Get window name and class if we are tracking them
  if (ps->o.track_wdata) {
    win_get_name(ps, w);
    win_get_class(ps, w);
    win_get_role(ps, w);
  }

  // Update everything related to conditions
  win_on_factor_change(ps, w);

  // Update window focus state
  win_update_focused(ps, w);
}

/**
 * Unmark current client window of a window.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 */
void win_unmark_client(session_t *ps, win *w) {
  xcb_window_t client = w->client_win;

  w->client_win = XCB_NONE;

  // Recheck event mask
  xcb_change_window_attributes(ps->c, client, XCB_CW_EVENT_MASK,
      (const uint32_t[]) { determine_evmask(ps, client, WIN_EVMODE_UNKNOWN) });
}

/**
 * Recheck client window of a window.
 *
 * @param ps current session
 * @param w struct _win of the parent window
 */
void win_recheck_client(session_t *ps, win *w) {
  // Initialize wmwin to false
  w->wmwin = false;

  // Look for the client window

  // Always recursively look for a window with WM_STATE, as Fluxbox
  // sets override-redirect flags on all frame windows.
  xcb_window_t cw = find_client_win(ps, w->id);
  if (cw)
    log_trace("(%#010x): client %#010x", w->id, cw);
  // Set a window's client window to itself if we couldn't find a
  // client window
  if (!cw) {
    cw = w->id;
    w->wmwin = !w->a.override_redirect;
    log_trace("(%#010x): client self (%s)", w->id,
                (w->wmwin ? "wmwin" : "override-redirected"));
  }

  // Unmark the old one
  if (w->client_win && w->client_win != cw)
    win_unmark_client(ps, w);

  // Mark the new one
  win_mark_client(ps, w, cw);
}

/**
 * Free all resources in a <code>struct _win</code>.
 */
void free_win(session_t *ps, win *w) {
  // Clear active_win if it's pointing to the destroyed window
  if (w == ps->active_win)
    ps->active_win = NULL;

  // No need to call backend release_win here because
  // finish_unmap_win should've done that for us.
  assert(w->win_data == NULL);
  pixman_region32_fini(&w->bounding_shape);
  // BadDamage may be thrown if the window is destroyed
  set_ignore_cookie(ps,
      xcb_damage_destroy(ps->c, w->damage));
  rc_region_unref(&w->reg_ignore);
  free(w->name);
  free(w->class_instance);
  free(w->class_general);
  free(w->role);

  // Drop w from all prev_trans to avoid accessing freed memory in
  // repair_win()
  for (win *w2 = ps->list; w2; w2 = w2->next)
    if (w == w2->prev_trans)
      w2->prev_trans = NULL;

  free(w);
}

// TODO: probably split into win_new (in win.c) and add_win (in compton.c)
bool add_win(session_t *ps, xcb_window_t id, xcb_window_t prev) {
  static const win win_def = {
      .win_data = NULL,
      .next = NULL,
      .prev_trans = NULL,

      .id = XCB_NONE,
      .a = {},
#ifdef CONFIG_XINERAMA
      .xinerama_scr = -1,
#endif
      .pictfmt = NULL,
      .mode = WMODE_TRANS,
      .ever_damaged = false,
      .damage = XCB_NONE,
      .pixmap_damaged = false,
      .flags = 0,
      .need_configure = false,
      .queue_configure = {},
      .reg_ignore = NULL,
      .reg_ignore_valid = false,

      .widthb = 0,
      .heightb = 0,
      .state = WSTATE_UNMAPPED,
      .destroying = false,
      .bounding_shaped = false,
      .rounded_corners = false,
      .to_paint = false,
      .in_openclose = false,

      .client_win = XCB_NONE,
      .window_type = WINTYPE_UNKNOWN,
      .wmwin = false,
      .leader = XCB_NONE,
      .cache_leader = XCB_NONE,

      .focused = false,
      .focused_force = UNSET,

      .name = NULL,
      .class_instance = NULL,
      .class_general = NULL,
      .role = NULL,
      .cache_sblst = NULL,
      .cache_fblst = NULL,
      .cache_fcblst = NULL,
      .cache_ivclst = NULL,
      .cache_bbblst = NULL,
      .cache_oparule = NULL,

      .opacity = 0,
      .opacity_tgt = 0,
      .has_opacity_prop = false,
      .opacity_prop = OPAQUE,
      .opacity_is_set = false,
      .opacity_set = OPAQUE,

      .fade = false,
      .fade_force = UNSET,
      .fade_callback = NULL,

      .frame_opacity = 1.0,
      .frame_extents = MARGIN_INIT,

      .shadow = false,
      .shadow_force = UNSET,
      .shadow_opacity = 0.0,
      .shadow_dx = 0,
      .shadow_dy = 0,
      .shadow_width = 0,
      .shadow_height = 0,
      .prop_shadow = -1,

      .dim = false,

      .invert_color = false,
      .invert_color_force = UNSET,

      .blur_background = false,
  };

  // Reject overlay window and already added windows
  if (id == ps->overlay || find_win(ps, id)) {
    return false;
  }

  // Allocate and initialize the new win structure
  auto new = cmalloc(win);

  log_trace("(%#010x): %p", id, new);

  *new = win_def;
  pixman_region32_init(&new->bounding_shape);

  // Find window insertion point
  win **p = NULL;
  if (prev) {
    for (p = &ps->list; *p; p = &(*p)->next) {
      if ((*p)->id == prev && !(*p)->destroying)
        break;
    }
  } else {
    p = &ps->list;
  }

  // Fill structure
  new->id = id;

  xcb_get_window_attributes_cookie_t acookie = xcb_get_window_attributes(ps->c, id);
  xcb_get_geometry_cookie_t gcookie = xcb_get_geometry(ps->c, id);
  xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(ps->c, acookie, NULL);
  xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(ps->c, gcookie, NULL);
  if (!a || a->map_state == XCB_MAP_STATE_UNVIEWABLE) {
    // Failed to get window attributes probably means the window is gone
    // already. Unviewable means the window is already reparented
    // elsewhere.
    free(a);
    free(g);
    free(new);
    return false;
  }

  new->a = *a;
  free(a);

  if (!g) {
    free(new);
    return false;
  }

  new->g = *g;
  free(g);

  // Delay window mapping
  int map_state = new->a.map_state;
  assert(map_state == XCB_MAP_STATE_VIEWABLE || map_state == XCB_MAP_STATE_UNMAPPED);
  new->a.map_state = XCB_MAP_STATE_UNMAPPED;

  if (new->a._class == XCB_WINDOW_CLASS_INPUT_OUTPUT) {
    // Create Damage for window
    new->damage = xcb_generate_id(ps->c);
    xcb_generic_error_t *e = xcb_request_check(ps->c,
      xcb_damage_create_checked(ps->c, new->damage, id, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY));
    if (e) {
      free(e);
      free(new);
      return false;
    }
    new->pictfmt = x_get_pictform_for_visual(ps, new->a.visual);
  }

  calc_win_size(ps, new);

  log_trace("Window %#010x: %s %p", id, new->name, new->pictfmt);

  new->next = *p;
  *p = new;
  win_update_bounding_shape(ps, new);

#ifdef CONFIG_DBUS
  // Send D-Bus signal
  if (ps->o.dbus) {
    cdbus_ev_win_added(ps, new);
  }
#endif

  if (map_state == XCB_MAP_STATE_VIEWABLE) {
    map_win(ps, id);
  }

  return true;
}

/**
 * Update focused state of a window.
 */
void win_update_focused(session_t *ps, win *w) {
  if (UNSET != w->focused_force) {
    w->focused = w->focused_force;
  }
  else {
    w->focused = win_is_focused_real(ps, w);

    // Use wintype_focus, and treat WM windows and override-redirected
    // windows specially
    if (ps->o.wintype_option[w->window_type].focus
        || (ps->o.mark_wmwin_focused && w->wmwin)
        || (ps->o.mark_ovredir_focused &&
            w->id == w->client_win && !w->wmwin)
        || (w->a.map_state == XCB_MAP_STATE_VIEWABLE &&
            c2_match(ps, w, ps->o.focus_blacklist, &w->cache_fcblst, NULL)))
      w->focused = true;

    // If window grouping detection is enabled, mark the window active if
    // its group is
    if (ps->o.track_leader && ps->active_leader
        && win_get_leader(ps, w) == ps->active_leader) {
      w->focused = true;
    }
  }

  // Always recalculate the window target opacity, since some opacity-related
  // options depend on the output value of win_is_focused_real() instead of
  // w->focused
  w->flags |= WFLAG_OPCT_CHANGE;
}

/**
 * Set leader of a window.
 */
static inline void win_set_leader(session_t *ps, win *w, xcb_window_t nleader) {
  // If the leader changes
  if (w->leader != nleader) {
    xcb_window_t cache_leader_old = win_get_leader(ps, w);

    w->leader = nleader;

    // Forcefully do this to deal with the case when a child window
    // gets mapped before parent, or when the window is a waypoint
    clear_cache_win_leaders(ps);

    // Update the old and new window group and active_leader if the window
    // could affect their state.
    xcb_window_t cache_leader = win_get_leader(ps, w);
    if (win_is_focused_real(ps, w) && cache_leader_old != cache_leader) {
      ps->active_leader = cache_leader;

      group_update_focused(ps, cache_leader_old);
      group_update_focused(ps, cache_leader);
    }
    // Otherwise, at most the window itself is affected
    else {
      win_update_focused(ps, w);
    }

    // Update everything related to conditions
    win_on_factor_change(ps, w);
  }
}

/**
 * Update leader of a window.
 */
void win_update_leader(session_t *ps, win *w) {
  xcb_window_t leader = XCB_NONE;

  // Read the leader properties
  if (ps->o.detect_transient && !leader)
    leader = wid_get_prop_window(ps, w->client_win, ps->atom_transient);

  if (ps->o.detect_client_leader && !leader)
    leader = wid_get_prop_window(ps, w->client_win, ps->atom_client_leader);

  win_set_leader(ps, w, leader);

  log_trace("(%#010x): client %#010x, leader %#010x, cache %#010x",
            w->id, w->client_win, w->leader, win_get_leader(ps, w));
}

/**
 * Internal function of win_get_leader().
 */
xcb_window_t win_get_leader_raw(session_t *ps, win *w, int recursions) {
  // Rebuild the cache if needed
  if (!w->cache_leader && (w->client_win || w->leader)) {
    // Leader defaults to client window
    if (!(w->cache_leader = w->leader))
      w->cache_leader = w->client_win;

    // If the leader of this window isn't itself, look for its ancestors
    if (w->cache_leader && w->cache_leader != w->client_win) {
      win *wp = find_toplevel(ps, w->cache_leader);
      if (wp) {
        // Dead loop?
        if (recursions > WIN_GET_LEADER_MAX_RECURSION)
          return XCB_NONE;

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
bool win_get_class(session_t *ps, win *w) {
  char **strlst = NULL;
  int nstr = 0;

  // Can't do anything if there's no client window
  if (!w->client_win)
    return false;

  // Free and reset old strings
  free(w->class_instance);
  free(w->class_general);
  w->class_instance = NULL;
  w->class_general = NULL;

  // Retrieve the property string list
  if (!wid_get_text_prop(ps, w->client_win, ps->atom_class, &strlst, &nstr))
    return false;

  // Copy the strings if successful
  w->class_instance = strdup(strlst[0]);

  if (nstr > 1)
    w->class_general = strdup(strlst[1]);

  XFreeStringList(strlst);

  log_trace("(%#010x): client = %#010x, "
            "instance = \"%s\", general = \"%s\"",
            w->id, w->client_win, w->class_instance, w->class_general);

  return true;
}



/**
 * Handle window focus change.
 */
static void
win_on_focus_change(session_t *ps, win *w) {
  // If window grouping detection is enabled
  if (ps->o.track_leader) {
    xcb_window_t leader = win_get_leader(ps, w);

    // If the window gets focused, replace the old active_leader
    if (win_is_focused_real(ps, w) && leader != ps->active_leader) {
      xcb_window_t active_leader_old = ps->active_leader;

      ps->active_leader = leader;

      group_update_focused(ps, active_leader_old);
      group_update_focused(ps, leader);
    }
    // If the group get unfocused, remove it from active_leader
    else if (!win_is_focused_real(ps, w) && leader && leader == ps->active_leader
        && !group_is_focused(ps, leader)) {
      ps->active_leader = XCB_NONE;
      group_update_focused(ps, leader);
    }

    // The window itself must be updated anyway
    win_update_focused(ps, w);
  }
  // Otherwise, only update the window itself
  else {
    win_update_focused(ps, w);
  }

  // Update everything related to conditions
  win_on_factor_change(ps, w);

#ifdef CONFIG_DBUS
  // Send D-Bus signal
  if (ps->o.dbus) {
    if (win_is_focused_real(ps, w))
      cdbus_ev_win_focusin(ps, w);
    else
      cdbus_ev_win_focusout(ps, w);
  }
#endif
}

/**
 * Set real focused state of a window.
 */
void
win_set_focused(session_t *ps, win *w, bool focused) {
  // Unmapped windows will have their focused state reset on map
  if (w->a.map_state == XCB_MAP_STATE_UNMAPPED)
    return;

  if (win_is_focused_real(ps, w) == focused) return;

  if (focused) {
    if (ps->active_win)
      win_set_focused(ps, ps->active_win, false);
    ps->active_win = w;
  }
  else if (w == ps->active_win)
    ps->active_win = NULL;

  assert(win_is_focused_real(ps, w) == focused);

  win_on_focus_change(ps, w);
}

/**
 * Get a rectangular region a window (and possibly its shadow) occupies.
 *
 * Note w->shadow and shadow geometry must be correct before calling this
 * function.
 */
void win_extents(win *w, region_t *res) {
  pixman_region32_clear(res);
  pixman_region32_union_rect(res, res, w->g.x, w->g.y, w->widthb, w->heightb);

  if (w->shadow)
    pixman_region32_union_rect(res, res, w->g.x + w->shadow_dx,
      w->g.y + w->shadow_dy, w->shadow_width, w->shadow_height);
}

gen_by_val(win_extents)

/**
 * Update the out-dated bounding shape of a window.
 *
 * Mark the window shape as updated
 */
void win_update_bounding_shape(session_t *ps, win *w) {
  if (ps->shape_exists)
    w->bounding_shaped = win_bounding_shaped(ps, w->id);

  pixman_region32_clear(&w->bounding_shape);
  // Start with the window rectangular region
  win_get_region_local(ps, w, &w->bounding_shape);

  // Only request for a bounding region if the window is shaped
  // (while loop is used to avoid goto, not an actual loop)
  while (w->bounding_shaped) {
    /*
     * if window doesn't exist anymore,  this will generate an error
     * as well as not generate a region.
     */

    xcb_shape_get_rectangles_reply_t *r = xcb_shape_get_rectangles_reply(ps->c,
        xcb_shape_get_rectangles(ps->c, w->id, XCB_SHAPE_SK_BOUNDING), NULL);

    if (!r)
      break;

    xcb_rectangle_t *xrects = xcb_shape_get_rectangles_rectangles(r);
    int nrects = xcb_shape_get_rectangles_rectangles_length(r);
    rect_t *rects = from_x_rects(nrects, xrects);
    free(r);

    region_t br;
    pixman_region32_init_rects(&br, rects, nrects);
    free(rects);

    // Add border width because we are using a different origin.
    // X thinks the top left of the inner window is the origin,
    // We think the top left of the border is the origin
    pixman_region32_translate(&br, w->g.border_width, w->g.border_width);

    // Intersect the bounding region we got with the window rectangle, to
    // make sure the bounding region is not bigger than the window
    // rectangle
    pixman_region32_intersect(&w->bounding_shape, &w->bounding_shape, &br);
    pixman_region32_fini(&br);
    break;
  }

  if (w->bounding_shaped && ps->o.detect_rounded_corners)
    win_rounded_corners(ps, w);

  // Window shape changed, we should free win_data
  if (ps->redirected && w->state == WSTATE_MAPPED) {
    // Note we only do this when screen is redirected, because
    // otherwise win_data is not valid
    backend_info_t *bi = backend_list[ps->o.backend];
    bi->release_win(ps->backend_data, ps, w, w->win_data);
    w->win_data = bi->prepare_win(ps->backend_data, ps, w);
    //log_trace("free out dated pict");
  }

  win_on_factor_change(ps, w);
}

/**
 * Reread opacity property of a window.
 */
void win_update_opacity_prop(session_t *ps, win *w) {
  // get frame opacity first
  w->has_opacity_prop =
    wid_get_opacity_prop(ps, w->id, OPAQUE, &w->opacity_prop);

  if (w->has_opacity_prop)
    // opacity found
    return;

  if (ps->o.detect_client_opacity && w->client_win && w->id == w->client_win)
    // checking client opacity not allowed
    return;

  // get client opacity
  w->has_opacity_prop =
    wid_get_opacity_prop(ps, w->client_win, OPAQUE, &w->opacity_prop);
}

/**
 * Retrieve frame extents from a window.
 */
void
win_update_frame_extents(session_t *ps, win *w, xcb_window_t client) {
  winprop_t prop = wid_get_prop(ps, client, ps->atom_frame_extents,
    4L, XCB_ATOM_CARDINAL, 32);

  if (prop.nitems == 4) {
    const uint32_t * const extents = prop.c32;
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
    if (ps->o.frame_opacity == 1 && changed)
      w->reg_ignore_valid = false;
  }

  log_trace("(%#010x): %d, %d, %d, %d", w->id,
      w->frame_extents.left, w->frame_extents.right,
      w->frame_extents.top, w->frame_extents.bottom);

  free_winprop(&prop);
}

bool win_is_region_ignore_valid(session_t *ps, win *w) {
  for(win *i = ps->list; w; w = w->next) {
    if (i == w)
      break;
    if (!i->reg_ignore_valid)
      return false;
  }
  return true;
}

/**
 * Stop listening for events on a particular window.
 */
void win_ev_stop(session_t *ps, win *w) {
  // Will get BadWindow if the window is destroyed
  set_ignore_cookie(ps,
      xcb_change_window_attributes(ps->c, w->id, XCB_CW_EVENT_MASK, (const uint32_t[]) { 0 }));

  if (w->client_win) {
    set_ignore_cookie(ps,
        xcb_change_window_attributes(ps->c, w->client_win, XCB_CW_EVENT_MASK, (const uint32_t[]) { 0 }));
  }

  if (ps->shape_exists) {
    set_ignore_cookie(ps,
        xcb_shape_select_input(ps->c, w->id, 0));
  }
}

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
    void (*callback) (session_t *ps, win **w), bool exec_callback) {
  win *w = *_w;
  void (*old_callback) (session_t *ps, win **w) = w->fade_callback;

  w->fade_callback = callback;
  // Must be the last line as the callback could destroy w!
  if (exec_callback && old_callback)
    old_callback(ps, _w);
}

/**
 * Execute fade callback of a window if fading finished.
 */
void
win_check_fade_finished(session_t *ps, win **_w) {
  win *w = *_w;
  if (w->fade_callback && w->opacity == w->opacity_tgt) {
    // Must be the last line as the callback could destroy w!
    win_set_fade_callback(ps, _w, NULL, true);
  }
}
