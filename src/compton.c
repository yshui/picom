// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE-mit for more information.
 *
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xlibint.h>
#include <X11/extensions/sync.h>
#include <xcb/randr.h>
#include <xcb/present.h>
#include <xcb/damage.h>
#include <xcb/render.h>
#include <xcb/xfixes.h>
#include <xcb/sync.h>
#include <xcb/composite.h>

#include <ev.h>

#include "err.h"
#include "kernel.h"
#include "common.h"
#include "compiler.h"
#include "compton.h"
#ifdef CONFIG_OPENGL
#include "opengl.h"
#endif
#include "win.h"
#include "x.h"
#include "config.h"
#include "diagnostic.h"
#include "render.h"
#include "utils.h"
#include "region.h"
#include "types.h"
#include "c2.h"
#include "log.h"
#ifdef CONFIG_DBUS
#include "dbus.h"
#endif
#include "options.h"

#define CASESTRRET(s)   case s: return #s

/// Get session_t pointer from a pointer to a member of session_t
#define session_ptr(ptr, member) ({ \
  const __typeof__( ((session_t *)0)->member ) *__mptr = (ptr); \
  (session_t *)((char *)__mptr - offsetof(session_t, member)); \
})

static void
update_refresh_rate(session_t *ps);

static bool
swopti_init(session_t *ps);

static void
cxinerama_upd_scrs(session_t *ps);

static void
session_destroy(session_t *ps);

#ifdef CONFIG_XINERAMA
static void
cxinerama_upd_scrs(session_t *ps);
#endif

static void
redir_start(session_t *ps);

static void
redir_stop(session_t *ps);

static win *
recheck_focus(session_t *ps);

static void
restack_win(session_t *ps, win *w, xcb_window_t new_above);

static void
update_ewmh_active_win(session_t *ps);

static void
draw_callback(EV_P_ ev_idle *w, int revents);

// === Global constants ===

/// Name strings for window types.
const char * const WINTYPES[NUM_WINTYPES] = {
  "unknown",
  "desktop",
  "dock",
  "toolbar",
  "menu",
  "utility",
  "splash",
  "dialog",
  "normal",
  "dropdown_menu",
  "popup_menu",
  "tooltip",
  "notify",
  "combo",
  "dnd",
};

/// Names of VSync modes.
const char * const VSYNC_STRS[NUM_VSYNC + 1] = {
  "none",             // VSYNC_NONE
  "drm",              // VSYNC_DRM
  "opengl",           // VSYNC_OPENGL
  "opengl-oml",       // VSYNC_OPENGL_OML
  "opengl-swc",       // VSYNC_OPENGL_SWC
  "opengl-mswc",      // VSYNC_OPENGL_MSWC
  NULL
};

/// Names of backends.
const char * const BACKEND_STRS[NUM_BKEND + 1] = {
  "xrender",      // BKEND_XRENDER
  "glx",          // BKEND_GLX
  "xr_glx_hybrid",// BKEND_XR_GLX_HYBRID
  NULL
};

// === Global variables ===

/// Pointer to current session, as a global variable. Only used by
/// xerror(), which could not have a pointer to current session passed in.
/// XXX Limit what xerror can access by not having this pointer
session_t *ps_g = NULL;

/**
 * Free Xinerama screen info.
 *
 * XXX consider moving to x.c
 */
static inline void
free_xinerama_info(session_t *ps) {
#ifdef CONFIG_XINERAMA
  if (ps->xinerama_scr_regs) {
    for (int i = 0; i < ps->xinerama_nscrs; ++i)
      pixman_region32_fini(&ps->xinerama_scr_regs[i]);
    free(ps->xinerama_scr_regs);
  }
  cxfree(ps->xinerama_scrs);
  ps->xinerama_scrs = NULL;
  ps->xinerama_nscrs = 0;
#endif
}

/**
 * Get current system clock in milliseconds.
 */
static inline unsigned long
get_time_ms(void) {
  struct timeval tv;

  gettimeofday(&tv, NULL);

  return tv.tv_sec % SEC_WRAP * 1000 + tv.tv_usec / 1000;
}

// XXX Move to x.c
static void
cxinerama_upd_scrs(session_t *ps) {
#ifdef CONFIG_XINERAMA
  // XXX Consider deprecating Xinerama, switch to RandR when necessary
  free_xinerama_info(ps);

  if (!ps->o.xinerama_shadow_crop || !ps->xinerama_exists) return;

  xcb_xinerama_is_active_reply_t *active =
    xcb_xinerama_is_active_reply(ps->c,
        xcb_xinerama_is_active(ps->c), NULL);
  if (!active || !active->state) {
    free(active);
    return;
  }
  free(active);

  ps->xinerama_scrs = xcb_xinerama_query_screens_reply(ps->c,
      xcb_xinerama_query_screens(ps->c), NULL);
  if (!ps->xinerama_scrs)
    return;

  xcb_xinerama_screen_info_t *scrs = xcb_xinerama_query_screens_screen_info(ps->xinerama_scrs);
  ps->xinerama_nscrs = xcb_xinerama_query_screens_screen_info_length(ps->xinerama_scrs);

  ps->xinerama_scr_regs = ccalloc(ps->xinerama_nscrs, region_t);
  for (int i = 0; i < ps->xinerama_nscrs; ++i) {
    const xcb_xinerama_screen_info_t * const s = &scrs[i];
    pixman_region32_init_rect(&ps->xinerama_scr_regs[i], s->x_org, s->y_org, s->width, s->height);
  }
#endif
}

/**
 * Find matched window.
 *
 * XXX move to win.c
 */
static inline win *
find_win_all(session_t *ps, const xcb_window_t wid) {
  if (!wid || PointerRoot == wid || wid == ps->root || wid == ps->overlay)
    return NULL;

  win *w = find_win(ps, wid);
  if (!w) w = find_toplevel(ps, wid);
  if (!w) w = find_toplevel2(ps, wid);
  return w;
}

void queue_redraw(session_t *ps) {
  // If --benchmark is used, redraw is always queued
  if (!ps->redraw_needed && !ps->o.benchmark)
    ev_idle_start(ps->loop, &ps->draw_idle);
  ps->redraw_needed = true;
}

/**
 * Get a region of the screen size.
 */
static inline void
get_screen_region(session_t *ps, region_t *res) {
  pixman_box32_t b = {
    .x1 = 0, .y1 = 0,
    .x2 = ps->root_width,
    .y2 = ps->root_height
  };
  pixman_region32_fini(res);
  pixman_region32_init_rects(res, &b, 1);
}

void add_damage(session_t *ps, const region_t *damage) {
  // Ignore damage when screen isn't redirected
  if (!ps->redirected)
    return;

  if (!damage)
    return;
  pixman_region32_union(ps->damage, ps->damage, (region_t *)damage);
}

// === Fading ===

/**
 * Get the time left before next fading point.
 *
 * In milliseconds.
 */
static double
fade_timeout(session_t *ps) {
  auto now = get_time_ms();
  if (ps->o.fade_delta + ps->fade_time < now)
    return 0;

  int diff = ps->o.fade_delta + ps->fade_time - now;

  diff = normalize_i_range(diff, 0, ps->o.fade_delta * 2);

  return diff / 1000.0;
}

/**
 * Run fading on a window.
 *
 * @param steps steps of fading
 * @return whether we are still in fading mode
 */
static bool
run_fade(session_t *ps, win **_w, unsigned steps) {
  win *w = *_w;
  if (w->state == WSTATE_MAPPED || w->state == WSTATE_UNMAPPED) {
    // We are not fading
    assert(w->opacity_tgt == w->opacity);
    return false;
  }

  if (!win_should_fade(ps, w)) {
    log_debug("Window %#010x %s doesn't need fading", w->id, w->name);
    w->opacity = w->opacity_tgt;
  }
  if (w->opacity == w->opacity_tgt) {
    // We have reached target opacity, wrapping up.
    // Note, we reach here after we have rendered the window with the target
    // opacity at least once. If the window is destroyed because it is faded out,
    // there is no need to add damage here.
    log_debug("Fading finished for window %#010x %s", w->id, w->name);
    win_check_fade_finished(ps, _w);
    return false;
  }

  if (steps) {
    if (w->opacity < w->opacity_tgt) {
      w->opacity = normalize_d_range(
          w->opacity + ps->o.fade_in_step * steps,
          0.0, w->opacity_tgt);
    } else {
      w->opacity = normalize_d_range(
          w->opacity - ps->o.fade_out_step * steps,
          w->opacity_tgt, 1);
    }
  }

  // Note even if opacity == opacity_tgt, we still want to run preprocess one last
  // time to finish state transition. So return true in that case too.
  return true;
}

// === Error handling ===

static void
discard_ignore(session_t *ps, unsigned long sequence) {
  while (ps->ignore_head) {
    if (sequence > ps->ignore_head->sequence) {
      ignore_t *next = ps->ignore_head->next;
      free(ps->ignore_head);
      ps->ignore_head = next;
      if (!ps->ignore_head) {
        ps->ignore_tail = &ps->ignore_head;
      }
    } else {
      break;
    }
  }
}

static int
should_ignore(session_t *ps, unsigned long sequence) {
  discard_ignore(ps, sequence);
  return ps->ignore_head && ps->ignore_head->sequence == sequence;
}

// === Windows ===

/**
 * Determine the event mask for a window.
 */
long determine_evmask(session_t *ps, xcb_window_t wid, win_evmode_t mode) {
  long evmask = 0;
  win *w = NULL;

  // Check if it's a mapped frame window
  if (WIN_EVMODE_FRAME == mode
      || ((w = find_win(ps, wid)) && w->a.map_state == XCB_MAP_STATE_VIEWABLE)) {
    evmask |= XCB_EVENT_MASK_PROPERTY_CHANGE;
    if (ps->o.track_focus && !ps->o.use_ewmh_active_win)
      evmask |= XCB_EVENT_MASK_FOCUS_CHANGE;
  }

  // Check if it's a mapped client window
  if (WIN_EVMODE_CLIENT == mode
      || ((w = find_toplevel(ps, wid)) && w->a.map_state == XCB_MAP_STATE_VIEWABLE)) {
    if (ps->o.frame_opacity || ps->o.track_wdata || ps->track_atom_lst
        || ps->o.detect_client_opacity)
      evmask |= XCB_EVENT_MASK_PROPERTY_CHANGE;
  }

  return evmask;
}

/**
 * Find out the WM frame of a client window by querying X.
 *
 * @param ps current session
 * @param wid window ID
 * @return struct _win object of the found window, NULL if not found
 */
win *find_toplevel2(session_t *ps, xcb_window_t wid) {
  // TODO this should probably be an "update tree", then find_toplevel.
  //      current approach is a bit more "racy"
  win *w = NULL;

  // We traverse through its ancestors to find out the frame
  while (wid && wid != ps->root && !(w = find_win(ps, wid))) {
    xcb_query_tree_reply_t *reply;

    // xcb_query_tree probably fails if you run compton when X is somehow
    // initializing (like add it in .xinitrc). In this case
    // just leave it alone.
    reply = xcb_query_tree_reply(ps->c, xcb_query_tree(ps->c, wid), NULL);
    if (reply == NULL) {
      break;
    }

    wid = reply->parent;

    free(reply);
  }

  return w;
}

/**
 * Recheck currently focused window and set its <code>w->focused</code>
 * to true.
 *
 * @param ps current session
 * @return struct _win of currently focused window, NULL if not found
 */
static win *
recheck_focus(session_t *ps) {
  // Use EWMH _NET_ACTIVE_WINDOW if enabled
  if (ps->o.use_ewmh_active_win) {
    update_ewmh_active_win(ps);
    return ps->active_win;
  }

  // Determine the currently focused window so we can apply appropriate
  // opacity on it
  xcb_window_t wid = XCB_NONE;
  xcb_get_input_focus_reply_t *reply =
    xcb_get_input_focus_reply(ps->c, xcb_get_input_focus(ps->c), NULL);

  if (reply) {
    wid = reply->focus;
    free(reply);
  }

  win *w = find_win_all(ps, wid);

  log_trace("%#010" PRIx32 " (%#010lx \"%s\") focused.", wid,
      (w ? w->id: XCB_NONE), (w ? w->name: NULL));

  // And we set the focus state here
  if (w) {
    win_set_focused(ps, w, true);
    return w;
  }

  return NULL;
}

/**
 * Look for the client window of a particular window.
 */
xcb_window_t
find_client_win(session_t *ps, xcb_window_t w) {
  if (wid_has_prop(ps, w, ps->atom_client)) {
    return w;
  }

  xcb_query_tree_reply_t *reply = xcb_query_tree_reply(ps->c,
      xcb_query_tree(ps->c, w), NULL);
  if (!reply)
    return 0;

  xcb_window_t *children = xcb_query_tree_children(reply);
  int nchildren = xcb_query_tree_children_length(reply);
  int i;
  xcb_window_t ret = 0;

  for (i = 0; i < nchildren; ++i) {
    if ((ret = find_client_win(ps, children[i])))
      break;
  }

  free(reply);

  return ret;
}

static win *
paint_preprocess(session_t *ps, win *list, bool *fade_running) {
  // XXX need better, more general name for `fade_running`. It really
  // means if fade is still ongoing after the current frame is rendered
  win *t = NULL, *next = NULL;
  *fade_running = false;

  // Fading step calculation
  unsigned long steps = 0L;
  auto now = get_time_ms();
  auto tolerance = FADE_DELTA_TOLERANCE*ps->o.fade_delta;
  if (ps->fade_time && now+tolerance >= ps->fade_time) {
    steps = (now - ps->fade_time + tolerance) / ps->o.fade_delta;
  } else {
    // Reset fade_time if unset, or there appears to be a time disorder
    ps->fade_time = get_time_ms();
    steps = 0L;
  }
  ps->fade_time += steps * ps->o.fade_delta;

  // First, let's process fading
  for (win *w = list; w; w = next) {
    next = w->next;
    const winmode_t mode_old = w->mode;
    const bool was_painted = w->to_paint;
    const opacity_t opacity_old = w->opacity;
    // Restore flags from last paint if the window is being faded out
    // TODO probably should just stop updating window flags when window
    //      is fading out
    if (w->state == WSTATE_UNMAPPING || w->state == WSTATE_DESTROYING) {
      win_set_shadow(ps, w, w->shadow_last);
      win_set_invert_color(ps, w, w->invert_color_last);
      win_set_blur_background(ps, w, w->blur_background_last);
    }

    if (win_should_dim(ps, w) != w->dim) {
        w->dim = win_should_dim(ps, w);
        add_damage_from_win(ps, w);
    }

    // Run fading
    if (run_fade(ps, &w, steps)) {
      *fade_running = true;
    }

    if (!w) {
      // the window might have been destroyed because fading finished
      continue;
    }

    if (win_has_frame(w)) {
      w->frame_opacity = ps->o.frame_opacity;
    } else {
      w->frame_opacity = 1.0;
    }

    // Update window mode
    win_determine_mode(ps, w);

    // Destroy all reg_ignore above when frame opaque state changes on
    // SOLID mode
    if (was_painted && w->mode != mode_old) {
      w->reg_ignore_valid = false;
    }

    // Add window to damaged area if its opacity changes
    // If was_painted == false, and to_paint is also false, we don't care
    // If was_painted == false, but to_paint is true, damage will be added in the loop below
    if (was_painted && w->opacity != opacity_old) {
      add_damage_from_win(ps, w);
    }
  }

  // Opacity will not change, from now on.
  rc_region_t *last_reg_ignore = rc_region_new();

  bool unredir_possible = false;
  // Trace whether it's the highest window to paint
  bool is_highest = true;
  bool reg_ignore_valid = true;
  for (win *w = list; w; w = next) {
    __label__ skip_window;
    bool to_paint = true;
    // w->to_paint remembers whether this window is painted last time
    const bool was_painted = w->to_paint;

    // In case calling the fade callback function destroys this window
    next = w->next;

    // Destroy reg_ignore if some window above us invalidated it
    if (!reg_ignore_valid)
      rc_region_unref(&w->reg_ignore);

    //log_trace("%d %d %s", w->a.map_state, w->ever_damaged, w->name);

    // Give up if it's not damaged or invisible, or it's unmapped and its
    // pixmap is gone (for example due to a ConfigureNotify), or when it's
    // excluded
    if (!w->ever_damaged
        || w->g.x + w->g.width < 1 || w->g.y + w->g.height < 1
        || w->g.x >= ps->root_width || w->g.y >= ps->root_height
        || w->state == WSTATE_UNMAPPED
        || (double) w->opacity * MAX_ALPHA < 1
        || w->paint_excluded)
      to_paint = false;
    //log_trace("%s %d %d %d", w->name, to_paint, w->opacity, w->paint_excluded);

    // Add window to damaged area if its painting status changes
    // or opacity changes
    if (to_paint != was_painted) {
      w->reg_ignore_valid = false;
      add_damage_from_win(ps, w);
    }

    // to_paint will never change afterward
    if (!to_paint)
      goto skip_window;

    // Calculate shadow opacity
    w->shadow_opacity = ps->o.shadow_opacity * w->opacity * ps->o.frame_opacity;

    // Generate ignore region for painting to reduce GPU load
    if (!w->reg_ignore)
      w->reg_ignore = rc_region_ref(last_reg_ignore);

    // If the window is solid, we add the window region to the
    // ignored region
    // Otherwise last_reg_ignore shouldn't change
    if (w->mode == WMODE_SOLID && !ps->o.force_win_blend) {
      region_t *tmp = rc_region_new();
      if (w->frame_opacity == 1)
        *tmp = win_get_bounding_shape_global_by_val(w);
      else {
        win_get_region_noframe_local(w, tmp);
        pixman_region32_intersect(tmp, tmp, &w->bounding_shape);
        pixman_region32_translate(tmp, w->g.x, w->g.y);
      }

      pixman_region32_union(tmp, tmp, last_reg_ignore);
      rc_region_unref(&last_reg_ignore);
      last_reg_ignore = tmp;
    }

    // (Un)redirect screen
    // We could definitely unredirect the screen when there's no window to
    // paint, but this is typically unnecessary, may cause flickering when
    // fading is enabled, and could create inconsistency when the wallpaper
    // is not correctly set.
    if (ps->o.unredir_if_possible && is_highest) {
      if (win_is_solid(ps, w)
          && (w->frame_opacity == 1 || !win_has_frame(w))
          && win_is_fullscreen(ps, w)
          && !w->unredir_if_possible_excluded)
        unredir_possible = true;
    }

    // Reset flags
    w->flags = 0;
    w->prev_trans = t;
    t = w;

    // If the screen is not redirected and the window has redir_ignore set,
    // this window should not cause the screen to become redirected
    if (!(ps->o.wintype_option[w->window_type].redir_ignore && !ps->redirected)) {
      is_highest = false;
    }

  skip_window:
    reg_ignore_valid = reg_ignore_valid && w->reg_ignore_valid;
    w->reg_ignore_valid = true;

    win_check_fade_finished(ps, &w);

    // Avoid setting w->to_paint if w is freed
    if (w) {
      w->to_paint = to_paint;

      if (w->to_paint) {
        // Save flags
        w->shadow_last = w->shadow;
        w->invert_color_last = w->invert_color;
        w->blur_background_last = w->blur_background;
      }
    }
  }

  rc_region_unref(&last_reg_ignore);

  // If possible, unredirect all windows and stop painting
  if (ps->o.redirected_force != UNSET)
    unredir_possible = !ps->o.redirected_force;
  else if (ps->o.unredir_if_possible && is_highest && !ps->redirected)
    // If there's no window to paint, and the screen isn't redirected,
    // don't redirect it.
    unredir_possible = true;
  if (unredir_possible) {
    if (ps->redirected) {
      if (!ps->o.unredir_if_possible_delay || ps->tmout_unredir_hit)
        redir_stop(ps);
      else if (!ev_is_active(&ps->unredir_timer)) {
        ev_timer_set(&ps->unredir_timer,
          ps->o.unredir_if_possible_delay / 1000.0, 0);
        ev_timer_start(ps->loop, &ps->unredir_timer);
      }
    }
  } else {
    ev_timer_stop(ps->loop, &ps->unredir_timer);
    redir_start(ps);
  }

  return t;
}

/*
 * WORK-IN-PROGRESS!
static void
xr_take_screenshot(session_t *ps) {
  XImage *img = XGetImage(ps->dpy, get_tgt_window(ps), 0, 0,
      ps->root_width, ps->root_height, AllPlanes, XYPixmap);
  if (!img) {
    log_error("Failed to get XImage.");
    return NULL;
  }
  assert(0 == img->xoffset);
}
*/

/**
 * Rebuild cached <code>screen_reg</code>.
 */
static void
rebuild_screen_reg(session_t *ps) {
  get_screen_region(ps, &ps->screen_reg);
}

/**
 * Rebuild <code>shadow_exclude_reg</code>.
 */
static void
rebuild_shadow_exclude_reg(session_t *ps) {
  bool ret = parse_geometry(ps, ps->o.shadow_exclude_reg_str,
      &ps->shadow_exclude_reg);
  if (!ret)
    exit(1);
}

static void
repair_win(session_t *ps, win *w) {
  if (w->a.map_state != XCB_MAP_STATE_VIEWABLE)
    return;

  region_t parts;
  pixman_region32_init(&parts);

  if (!w->ever_damaged) {
    win_extents(w, &parts);
    set_ignore_cookie(ps,
        xcb_damage_subtract(ps->c, w->damage, XCB_NONE, XCB_NONE));
  } else {
    xcb_xfixes_region_t tmp = xcb_generate_id(ps->c);
    xcb_xfixes_create_region(ps->c, tmp, 0, NULL);
    set_ignore_cookie(ps,
        xcb_damage_subtract(ps->c, w->damage, XCB_NONE, tmp));
    xcb_xfixes_translate_region(ps->c, tmp,
      w->g.x + w->g.border_width,
      w->g.y + w->g.border_width);
    x_fetch_region(ps->c, tmp, &parts);
    xcb_xfixes_destroy_region(ps->c, tmp);
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
static void
restack_win(session_t *ps, win *w, xcb_window_t new_above) {
  xcb_window_t old_above;

  if (w->next) {
    old_above = w->next->id;
  } else {
    old_above = XCB_NONE;
  }

  if (old_above != new_above) {
    w->reg_ignore_valid = false;
    rc_region_unref(&w->reg_ignore);
    if (w->next) {
      w->next->reg_ignore_valid = false;
      rc_region_unref(&w->next->reg_ignore);
    }

    win **prev = NULL, **prev_old = NULL;

    // unhook
    for (prev = &ps->list; *prev; prev = &(*prev)->next) {
      if ((*prev) == w) break;
    }

    prev_old = prev;

    bool found = false;

    // rehook
    for (prev = &ps->list; *prev; prev = &(*prev)->next) {
      if ((*prev)->id == new_above && (*prev)->state != WSTATE_DESTROYING) {
        found = true;
        break;
      }
    }

    if (new_above && !found) {
      log_error("(%#010x, %#010x): Failed to found new above window.", w->id, new_above);
      return;
    }

    *prev_old = w->next;

    w->next = *prev;
    *prev = w;

    // add damage for this window
    add_damage_from_win(ps, w);

#ifdef DEBUG_RESTACK
    {
      const char *desc;
      char *window_name = NULL;
      bool to_free;
      win* c = ps->list;

      log_trace("(%#010lx, %#010lx): "
                "Window stack modified. Current stack:", w->id, new_above);

      for (; c; c = c->next) {
        window_name = "(Failed to get title)";

        to_free = ev_window_name(ps, c->id, &window_name);

        desc = "";
        if (c->destroying) desc = "(D) ";
        printf("%#010lx \"%s\" %s", c->id, window_name, desc);
        if (c->next)
          printf("-> ");

        if (to_free) {
          cxfree(window_name);
          window_name = NULL;
        }
      }
      fputs("\n", stdout);
    }
#endif
  }
}

void
configure_win(session_t *ps, xcb_configure_notify_event_t *ce) {
  // On root window changes
  if (ce->window == ps->root) {
    free_paint(ps, &ps->tgt_buffer);

    ps->root_width = ce->width;
    ps->root_height = ce->height;

    rebuild_screen_reg(ps);
    rebuild_shadow_exclude_reg(ps);
    for (int i = 0; i < ps->ndamage; i++) {
	    pixman_region32_clear(&ps->damage_ring[i]);
    }
    ps->damage = ps->damage_ring + ps->ndamage - 1;

    // Re-redirect screen if required
    if (ps->o.reredir_on_root_change && ps->redirected) {
      redir_stop(ps);
      redir_start(ps);
    }

    // Invalidate reg_ignore from the top
    rc_region_unref(&ps->list->reg_ignore);
    ps->list->reg_ignore_valid = false;

#ifdef CONFIG_OPENGL
    // Reinitialize GLX on root change
    if (ps->o.glx_reinit_on_root_change && ps->psglx) {
      if (!glx_reinit(ps, bkend_use_glx(ps)))
        log_error("Failed to reinitialize GLX, troubles ahead.");
      if (BKEND_GLX == ps->o.backend && !glx_init_blur(ps))
        log_error("Failed to initialize filters.");
    }

    // GLX root change callback
    if (BKEND_GLX == ps->o.backend)
      glx_on_root_change(ps);
#endif

    force_repaint(ps);

    return;
  }

  // Other window changes
  win *w = find_win(ps, ce->window);
  region_t damage;
  pixman_region32_init(&damage);

  if (!w)
    return;

  if (w->a.map_state == XCB_MAP_STATE_UNMAPPED) {
    /* save the configure event for when the window maps */
    w->need_configure = true;
    w->queue_configure = *ce;
    restack_win(ps, w, ce->above_sibling);
  } else {
    if (!w->need_configure)
      restack_win(ps, w, ce->above_sibling);

    bool factor_change = false;

    w->need_configure = false;
    win_extents(w, &damage);

    // If window geometry change, free old extents
    if (w->g.x != ce->x || w->g.y != ce->y
        || w->g.width != ce->width || w->g.height != ce->height
        || w->g.border_width != ce->border_width)
      factor_change = true;

    w->g.x = ce->x;
    w->g.y = ce->y;

    if (w->g.width != ce->width || w->g.height != ce->height
        || w->g.border_width != ce->border_width) {
      w->g.width = ce->width;
      w->g.height = ce->height;
      w->g.border_width = ce->border_width;
      calc_win_size(ps, w);
      win_update_bounding_shape(ps, w);
    }

    region_t new_extents;
    pixman_region32_init(&new_extents);
    win_extents(w, &new_extents);
    pixman_region32_union(&damage, &damage, &new_extents);
    pixman_region32_fini(&new_extents);

    if (factor_change) {
      win_on_factor_change(ps, w);
      add_damage(ps, &damage);
      win_update_screen(ps, w);
    }
  }

  pixman_region32_fini(&damage);

  // override_redirect flag cannot be changed after window creation, as far
  // as I know, so there's no point to re-match windows here.
  w->a.override_redirect = ce->override_redirect;
}

static void
circulate_win(session_t *ps, xcb_circulate_notify_event_t *ce) {
  win *w = find_win(ps, ce->window);
  xcb_window_t new_above;

  if (!w) return;

  if (ce->place == PlaceOnTop) {
    new_above = ps->list->id;
  } else {
    new_above = XCB_NONE;
  }

  restack_win(ps, w, new_above);
}

static inline void
root_damaged(session_t *ps) {
  if (ps->root_tile_paint.pixmap) {
    xcb_clear_area(ps->c, true, ps->root, 0, 0, 0, 0);
    free_root_tile(ps);
  }

  // Mark screen damaged
  force_repaint(ps);
}

/**
 * Xlib error handler function.
 */
static int
xerror(Display attr_unused *dpy, XErrorEvent *ev) {
  if (!should_ignore(ps_g, ev->serial))
    x_print_error(ev->serial, ev->request_code, ev->minor_code, ev->error_code);
  return 0;
}

/**
 * XCB error handler function.
 */
void
ev_xcb_error(session_t *ps, xcb_generic_error_t *err) {
  if (!should_ignore(ps, err->sequence))
    x_print_error(err->sequence, err->major_code, err->minor_code, err->error_code);
}

static void
expose_root(session_t *ps, const rect_t *rects, int nrects) {
  region_t region;
  pixman_region32_init_rects(&region, rects, nrects);
  add_damage(ps, &region);
}
/**
 * Force a full-screen repaint.
 */
void
force_repaint(session_t *ps) {
  assert(pixman_region32_not_empty(&ps->screen_reg));
  queue_redraw(ps);
  add_damage(ps, &ps->screen_reg);
}

#ifdef CONFIG_DBUS
/** @name DBus hooks
 */
///@{

/**
 * Set w->shadow_force of a window.
 */
void
win_set_shadow_force(session_t *ps, win *w, switch_t val) {
  if (val != w->shadow_force) {
    w->shadow_force = val;
    win_determine_shadow(ps, w);
    queue_redraw(ps);
  }
}

/**
 * Set w->fade_force of a window.
 *
 * Doesn't affect fading already in progress
 */
void
win_set_fade_force(session_t *ps, win *w, switch_t val) {
  w->fade_force = val;
}

/**
 * Set w->focused_force of a window.
 */
void
win_set_focused_force(session_t *ps, win *w, switch_t val) {
  if (val != w->focused_force) {
    w->focused_force = val;
    win_update_focused(ps, w);
    queue_redraw(ps);
  }
}

/**
 * Set w->invert_color_force of a window.
 */
void
win_set_invert_color_force(session_t *ps, win *w, switch_t val) {
  if (val != w->invert_color_force) {
    w->invert_color_force = val;
    win_determine_invert_color(ps, w);
    queue_redraw(ps);
  }
}

/**
 * Enable focus tracking.
 */
void
opts_init_track_focus(session_t *ps) {
  // Already tracking focus
  if (ps->o.track_focus)
    return;

  ps->o.track_focus = true;

  if (!ps->o.use_ewmh_active_win) {
    // Start listening to FocusChange events
    for (win *w = ps->list; w; w = w->next)
      if (w->a.map_state == XCB_MAP_STATE_VIEWABLE)
        xcb_change_window_attributes(ps->c, w->id, XCB_CW_EVENT_MASK,
            (const uint32_t[]) { determine_evmask(ps, w->id, WIN_EVMODE_FRAME) });
  }

  // Recheck focus
  recheck_focus(ps);
}

/**
 * Set no_fading_openclose option.
 *
 * Don't affect fading already in progress
 */
void
opts_set_no_fading_openclose(session_t *ps, bool newval) {
  ps->o.no_fading_openclose = newval;
}

//!@}
#endif

static inline int attr_unused
ev_serial(xcb_generic_event_t *ev) {
  return ev->full_sequence;
}

static inline const char * attr_unused
ev_name(session_t *ps, xcb_generic_event_t *ev) {
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

static inline xcb_window_t attr_unused
ev_window(session_t *ps, xcb_generic_event_t *ev) {
  switch (ev->response_type) {
    case FocusIn:
    case FocusOut:
      return ((xcb_focus_in_event_t *)ev)->event;
    case CreateNotify:
      return ((xcb_create_notify_event_t *)ev)->window;
    case ConfigureNotify:
      return ((xcb_configure_notify_event_t *)ev)->window;
    case DestroyNotify:
      return ((xcb_destroy_notify_event_t *)ev)->window;
    case MapNotify:
      return ((xcb_map_notify_event_t *)ev)->window;
    case UnmapNotify:
      return ((xcb_unmap_notify_event_t *)ev)->window;
    case ReparentNotify:
      return ((xcb_reparent_notify_event_t *)ev)->window;
    case CirculateNotify:
      return ((xcb_circulate_notify_event_t *)ev)->window;
    case Expose:
      return ((xcb_expose_event_t *)ev)->window;
    case PropertyNotify:
      return ((xcb_property_notify_event_t *)ev)->window;
    case ClientMessage:
      return ((xcb_client_message_event_t *)ev)->window;
    default:
      if (ps->damage_event + XCB_DAMAGE_NOTIFY == ev->response_type) {
        return ((xcb_damage_notify_event_t *)ev)->drawable;
      }

      if (ps->shape_exists && ev->response_type == ps->shape_event) {
        return ((xcb_shape_notify_event_t *) ev)->affected_window;
      }

      return 0;
  }
}

static inline const char *
ev_focus_mode_name(xcb_focus_in_event_t* ev) {
  switch (ev->mode) {
    CASESTRRET(NotifyNormal);
    CASESTRRET(NotifyWhileGrabbed);
    CASESTRRET(NotifyGrab);
    CASESTRRET(NotifyUngrab);
  }

  return "Unknown";
}

static inline const char *
ev_focus_detail_name(xcb_focus_in_event_t* ev) {
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

static inline void attr_unused
ev_focus_report(xcb_focus_in_event_t *ev) {
  log_trace("{ mode: %s, detail: %s }\n", ev_focus_mode_name(ev),
      ev_focus_detail_name(ev));
}

// === Events ===

/**
 * Determine whether we should respond to a <code>FocusIn/Out</code>
 * event.
 */
/*
inline static bool
ev_focus_accept(XFocusChangeEvent *ev) {
  return NotifyNormal == ev->mode || NotifyUngrab == ev->mode;
}
*/

static inline void
ev_focus_in(session_t *ps, xcb_focus_in_event_t *ev) {
#ifdef DEBUG_EVENTS
  ev_focus_report(ev);
#endif

  recheck_focus(ps);
}

inline static void
ev_focus_out(session_t *ps, xcb_focus_out_event_t *ev) {
#ifdef DEBUG_EVENTS
  ev_focus_report(ev);
#endif

  recheck_focus(ps);
}

inline static void
ev_create_notify(session_t *ps, xcb_create_notify_event_t *ev) {
  assert(ev->parent == ps->root);
  add_win(ps, ev->window, 0);
}

inline static void
ev_configure_notify(session_t *ps, xcb_configure_notify_event_t *ev) {
  log_trace("{ send_event: %d, above: %#010x, override_redirect: %d }",
         ev->event, ev->above_sibling, ev->override_redirect);
  configure_win(ps, ev);
}

inline static void
ev_destroy_notify(session_t *ps, xcb_destroy_notify_event_t *ev) {
  win *w = find_win(ps, ev->window);
  if (w) {
    unmap_win(ps, &w, true);
  }
}

inline static void
ev_map_notify(session_t *ps, xcb_map_notify_event_t *ev) {
  map_win(ps, ev->window);
  // FocusIn/Out may be ignored when the window is unmapped, so we must
  // recheck focus here
  if (ps->o.track_focus) {
    recheck_focus(ps);
  }
}

inline static void
ev_unmap_notify(session_t *ps, xcb_unmap_notify_event_t *ev) {
  win *w = find_win(ps, ev->window);
  if (w) {
    unmap_win(ps, &w, false);
  }
}

inline static void
ev_reparent_notify(session_t *ps, xcb_reparent_notify_event_t *ev) {
  log_trace("{ new_parent: %#010x, override_redirect: %d }",
            ev->parent, ev->override_redirect);

  if (ev->parent == ps->root) {
    // new window
    add_win(ps, ev->window, 0);
  } else {
    // otherwise, find and destroy the window first
    win *w = find_win(ps, ev->window);
    if (w) {
      unmap_win(ps, &w, true);
    }

    // Reset event mask in case something wrong happens
    xcb_change_window_attributes(ps->c, ev->window, XCB_CW_EVENT_MASK,
        (const uint32_t[]) { determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN) });

    // Check if the window is an undetected client window
    // Firstly, check if it's a known client window
    if (!find_toplevel(ps, ev->window)) {
      // If not, look for its frame window
      win *w_top = find_toplevel2(ps, ev->parent);
      // If found, and the client window has not been determined, or its
      // frame may not have a correct client, continue
      if (w_top && (!w_top->client_win
            || w_top->client_win == w_top->id)) {
        // If it has WM_STATE, mark it the client window
        if (wid_has_prop(ps, ev->window, ps->atom_client)) {
          w_top->wmwin = false;
          win_unmark_client(ps, w_top);
          win_mark_client(ps, w_top, ev->window);
        }
        // Otherwise, watch for WM_STATE on it
        else {
          xcb_change_window_attributes(ps->c, ev->window, XCB_CW_EVENT_MASK, (const uint32_t[]) {
              determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN) | XCB_EVENT_MASK_PROPERTY_CHANGE });
        }
      }
    }
  }
}

inline static void
ev_circulate_notify(session_t *ps, xcb_circulate_notify_event_t *ev) {
  circulate_win(ps, ev);
}

inline static void
ev_expose(session_t *ps, xcb_expose_event_t *ev) {
  if (ev->window == ps->root || (ps->overlay && ev->window == ps->overlay)) {
    int more = ev->count + 1;
    if (ps->n_expose == ps->size_expose) {
      if (ps->expose_rects) {
        ps->expose_rects = crealloc(ps->expose_rects, ps->size_expose + more);
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

/**
 * Update current active window based on EWMH _NET_ACTIVE_WIN.
 *
 * Does not change anything if we fail to get the attribute or the window
 * returned could not be found.
 */
static void
update_ewmh_active_win(session_t *ps) {
  // Search for the window
  xcb_window_t wid = wid_get_prop_window(ps, ps->root, ps->atom_ewmh_active_win);
  win *w = find_win_all(ps, wid);

  // Mark the window focused. No need to unfocus the previous one.
  if (w) win_set_focused(ps, w, true);
}

inline static void
ev_property_notify(session_t *ps, xcb_property_notify_event_t *ev) {
#ifdef DEBUG_EVENTS
  {
    // Print out changed atom
    xcb_get_atom_name_reply_t *reply =
      xcb_get_atom_name_reply(ps->c, xcb_get_atom_name(ps->c, ev->atom), NULL);
    const char *name = "?";
    int name_len = 1;
    if (reply) {
        name = xcb_get_atom_name_name(reply);
        name_len = xcb_get_atom_name_name_length(reply);
    }

    log_trace("{ atom = %.*s }", name_len, name);
    free(reply);
  }
#endif

  if (ps->root == ev->window) {
    if (ps->o.track_focus && ps->o.use_ewmh_active_win
        && ps->atom_ewmh_active_win == ev->atom) {
      update_ewmh_active_win(ps);
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
  if (ev->atom == ps->atom_client) {
    // Check whether it could be a client window
    if (!find_toplevel(ps, ev->window)) {
      // Reset event mask anyway
      xcb_change_window_attributes(ps->c, ev->window, XCB_CW_EVENT_MASK, (const uint32_t[]) {
          determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN) });

      win *w_top = find_toplevel2(ps, ev->window);
      // Initialize client_win as early as possible
      if (w_top && (!w_top->client_win || w_top->client_win == w_top->id)
          && wid_has_prop(ps, ev->window, ps->atom_client)) {
        w_top->wmwin = false;
        win_unmark_client(ps, w_top);
        win_mark_client(ps, w_top, ev->window);
      }
    }
  }

  // If _NET_WM_WINDOW_TYPE changes... God knows why this would happen, but
  // there are always some stupid applications. (#144)
  if (ev->atom == ps->atom_win_type) {
    win *w = NULL;
    if ((w = find_toplevel(ps, ev->window)))
      win_upd_wintype(ps, w);
  }

  // If _NET_WM_OPACITY changes
  if (ev->atom == ps->atom_opacity) {
    win *w = find_win(ps, ev->window) ?: find_toplevel(ps, ev->window);
    if (w) {
      win_update_opacity_prop(ps, w);
      // we cannot receive OPACITY change when window is destroyed
      assert(w->state != WSTATE_DESTROYING);
      if (w->state == WSTATE_MAPPED) {
        // See the winstate_t transition table
        w->state = WSTATE_FADING;
      }
      w->opacity_tgt = win_get_opacity_target(ps, w);
    }
  }

  // If frame extents property changes
  if (ps->o.frame_opacity && ev->atom == ps->atom_frame_extents) {
    win *w = find_toplevel(ps, ev->window);
    if (w) {
      win_update_frame_extents(ps, w, ev->window);
      // If frame extents change, the window needs repaint
      add_damage_from_win(ps, w);
    }
  }

  // If name changes
  if (ps->o.track_wdata
      && (ps->atom_name == ev->atom || ps->atom_name_ewmh == ev->atom)) {
    win *w = find_toplevel(ps, ev->window);
    if (w && 1 == win_get_name(ps, w)) {
      win_on_factor_change(ps, w);
    }
  }

  // If class changes
  if (ps->o.track_wdata && ps->atom_class == ev->atom) {
    win *w = find_toplevel(ps, ev->window);
    if (w) {
      win_get_class(ps, w);
      win_on_factor_change(ps, w);
    }
  }

  // If role changes
  if (ps->o.track_wdata && ps->atom_role == ev->atom) {
    win *w = find_toplevel(ps, ev->window);
    if (w && 1 == win_get_role(ps, w)) {
      win_on_factor_change(ps, w);
    }
  }

  // If _COMPTON_SHADOW changes
  if (ps->o.respect_prop_shadow && ps->atom_compton_shadow == ev->atom) {
    win *w = find_win(ps, ev->window);
    if (w)
      win_update_prop_shadow(ps, w);
  }

  // If a leader property changes
  if ((ps->o.detect_transient && ps->atom_transient == ev->atom)
      || (ps->o.detect_client_leader && ps->atom_client_leader == ev->atom)) {
    win *w = find_toplevel(ps, ev->window);
    if (w) {
      win_update_leader(ps, w);
    }
  }

  // Check for other atoms we are tracking
  for (latom_t *platom = ps->track_atom_lst; platom; platom = platom->next) {
    if (platom->atom == ev->atom) {
      win *w = find_win(ps, ev->window);
      if (!w)
        w = find_toplevel(ps, ev->window);
      if (w)
        win_on_factor_change(ps, w);
      break;
    }
  }
}

inline static void
ev_damage_notify(session_t *ps, xcb_damage_notify_event_t *de) {
  /*
  if (ps->root == de->drawable) {
    root_damaged();
    return;
  } */

  win *w = find_win(ps, de->drawable);

  if (!w) return;

  repair_win(ps, w);
}

inline static void
ev_shape_notify(session_t *ps, xcb_shape_notify_event_t *ev) {
  win *w = find_win(ps, ev->affected_window);
  if (!w || w->a.map_state == XCB_MAP_STATE_UNMAPPED) return;

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

/**
 * Handle ScreenChangeNotify events from X RandR extension.
 */
static void
ev_screen_change_notify(session_t *ps,
    xcb_randr_screen_change_notify_event_t attr_unused *ev) {
  if (ps->o.xinerama_shadow_crop)
    cxinerama_upd_scrs(ps);

  if (ps->o.sw_opti && !ps->o.refresh_rate) {
    update_refresh_rate(ps);
    if (!ps->refresh_rate) {
      log_warn("Refresh rate detection failed. swopti will be temporarily disabled");
    }
  }
}

inline static void
ev_selection_clear(session_t *ps,
    xcb_selection_clear_event_t attr_unused *ev) {
  // The only selection we own is the _NET_WM_CM_Sn selection.
  // If we lose that one, we should exit.
  log_fatal("Another composite manager started and took the _NET_WM_CM_Sn selection.");
  exit(1);
}

/**
 * Get a window's name from window ID.
 */
static inline void attr_unused
ev_window_name(session_t *ps, xcb_window_t wid, char **name) {
  *name = "";
  if (wid) {
    *name = "(Failed to get title)";
    if (ps->root == wid)
      *name = "(Root window)";
    else if (ps->overlay == wid)
      *name = "(Overlay)";
    else {
      win *w = find_win(ps, wid);
      if (!w)
        w = find_toplevel(ps, wid);

      if (w)
        win_get_name(ps, w);
      if (w && w->name)
        *name = w->name;
      else
        *name = "unknown";
    }
  }
}

static void
ev_handle(session_t *ps, xcb_generic_event_t *ev) {
  if ((ev->response_type & 0x7f) != KeymapNotify) {
    discard_ignore(ps, ev->full_sequence);
  }

#ifdef DEBUG_EVENTS
  if (ev->response_type != ps->damage_event + XCB_DAMAGE_NOTIFY) {
    xcb_window_t wid = ev_window(ps, ev);
    char *window_name = NULL;
    ev_window_name(ps, wid, &window_name);

    log_trace("event %10.10s serial %#010x window %#010lx \"%s\"",
              ev_name(ps, ev), ev_serial(ev), wid, window_name);
  }
#endif

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
    case FocusIn:
      ev_focus_in(ps, (xcb_focus_in_event_t *)ev);
      break;
    case FocusOut:
      ev_focus_out(ps, (xcb_focus_out_event_t *)ev);
      break;
    case CreateNotify:
      ev_create_notify(ps, (xcb_create_notify_event_t *)ev);
      break;
    case ConfigureNotify:
      ev_configure_notify(ps, (xcb_configure_notify_event_t *)ev);
      break;
    case DestroyNotify:
      ev_destroy_notify(ps, (xcb_destroy_notify_event_t *)ev);
      break;
    case MapNotify:
      ev_map_notify(ps, (xcb_map_notify_event_t *)ev);
      break;
    case UnmapNotify:
      ev_unmap_notify(ps, (xcb_unmap_notify_event_t *)ev);
      break;
    case ReparentNotify:
      ev_reparent_notify(ps, (xcb_reparent_notify_event_t *)ev);
      break;
    case CirculateNotify:
      ev_circulate_notify(ps, (xcb_circulate_notify_event_t *)ev);
      break;
    case Expose:
      ev_expose(ps, (xcb_expose_event_t *)ev);
      break;
    case PropertyNotify:
      ev_property_notify(ps, (xcb_property_notify_event_t *)ev);
      break;
    case SelectionClear:
      ev_selection_clear(ps, (xcb_selection_clear_event_t *)ev);
      break;
    case 0:
      ev_xcb_error(ps, (xcb_generic_error_t *)ev);
      break;
    default:
      if (ps->shape_exists && ev->response_type == ps->shape_event) {
        ev_shape_notify(ps, (xcb_shape_notify_event_t *) ev);
        break;
      }
      if (ps->randr_exists && ev->response_type == (ps->randr_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY)) {
        ev_screen_change_notify(ps, (xcb_randr_screen_change_notify_event_t *) ev);
        break;
      }
      if (ps->damage_event + XCB_DAMAGE_NOTIFY == ev->response_type) {
        ev_damage_notify(ps, (xcb_damage_notify_event_t *) ev);
        break;
      }
  }
}

// === Main ===


/**
 * Register a window as symbol, and initialize GLX context if wanted.
 */
static bool
register_cm(session_t *ps) {
  assert(!ps->reg_win);

  ps->reg_win = XCreateSimpleWindow(ps->dpy, ps->root, 0, 0, 1, 1, 0,
        XCB_NONE, XCB_NONE);

  if (!ps->reg_win) {
    log_fatal("Failed to create window.");
    return false;
  }

  // Unredirect the window if it's redirected, just in case
  if (ps->redirected)
    xcb_composite_unredirect_window(ps->c, ps->reg_win, XCB_COMPOSITE_REDIRECT_MANUAL);

  {
    XClassHint *h = XAllocClassHint();
    if (h) {
      h->res_name = "compton";
      h->res_class = "xcompmgr";
    }
    Xutf8SetWMProperties(ps->dpy, ps->reg_win, "xcompmgr", "xcompmgr",
        NULL, 0, NULL, NULL, h);
    cxfree(h);
  }

  // Set _NET_WM_PID
  {
    uint32_t pid = getpid();
    xcb_change_property(ps->c, XCB_PROP_MODE_REPLACE, ps->reg_win,
        get_atom(ps, "_NET_WM_PID"), XCB_ATOM_CARDINAL, 32, 1, &pid);
  }

  // Set COMPTON_VERSION
  if (!wid_set_text_prop(ps, ps->reg_win, get_atom(ps, "COMPTON_VERSION"),
                         COMPTON_VERSION)) {
    log_error("Failed to set COMPTON_VERSION.");
  }

  // Acquire X Selection _NET_WM_CM_S?
  if (!ps->o.no_x_selection) {
    unsigned len = strlen(REGISTER_PROP) + 2;
    int s = ps->scr;
    Atom atom;

    while (s >= 10) {
      ++len;
      s /= 10;
    }

    auto buf = ccalloc(len, char);
    snprintf(buf, len, REGISTER_PROP "%d", ps->scr);
    buf[len - 1] = '\0';
    atom = get_atom(ps, buf);
    free(buf);

    xcb_get_selection_owner_reply_t *reply =
      xcb_get_selection_owner_reply(ps->c,
          xcb_get_selection_owner(ps->c, atom), NULL);

    if (reply && reply->owner != XCB_NONE) {
      free(reply);
      log_fatal("Another composite manager is already running");
      return false;
    }
    free(reply);
    xcb_set_selection_owner(ps->c, ps->reg_win, atom, 0);
  }

  return true;
}

/**
 * Write PID to a file.
 */
static inline bool
write_pid(session_t *ps) {
  if (!ps->o.write_pid_path)
    return true;

  FILE *f = fopen(ps->o.write_pid_path, "w");
  if (unlikely(!f)) {
    log_error("Failed to write PID to \"%s\".", ps->o.write_pid_path);
    return false;
  }

  fprintf(f, "%ld\n", (long) getpid());
  fclose(f);

  return true;
}

/**
 * Fetch all required atoms and save them to a session.
 */
static void
init_atoms(session_t *ps) {
  ps->atom_opacity = get_atom(ps, "_NET_WM_WINDOW_OPACITY");
  ps->atom_frame_extents = get_atom(ps, "_NET_FRAME_EXTENTS");
  ps->atom_client = get_atom(ps, "WM_STATE");
  ps->atom_name = XCB_ATOM_WM_NAME;
  ps->atom_name_ewmh = get_atom(ps, "_NET_WM_NAME");
  ps->atom_class = XCB_ATOM_WM_CLASS;
  ps->atom_role = get_atom(ps, "WM_WINDOW_ROLE");
  ps->atom_transient = XCB_ATOM_WM_TRANSIENT_FOR;
  ps->atom_client_leader = get_atom(ps, "WM_CLIENT_LEADER");
  ps->atom_ewmh_active_win = get_atom(ps, "_NET_ACTIVE_WINDOW");
  ps->atom_compton_shadow = get_atom(ps, "_COMPTON_SHADOW");

  ps->atom_win_type = get_atom(ps, "_NET_WM_WINDOW_TYPE");
  ps->atoms_wintypes[WINTYPE_UNKNOWN] = 0;
  ps->atoms_wintypes[WINTYPE_DESKTOP] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DESKTOP");
  ps->atoms_wintypes[WINTYPE_DOCK] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DOCK");
  ps->atoms_wintypes[WINTYPE_TOOLBAR] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_TOOLBAR");
  ps->atoms_wintypes[WINTYPE_MENU] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_MENU");
  ps->atoms_wintypes[WINTYPE_UTILITY] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_UTILITY");
  ps->atoms_wintypes[WINTYPE_SPLASH] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_SPLASH");
  ps->atoms_wintypes[WINTYPE_DIALOG] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DIALOG");
  ps->atoms_wintypes[WINTYPE_NORMAL] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_NORMAL");
  ps->atoms_wintypes[WINTYPE_DROPDOWN_MENU] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU");
  ps->atoms_wintypes[WINTYPE_POPUP_MENU] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_POPUP_MENU");
  ps->atoms_wintypes[WINTYPE_TOOLTIP] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_TOOLTIP");
  ps->atoms_wintypes[WINTYPE_NOTIFY] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_NOTIFICATION");
  ps->atoms_wintypes[WINTYPE_COMBO] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_COMBO");
  ps->atoms_wintypes[WINTYPE_DND] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DND");
}

/**
 * Update refresh rate info with X Randr extension.
 */
static void
update_refresh_rate(session_t *ps) {
  xcb_randr_get_screen_info_reply_t *randr_info =
    xcb_randr_get_screen_info_reply(ps->c,
        xcb_randr_get_screen_info(ps->c, ps->root), NULL);

  if (!randr_info)
    return;
  ps->refresh_rate = randr_info->rate;
  free(randr_info);

  if (ps->refresh_rate)
    ps->refresh_intv = US_PER_SEC / ps->refresh_rate;
  else
    ps->refresh_intv = 0;
}

/**
 * Initialize refresh-rated based software optimization.
 *
 * @return true for success, false otherwise
 */
static bool
swopti_init(session_t *ps) {
  // Prepare refresh rate
  // Check if user provides one
  ps->refresh_rate = ps->o.refresh_rate;
  if (ps->refresh_rate)
    ps->refresh_intv = US_PER_SEC / ps->refresh_rate;

  // Auto-detect refresh rate otherwise
  if (!ps->refresh_rate && ps->randr_exists) {
    update_refresh_rate(ps);
  }

  // Turn off vsync_sw if we can't get the refresh rate
  if (!ps->refresh_rate)
    return false;

  return true;
}

/**
 * Modify a struct timeval timeout value to render at a fixed pace.
 *
 * @param ps current session
 * @param[in,out] ptv pointer to the timeout
 */
static double
swopti_handle_timeout(session_t *ps) {
  if (!ps->refresh_intv)
    return 0;

  // Get the microsecond offset of the time when the we reach the timeout
  // I don't think a 32-bit long could overflow here.
  long offset = (get_time_timeval().tv_usec - ps->paint_tm_offset) % ps->refresh_intv;
  if (offset < 0)
    offset += ps->refresh_intv;

  // If the target time is sufficiently close to a refresh time, don't add
  // an offset, to avoid certain blocking conditions.
  if (offset < SWOPTI_TOLERANCE
      || offset > ps->refresh_intv - SWOPTI_TOLERANCE)
    return 0;

  // Add an offset so we wait until the next refresh after timeout
  return (ps->refresh_intv - offset) / 1e6;
}

/**
 * Initialize X composite overlay window.
 */
static bool
init_overlay(session_t *ps) {
  xcb_composite_get_overlay_window_reply_t *reply =
    xcb_composite_get_overlay_window_reply(ps->c,
        xcb_composite_get_overlay_window(ps->c, ps->root), NULL);
  if (reply) {
    ps->overlay = reply->overlay_win;
    free(reply);
  } else {
    ps->overlay = XCB_NONE;
  }
  if (ps->overlay) {
    // Set window region of the overlay window, code stolen from
    // compiz-0.8.8
    xcb_generic_error_t *e;
    e = XCB_SYNCED_VOID(xcb_shape_mask, ps->c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING,
      ps->overlay, 0, 0, 0);
    if (e) {
      log_fatal("Failed to set the bounding shape of overlay, giving up.");
      exit(1);
    }
    e = XCB_SYNCED_VOID(xcb_shape_rectangles, ps->c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
      XCB_CLIP_ORDERING_UNSORTED, ps->overlay, 0, 0, 0, NULL);
    if (e) {
      log_fatal("Failed to set the input shape of overlay, giving up.");
      exit(1);
    }

    // Listen to Expose events on the overlay
    xcb_change_window_attributes(ps->c, ps->overlay, XCB_CW_EVENT_MASK,
        (const uint32_t[]) { XCB_EVENT_MASK_EXPOSURE });

    // Retrieve DamageNotify on root window if we are painting on an
    // overlay
    // root_damage = XDamageCreate(ps->dpy, root, XDamageReportNonEmpty);

    // Unmap overlay, firstly. But this typically does not work because
    // the window isn't created yet.
    // xcb_unmap_window(c, ps->overlay);
    // XFlush(ps->dpy);
  } else {
    log_error("Cannot get X Composite overlay window. Falling "
              "back to painting on root window.");
  }
  log_debug("overlay = %#010x", ps->overlay);

  return ps->overlay;
}

/**
 * Redirect all windows.
 */
static void
redir_start(session_t *ps) {
  if (!ps->redirected) {
    log_debug("Screen redirected.");

    // Map overlay window. Done firstly according to this:
    // https://bugzilla.gnome.org/show_bug.cgi?id=597014
    if (ps->overlay)
      xcb_map_window(ps->c, ps->overlay);

    xcb_composite_redirect_subwindows(ps->c, ps->root, XCB_COMPOSITE_REDIRECT_MANUAL);

    /*
    // Unredirect GL context window as this may have an effect on VSync:
    // < http://dri.freedesktop.org/wiki/CompositeSwap >
    xcb_composite_unredirect_window(c, ps->reg_win, XCB_COMPOSITE_REDIRECT_MANUAL);
    if (ps->o.paint_on_overlay && ps->overlay) {
      xcb_composite_unredirect_window(c, ps->overlay,
          XCB_COMPOSITE_REDIRECT_MANUAL);
    } */

    // Must call XSync() here
    x_sync(ps->c);

    ps->redirected = true;

    // Repaint the whole screen
    force_repaint(ps);
  }
}

/**
 * Unredirect all windows.
 */
static void
redir_stop(session_t *ps) {
  if (ps->redirected) {
    log_debug("Screen unredirected.");
    // Destroy all Pictures as they expire once windows are unredirected
    // If we don't destroy them here, looks like the resources are just
    // kept inaccessible somehow
    for (win *w = ps->list; w; w = w->next) {
      free_paint(ps, &w->paint);
    }

    xcb_composite_unredirect_subwindows(ps->c, ps->root, XCB_COMPOSITE_REDIRECT_MANUAL);
    // Unmap overlay window
    if (ps->overlay)
      xcb_unmap_window(ps->c, ps->overlay);

    // Must call XSync() here
    x_sync(ps->c);

    ps->redirected = false;
  }
}

// Handle queued events before we go to sleep
static void
handle_queued_x_events(EV_P_ ev_prepare *w, int revents) {
  session_t *ps = session_ptr(w, event_check);
  xcb_generic_event_t *ev;
  while ((ev = xcb_poll_for_queued_event(ps->c))) {
    ev_handle(ps, ev);
    free(ev);
  };
  // Flush because if we go into sleep when there is still
  // requests in the outgoing buffer, they will not be sent
  // for an indefinite amount of time.
  // Use XFlush here too, we might still use some Xlib functions
  // because OpenGL.
  XFlush(ps->dpy);
  xcb_flush(ps->c);
  int err = xcb_connection_has_error(ps->c);
  if (err) {
    log_fatal("X11 server connection broke (error %d)", err);
    exit(1);
  }
}

/**
 * Unredirection timeout callback.
 */
static void
tmout_unredir_callback(EV_P_ ev_timer *w, int revents) {
  session_t *ps = session_ptr(w, unredir_timer);
  ps->tmout_unredir_hit = true;
  queue_redraw(ps);
}

static void
fade_timer_callback(EV_P_ ev_timer *w, int revents) {
  session_t *ps = session_ptr(w, fade_timer);
  queue_redraw(ps);
}

static void
_draw_callback(EV_P_ session_t *ps, int revents) {
  if (ps->o.benchmark) {
    if (ps->o.benchmark_wid) {
      win *wi = find_win(ps, ps->o.benchmark_wid);
      if (!wi) {
        log_fatal("Couldn't find specified benchmark window.");
        exit(1);
      }
      add_damage_from_win(ps, wi);
    }
    else {
      force_repaint(ps);
    }
  }

  bool fade_running = false;
  win *t = paint_preprocess(ps, ps->list, &fade_running);
  ps->tmout_unredir_hit = false;

  // Start/stop fade timer depends on whether window are fading
  if (!fade_running && ev_is_active(&ps->fade_timer))
    ev_timer_stop(ps->loop, &ps->fade_timer);
  else if (fade_running && !ev_is_active(&ps->fade_timer)) {
    ev_timer_set(&ps->fade_timer, fade_timeout(ps), 0);
    ev_timer_start(ps->loop, &ps->fade_timer);
  }

  // If the screen is unredirected, free all_damage to stop painting
  if (ps->redirected && ps->o.stoppaint_force != ON) {
    static int paint = 0;
    paint_all(ps, t, false);

    paint++;
    if (ps->o.benchmark && paint >= ps->o.benchmark)
      exit(0);
  }

  if (!fade_running)
    ps->fade_time = 0L;

  ps->redraw_needed = false;
}

static void
draw_callback(EV_P_ ev_idle *w, int revents) {
  // This function is not used if we are using --swopti
  session_t *ps = session_ptr(w, draw_idle);

  _draw_callback(EV_A_ ps, revents);

  // Don't do painting non-stop unless we are in benchmark mode
  if (!ps->o.benchmark)
    ev_idle_stop(ps->loop, &ps->draw_idle);
}

static void
delayed_draw_timer_callback(EV_P_ ev_timer *w, int revents) {
  session_t *ps = session_ptr(w, delayed_draw_timer);
  _draw_callback(EV_A_ ps, revents);

  // We might have stopped the ev_idle in delayed_draw_callback,
  // so we restart it if we are in benchmark mode
  if (ps->o.benchmark)
    ev_idle_start(EV_A_ &ps->draw_idle);
}

static void
delayed_draw_callback(EV_P_ ev_idle *w, int revents) {
  // This function is only used if we are using --swopti
  session_t *ps = session_ptr(w, draw_idle);
  assert(ps->redraw_needed);
  assert(!ev_is_active(&ps->delayed_draw_timer));

  double delay = swopti_handle_timeout(ps);
  if (delay < 1e-6) {
    if (!ps->o.benchmark) {
      ev_idle_stop(ps->loop, &ps->draw_idle);
    }
    return _draw_callback(EV_A_ ps, revents);
  }

  // This is a little bit hacky. When we get to this point in code, we need
  // to update the screen , but we will only be updating after a delay, So
  // we want to stop the ev_idle, so this callback doesn't get call repeatedly
  // during the delay, we also want queue_redraw to not restart the ev_idle.
  // So we stop ev_idle and leave ps->redraw_needed to be true. (effectively,
  // ps->redraw_needed means if redraw is needed or if draw is in progress).
  //
  // We do this anyway even if we are in benchmark mode. That means we will
  // have to restart draw_idle after the draw actually happened when we are in
  // benchmark mode.
  ev_idle_stop(ps->loop, &ps->draw_idle);

  ev_timer_set(&ps->delayed_draw_timer, delay, 0);
  ev_timer_start(ps->loop, &ps->delayed_draw_timer);
}

static void
x_event_callback(EV_P_ ev_io *w, int revents) {
  session_t *ps = (session_t *)w;
  xcb_generic_event_t *ev = xcb_poll_for_event(ps->c);
  if (ev) {
    ev_handle(ps, ev);
    free(ev);
  }
}

/**
 * Turn on the program reset flag.
 *
 * This will result in compton resetting itself after next paint.
 */
static void
reset_enable(EV_P_ ev_signal *w, int revents) {
  session_t *ps = session_ptr(w, usr1_signal);
  log_info("compton is resetting...");
  ev_break(ps->loop, EVBREAK_ALL);
}

static void
exit_enable(EV_P_ ev_signal *w, int revents) {
  session_t *ps = session_ptr(w, int_signal);
  log_info("compton is quitting...");
  ps->quit = true;
  ev_break(ps->loop, EVBREAK_ALL);
}

/**
 * Initialize a session.
 *
 * @param argc number of commandline arguments
 * @param argv commandline arguments
 * @param dpy  the X Display
 * @param config_file the path to the config file
 * @param all_xerros whether we should report all X errors
 * @param fork whether we will fork after initialization
 */
static session_t *
session_init(int argc, char **argv, Display *dpy, const char *config_file,
             bool all_xerrors, bool fork) {
  static const session_t s_def = {
    .backend_data = NULL,
    .dpy = NULL,
    .scr = 0,
    .c = NULL,
    .vis = 0,
    .depth = 0,
    .root = XCB_NONE,
    .root_height = 0,
    .root_width = 0,
    // .root_damage = XCB_NONE,
    .overlay = XCB_NONE,
    .root_tile_fill = false,
    .root_tile_paint = PAINT_INIT,
    .tgt_picture = XCB_NONE,
    .tgt_buffer = PAINT_INIT,
    .reg_win = XCB_NONE,
#ifdef CONFIG_OPENGL
    .glx_prog_win = GLX_PROG_MAIN_INIT,
#endif
    .o = {
      .backend = BKEND_XRENDER,
      .glx_no_stencil = false,
      .mark_wmwin_focused = false,
      .mark_ovredir_focused = false,
      .detect_rounded_corners = false,
      .resize_damage = 0,
      .unredir_if_possible = false,
      .unredir_if_possible_blacklist = NULL,
      .unredir_if_possible_delay = 0,
      .redirected_force = UNSET,
      .stoppaint_force = UNSET,
      .dbus = false,
      .benchmark = 0,
      .benchmark_wid = XCB_NONE,
      .logpath = NULL,

      .refresh_rate = 0,
      .sw_opti = false,
      .vsync = VSYNC_NONE,
      .vsync_aggressive = false,

      .shadow_red = 0.0,
      .shadow_green = 0.0,
      .shadow_blue = 0.0,
      .shadow_radius = 18,
      .shadow_offset_x = -15,
      .shadow_offset_y = -15,
      .shadow_opacity = .75,
      .shadow_blacklist = NULL,
      .shadow_ignore_shaped = false,
      .respect_prop_shadow = false,
      .xinerama_shadow_crop = false,

      .fade_in_step = 0.028,
      .fade_out_step = 0.03,
      .fade_delta = 10,
      .no_fading_openclose = false,
      .no_fading_destroyed_argb = false,
      .fade_blacklist = NULL,

      .inactive_opacity = 1.0,
      .inactive_opacity_override = false,
      .active_opacity = 1.0,
      .frame_opacity = 1.0,
      .detect_client_opacity = false,

      .blur_background = false,
      .blur_background_frame = false,
      .blur_background_fixed = false,
      .blur_background_blacklist = NULL,
      .blur_kerns = { NULL },
      .inactive_dim = 0.0,
      .inactive_dim_fixed = false,
      .invert_color_list = NULL,
      .opacity_rules = NULL,

      .use_ewmh_active_win = false,
      .focus_blacklist = NULL,
      .detect_transient = false,
      .detect_client_leader = false,

      .track_focus = false,
      .track_wdata = false,
      .track_leader = false,
    },

    .time_start = { 0, 0 },
    .redirected = false,
    .alpha_picts = NULL,
    .fade_time = 0L,
    .ignore_head = NULL,
    .ignore_tail = NULL,
    .quit = false,

    .expose_rects = NULL,
    .size_expose = 0,
    .n_expose = 0,

    .list = NULL,
    .active_win = NULL,
    .active_leader = XCB_NONE,

    .black_picture = XCB_NONE,
    .cshadow_picture = XCB_NONE,
    .white_picture = XCB_NONE,
    .gaussian_map = NULL,

    .refresh_rate = 0,
    .refresh_intv = 0UL,
    .paint_tm_offset = 0L,

#ifdef CONFIG_VSYNC_DRM
    .drm_fd = -1,
#endif

    .xfixes_event = 0,
    .xfixes_error = 0,
    .damage_event = 0,
    .damage_error = 0,
    .render_event = 0,
    .render_error = 0,
    .composite_event = 0,
    .composite_error = 0,
    .composite_opcode = 0,
    .has_name_pixmap = false,
    .shape_exists = false,
    .shape_event = 0,
    .shape_error = 0,
    .randr_exists = 0,
    .randr_event = 0,
    .randr_error = 0,
#ifdef CONFIG_OPENGL
    .glx_exists = false,
    .glx_event = 0,
    .glx_error = 0,
#endif
    .xrfilter_convolution_exists = false,

    .atom_opacity = XCB_NONE,
    .atom_frame_extents = XCB_NONE,
    .atom_client = XCB_NONE,
    .atom_name = XCB_NONE,
    .atom_name_ewmh = XCB_NONE,
    .atom_class = XCB_NONE,
    .atom_role = XCB_NONE,
    .atom_transient = XCB_NONE,
    .atom_ewmh_active_win = XCB_NONE,
    .atom_compton_shadow = XCB_NONE,
    .atom_win_type = XCB_NONE,
    .atoms_wintypes = { 0 },
    .track_atom_lst = NULL,

#ifdef CONFIG_DBUS
    .dbus_data = NULL,
#endif
  };

  log_init_tls();
  auto stderr_logger = stderr_logger_new();
  if (stderr_logger) {
    // stderr logger might fail to create if we are already
    // daemonized.
    log_add_target_tls(stderr_logger);
  }

  // Allocate a session and copy default values into it
  session_t *ps = cmalloc(session_t);
  *ps = s_def;
  ps->loop = EV_DEFAULT;
  pixman_region32_init(&ps->screen_reg);

  ps->ignore_tail = &ps->ignore_head;
  gettimeofday(&ps->time_start, NULL);

  ps->o.show_all_xerrors = all_xerrors;

  // Use the same Display across reset, primarily for resource leak checking
  ps->dpy = dpy;
  ps->c = XGetXCBConnection(ps->dpy);

  const xcb_query_extension_reply_t *ext_info;

  XSetErrorHandler(xerror);

  ps->scr = DefaultScreen(ps->dpy);
  ps->root = RootWindow(ps->dpy, ps->scr);

  ps->vis = XVisualIDFromVisual(DefaultVisual(ps->dpy, ps->scr));
  ps->depth = DefaultDepth(ps->dpy, ps->scr);

  // Start listening to events on root earlier to catch all possible
  // root geometry changes
  auto e = xcb_request_check(
      ps->c, xcb_change_window_attributes_checked(
           ps->c, ps->root, XCB_CW_EVENT_MASK,
           (const uint32_t[]){XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                              XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                              XCB_EVENT_MASK_PROPERTY_CHANGE}));
  if (e) {
    log_error("Failed to setup root window event mask");
    free(e);
  }

  ps->root_width = DisplayWidth(ps->dpy, ps->scr);
  ps->root_height = DisplayHeight(ps->dpy, ps->scr);

  xcb_prefetch_extension_data(ps->c, &xcb_render_id);
  xcb_prefetch_extension_data(ps->c, &xcb_composite_id);
  xcb_prefetch_extension_data(ps->c, &xcb_damage_id);
  xcb_prefetch_extension_data(ps->c, &xcb_shape_id);
  xcb_prefetch_extension_data(ps->c, &xcb_xfixes_id);
  xcb_prefetch_extension_data(ps->c, &xcb_randr_id);
  xcb_prefetch_extension_data(ps->c, &xcb_xinerama_id);
  xcb_prefetch_extension_data(ps->c, &xcb_present_id);
  xcb_prefetch_extension_data(ps->c, &xcb_sync_id);

  ext_info = xcb_get_extension_data(ps->c, &xcb_render_id);
  if (!ext_info || !ext_info->present) {
    log_fatal("No render extension");
    exit(1);
  }
  ps->render_event = ext_info->first_event;
  ps->render_error = ext_info->first_error;

  ext_info = xcb_get_extension_data(ps->c, &xcb_composite_id);
  if (!ext_info || !ext_info->present) {
    log_fatal("No composite extension");
    exit(1);
  }
  ps->composite_opcode = ext_info->major_opcode;
  ps->composite_event = ext_info->first_event;
  ps->composite_error = ext_info->first_error;

  {
    xcb_composite_query_version_reply_t *reply =
      xcb_composite_query_version_reply(ps->c,
          xcb_composite_query_version(ps->c, XCB_COMPOSITE_MAJOR_VERSION, XCB_COMPOSITE_MINOR_VERSION),
          NULL);

    if (reply && (reply->major_version > 0 || reply->minor_version >= 2)) {
      ps->has_name_pixmap = true;
    }
    free(reply);
  }

  ext_info = xcb_get_extension_data(ps->c, &xcb_damage_id);
  if (!ext_info || !ext_info->present) {
    log_fatal("No damage extension");
    exit(1);
  }
  ps->damage_event = ext_info->first_event;
  ps->damage_error = ext_info->first_error;
  xcb_discard_reply(ps->c,
      xcb_damage_query_version(ps->c, XCB_DAMAGE_MAJOR_VERSION, XCB_DAMAGE_MINOR_VERSION).sequence);

  ext_info = xcb_get_extension_data(ps->c, &xcb_xfixes_id);
  if (!ext_info || !ext_info->present) {
    log_fatal("No XFixes extension");
    exit(1);
  }
  ps->xfixes_event = ext_info->first_event;
  ps->xfixes_error = ext_info->first_error;
  xcb_discard_reply(ps->c,
      xcb_xfixes_query_version(ps->c, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION).sequence);

  // Parse configuration file
  win_option_mask_t winopt_mask[NUM_WINTYPES] = {{0}};
  bool shadow_enabled = false, fading_enable = false, hasneg = false;
  char *config_file_to_free = NULL;
  config_file = config_file_to_free =
    parse_config(&ps->o, config_file, &shadow_enabled,
                 &fading_enable, &hasneg, winopt_mask);

  if (IS_ERR(config_file_to_free)) {
    return NULL;
  }

  // Parse all of the rest command line options
  get_cfg(&ps->o, argc, argv, shadow_enabled, fading_enable, hasneg, winopt_mask);

  if (ps->o.logpath) {
    auto l = file_logger_new(ps->o.logpath);
    if (l) {
      log_info("Switching to log file: %s", ps->o.logpath);
      if (stderr_logger) {
        log_remove_target_tls(stderr_logger);
        stderr_logger = NULL;
      }
      log_add_target_tls(l);
      stderr_logger = NULL;
    } else {
      log_error("Failed to setup log file %s, I will keep using stderr", ps->o.logpath);
    }
  }

  // Get needed atoms for c2 condition lists
  if (!(c2_list_postprocess(ps, ps->o.unredir_if_possible_blacklist) &&
        c2_list_postprocess(ps, ps->o.paint_blacklist) &&
        c2_list_postprocess(ps, ps->o.shadow_blacklist) &&
        c2_list_postprocess(ps, ps->o.fade_blacklist) &&
        c2_list_postprocess(ps, ps->o.blur_background_blacklist) &&
        c2_list_postprocess(ps, ps->o.invert_color_list) &&
        c2_list_postprocess(ps, ps->o.opacity_rules) &&
        c2_list_postprocess(ps, ps->o.focus_blacklist))) {
    log_error("Post-processing of conditionals failed, some of your rules might not work");
  }

  rebuild_shadow_exclude_reg(ps);

  // Query X Shape
  ext_info = xcb_get_extension_data(ps->c, &xcb_shape_id);
  if (ext_info && ext_info->present) {
    ps->shape_event = ext_info->first_event;
    ps->shape_error = ext_info->first_error;
    ps->shape_exists = true;
  }

  ext_info = xcb_get_extension_data(ps->c, &xcb_randr_id);
  if (ext_info && ext_info->present) {
    ps->randr_exists = true;
    ps->randr_event = ext_info->first_event;
    ps->randr_error = ext_info->first_error;
  }

  ext_info = xcb_get_extension_data(ps->c, &xcb_present_id);
  if (ext_info && ext_info->present) {
    auto r =
      xcb_present_query_version_reply(ps->c,
                                      xcb_present_query_version(ps->c,
                                                                XCB_PRESENT_MAJOR_VERSION,
                                                                XCB_PRESENT_MINOR_VERSION),
                                      NULL);
    if (r) {
      ps->present_exists = true;
      free(r);
    }
  }

  // Query X Sync
  ext_info = xcb_get_extension_data(ps->c, &xcb_sync_id);
  if (ext_info && ext_info->present) {
    ps->xsync_error = ext_info->first_error;
    ps->xsync_event = ext_info->first_event;
    // Need X Sync 3.1 for fences
    auto r = xcb_sync_initialize_reply(ps->c,
                                       xcb_sync_initialize(ps->c,
                                                           XCB_SYNC_MAJOR_VERSION,
                                                           XCB_SYNC_MINOR_VERSION),
                                       NULL);
    if (r && (r->major_version > 3 ||
              (r->major_version == 3 && r->minor_version >= 1))) {
      ps->xsync_exists = true;
      free(r);
    }
  }

  ps->sync_fence = XCB_NONE;
  if (!ps->xsync_exists && ps->o.xrender_sync_fence) {
    log_error("XSync extension not found. No XSync fence sync is "
              "possible. (xrender-sync-fence can't be enabled)");
    ps->o.xrender_sync_fence = false;
  }

  if (ps->o.xrender_sync_fence) {
    ps->sync_fence = xcb_generate_id(ps->c);
    e = xcb_request_check(ps->c, xcb_sync_create_fence(ps->c, ps->root, ps->sync_fence, 0));
    if (e) {
      log_error("Failed to create a XSync fence. xrender-sync-fence will be disabled");
      ps->o.xrender_sync_fence = false;
      ps->sync_fence = XCB_NONE;
      free(e);
    }
  }

  // Query X RandR
  if ((ps->o.sw_opti && !ps->o.refresh_rate) || ps->o.xinerama_shadow_crop) {
    if (!ps->randr_exists) {
      log_fatal("No XRandR extension. sw-opti, refresh-rate or xinerama-shadow-crop "
                "cannot be enabled.");
      exit(1);
    }
  }

  // Query X Xinerama extension
  if (ps->o.xinerama_shadow_crop) {
#ifdef CONFIG_XINERAMA
    ext_info = xcb_get_extension_data(ps->c, &xcb_xinerama_id);
    ps->xinerama_exists = ext_info && ext_info->present;
#else
    log_fatal("Xinerama support not compiled in. xinerama-shadow-crop cannot be enabled");
    exit(1);
#endif
  }

  rebuild_screen_reg(ps);

  // Overlay must be initialized before double buffer, and before creation
  // of OpenGL context.
  init_overlay(ps);

  // Initialize filters, must be preceded by OpenGL context creation
  if (!init_render(ps)) {
    log_fatal("Failed to initialize the backend");
    exit(1);
  }

  if (ps->o.print_diagnostics) {
    print_diagnostics(ps, config_file);
    free(config_file_to_free);
    exit(0);
  }
  free(config_file_to_free);

  if (bkend_use_glx(ps)) {
    auto glx_logger = glx_string_marker_logger_new();
    if (glx_logger) {
      log_info("Enabling gl string marker");
      log_add_target_tls(glx_logger);
    }
  }

  // Initialize software optimization
  if (ps->o.sw_opti)
    ps->o.sw_opti = swopti_init(ps);

  // Monitor screen changes if vsync_sw is enabled and we are using
  // an auto-detected refresh rate, or when Xinerama features are enabled
  if (ps->randr_exists && ((ps->o.sw_opti && !ps->o.refresh_rate)
        || ps->o.xinerama_shadow_crop))
    xcb_randr_select_input(ps->c, ps->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);

  cxinerama_upd_scrs(ps);

  // Create registration window
  if (!ps->reg_win && !register_cm(ps))
    exit(1);

  init_atoms(ps);

  {
    xcb_render_create_picture_value_list_t pa = {
      .subwindowmode = IncludeInferiors,
    };

    ps->root_picture = x_create_picture_with_visual_and_pixmap(ps->c,
      ps->vis, ps->root, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
    if (ps->overlay != XCB_NONE) {
      ps->tgt_picture = x_create_picture_with_visual_and_pixmap(ps->c,
        ps->vis, ps->overlay, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
    } else
      ps->tgt_picture = ps->root_picture;
  }

  ev_io_init(&ps->xiow, x_event_callback, ConnectionNumber(ps->dpy), EV_READ);
  ev_io_start(ps->loop, &ps->xiow);
  ev_init(&ps->unredir_timer, tmout_unredir_callback);
  if (ps->o.sw_opti)
    ev_idle_init(&ps->draw_idle, delayed_draw_callback);
  else
    ev_idle_init(&ps->draw_idle, draw_callback);

  ev_init(&ps->fade_timer, fade_timer_callback);
  ev_init(&ps->delayed_draw_timer, delayed_draw_timer_callback);

  // Set up SIGUSR1 signal handler to reset program
  ev_signal_init(&ps->usr1_signal, reset_enable, SIGUSR1);
  ev_signal_init(&ps->int_signal, exit_enable, SIGINT);
  ev_signal_start(ps->loop, &ps->usr1_signal);
  ev_signal_start(ps->loop, &ps->int_signal);

  // xcb can read multiple events from the socket when a request with reply is
  // made.
  //
  // Use an ev_prepare to make sure we cannot accidentally forget to handle them
  // before we go to sleep.
  //
  // If we don't drain the queue before goes to sleep (i.e. blocking on socket
  // input), we will be sleeping with events available in queue. Which might
  // cause us to block indefinitely because arrival of new events could be
  // dependent on processing of existing events (e.g. if we don't process damage
  // event and do damage subtract, new damage event won't be generated).
  //
  // So we make use of a ev_prepare handle, which is called right before libev
  // goes into sleep, to handle all the queued X events.
  ev_prepare_init(&ps->event_check, handle_queued_x_events);
  // Make sure nothing can cause xcb to read from the X socket after events are
  // handled and before we going to sleep.
  ev_set_priority(&ps->event_check, EV_MINPRI);
  ev_prepare_start(ps->loop, &ps->event_check);

  xcb_grab_server(ps->c);

  // Initialize DBus. We need to do this early, because add_win might call dbus functions
  if (ps->o.dbus) {
#ifdef CONFIG_DBUS
    cdbus_init(ps, DisplayString(ps->dpy));
    if (!ps->dbus_data) {
      ps->o.dbus = false;
    }
#else
    log_fatal("DBus support not compiled in!");
    exit(1);
#endif
  }

  {
    xcb_window_t *children;
    int nchildren;

    xcb_query_tree_reply_t *reply = xcb_query_tree_reply(ps->c,
        xcb_query_tree(ps->c, ps->root), NULL);

    if (reply) {
      children = xcb_query_tree_children(reply);
      nchildren = xcb_query_tree_children_length(reply);
    } else {
      children = NULL;
      nchildren = 0;
    }

    for (int i = 0; i < nchildren; i++) {
      add_win(ps, children[i], i ? children[i-1] : XCB_NONE);
    }

    free(reply);
  }

  if (ps->o.track_focus) {
    recheck_focus(ps);
  }

  e = xcb_request_check(ps->c, xcb_ungrab_server(ps->c));
  if (e) {
    log_error("Failed to ungrad server");
    free(e);
  }

  write_pid(ps);

  if (fork && stderr_logger) {
    // Remove the stderr logger if we will fork
    log_remove_target_tls(stderr_logger);
  }
  return ps;
}

/**
 * Destroy a session.
 *
 * Does not close the X connection or free the <code>session_t</code>
 * structure, though.
 *
 * @param ps session to destroy
 */
static void
session_destroy(session_t *ps) {
  redir_stop(ps);

  // Stop listening to events on root window
  xcb_change_window_attributes(ps->c, ps->root, XCB_CW_EVENT_MASK,
      (const uint32_t[]) { 0 });

#ifdef CONFIG_DBUS
  // Kill DBus connection
  if (ps->o.dbus) {
    assert(ps->dbus_data);
    cdbus_destroy(ps);
  }
#endif

  // Free window linked list
  {
    win *next = NULL;
    win *list = ps->list;
    ps->list = NULL;

    for (win *w = list; w; w = next) {
      next = w->next;

      if (w->a.map_state == XCB_MAP_STATE_VIEWABLE && !w->destroying)
        win_ev_stop(ps, w);

      free_win_res(ps, w);
      free(w);
    }
  }

  // Free blacklists
  free_wincondlst(&ps->o.shadow_blacklist);
  free_wincondlst(&ps->o.fade_blacklist);
  free_wincondlst(&ps->o.focus_blacklist);
  free_wincondlst(&ps->o.invert_color_list);
  free_wincondlst(&ps->o.blur_background_blacklist);
  free_wincondlst(&ps->o.opacity_rules);
  free_wincondlst(&ps->o.paint_blacklist);
  free_wincondlst(&ps->o.unredir_if_possible_blacklist);

  // Free tracked atom list
  {
    latom_t *next = NULL;
    for (latom_t *this = ps->track_atom_lst; this; this = next) {
      next = this->next;
      free(this);
    }

    ps->track_atom_lst = NULL;
  }

  // Free ignore linked list
  {
    ignore_t *next = NULL;
    for (ignore_t *ign = ps->ignore_head; ign; ign = next) {
      next = ign->next;

      free(ign);
    }

    // Reset head and tail
    ps->ignore_head = NULL;
    ps->ignore_tail = &ps->ignore_head;
  }

  // Free tgt_{buffer,picture} and root_picture
  if (ps->tgt_buffer.pict == ps->tgt_picture)
    ps->tgt_buffer.pict = XCB_NONE;

  if (ps->tgt_picture == ps->root_picture)
    ps->tgt_picture = XCB_NONE;
  else
    free_picture(ps->c, &ps->tgt_picture);

  free_picture(ps->c, &ps->root_picture);
  free_paint(ps, &ps->tgt_buffer);

  pixman_region32_fini(&ps->screen_reg);
  free(ps->expose_rects);

  free(ps->o.write_pid_path);
  free(ps->o.logpath);
  for (int i = 0; i < MAX_BLUR_PASS; ++i) {
    free(ps->o.blur_kerns[i]);
    free(ps->blur_kerns_cache[i]);
  }
  free(ps->o.glx_fshader_win_str);
  free_xinerama_info(ps);

  deinit_render(ps);

#ifdef CONFIG_VSYNC_DRM
  // Close file opened for DRM VSync
  if (ps->drm_fd >= 0) {
    close(ps->drm_fd);
    ps->drm_fd = -1;
  }
#endif

  // Release overlay window
  if (ps->overlay) {
    xcb_composite_release_overlay_window(ps->c, ps->overlay);
    ps->overlay = XCB_NONE;
  }

  if (ps->sync_fence) {
    xcb_sync_destroy_fence(ps->c, ps->sync_fence);
    ps->sync_fence = XCB_NONE;
  }

  // Free reg_win
  if (ps->reg_win) {
    xcb_destroy_window(ps->c, ps->reg_win);
    ps->reg_win = XCB_NONE;
  }

  // Flush all events
  x_sync(ps->c);
  ev_io_stop(ps->loop, &ps->xiow);

#ifdef DEBUG_XRC
  // Report about resource leakage
  xrc_report_xid();
#endif

  // Stop libev event handlers
  ev_timer_stop(ps->loop, &ps->unredir_timer);
  ev_timer_stop(ps->loop, &ps->fade_timer);
  ev_idle_stop(ps->loop, &ps->draw_idle);
  ev_prepare_stop(ps->loop, &ps->event_check);
  ev_signal_stop(ps->loop, &ps->usr1_signal);
  ev_signal_stop(ps->loop, &ps->int_signal);

  log_deinit_tls();
}

/**
 * Do the actual work.
 *
 * @param ps current session
 */
static void
session_run(session_t *ps) {
  if (ps->o.sw_opti)
    ps->paint_tm_offset = get_time_timeval().tv_usec;

  // In benchmark mode, we want draw_idle handler to always be active
  if (ps->o.benchmark) {
    ev_idle_start(ps->loop, &ps->draw_idle);
  } else {
    // Let's draw our first frame!
    queue_redraw(ps);
  }
  ev_run(ps->loop, 0);
}

/**
 * The function that everybody knows.
 */
int
main(int argc, char **argv) {
  // Set locale so window names with special characters are interpreted
  // correctly
  setlocale(LC_ALL, "");

  int exit_code;
  char *config_file = NULL;
  bool all_xerrors = false, need_fork = false;
  if (get_early_config(argc, argv, &config_file, &all_xerrors, &need_fork, &exit_code)) {
    return exit_code;
  }

  int pfds[2];
  if (need_fork) {
    if (pipe2(pfds, O_CLOEXEC)) {
      perror("pipe2");
      return 1;
    }
    auto pid = fork();
    if (pid < 0) {
      perror("fork");
      return 1;
    }
    if (pid > 0) {
      // We are the parent
      close(pfds[1]);
      // We wait for the child to tell us it has finished initialization
      // by sending us something via the pipe.
      int tmp;
      if (read(pfds[0], &tmp, sizeof tmp) <= 0) {
        // Failed to read, the child has most likely died
        // We can probably waitpid() here.
        return 1;
      } else {
        // We are done
        return 0;
      }
    }
    // We are the child
    close(pfds[0]);
  }

  // Main loop
  bool quit = false;
  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) {
    log_fatal("Can't open display.");
    return 1;
  }
  XSetEventQueueOwner(dpy, XCBOwnsEventQueue);

  do {
    ps_g = session_init(argc, argv, dpy, config_file, all_xerrors, need_fork);
    if (!ps_g) {
      log_fatal("Failed to create new compton session.");
      return 1;
    }
    if (need_fork) {
      // Finishing up daemonization
      // Close files
      if (fclose(stdout) || fclose(stderr) || fclose(stdin)) {
        log_fatal("Failed to close standard input/output");
        return 1;
      }
      // Make us the session and process group leader so we don't get killed when
      // our parent die.
      setsid();
      // Notify the parent that we are done. This might cause the parent to quit,
      // so only do this after setsid()
      int tmp = 1;
      write(pfds[1], &tmp, sizeof tmp);
      close(pfds[1]);
      // We only do this once
      need_fork = false;
    }
    session_run(ps_g);
    quit = ps_g->quit;
    session_destroy(ps_g);
    free(ps_g);
    ps_g = NULL;
  } while (!quit);

  if (dpy) {
    XCloseDisplay(dpy);
  }
  free(config_file);

  return 0;
}

// vim: set et sw=2 :
