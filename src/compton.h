// SPDX-License-Identifier: MIT
// Copyright (c)

// Throw everything in here.


// === Includes ===

#include <math.h>
#include <sys/select.h>
#include <limits.h>
#include <unistd.h>
#include <getopt.h>
#include <locale.h>
#include <signal.h>

#ifdef CONFIG_VSYNC_DRM
#include <fcntl.h>
// We references some definitions in drm.h, which could also be found in
// /usr/src/linux/include/drm/drm.h, but that path is probably even less
// reliable than libdrm
#include <drm.h>
#include <sys/ioctl.h>
#include <errno.h>
#endif

#include <X11/Xutil.h>
#include <pixman.h>
#ifdef CONFIG_OPENGL
#include "opengl.h" // XXX clean up
#endif
#include "common.h"
#include "win.h"
#include "x.h"
#include "c2.h"

// == Functions ==
// TODO move static inline functions that are only used in compton.c, into
//      compton.c

// inline functions must be made static to compile correctly under clang:
// http://clang.llvm.org/compatibility.html#inline

void add_damage(session_t *ps, const region_t *damage);

long determine_evmask(session_t *ps, Window wid, win_evmode_t mode);

xcb_window_t
find_client_win(session_t *ps, xcb_window_t w);

win *find_toplevel2(session_t *ps, Window wid);

void map_win(session_t *ps, Window id);

/**
 * Reset filter on a <code>Picture</code>.
 */
static inline void
xrfilter_reset(session_t *ps, xcb_render_picture_t p) {
#define FILTER "Nearest"
  xcb_render_set_picture_filter(ps->c, p, strlen(FILTER), FILTER, 0, NULL);
#undef FILTER
}

/**
 * Subtract two unsigned long values.
 *
 * Truncate to 0 if the result is negative.
 */
static inline unsigned long __attribute__((const))
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
array_wid_exists(const Window *arr, int count, Window wid) {
  while (count--) {
    if (arr[count] == wid) {
      return true;
    }
  }

  return false;
}

/**
 * Destroy a <code>Picture</code>.
 */
inline static void
free_picture(session_t *ps, xcb_render_picture_t *p) {
  if (*p) {
    xcb_render_free_picture(ps->c, *p);
    *p = None;
  }
}

/**
 * Destroy a condition list.
 */
static inline void
free_wincondlst(c2_lptr_t **pcondlst) {
  while ((*pcondlst = c2_free_lptr(*pcondlst)))
    continue;
}

#ifdef CONFIG_OPENGL
/**
 * Bind texture in paint_t if we are using GLX backend.
 */
static inline bool
paint_bind_tex(session_t *ps, paint_t *ppaint,
  unsigned wid, unsigned hei, unsigned depth, bool force)
{
  if (!ppaint->pixmap)
    return false;

  if (force || !glx_tex_binded(ppaint->ptex, ppaint->pixmap))
    return glx_bind_pixmap(ps, &ppaint->ptex, ppaint->pixmap, wid, hei, depth);

  return true;
}
#else
static inline bool
paint_bind_tex(session_t *ps, paint_t *ppaint,
  unsigned wid, unsigned hei, unsigned depth, bool force)
{
  return true;
}
static inline void
free_paint_glx(session_t *ps, paint_t *p) {}
static inline void
free_win_res_glx(session_t *ps, win *w) {}
static inline void
free_texture(session_t *ps, glx_texture_t **t) {
  assert(!*t);
}
#endif

/**
 * Free paint_t.
 */
static inline void
free_paint(session_t *ps, paint_t *ppaint) {
  free_paint_glx(ps, ppaint);
  free_picture(ps, &ppaint->pict);
  if (ppaint->pixmap)
    xcb_free_pixmap(ps->c, ppaint->pixmap);
  ppaint->pixmap = XCB_NONE;
}

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
wid_set_text_prop(session_t *ps, Window wid, Atom prop_atom, char *str) {
  XTextProperty *pprop = make_text_prop(ps, str);
  if (!pprop) {
    printf_errf("(\"%s\"): Failed to make text property.", str);
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
dump_drawable(session_t *ps, Drawable drawable) {
  Window rroot = None;
  int x = 0, y = 0;
  unsigned width = 0, height = 0, border = 0, depth = 0;
  if (XGetGeometry(ps->dpy, drawable, &rroot, &x, &y, &width, &height,
        &border, &depth)) {
    printf_dbgf("(%#010lx): x = %u, y = %u, wid = %u, hei = %d, b = %u, d = %u\n", drawable, x, y, width, height, border, depth);
  }
  else {
    printf_dbgf("(%#010lx): Failed\n", drawable);
  }
}


/**
 * Validate a pixmap.
 *
 * Detect whether the pixmap is valid with XGetGeometry. Well, maybe there
 * are better ways.
 */
static inline bool
validate_pixmap(session_t *ps, xcb_pixmap_t pxmap) {
  if (!pxmap) return false;

  Window rroot = None;
  int rx = 0, ry = 0;
  unsigned rwid = 0, rhei = 0, rborder = 0, rdepth = 0;
  return XGetGeometry(ps->dpy, pxmap, &rroot, &rx, &ry,
        &rwid, &rhei, &rborder, &rdepth) && rwid && rhei;
}

/**
 * Validate pixmap of a window, and destroy pixmap and picture if invalid.
 */
static inline void
win_validate_pixmap(session_t *ps, win *w) {
  // Destroy pixmap and picture, if invalid
  if (!validate_pixmap(ps, w->paint.pixmap))
    free_paint(ps, &w->paint);
}

/**
 * Normalize a convolution kernel.
 */
static inline void
normalize_conv_kern(int wid, int hei, xcb_render_fixed_t *kern) {
  double sum = 0.0;
  for (int i = 0; i < wid * hei; ++i)
    sum += XFIXED_TO_DOUBLE(kern[i]);
  double factor = 1.0 / sum;
  for (int i = 0; i < wid * hei; ++i)
    kern[i] = DOUBLE_TO_XFIXED(XFIXED_TO_DOUBLE(kern[i]) * factor);
}

#ifdef CONFIG_OPENGL
/**
 * Ensure we have a GLX context.
 */
static inline bool
ensure_glx_context(session_t *ps) {
  // Create GLX context
  if (!glx_has_context(ps))
    glx_init(ps, false);

  return ps->psglx->context;
}
#endif

// vim: set et sw=2 :
