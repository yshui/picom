#include <X11/Xlib.h>
#include <xcb/render.h>
#include <xcb/damage.h>
#include <xcb/xcb_renderutil.h>
#include <stdbool.h>
#include <math.h>

#include "common.h"
#include "compton.h"
#include "c2.h"
#include "x.h"

#include "win.h"

/**
 * Clear leader cache of all windows.
 */
static inline void
clear_cache_win_leaders(session_t *ps) {
  for (win *w = ps->list; w; w = w->next)
    w->cache_leader = None;
}

static inline void
wid_set_opacity_prop(session_t *ps, Window wid, opacity_t val) {
  const unsigned long v = val;
  XChangeProperty(ps->dpy, wid, ps->atom_opacity, XA_CARDINAL, 32,
      PropModeReplace, (unsigned char *) &v, 1);
}

static inline void
wid_rm_opacity_prop(session_t *ps, Window wid) {
  XDeleteProperty(ps->dpy, wid, ps->atom_opacity);
}

/**
 * Run win_update_focused() on all windows with the same leader window.
 *
 * @param leader leader window ID
 */
static inline void
group_update_focused(session_t *ps, Window leader) {
  if (!leader)
    return;

  for (win *w = ps->list; w; w = w->next) {
    if (win_get_leader(ps, w) == leader && !w->destroyed)
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
group_is_focused(session_t *ps, Window leader) {
  if (!leader)
    return false;

  for (win *w = ps->list; w; w = w->next) {
    if (win_get_leader(ps, w) == leader && !w->destroyed
        && win_is_focused_real(ps, w))
      return true;
  }

  return false;
}

/**
 * Get a rectangular region a window occupies, excluding shadow.
 */
void win_get_region(session_t *ps, win *w, bool global, region_t *res) {
  pixman_region32_fini(res);
  pixman_region32_init_rect(res,
      global ? w->g.x : 0,
      global ? w->g.y : 0,
      w->widthb, w->heightb);
}


/**
 * Get a rectangular region a window occupies, excluding frame and shadow.
 */
void win_get_region_noframe(session_t *ps, win *w, bool global, region_t *res) {
  const margin_t extents = win_calc_frame_extents(ps, w);

  int x = (global ? w->g.x: 0) + extents.left;
  int y = (global ? w->g.y: 0) + extents.top;
  int width = max_i(w->g.width - extents.left - extents.right, 0);
  int height = max_i(w->g.height - extents.top - extents.bottom, 0);

  pixman_region32_fini(res);
  if (width > 0 && height > 0)
    pixman_region32_init_rect(res, x, y, width, height);
}

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

  // Fetch its bounding region
  if (!pixman_region32_not_empty(&w->bounding_shape))
    win_update_bounding_shape(ps, w);

  // Quit if border_size() returns None
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
  XTextProperty text_prop = { NULL, None, 0, 0 };
  char **strlst = NULL;
  int nstr = 0;

  if (!w->client_win)
    return 0;

  if (!(wid_get_text_prop(ps, w->client_win, ps->atom_name_ewmh, &strlst, &nstr))) {
#ifdef DEBUG_WINDATA
    printf_dbgf("(%#010lx): _NET_WM_NAME unset, falling back to WM_NAME.\n", wid);
#endif

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
    w->name = mstrcpy(strlst[0]);
  }

  XFreeStringList(strlst);

#ifdef DEBUG_WINDATA
  printf_dbgf("(%#010lx): client = %#010lx, name = \"%s\", "
      "ret = %d\n", w->id, w->client_win, w->name, ret);
#endif
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
    w->role = mstrcpy(strlst[0]);
  }

  XFreeStringList(strlst);

#ifdef DEBUG_WINDATA
  printf_dbgf("(%#010lx): client = %#010lx, role = \"%s\", "
      "ret = %d\n", w->id, w->client_win, w->role, ret);
#endif
  return ret;
}

/**
 * Check if a window is bounding-shaped.
 */
static inline bool win_bounding_shaped(const session_t *ps, Window wid) {
  if (ps->shape_exists) {
    xcb_shape_query_extents_reply_t *reply;
    Bool bounding_shaped;
    xcb_connection_t *c = XGetXCBConnection(ps->dpy);

    reply = xcb_shape_query_extents_reply(c,
        xcb_shape_query_extents(c, wid), NULL);
    bounding_shaped = reply && reply->bounding_shaped;
    free(reply);

    return bounding_shaped;
  }

  return false;
}

wintype_t wid_get_prop_wintype(session_t *ps, Window wid) {
  set_ignore_next(ps);
  winprop_t prop = wid_get_prop(ps, wid, ps->atom_win_type, 32L, XA_ATOM, 32);

  for (unsigned i = 0; i < prop.nitems; ++i) {
    for (wintype_t j = 1; j < NUM_WINTYPES; ++j) {
      if (ps->atoms_wintypes[j] == (Atom)prop.data.p32[i]) {
        free_winprop(&prop);
        return j;
      }
    }
  }

  free_winprop(&prop);

  return WINTYPE_UNKNOWN;
}

bool wid_get_opacity_prop(session_t *ps, Window wid, opacity_t def,
                          opacity_t *out) {
  bool ret = false;
  *out = def;

  winprop_t prop = wid_get_prop(ps, wid, ps->atom_opacity, 1L, XA_CARDINAL, 32);

  if (prop.nitems) {
    *out = *prop.data.p32;
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

  if (w->destroyed || IsViewable != w->a.map_state)
    opacity = 0;
  else {
    // Try obeying opacity property and window type opacity firstly
    if (w->has_opacity_prop)
      opacity = w->opacity_prop;
    else if (!isnan(ps->o.wintype_opacity[w->window_type]))
      opacity = ps->o.wintype_opacity[w->window_type] * OPAQUE;
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

  // Make sure we do nothing if the window is unmapped / destroyed
  if (w->destroyed || IsViewable != w->a.map_state)
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
  else if (ps->o.no_fading_destroyed_argb && w->destroyed &&
           win_has_alpha(w) && w->client_win && w->client_win != w->id) {
    w->fade_last = w->fade = false;
  }
  // Ignore other possible causes of fading state changes after window
  // gets unmapped
  else if (IsViewable != w->a.map_state) {
  } else if (c2_match(ps, w, ps->o.fade_blacklist, &w->cache_fblst))
    w->fade = false;
  else
    w->fade = ps->o.wintype_fade[w->window_type];
}

/**
 * Reread _COMPTON_SHADOW property from a window.
 *
 * The property must be set on the outermost window, usually the WM frame.
 */
void win_update_prop_shadow_raw(session_t *ps, win *w) {
  winprop_t prop =
      wid_get_prop(ps, w->id, ps->atom_compton_shadow, 1, XA_CARDINAL, 32);

  if (!prop.nitems) {
    w->prop_shadow = -1;
  } else {
    w->prop_shadow = *prop.data.p32;
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
  else if (IsViewable == w->a.map_state)
    shadow_new = (ps->o.wintype_shadow[w->window_type] &&
                  !c2_match(ps, w, ps->o.shadow_blacklist, &w->cache_sblst) &&
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
  else if (IsViewable == w->a.map_state)
    invert_color_new =
        c2_match(ps, w, ps->o.invert_color_list, &w->cache_ivclst);

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
  if (IsViewable != w->a.map_state)
    return;

  bool blur_background_new =
      ps->o.blur_background &&
      !c2_match(ps, w, ps->o.blur_background_blacklist, &w->cache_bbblst);

  win_set_blur_background(ps, w, blur_background_new);
}

/**
 * Update window opacity according to opacity rules.
 */
void win_update_opacity_rule(session_t *ps, win *w) {
  if (IsViewable != w->a.map_state)
    return;

  opacity_t opacity = OPAQUE;
  bool is_set = false;
  void *val = NULL;
  if (c2_matchd(ps, w, ps->o.opacity_rules, &w->cache_oparule, &val)) {
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
  if (IsViewable == w->a.map_state && ps->o.paint_blacklist)
    w->paint_excluded =
        c2_match(ps, w, ps->o.paint_blacklist, &w->cache_pblst);
  if (IsViewable == w->a.map_state && ps->o.unredir_if_possible_blacklist)
    w->unredir_if_possible_excluded = c2_match(
        ps, w, ps->o.unredir_if_possible_blacklist, &w->cache_uipblst);
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
  free_paint(ps, &w->shadow_paint);
}

/**
 * Calculate and update geometry of the shadow of a window.
 */
void calc_shadow_geometry(session_t *ps, win *w) {
  w->shadow_dx = ps->o.shadow_offset_x;
  w->shadow_dy = ps->o.shadow_offset_y;
  w->shadow_width = w->widthb + ps->gaussian_map->size;
  w->shadow_height = w->heightb + ps->gaussian_map->size;
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
void win_mark_client(session_t *ps, win *w, Window client) {
  w->client_win = client;

  // If the window isn't mapped yet, stop here, as the function will be
  // called in map_win()
  if (IsViewable != w->a.map_state)
    return;

  XSelectInput(ps->dpy, client,
               determine_evmask(ps, client, WIN_EVMODE_CLIENT));

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
  Window client = w->client_win;

  w->client_win = None;

  // Recheck event mask
  XSelectInput(ps->dpy, client,
               determine_evmask(ps, client, WIN_EVMODE_UNKNOWN));
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
  Window cw = find_client_win(ps, w->id);
#ifdef DEBUG_CLIENTWIN
  if (cw)
    printf_dbgf("(%#010lx): client %#010lx\n", w->id, cw);
#endif
  // Set a window's client window to itself if we couldn't find a
  // client window
  if (!cw) {
    cw = w->id;
    w->wmwin = !w->a.override_redirect;
#ifdef DEBUG_CLIENTWIN
    printf_dbgf("(%#010lx): client self (%s)\n", w->id,
                (w->wmwin ? "wmwin" : "override-redirected"));
#endif
  }

  // Unmark the old one
  if (w->client_win && w->client_win != cw)
    win_unmark_client(ps, w);

  // Mark the new one
  win_mark_client(ps, w, cw);
}

// TODO: probably split into win_new (in win.c) and add_win (in compton.c)
bool add_win(session_t *ps, Window id, Window prev) {
  static const win win_def = {
      .next = NULL,
      .prev_trans = NULL,

      .id = None,
      .a = {},
#ifdef CONFIG_XINERAMA
      .xinerama_scr = -1,
#endif
      .pictfmt = NULL,
      .mode = WMODE_TRANS,
      .ever_damaged = false,
      .damage = None,
      .pixmap_damaged = false,
      .paint = PAINT_INIT,
      .flags = 0,
      .need_configure = false,
      .queue_configure = {},
      .reg_ignore = NULL,
      .reg_ignore_valid = false,

      .widthb = 0,
      .heightb = 0,
      .destroyed = false,
      .bounding_shaped = false,
      .rounded_corners = false,
      .to_paint = false,
      .in_openclose = false,

      .client_win = None,
      .window_type = WINTYPE_UNKNOWN,
      .wmwin = false,
      .leader = None,
      .cache_leader = None,

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
      .shadow_paint = PAINT_INIT,
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
  win *new = malloc(sizeof(win));

#ifdef DEBUG_EVENTS
  printf_dbgf("(%#010lx): %p\n", id, new);
#endif

  if (!new) {
    printf_errf("(%#010lx): Failed to allocate memory for the new window.", id);
    return false;
  }

  *new = win_def;
  pixman_region32_init(&new->bounding_shape);

  // Find window insertion point
  win **p = NULL;
  if (prev) {
    for (p = &ps->list; *p; p = &(*p)->next) {
      if ((*p)->id == prev && !(*p)->destroyed)
        break;
    }
  } else {
    p = &ps->list;
  }

  // Fill structure
  new->id = id;

  xcb_connection_t *c = XGetXCBConnection(ps->dpy);
  xcb_get_window_attributes_cookie_t acookie = xcb_get_window_attributes(c, id);
  xcb_get_geometry_cookie_t gcookie = xcb_get_geometry(c, id);
  xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(c, acookie, NULL);
  xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, gcookie, NULL);
  if (!a || IsUnviewable == a->map_state) {
    // Failed to get window attributes probably means the window is gone
    // already. IsUnviewable means the window is already reparented
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
  assert(IsViewable == map_state || IsUnmapped == map_state);
  new->a.map_state = IsUnmapped;

  if (InputOutput == new->a._class) {
    // Create Damage for window
    new->damage = xcb_generate_id(c);
    xcb_generic_error_t *e = xcb_request_check(c,
      xcb_damage_create_checked(c, new->damage, id, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY));
    if (e) {
      free(e);
      free(new);
      return false;
    }
    new->pictfmt = x_get_pictform_for_visual(ps, new->a.visual);
  }

  calc_win_size(ps, new);

  new->next = *p;
  *p = new;

#ifdef CONFIG_DBUS
  // Send D-Bus signal
  if (ps->o.dbus) {
    cdbus_ev_win_added(ps, new);
  }
#endif

  if (IsViewable == map_state) {
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
    if (ps->o.wintype_focus[w->window_type]
        || (ps->o.mark_wmwin_focused && w->wmwin)
        || (ps->o.mark_ovredir_focused
          && w->id == w->client_win && !w->wmwin)
        || (IsViewable == w->a.map_state && c2_match(ps, w, ps->o.focus_blacklist, &w->cache_fcblst)))
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
static inline void win_set_leader(session_t *ps, win *w, Window nleader) {
  // If the leader changes
  if (w->leader != nleader) {
    Window cache_leader_old = win_get_leader(ps, w);

    w->leader = nleader;

    // Forcefully do this to deal with the case when a child window
    // gets mapped before parent, or when the window is a waypoint
    clear_cache_win_leaders(ps);

    // Update the old and new window group and active_leader if the window
    // could affect their state.
    Window cache_leader = win_get_leader(ps, w);
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
  Window leader = None;

  // Read the leader properties
  if (ps->o.detect_transient && !leader)
    leader = wid_get_prop_window(ps, w->client_win, ps->atom_transient);

  if (ps->o.detect_client_leader && !leader)
    leader = wid_get_prop_window(ps, w->client_win, ps->atom_client_leader);

  win_set_leader(ps, w, leader);

#ifdef DEBUG_LEADER
  printf_dbgf("(%#010lx): client %#010lx, leader %#010lx, cache %#010lx\n", w->id, w->client_win, w->leader, win_get_leader(ps, w));
#endif
}

/**
 * Internal function of win_get_leader().
 */
Window win_get_leader_raw(session_t *ps, win *w, int recursions) {
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
          return None;

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
  w->class_instance = mstrcpy(strlst[0]);

  if (nstr > 1)
    w->class_general = mstrcpy(strlst[1]);

  XFreeStringList(strlst);

#ifdef DEBUG_WINDATA
  printf_dbgf("(%#010lx): client = %#010lx, "
      "instance = \"%s\", general = \"%s\"\n",
      w->id, w->client_win, w->class_instance, w->class_general);
#endif

  return true;
}



/**
 * Handle window focus change.
 */
static void
win_on_focus_change(session_t *ps, win *w) {
  // If window grouping detection is enabled
  if (ps->o.track_leader) {
    Window leader = win_get_leader(ps, w);

    // If the window gets focused, replace the old active_leader
    if (win_is_focused_real(ps, w) && leader != ps->active_leader) {
      Window active_leader_old = ps->active_leader;

      ps->active_leader = leader;

      group_update_focused(ps, active_leader_old);
      group_update_focused(ps, leader);
    }
    // If the group get unfocused, remove it from active_leader
    else if (!win_is_focused_real(ps, w) && leader && leader == ps->active_leader
        && !group_is_focused(ps, leader)) {
      ps->active_leader = None;
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
  if (IsUnmapped == w->a.map_state)
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
    pixman_region32_union_rect(res, res, w->g.x + w->shadow_dx, w->g.y + w->shadow_dy, w->shadow_width, w->shadow_height);
}

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
  win_get_region(ps, w, true, &w->bounding_shape);

  // Only request for a bounding region if the window is shaped
  if (w->bounding_shaped) {
    /*
     * if window doesn't exist anymore,  this will generate an error
     * as well as not generate a region.
     */

    xcb_connection_t *c = XGetXCBConnection(ps->dpy);
    xcb_shape_get_rectangles_reply_t *r = xcb_shape_get_rectangles_reply(c,
        xcb_shape_get_rectangles(c, w->id, XCB_SHAPE_SK_BOUNDING), NULL);

    if (!r)
      return;

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
    pixman_region32_translate(&br, w->g.x + w->g.border_width,
      w->g.y + w->g.border_width);

    // Intersect the bounding region we got with the window rectangle, to
    // make sure the bounding region is not bigger than the window
    // rectangle
    pixman_region32_intersect(&w->bounding_shape, &w->bounding_shape, &br);
    pixman_region32_fini(&br);
  }

  if (w->bounding_shaped && ps->o.detect_rounded_corners)
    win_rounded_corners(ps, w);

  // XXX Window shape changed, and if we didn't fill in the pixels
  // behind the window (not implemented yet), we should rebuild
  // the shadow_pict
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
win_update_frame_extents(session_t *ps, win *w, Window client) {
  winprop_t prop = wid_get_prop(ps, client, ps->atom_frame_extents,
    4L, XA_CARDINAL, 32);

  if (prop.nitems == 4) {
    const long * const extents = prop.data.p32;
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

#ifdef DEBUG_FRAME
  printf_dbgf("(%#010lx): %d, %d, %d, %d\n", w->id,
      w->frame_extents.left, w->frame_extents.right,
      w->frame_extents.top, w->frame_extents.bottom);
#endif

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
