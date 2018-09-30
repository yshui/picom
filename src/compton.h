/**
 * compton.h
 */

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

#include <pixman.h>
#ifdef CONFIG_OPENGL
#include "opengl.h" // XXX clean up
#endif
#include "common.h"
#include "x.h"
#include "c2.h"

// == Functions ==
// TODO move static inline functions that are only used in compton.c, into
//      compton.c

// inline functions must be made static to compile correctly under clang:
// http://clang.llvm.org/compatibility.html#inline

void add_damage(session_t *ps, const region_t *damage);

long determine_evmask(session_t *ps, Window wid, win_evmode_t mode);

Window
find_client_win(session_t *ps, Window w);

win *find_toplevel2(session_t *ps, Window wid);

void map_win(session_t *ps, Window id);

void
render(session_t *ps, int x, int y, int dx, int dy, int wid, int hei,
    double opacity, bool argb, bool neg,
    xcb_render_picture_t pict, glx_texture_t *ptex,
    const region_t *reg_paint, const glx_prog_main_t *pprogram);

/**
 * Reset filter on a <code>Picture</code>.
 */
static inline void
xrfilter_reset(session_t *ps, xcb_render_picture_t p) {
#define FILTER "Nearest"
  xcb_connection_t *c = XGetXCBConnection(ps->dpy);
  xcb_render_set_picture_filter(c, p, strlen(FILTER), FILTER, 0, NULL);
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
    xcb_connection_t *c = XGetXCBConnection(ps->dpy);
    xcb_render_free_picture(c, *p);
    *p = None;
  }
}

/**
 * Destroy a <code>Damage</code>.
 */
inline static void
free_damage(session_t *ps, xcb_damage_damage_t *p) {
  if (*p) {
    // BadDamage will be thrown if the window is destroyed
    set_ignore_cookie(ps,
        xcb_damage_destroy(XGetXCBConnection(ps->dpy), *p));
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

/**
 * Free Xinerama screen info.
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
  free_pixmap(ps, &ppaint->pixmap);
}

/**
 * Free w->paint.
 */
static inline void
free_wpaint(session_t *ps, win *w) {
  free_paint(ps, &w->paint);
  free_fence(ps, &w->fence);
}

/**
 * Destroy all resources in a <code>struct _win</code>.
 */
static inline void
free_win_res(session_t *ps, win *w) {
  free_win_res_glx(ps, w);
  free_paint(ps, &w->paint);
  pixman_region32_fini(&w->bounding_shape);
  free_paint(ps, &w->shadow_paint);
  free_damage(ps, &w->damage);
  rc_region_unref(&w->reg_ignore);
  free(w->name);
  free(w->class_instance);
  free(w->class_general);
  free(w->role);
}

/**
 * Free root tile related things.
 */
static inline void
free_root_tile(session_t *ps) {
  free_picture(ps, &ps->root_tile_paint.pict);
  free_texture(ps, &ps->root_tile_paint.ptex);
  if (ps->root_tile_fill)
    free_pixmap(ps, &ps->root_tile_paint.pixmap);
  ps->root_tile_paint.pixmap = None;
  ps->root_tile_fill = false;
}

/**
 * Get current system clock in milliseconds.
 */
static inline time_ms_t
get_time_ms(void) {
  struct timeval tv;

  gettimeofday(&tv, NULL);

  return tv.tv_sec % SEC_WRAP * 1000 + tv.tv_usec / 1000;
}

/**
 * Convert time from milliseconds to struct timeval.
 */
static inline struct timeval
ms_to_tv(int timeout) {
  return (struct timeval) {
    .tv_sec = timeout / MS_PER_SEC,
    .tv_usec = timeout % MS_PER_SEC * (US_PER_SEC / MS_PER_SEC)
  };
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
 * Stop listening for events on a particular window.
 */
static inline void
win_ev_stop(session_t *ps, win *w) {
  xcb_connection_t *c = XGetXCBConnection(ps->dpy);

  // Will get BadWindow if the window is destroyed
  set_ignore_next(ps);
  XSelectInput(ps->dpy, w->id, 0);

  if (w->client_win) {
    set_ignore_next(ps);
    XSelectInput(ps->dpy, w->client_win, 0);
  }

  if (ps->shape_exists) {
    set_ignore_cookie(ps,
        xcb_shape_select_input(c, w->id, 0));
  }
}

/**
 * Get the children of a window.
 *
 * @param ps current session
 * @param w window to check
 * @param children [out] an array of child window IDs
 * @param nchildren [out] number of children
 * @return 1 if successful, 0 otherwise
 */
static inline bool
wid_get_children(session_t *ps, Window w,
    Window **children, unsigned *nchildren) {
  Window troot, tparent;

  if (!XQueryTree(ps->dpy, w, &troot, &tparent, children, nchildren)) {
    *nchildren = 0;
    return false;
  }

  return true;
}

/**
 * Check whether a window has WM frames.
 */
static inline bool __attribute__((pure))
win_has_frame(const win *w) {
  return w->g.border_width
    || w->frame_extents.top || w->frame_extents.left
    || w->frame_extents.right || w->frame_extents.bottom;
}

/**
 * Calculate the extents of the frame of the given window based on EWMH
 * _NET_FRAME_EXTENTS and the X window border width.
 */
static inline margin_t __attribute__((pure))
win_calc_frame_extents(session_t *ps, const win *w) {
  margin_t result = w->frame_extents;
  result.top = max_i(result.top, w->g.border_width);
  result.left = max_i(result.left, w->g.border_width);
  result.bottom = max_i(result.bottom, w->g.border_width);
  result.right = max_i(result.right, w->g.border_width);
  return result;
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
validate_pixmap(session_t *ps, Pixmap pxmap) {
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
 * Find matched window.
 */
static inline win *
find_win_all(session_t *ps, const Window wid) {
  if (!wid || PointerRoot == wid || wid == ps->root || wid == ps->overlay)
    return NULL;

  win *w = find_win(ps, wid);
  if (!w) w = find_toplevel(ps, wid);
  if (!w) w = find_toplevel2(ps, wid);
  return w;
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

/**
 * Resize a region.
 */
static inline void
resize_region(session_t *ps, region_t *region, short mod) {
  if (!mod || !region) return;
  // Loop through all rectangles
  int nrects;
  int nnewrects = 0;
  pixman_box32_t *rects = pixman_region32_rectangles(region, &nrects);
  pixman_box32_t *newrects = calloc(nrects, sizeof *newrects);
  for (int i = 0; i < nrects; i++) {
    int x1 = max_i(rects[i].x1 - mod, 0);
    int y1 = max_i(rects[i].y1 - mod, 0);
    int x2 = min_i(rects[i].x2 + mod, ps->root_width);
    int y2 = min_i(rects[i].y2 + mod, ps->root_height);
    int wid = x2 - x1;
    int hei = y2 - y1;
    if (wid <= 0 || hei <= 0)
      continue;
    newrects[nnewrects] = (pixman_box32_t) {
      .x1 = x1, .x2 = x2, .y1 = y1, .y2 = y2
    };
    ++nnewrects;
  }

  pixman_region32_fini(region);
  pixman_region32_init_rects(region, newrects, nnewrects);

  free(newrects);
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

static inline time_ms_t
timeout_get_newrun(const timeout_t *ptmout) {
  return ptmout->firstrun + (max_l((ptmout->lastrun + (time_ms_t) (ptmout->interval * TIMEOUT_RUN_TOLERANCE) - ptmout->firstrun) / ptmout->interval, (ptmout->lastrun + (time_ms_t) (ptmout->interval * (1 - TIMEOUT_RUN_TOLERANCE)) - ptmout->firstrun) / ptmout->interval) + 1) * ptmout->interval;
}

/**
 * Get the Xinerama screen a window is on.
 *
 * Return an index >= 0, or -1 if not found.
 */
static inline void
cxinerama_win_upd_scr(session_t *ps, win *w) {
#ifdef CONFIG_XINERAMA
  w->xinerama_scr = -1;

  if (!ps->xinerama_scrs)
    return;

  xcb_xinerama_screen_info_t *scrs = xcb_xinerama_query_screens_screen_info(ps->xinerama_scrs);
  int length = xcb_xinerama_query_screens_screen_info_length(ps->xinerama_scrs);
  for (int i = 0; i < length; i++) {
    xcb_xinerama_screen_info_t *s = &scrs[i];
    if (s->x_org <= w->g.x && s->y_org <= w->g.y
        && s->x_org + s->width >= w->g.x + w->widthb
        && s->y_org + s->height >= w->g.y + w->heightb) {
      w->xinerama_scr = i;
      return;
    }
  }
#endif
}

// vim: set et sw=2 :
