// SPDX-License-Identifier: MIT
// Copyright (c)

// Throw everything in here.
// !!! DON'T !!!

// === Includes ===

#include <stdlib.h>
#include <stdbool.h>
#include <locale.h>
#include <xcb/xproto.h>

#include <X11/Xutil.h>
#include "common.h"
#include "win.h"
#include "x.h"
#include "c2.h"
#include "log.h" // XXX clean up
#include "region.h"
#include "compiler.h"
#include "types.h"
#include "utils.h"
#include "render.h"
#include "config.h"

// == Functions ==
// TODO move static inline functions that are only used in compton.c, into
//      compton.c

// inline functions must be made static to compile correctly under clang:
// http://clang.llvm.org/compatibility.html#inline

void add_damage(session_t *ps, const region_t *damage);

long determine_evmask(session_t *ps, xcb_window_t wid, win_evmode_t mode);

xcb_window_t
find_client_win(session_t *ps, xcb_window_t w);

win *find_toplevel2(session_t *ps, xcb_window_t wid);

void map_win(session_t *ps, xcb_window_t id);

/**
 * Subtract two unsigned long values.
 *
 * Truncate to 0 if the result is negative.
 */
static inline unsigned long attr_const
sub_unslong(unsigned long a, unsigned long b) {
  return (a > b) ? a - b : 0;
}

/**
 * Set a <code>switch_t</code> array of all unset wintypes to true.
 */
static inline void
wintype_arr_enable_unset(switch_t arr[]) {
  wintype_t i;

  for (i = 0; i < NUM_WINTYPES; ++i)
    if (UNSET == arr[i])
      arr[i] = ON;
}

/**
 * Check if a window ID exists in an array of window IDs.
 *
 * @param arr the array of window IDs
 * @param count amount of elements in the array
 * @param wid window ID to search for
 */
static inline bool
array_wid_exists(const xcb_window_t *arr, int count, xcb_window_t wid) {
  while (count--) {
    if (arr[count] == wid) {
      return true;
    }
  }

  return false;
}

/**
 * Destroy a condition list.
 */
static inline void
free_wincondlst(c2_lptr_t **pcondlst) {
  while ((*pcondlst = c2_free_lptr(*pcondlst)))
    continue;
}

#ifndef CONFIG_OPENGL
static inline void
free_paint_glx(session_t *ps, paint_t *p) {}
static inline void
free_win_res_glx(session_t *ps, win *w) {}
#endif

/**
 * Create a XTextProperty of a single string.
 */
static inline XTextProperty *
make_text_prop(session_t *ps, char *str) {
  XTextProperty *pprop = ccalloc(1, XTextProperty);

  if (XmbTextListToTextProperty(ps->dpy, &str, 1,  XStringStyle, pprop)) {
    cxfree(pprop->value);
    free(pprop);
    pprop = NULL;
  }

  return pprop;
}


/**
 * Set a single-string text property on a window.
 */
static inline bool
wid_set_text_prop(session_t *ps, xcb_window_t wid, xcb_atom_t prop_atom, char *str) {
  XTextProperty *pprop = make_text_prop(ps, str);
  if (!pprop) {
    log_error("Failed to make text property: %s.", str);
    return false;
  }

  XSetTextProperty(ps->dpy, wid, pprop, prop_atom);
  cxfree(pprop->value);
  cxfree(pprop);

  return true;
}

/**
 * Dump an drawable's info.
 */
static inline void
dump_drawable(session_t *ps, xcb_drawable_t drawable) {
  auto r = xcb_get_geometry_reply(ps->c, xcb_get_geometry(ps->c, drawable), NULL);
  if (!r) {
    log_trace("Drawable %#010x: Failed", drawable);
    return;
  }
  log_trace("Drawable %#010x: x = %u, y = %u, wid = %u, hei = %d, b = %u, d = %u",
            drawable, r->x, r->y, r->width, r->height, r->border_width, r->depth);
  free(r);
}

// vim: set et sw=2 :
