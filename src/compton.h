/**
 * compton.h
 */

// Throw everything in here.


// === Includes ===

#include "common.h"

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

// == Functions ==

// inline functions must be made static to compile correctly under clang:
// http://clang.llvm.org/compatibility.html#inline

// Helper functions

static void
discard_ignore(session_t *ps, unsigned long sequence);

static void
set_ignore(session_t *ps, unsigned long sequence);

/**
 * Ignore X errors caused by next X request.
 */
static inline void
set_ignore_next(session_t *ps) {
  set_ignore(ps, NextRequest(ps->dpy));
}

static int
should_ignore(session_t *ps, unsigned long sequence);

/**
 * Reset filter on a <code>Picture</code>.
 */
static inline void
xrfilter_reset(session_t *ps, Picture p) {
  XRenderSetPictureFilter(ps->dpy, p, "Nearest", NULL, 0);
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
 * Set a <code>bool</code> array of all wintypes to true.
 */
static inline void
wintype_arr_enable(bool arr[]) {
  wintype_t i;

  for (i = 0; i < NUM_WINTYPES; ++i) {
    arr[i] = true;
  }
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
 * Convert a geometry_t value to XRectangle.
 */
static inline XRectangle
geom_to_rect(session_t *ps, const geometry_t *src, const XRectangle *def) {
  XRectangle rect_def = { .x = 0, .y = 0,
    .width = ps->root_width, .height = ps->root_height };
  if (!def) def = &rect_def;

  XRectangle rect = { .x = src->x, .y = src->y,
    .width = src->wid, .height = src->hei };
  if (src->wid < 0) rect.width = def->width;
  if (src->hei < 0) rect.height = def->height;
  if (-1 == src->x) rect.x = def->x;
  else if (src->x < 0) rect.x = ps->root_width + rect.x + 2 - rect.width;
  if (-1 == src->y) rect.y = def->y;
  else if (src->y < 0) rect.y = ps->root_height + rect.y + 2 - rect.height;
  return rect;
}

/**
 * Convert a XRectangle to a XServerRegion.
 */
static inline XserverRegion
rect_to_reg(session_t *ps, const XRectangle *src) {
  if (!src) return None;
  XRectangle bound = { .x = 0, .y = 0,
    .width = ps->root_width, .height = ps->root_height };
  XRectangle res = { };
  rect_crop(&res, src, &bound);
  if (res.width && res.height)
    return XFixesCreateRegion(ps->dpy, &res, 1);
  return None;
}

/**
 * Destroy a <code>Picture</code>.
 */
inline static void
free_picture(session_t *ps, Picture *p) {
  if (*p) {
    XRenderFreePicture(ps->dpy, *p);
    *p = None;
  }
}

/**
 * Destroy a <code>Pixmap</code>.
 */
inline static void
free_pixmap(session_t *ps, Pixmap *p) {
  if (*p) {
    XFreePixmap(ps->dpy, *p);
    *p = None;
  }
}

/**
 * Destroy a <code>Damage</code>.
 */
inline static void
free_damage(session_t *ps, Damage *p) {
  if (*p) {
    // BadDamage will be thrown if the window is destroyed
    set_ignore_next(ps);
    XDamageDestroy(ps->dpy, *p);
    *p = None;
  }
}

/**
 * Destroy a condition list.
 */
static inline void
free_wincondlst(c2_lptr_t **pcondlst) {
#ifdef CONFIG_C2
  while ((*pcondlst = c2_free_lptr(*pcondlst)))
    continue;
#endif
}

/**
 * Free Xinerama screen info.
 */
static inline void
free_xinerama_info(session_t *ps) {
#ifdef CONFIG_XINERAMA
  if (ps->xinerama_scr_regs) {
    for (int i = 0; i < ps->xinerama_nscrs; ++i)
      free_region(ps, &ps->xinerama_scr_regs[i]);
    free(ps->xinerama_scr_regs);
  }
  cxfree(ps->xinerama_scrs);
  ps->xinerama_scrs = NULL;
  ps->xinerama_nscrs = 0;
#endif
}

/**
 * Check whether a paint_t contains enough data.
 */
static inline bool
paint_isvalid(session_t *ps, const paint_t *ppaint) {
  // Don't check for presence of Pixmap here, because older X Composite doesn't
  // provide it
  if (!ppaint)
    return false;

  if (bkend_use_xrender(ps) && !ppaint->pict)
    return false;

#ifdef CONFIG_VSYNC_OPENGL
  if (BKEND_GLX == ps->o.backend && !glx_tex_binded(ppaint->ptex, None))
    return false;
#endif

  return true;
}

/**
 * Bind texture in paint_t if we are using GLX backend.
 */
static inline bool
paint_bind_tex_real(session_t *ps, paint_t *ppaint,
    unsigned wid, unsigned hei, unsigned depth, bool force) {
#ifdef CONFIG_VSYNC_OPENGL
  if (!ppaint->pixmap)
    return false;

  if (force || !glx_tex_binded(ppaint->ptex, ppaint->pixmap))
    return glx_bind_pixmap(ps, &ppaint->ptex, ppaint->pixmap, wid, hei, depth);
#endif

  return true;
}

static inline bool
paint_bind_tex(session_t *ps, paint_t *ppaint,
    unsigned wid, unsigned hei, unsigned depth, bool force) {
  if (BKEND_GLX == ps->o.backend)
    return paint_bind_tex_real(ps, ppaint, wid, hei, depth, force);
  return true;
}

/**
 * Free data in a reg_data_t.
 */
static inline void
free_reg_data(reg_data_t *pregd) {
  cxfree(pregd->rects);
  pregd->rects = NULL;
  pregd->nrects = 0;
}

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
  free_region(ps, &w->extents);
  free_paint(ps, &w->paint);
  free_region(ps, &w->border_size);
  free_paint(ps, &w->shadow_paint);
  free_damage(ps, &w->damage);
  free_region(ps, &w->reg_ignore);
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
 * Whether an event is DamageNotify.
 */
static inline bool
isdamagenotify(session_t *ps, const XEvent *ev) {
  return ps->damage_event + XDamageNotify == ev->type;
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

static void
run_fade(session_t *ps, win *w, unsigned steps);

static void
set_fade_callback(session_t *ps, win *w,
    void (*callback) (session_t *ps, win *w), bool exec_callback);

/**
 * Execute fade callback of a window if fading finished.
 */
static inline void
check_fade_fin(session_t *ps, win *w) {
  if (w->fade_callback && w->opacity == w->opacity_tgt) {
    // Must be the last line as the callback could destroy w!
    set_fade_callback(ps, w, NULL, true);
  }
}

static void
set_fade_callback(session_t *ps, win *w,
    void (*callback) (session_t *ps, win *w), bool exec_callback);

static double
gaussian(double r, double x, double y);

static conv *
make_gaussian_map(double r);

static unsigned char
sum_gaussian(conv *map, double opacity,
             int x, int y, int width, int height);

static void
presum_gaussian(session_t *ps, conv *map);

static XImage *
make_shadow(session_t *ps, double opacity, int width, int height);

static bool
win_build_shadow(session_t *ps, win *w, double opacity);

static Picture
solid_picture(session_t *ps, bool argb, double a,
              double r, double g, double b);

/**
 * Stop listening for events on a particular window.
 */
static inline void
win_ev_stop(session_t *ps, win *w) {
  // Will get BadWindow if the window is destroyed
  set_ignore_next(ps);
  XSelectInput(ps->dpy, w->id, 0);

  if (w->client_win) {
    set_ignore_next(ps);
    XSelectInput(ps->dpy, w->client_win, 0);
  }

  if (ps->shape_exists) {
    set_ignore_next(ps);
    XShapeSelectInput(ps->dpy, w->id, 0);
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
 * Check if a window is bounding-shaped.
 */
static inline bool
wid_bounding_shaped(const session_t *ps, Window wid) {
  if (ps->shape_exists) {
    Bool bounding_shaped = False, clip_shaped = False;
    int x_bounding, y_bounding, x_clip, y_clip;
    unsigned int w_bounding, h_bounding, w_clip, h_clip;

    XShapeQueryExtents(ps->dpy, wid, &bounding_shaped,
        &x_bounding, &y_bounding, &w_bounding, &h_bounding,
        &clip_shaped, &x_clip, &y_clip, &w_clip, &h_clip);
    return bounding_shaped;
  }

  return false;
}

/**
 * Determine if a window change affects <code>reg_ignore</code> and set
 * <code>reg_ignore_expire</code> accordingly.
 */
static inline void
update_reg_ignore_expire(session_t *ps, const win *w) {
  if (w->to_paint && WMODE_SOLID == w->mode)
    ps->reg_ignore_expire = true;
}

/**
 * Check whether a window has WM frames.
 */
static inline bool __attribute__((pure))
win_has_frame(const win *w) {
  return w->a.border_width
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
  result.top = max_i(result.top, w->a.border_width);
  result.left = max_i(result.left, w->a.border_width);
  result.bottom = max_i(result.bottom, w->a.border_width);
  result.right = max_i(result.right, w->a.border_width);
  return result;
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

static void
win_rounded_corners(session_t *ps, win *w);

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
 * Wrapper of c2_match().
 */
static inline bool
win_match(session_t *ps, win *w, c2_lptr_t *condlst, const c2_lptr_t **cache) {
#ifdef CONFIG_C2
  return c2_match(ps, w, condlst, cache);
#else
  return false;
#endif
}

static bool
condlst_add(session_t *ps, c2_lptr_t **pcondlst, const char *pattern);

static long
determine_evmask(session_t *ps, Window wid, win_evmode_t mode);

/**
 * Clear leader cache of all windows.
 */
static void
clear_cache_win_leaders(session_t *ps) {
  for (win *w = ps->list; w; w = w->next)
    w->cache_leader = None;
}

static win *
find_toplevel2(session_t *ps, Window wid);

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

static Window
win_get_leader_raw(session_t *ps, win *w, int recursions);

/**
 * Get the leader of a window.
 *
 * This function updates w->cache_leader if necessary.
 */
static inline Window
win_get_leader(session_t *ps, win *w) {
  return win_get_leader_raw(ps, w, 0);
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

static win *
recheck_focus(session_t *ps);

static bool
get_root_tile(session_t *ps);

static void
paint_root(session_t *ps, XserverRegion reg_paint);

static XserverRegion
win_get_region(session_t *ps, win *w, bool use_offset);

static XserverRegion
win_get_region_noframe(session_t *ps, win *w, bool use_offset);

static XserverRegion
win_extents(session_t *ps, win *w);

static XserverRegion
border_size(session_t *ps, win *w, bool use_offset);

static Window
find_client_win(session_t *ps, Window w);

static void
get_frame_extents(session_t *ps, win *w, Window client);

static win *
paint_preprocess(session_t *ps, win *list);

static void
render_(session_t *ps, int x, int y, int dx, int dy, int wid, int hei,
    double opacity, bool argb, bool neg,
    Picture pict, glx_texture_t *ptex,
    XserverRegion reg_paint, const reg_data_t *pcache_reg
#ifdef CONFIG_VSYNC_OPENGL_GLSL
    , const glx_prog_main_t *pprogram
#endif
    );

#ifdef CONFIG_VSYNC_OPENGL_GLSL
#define \
   render(ps, x, y, dx, dy, wid, hei, opacity, argb, neg, pict, ptex, reg_paint, pcache_reg, pprogram) \
  render_(ps, x, y, dx, dy, wid, hei, opacity, argb, neg, pict, ptex, reg_paint, pcache_reg, pprogram)
#else
#define \
   render(ps, x, y, dx, dy, wid, hei, opacity, argb, neg, pict, ptex, reg_paint, pcache_reg, pprogram) \
  render_(ps, x, y, dx, dy, wid, hei, opacity, argb, neg, pict, ptex, reg_paint, pcache_reg)
#endif

static inline void
win_render(session_t *ps, win *w, int x, int y, int wid, int hei,
    double opacity, XserverRegion reg_paint, const reg_data_t *pcache_reg,
    Picture pict) {
  const int dx = (w ? w->a.x: 0) + x;
  const int dy = (w ? w->a.y: 0) + y;
  const bool argb = (w && (WMODE_ARGB == w->mode || ps->o.force_win_blend));
  const bool neg = (w && w->invert_color);

  render(ps, x, y, dx, dy, wid, hei, opacity, argb, neg,
      pict, (w ? w->paint.ptex: ps->root_tile_paint.ptex),
      reg_paint, pcache_reg, (w ? &ps->o.glx_prog_win: NULL));
}

static inline void
set_tgt_clip(session_t *ps, XserverRegion reg, const reg_data_t *pcache_reg) {
  switch (ps->o.backend) {
    case BKEND_XRENDER:
    case BKEND_XR_GLX_HYBRID:
      XFixesSetPictureClipRegion(ps->dpy, ps->tgt_buffer.pict, 0, 0, reg);
      break;
#ifdef CONFIG_VSYNC_OPENGL
    case BKEND_GLX:
      glx_set_clip(ps, reg, pcache_reg);
      break;
#endif
  }
}

static bool
xr_blur_dst(session_t *ps, Picture tgt_buffer,
    int x, int y, int wid, int hei, XFixed **blur_kerns,
    XserverRegion reg_clip);

/**
 * Normalize a convolution kernel.
 */
static inline void
normalize_conv_kern(int wid, int hei, XFixed *kern) {
  double sum = 0.0;
  for (int i = 0; i < wid * hei; ++i)
    sum += XFixedToDouble(kern[i]);
  double factor = 1.0 / sum;
  for (int i = 0; i < wid * hei; ++i)
    kern[i] = XDoubleToFixed(XFixedToDouble(kern[i]) * factor);
}

static void
paint_all(session_t *ps, XserverRegion region, XserverRegion region_real, win *t);

static void
add_damage(session_t *ps, XserverRegion damage);

static void
repair_win(session_t *ps, win *w);

static wintype_t
wid_get_prop_wintype(session_t *ps, Window w);

static void
map_win(session_t *ps, Window id);

static void
finish_map_win(session_t *ps, win *w);

static void
finish_unmap_win(session_t *ps, win *w);

static void
unmap_callback(session_t *ps, win *w);

static void
unmap_win(session_t *ps, win *w);

static opacity_t
wid_get_opacity_prop(session_t *ps, Window wid, opacity_t def);

/**
 * Reread opacity property of a window.
 */
static inline void
win_update_opacity_prop(session_t *ps, win *w) {
  w->opacity_prop = wid_get_opacity_prop(ps, w->id, OPAQUE);
  if (!ps->o.detect_client_opacity || !w->client_win
      || w->id == w->client_win)
    w->opacity_prop_client = OPAQUE;
  else
    w->opacity_prop_client = wid_get_opacity_prop(ps, w->client_win,
          OPAQUE);
}

static double
get_opacity_percent(win *w);

static void
win_determine_mode(session_t *ps, win *w);

static void
calc_opacity(session_t *ps, win *w);

static void
calc_dim(session_t *ps, win *w);

static Window
wid_get_prop_window(session_t *ps, Window wid, Atom aprop);

static void
win_update_leader(session_t *ps, win *w);

static void
win_set_leader(session_t *ps, win *w, Window leader);

static void
win_update_focused(session_t *ps, win *w);

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

static inline void
win_set_focused(session_t *ps, win *w, bool focused);

static void
win_on_focus_change(session_t *ps, win *w);

static void
win_determine_fade(session_t *ps, win *w);

static void
win_update_shape_raw(session_t *ps, win *w);

static void
win_update_shape(session_t *ps, win *w);

static void
win_update_prop_shadow_raw(session_t *ps, win *w);

static void
win_update_prop_shadow(session_t *ps, win *w);

static void
win_set_shadow(session_t *ps, win *w, bool shadow_new);

static void
win_determine_shadow(session_t *ps, win *w);

static void
win_set_invert_color(session_t *ps, win *w, bool invert_color_new);

static void
win_determine_invert_color(session_t *ps, win *w);

static void
win_set_blur_background(session_t *ps, win *w, bool blur_background_new);

static void
win_determine_blur_background(session_t *ps, win *w);

static void
win_on_wtype_change(session_t *ps, win *w);

static void
win_on_factor_change(session_t *ps, win *w);

static void
win_upd_run(session_t *ps, win *w, win_upd_t *pupd);

static void
calc_win_size(session_t *ps, win *w);

static void
calc_shadow_geometry(session_t *ps, win *w);

static void
win_upd_wintype(session_t *ps, win *w);

static void
win_mark_client(session_t *ps, win *w, Window client);

static void
win_unmark_client(session_t *ps, win *w);

static void
win_recheck_client(session_t *ps, win *w);

static bool
add_win(session_t *ps, Window id, Window prev);

static void
restack_win(session_t *ps, win *w, Window new_above);

static void
configure_win(session_t *ps, XConfigureEvent *ce);

static void
circulate_win(session_t *ps, XCirculateEvent *ce);

static void
finish_destroy_win(session_t *ps, Window id);

static void
destroy_callback(session_t *ps, win *w);

static void
destroy_win(session_t *ps, Window id);

static void
damage_win(session_t *ps, XDamageNotifyEvent *de);

static int
xerror(Display *dpy, XErrorEvent *ev);

static void
expose_root(session_t *ps, XRectangle *rects, int nrects);

static Window
wid_get_prop_window(session_t *ps, Window wid, Atom aprop);

static bool
wid_get_name(session_t *ps, Window w, char **name);

static bool
wid_get_role(session_t *ps, Window w, char **role);

static int
win_get_prop_str(session_t *ps, win *w, char **tgt,
    bool (*func_wid_get_prop_str)(session_t *ps, Window wid, char **tgt));

static inline int
win_get_name(session_t *ps, win *w) {
  int ret = win_get_prop_str(ps, w, &w->name, wid_get_name);

#ifdef DEBUG_WINDATA
  printf_dbgf("(%#010lx): client = %#010lx, name = \"%s\", "
      "ret = %d\n", w->id, w->client_win, w->name, ret);
#endif

  return ret;
}

static inline int
win_get_role(session_t *ps, win *w) {
  int ret = win_get_prop_str(ps, w, &w->role, wid_get_role);

#ifdef DEBUG_WINDATA
  printf_dbgf("(%#010lx): client = %#010lx, role = \"%s\", "
      "ret = %d\n", w->id, w->client_win, w->role, ret);
#endif

  return ret;
}

static bool
win_get_class(session_t *ps, win *w);

#ifdef DEBUG_EVENTS
static int
ev_serial(XEvent *ev);

static const char *
ev_name(session_t *ps, XEvent *ev);

static Window
ev_window(session_t *ps, XEvent *ev);
#endif

static void __attribute__ ((noreturn))
usage(int ret);

static bool
register_cm(session_t *ps);

inline static void
ev_focus_in(session_t *ps, XFocusChangeEvent *ev);

inline static void
ev_focus_out(session_t *ps, XFocusChangeEvent *ev);

inline static void
ev_create_notify(session_t *ps, XCreateWindowEvent *ev);

inline static void
ev_configure_notify(session_t *ps, XConfigureEvent *ev);

inline static void
ev_destroy_notify(session_t *ps, XDestroyWindowEvent *ev);

inline static void
ev_map_notify(session_t *ps, XMapEvent *ev);

inline static void
ev_unmap_notify(session_t *ps, XUnmapEvent *ev);

inline static void
ev_reparent_notify(session_t *ps, XReparentEvent *ev);

inline static void
ev_circulate_notify(session_t *ps, XCirculateEvent *ev);

inline static void
ev_expose(session_t *ps, XExposeEvent *ev);

static void
update_ewmh_active_win(session_t *ps);

inline static void
ev_property_notify(session_t *ps, XPropertyEvent *ev);

inline static void
ev_damage_notify(session_t *ps, XDamageNotifyEvent *ev);

inline static void
ev_shape_notify(session_t *ps, XShapeEvent *ev);

/**
 * Get a region of the screen size.
 */
inline static XserverRegion
get_screen_region(session_t *ps) {
  XRectangle r;

  r.x = 0;
  r.y = 0;
  r.width = ps->root_width;
  r.height = ps->root_height;
  return XFixesCreateRegion(ps->dpy, &r, 1);
}

/**
 * Resize a region.
 */
static inline void
resize_region(session_t *ps, XserverRegion region, short mod) {
  if (!mod || !region) return;

  int nrects = 0, nnewrects = 0;
  XRectangle *newrects = NULL;
  XRectangle *rects = XFixesFetchRegion(ps->dpy, region, &nrects);
  if (!rects || !nrects)
    goto resize_region_end;

  // Allocate memory for new rectangle list, because I don't know if it's
  // safe to write in the memory Xlib allocates
  newrects = calloc(nrects, sizeof(XRectangle));
  if (!newrects) {
    printf_errf("(): Failed to allocate memory.");
    exit(1);
  }

  // Loop through all rectangles
  for (int i = 0; i < nrects; ++i) {
    int x1 = max_i(rects[i].x - mod, 0);
    int y1 = max_i(rects[i].y - mod, 0);
    int x2 = min_i(rects[i].x + rects[i].width + mod, ps->root_width);
    int y2 = min_i(rects[i].y + rects[i].height + mod, ps->root_height);
    int wid = x2 - x1;
    int hei = y2 - y1;
    if (wid <= 0 || hei <= 0)
      continue;
    newrects[nnewrects].x = x1;
    newrects[nnewrects].y = y1;
    newrects[nnewrects].width = wid;
    newrects[nnewrects].height = hei;
    ++nnewrects;
  }

  // Set region
  XFixesSetRegion(ps->dpy, region, newrects, nnewrects);

resize_region_end:
  cxfree(rects);
  free(newrects);
}

/**
 * Dump a region.
 */
static inline void
dump_region(const session_t *ps, XserverRegion region) {
  int nrects = 0;
  XRectangle *rects = NULL;
  if (!rects && region)
    rects = XFixesFetchRegion(ps->dpy, region, &nrects);

  printf_dbgf("(%#010lx): %d rects\n", region, nrects);
  if (!rects) return;
  for (int i = 0; i < nrects; ++i)
    printf("Rect #%d: %8d, %8d, %8d, %8d\n", i, rects[i].x, rects[i].y,
        rects[i].width, rects[i].height);
  putchar('\n');
  fflush(stdout);

  cxfree(rects);
}

/**
 * Check if a region is empty.
 *
 * Keith Packard said this is slow:
 * http://lists.freedesktop.org/archives/xorg/2007-November/030467.html
 *
 * @param ps current session
 * @param region region to check for
 * @param pcache_rects a place to cache the dumped rectangles
 * @param ncache_nrects a place to cache the number of dumped rectangles
 */
static inline bool
is_region_empty(const session_t *ps, XserverRegion region,
    reg_data_t *pcache_reg) {
  int nrects = 0;
  XRectangle *rects = XFixesFetchRegion(ps->dpy, region, &nrects);

  if (pcache_reg) {
    pcache_reg->rects = rects;
    pcache_reg->nrects = nrects;
  }
  else
    cxfree(rects);

  return !nrects;
}

/**
 * Add a window to damaged area.
 *
 * @param ps current session
 * @param w struct _win element representing the window
 */
static inline void
add_damage_win(session_t *ps, win *w) {
  if (w->extents) {
    add_damage(ps, copy_region(ps, w->extents));
  }
}

#if defined(DEBUG_EVENTS) || defined(DEBUG_RESTACK)
static bool
ev_window_name(session_t *ps, Window wid, char **name);
#endif

inline static void
ev_handle(session_t *ps, XEvent *ev);

static bool
fork_after(session_t *ps);

#ifdef CONFIG_LIBCONFIG
/**
 * Wrapper of libconfig's <code>config_lookup_int</code>.
 *
 * To convert <code>int</code> value <code>config_lookup_bool</code>
 * returns to <code>bool</code>.
 */
static inline void
lcfg_lookup_bool(const config_t *config, const char *path,
    bool *value) {
  int ival;

  if (config_lookup_bool(config, path, &ival))
    *value = ival;
}

/**
 * Wrapper of libconfig's <code>config_lookup_int</code>.
 *
 * To deal with the different value types <code>config_lookup_int</code>
 * returns in libconfig-1.3 and libconfig-1.4.
 */
static inline int
lcfg_lookup_int(const config_t *config, const char *path, int *value) {
#ifndef CONFIG_LIBCONFIG_LEGACY
  return config_lookup_int(config, path, value);
#else
  long lval;
  int ret;

  if ((ret = config_lookup_int(config, path, &lval)))
    *value = lval;

  return ret;
#endif
}

static FILE *
open_config_file(char *cpath, char **path);

static void
parse_cfg_condlst(session_t *ps, const config_t *pcfg, c2_lptr_t **pcondlst,
    const char *name);

static void
parse_config(session_t *ps, struct options_tmp *pcfgtmp);
#endif

static void
get_cfg(session_t *ps, int argc, char *const *argv, bool first_pass);

static void
init_atoms(session_t *ps);

static void
update_refresh_rate(session_t *ps);

static bool
swopti_init(session_t *ps);

static void
swopti_handle_timeout(session_t *ps, struct timeval *ptv);

#ifdef CONFIG_VSYNC_OPENGL
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

static bool
vsync_drm_init(session_t *ps);

#ifdef CONFIG_VSYNC_DRM
static int
vsync_drm_wait(session_t *ps);
#endif

static bool
vsync_opengl_init(session_t *ps);

static bool
vsync_opengl_oml_init(session_t *ps);

static bool
vsync_opengl_swc_init(session_t *ps);

static bool
vsync_opengl_mswc_init(session_t *ps);

#ifdef CONFIG_VSYNC_OPENGL
static int
vsync_opengl_wait(session_t *ps);

static int
vsync_opengl_oml_wait(session_t *ps);

static void
vsync_opengl_swc_deinit(session_t *ps);

static void
vsync_opengl_mswc_deinit(session_t *ps);
#endif

static void
vsync_wait(session_t *ps);

static void
init_alpha_picts(session_t *ps);

static bool
init_dbe(session_t *ps);

static bool
init_overlay(session_t *ps);

static void
redir_start(session_t *ps);

static void
redir_stop(session_t *ps);

static inline time_ms_t
timeout_get_newrun(const timeout_t *ptmout) {
  return ptmout->firstrun + (max_l((ptmout->lastrun + (time_ms_t) (ptmout->interval * TIMEOUT_RUN_TOLERANCE) - ptmout->firstrun) / ptmout->interval, (ptmout->lastrun + (time_ms_t) (ptmout->interval * (1 - TIMEOUT_RUN_TOLERANCE)) - ptmout->firstrun) / ptmout->interval) + 1) * ptmout->interval;
}

static time_ms_t
timeout_get_poll_time(session_t *ps);

static void
timeout_clear(session_t *ps);

static bool
tmout_unredir_callback(session_t *ps, timeout_t *tmout);

static bool
mainloop(session_t *ps);

#ifdef CONFIG_XINERAMA
static void
cxinerama_upd_scrs(session_t *ps);
#endif

/**
 * Get the Xinerama screen a window is on.
 *
 * Return an index >= 0, or -1 if not found.
 */
static inline void
cxinerama_win_upd_scr(session_t *ps, win *w) {
#ifdef CONFIG_XINERAMA
  w->xinerama_scr = -1;
  for (XineramaScreenInfo *s = ps->xinerama_scrs;
      s < ps->xinerama_scrs + ps->xinerama_nscrs; ++s)
    if (s->x_org <= w->a.x && s->y_org <= w->a.y
        && s->x_org + s->width >= w->a.x + w->widthb
        && s->y_org + s->height >= w->a.y + w->heightb) {
      w->xinerama_scr = s - ps->xinerama_scrs;
      return;
    }
#endif
}

static void
cxinerama_upd_scrs(session_t *ps);

static session_t *
session_init(session_t *ps_old, int argc, char **argv);

static void
session_destroy(session_t *ps);

static void
session_run(session_t *ps);

static void
reset_enable(int __attribute__((unused)) signum);
