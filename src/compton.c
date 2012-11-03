/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011, Christopher Jeffrey
 * See LICENSE for more information.
 *
 */

#include "compton.h"

#define MSTR_(s)        #s
#define MSTR(s)         MSTR_(s)

// Use #s here to prevent macro expansion
#define CASESTRRET(s)   case s: return #s

/**
 * Shared
 */

const char *WINTYPES[NUM_WINTYPES] = {
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

struct timeval time_start = { 0, 0 };

win *list;
Display *dpy = NULL;
int scr;

/// Root window.
Window root = None;
/// Damage of root window.
Damage root_damage = None;
/// X Composite overlay window. Used if --paint-on-overlay.
Window overlay = None;

/// Picture of root window. Destination of painting in no-DBE painting
/// mode.
Picture root_picture = None;
/// A Picture acting as the painting target.
Picture tgt_picture = None;
/// Temporary buffer to paint to before sending to display.
Picture tgt_buffer = None;
/// DBE back buffer for root window. Used in DBE painting mode.
XdbeBackBuffer root_dbe = None;

Picture black_picture;
Picture cshadow_picture;
/// Picture used for dimming inactive windows.
Picture dim_picture = 0;
Picture root_tile;
XserverRegion all_damage;
Bool has_name_pixmap;
int root_height, root_width;
/// A region of the size of the screen.
XserverRegion screen_reg = None;

/// Pregenerated alpha pictures.
Picture *alpha_picts = NULL;
/// Whether the program is idling. I.e. no fading, no potential window
/// changes.
Bool idling;
/// Whether all reg_ignore of windows should expire in this paint.
Bool reg_ignore_expire = False;
/// Window ID of the window we register as a symbol.
Window reg_win = 0;

/// Currently used refresh rate. Used for sw_opti.
short refresh_rate = 0;
/// Interval between refresh in nanoseconds. Used for sw_opti.
unsigned long refresh_intv = 0;
/// Nanosecond-level offset of the first painting. Used for sw_opti.
long paint_tm_offset = 0;

#ifdef CONFIG_VSYNC_DRM
/// File descriptor of DRI device file. Used for DRM VSync.
int drm_fd = 0;
#endif

#ifdef CONFIG_VSYNC_OPENGL
/// GLX context.
GLXContext glx_context;

/// Pointer to glXGetVideoSyncSGI function. Used by OpenGL VSync.
f_GetVideoSync glx_get_video_sync = NULL;

/// Pointer to glXWaitVideoSyncSGI function. Used by OpenGL VSync.
f_WaitVideoSync glx_wait_video_sync = NULL;
#endif

/* errors */
ignore *ignore_head = NULL, **ignore_tail = &ignore_head;
int xfixes_event, xfixes_error;
int damage_event, damage_error;
int composite_event, composite_error;
int render_event, render_error;
int composite_opcode;

/// Whether X Shape extension exists.
Bool shape_exists = False;
/// Event base number and error base number for X Shape extension.
int shape_event, shape_error;

/// Whether X RandR extension exists.
Bool randr_exists = False;
/// Event base number and error base number for X RandR extension.
int randr_event, randr_error;

#ifdef CONFIG_VSYNC_OPENGL
/// Whether X GLX extension exists.
Bool glx_exists = False;
/// Event base number and error base number for X GLX extension.
int glx_event, glx_error;
#endif

Bool dbe_exists = False;

/* shadows */
conv *gaussian_map;

/* for shadow precomputation */
int cgsize = -1;
unsigned char *shadow_corner = NULL;
unsigned char *shadow_top = NULL;

/* for root tile */
static const char *background_props[] = {
  "_XROOTPMAP_ID",
  "_XSETROOT_ID",
  0,
};

/* for expose events */
XRectangle *expose_rects = 0;
int size_expose = 0;
int n_expose = 0;

// atoms
Atom extents_atom;
Atom opacity_atom;
Atom frame_extents_atom;
Atom client_atom;
Atom name_atom;
Atom name_ewmh_atom;
Atom class_atom;
Atom transient_atom;

Atom win_type_atom;
Atom win_type[NUM_WINTYPES];

/**
 * Macros
 */

#define HAS_FRAME_OPACITY(w) \
  (frame_opacity && (w)->top_width)

/**
 * Options
 */

static options_t opts = {
  .display = NULL,
  .mark_wmwin_focused = False,
  .mark_ovredir_focused = False,
  .fork_after_register = False,
  .synchronize = False,
  .detect_rounded_corners = False,
  .paint_on_overlay = False,

  .refresh_rate = 0,
  .sw_opti = False,
  .vsync = VSYNC_NONE,
  .dbe = False,
  .vsync_aggressive = False,

  .wintype_shadow = { False },
  .shadow_red = 0.0,
  .shadow_green = 0.0,
  .shadow_blue = 0.0,
  .shadow_radius = 12,
  .shadow_offset_x = -15,
  .shadow_offset_y = -15,
  .shadow_opacity = .75,
  .clear_shadow = False,
  .shadow_blacklist = NULL,
  .shadow_ignore_shaped = False,

  .wintype_fade = { False },
  .fade_in_step = 0.028 * OPAQUE,
  .fade_out_step = 0.03 * OPAQUE,
  .fade_delta = 10,
  .no_fading_openclose = False,
  .fade_blacklist = NULL,

  .wintype_opacity = { 0.0 },
  .inactive_opacity = 0,
  .inactive_opacity_override = False,
  .frame_opacity = 0.0,
  .detect_client_opacity = False,
  .inactive_dim = 0.0,
  .alpha_step = 0.03,

  .track_focus = False,
  .track_wdata = False,
};

/**
 * Fades
 */

unsigned long fade_time = 0;

/**
 * Get the time left before next fading point.
 *
 * In milliseconds.
 */
static int
fade_timeout(void) {
  int diff = opts.fade_delta - get_time_ms() + fade_time;

  if (diff < 0)
    diff = 0;

  return diff;
}

/**
 * Run fading on a window.
 *
 * @param steps steps of fading
 */
static void
run_fade(Display *dpy, win *w, unsigned steps) {
  // If we have reached target opacity, return
  if (w->opacity == w->opacity_tgt) {
    return;
  }

  if (!w->fade)
    w->opacity = w->opacity_tgt;
  else if (steps) {
    // Use double below because opacity_t will probably overflow during
    // calculations
    if (w->opacity < w->opacity_tgt)
      w->opacity = normalize_d_range(
          (double) w->opacity + (double) opts.fade_in_step * steps,
          0.0, w->opacity_tgt);
    else
      w->opacity = normalize_d_range(
          (double) w->opacity - (double) opts.fade_out_step * steps,
          w->opacity_tgt, OPAQUE);
  }

  if (w->opacity != w->opacity_tgt) {
    idling = False;
  }
}

/**
 * Set fade callback of a window, and possibly execute the previous
 * callback.
 *
 * @param exec_callback whether the previous callback is to be executed
 */
static void
set_fade_callback(Display *dpy, win *w,
    void (*callback) (Display *dpy, win *w), Bool exec_callback) {
  void (*old_callback) (Display *dpy, win *w) = w->fade_callback;

  w->fade_callback = callback;
  // Must be the last line as the callback could destroy w!
  if (exec_callback && old_callback) {
    old_callback(dpy, w);
    // Although currently no callback function affects window state on
    // next paint, it could, in the future
    idling = False;
  }
}

/**
 * Shadows
 */

static double
gaussian(double r, double x, double y) {
  return ((1 / (sqrt(2 * M_PI * r))) *
    exp((- (x * x + y * y)) / (2 * r * r)));
}

static conv *
make_gaussian_map(Display *dpy, double r) {
  conv *c;
  int size = ((int) ceil((r * 3)) + 1) & ~1;
  int center = size / 2;
  int x, y;
  double t;
  double g;

  c = malloc(sizeof(conv) + size * size * sizeof(double));
  c->size = size;
  c->data = (double *) (c + 1);
  t = 0.0;

  for (y = 0; y < size; y++) {
    for (x = 0; x < size; x++) {
      g = gaussian(r, (double) (x - center), (double) (y - center));
      t += g;
      c->data[y * size + x] = g;
    }
  }

  for (y = 0; y < size; y++) {
    for (x = 0; x < size; x++) {
      c->data[y * size + x] /= t;
    }
  }

  return c;
}

/*
 * A picture will help
 *
 *      -center   0                width  width+center
 *  -center +-----+-------------------+-----+
 *          |     |                   |     |
 *          |     |                   |     |
 *        0 +-----+-------------------+-----+
 *          |     |                   |     |
 *          |     |                   |     |
 *          |     |                   |     |
 *   height +-----+-------------------+-----+
 *          |     |                   |     |
 * height+  |     |                   |     |
 *  center  +-----+-------------------+-----+
 */

static unsigned char
sum_gaussian(conv *map, double opacity,
             int x, int y, int width, int height) {
  int fx, fy;
  double *g_data;
  double *g_line = map->data;
  int g_size = map->size;
  int center = g_size / 2;
  int fx_start, fx_end;
  int fy_start, fy_end;
  double v;

  /*
   * Compute set of filter values which are "in range",
   * that's the set with:
   *    0 <= x + (fx-center) && x + (fx-center) < width &&
   *  0 <= y + (fy-center) && y + (fy-center) < height
   *
   *  0 <= x + (fx - center)    x + fx - center < width
   *  center - x <= fx    fx < width + center - x
   */

  fx_start = center - x;
  if (fx_start < 0) fx_start = 0;
  fx_end = width + center - x;
  if (fx_end > g_size) fx_end = g_size;

  fy_start = center - y;
  if (fy_start < 0) fy_start = 0;
  fy_end = height + center - y;
  if (fy_end > g_size) fy_end = g_size;

  g_line = g_line + fy_start * g_size + fx_start;

  v = 0;

  for (fy = fy_start; fy < fy_end; fy++) {
    g_data = g_line;
    g_line += g_size;

    for (fx = fx_start; fx < fx_end; fx++) {
      v += *g_data++;
    }
  }

  if (v > 1) v = 1;

  return ((unsigned char) (v * opacity * 255.0));
}

/* precompute shadow corners and sides
   to save time for large windows */

static void
presum_gaussian(conv *map) {
  int center = map->size / 2;
  int opacity, x, y;

  cgsize = map->size;

  if (shadow_corner) free((void *)shadow_corner);
  if (shadow_top) free((void *)shadow_top);

  shadow_corner = (unsigned char *)(malloc((cgsize + 1) * (cgsize + 1) * 26));
  shadow_top = (unsigned char *)(malloc((cgsize + 1) * 26));

  for (x = 0; x <= cgsize; x++) {
    shadow_top[25 * (cgsize + 1) + x] =
      sum_gaussian(map, 1, x - center, center, cgsize * 2, cgsize * 2);

    for (opacity = 0; opacity < 25; opacity++) {
      shadow_top[opacity * (cgsize + 1) + x] =
        shadow_top[25 * (cgsize + 1) + x] * opacity / 25;
    }

    for (y = 0; y <= x; y++) {
      shadow_corner[25 * (cgsize + 1) * (cgsize + 1) + y * (cgsize + 1) + x]
        = sum_gaussian(map, 1, x - center, y - center, cgsize * 2, cgsize * 2);
      shadow_corner[25 * (cgsize + 1) * (cgsize + 1) + x * (cgsize + 1) + y]
        = shadow_corner[25 * (cgsize + 1) * (cgsize + 1) + y * (cgsize + 1) + x];

      for (opacity = 0; opacity < 25; opacity++) {
        shadow_corner[opacity * (cgsize + 1) * (cgsize + 1)
                      + y * (cgsize + 1) + x]
          = shadow_corner[opacity * (cgsize + 1) * (cgsize + 1)
                          + x * (cgsize + 1) + y]
          = shadow_corner[25 * (cgsize + 1) * (cgsize + 1)
                          + y * (cgsize + 1) + x] * opacity / 25;
      }
    }
  }
}

static XImage *
make_shadow(Display *dpy, double opacity,
            int width, int height, Bool clear_shadow) {
  XImage *ximage;
  unsigned char *data;
  int ylimit, xlimit;
  int swidth = width + cgsize;
  int sheight = height + cgsize;
  int center = cgsize / 2;
  int x, y;
  unsigned char d;
  int x_diff;
  int opacity_int = (int)(opacity * 25);

  data = malloc(swidth * sheight * sizeof(unsigned char));
  if (!data) return 0;

  ximage = XCreateImage(
    dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 8,
    ZPixmap, 0, (char *) data, swidth, sheight, 8,
    swidth * sizeof(unsigned char));

  if (!ximage) {
    free(data);
    return 0;
  }

  /*
   * Build the gaussian in sections
   */

  /*
   * center (fill the complete data array)
   */

  // If clear_shadow is enabled and the border & corner shadow (which
  // later will be filled) could entirely cover the area of the shadow
  // that will be displayed, do not bother filling other pixels. If it
  // can't, we must fill the other pixels here.
  /* if (!(clear_shadow && opts.shadow_offset_x <= 0 && opts.shadow_offset_x >= -cgsize
        && opts.shadow_offset_y <= 0 && opts.shadow_offset_y >= -cgsize)) { */
    if (cgsize > 0) {
      d = shadow_top[opacity_int * (cgsize + 1) + cgsize];
    } else {
      d = sum_gaussian(gaussian_map,
        opacity, center, center, width, height);
    }
    memset(data, d, sheight * swidth);
  // }

  /*
   * corners
   */

  ylimit = cgsize;
  if (ylimit > sheight / 2) ylimit = (sheight + 1) / 2;

  xlimit = cgsize;
  if (xlimit > swidth / 2) xlimit = (swidth + 1) / 2;

  for (y = 0; y < ylimit; y++) {
    for (x = 0; x < xlimit; x++) {
      if (xlimit == cgsize && ylimit == cgsize) {
        d = shadow_corner[opacity_int * (cgsize + 1) * (cgsize + 1)
                          + y * (cgsize + 1) + x];
      } else {
        d = sum_gaussian(gaussian_map,
          opacity, x - center, y - center, width, height);
      }
      data[y * swidth + x] = d;
      data[(sheight - y - 1) * swidth + x] = d;
      data[(sheight - y - 1) * swidth + (swidth - x - 1)] = d;
      data[y * swidth + (swidth - x - 1)] = d;
    }
  }

  /*
   * top/bottom
   */

  x_diff = swidth - (cgsize * 2);
  if (x_diff > 0 && ylimit > 0) {
    for (y = 0; y < ylimit; y++) {
      if (ylimit == cgsize) {
        d = shadow_top[opacity_int * (cgsize + 1) + y];
      } else {
        d = sum_gaussian(gaussian_map,
          opacity, center, y - center, width, height);
      }
      memset(&data[y * swidth + cgsize], d, x_diff);
      memset(&data[(sheight - y - 1) * swidth + cgsize], d, x_diff);
    }
  }

  /*
   * sides
   */

  for (x = 0; x < xlimit; x++) {
    if (xlimit == cgsize) {
      d = shadow_top[opacity_int * (cgsize + 1) + x];
    } else {
      d = sum_gaussian(gaussian_map,
        opacity, x - center, center, width, height);
    }
    for (y = cgsize; y < sheight - cgsize; y++) {
      data[y * swidth + x] = d;
      data[y * swidth + (swidth - x - 1)] = d;
    }
  }

  assert(!clear_shadow);
  /*
  if (clear_shadow) {
    // Clear the region in the shadow that the window would cover based
    // on shadow_offset_{x,y} user provides
    int xstart = normalize_i_range(- (int) opts.shadow_offset_x, 0, swidth);
    int xrange = normalize_i_range(width - (int) opts.shadow_offset_x,
        0, swidth) - xstart;
    int ystart = normalize_i_range(- (int) opts.shadow_offset_y, 0, sheight);
    int yend = normalize_i_range(height - (int) opts.shadow_offset_y,
        0, sheight);
    int y;

    for (y = ystart; y < yend; y++) {
      memset(&data[y * swidth + xstart], 0, xrange);
    }
  }
  */

  return ximage;
}

static Picture
shadow_picture(Display *dpy, double opacity, int width, int height,
    Bool clear_shadow) {
  XImage *shadow_image = NULL;
  Pixmap shadow_pixmap = None, shadow_pixmap_argb = None;
  Picture shadow_picture = None, shadow_picture_argb = None;
  GC gc = None;

  shadow_image = make_shadow(dpy, opacity, width, height, clear_shadow);
  if (!shadow_image)
    return None;

  shadow_pixmap = XCreatePixmap(dpy, root,
    shadow_image->width, shadow_image->height, 8);
  shadow_pixmap_argb = XCreatePixmap(dpy, root,
    shadow_image->width, shadow_image->height, 32);

  if (!shadow_pixmap || !shadow_pixmap_argb)
    goto shadow_picture_err;

  shadow_picture = XRenderCreatePicture(dpy, shadow_pixmap,
    XRenderFindStandardFormat(dpy, PictStandardA8), 0, 0);
  shadow_picture_argb = XRenderCreatePicture(dpy, shadow_pixmap_argb,
    XRenderFindStandardFormat(dpy, PictStandardARGB32), 0, 0);
  if (!shadow_picture || !shadow_picture_argb)
    goto shadow_picture_err;

  gc = XCreateGC(dpy, shadow_pixmap, 0, 0);
  if (!gc)
    goto shadow_picture_err;

  XPutImage(dpy, shadow_pixmap, gc, shadow_image, 0, 0, 0, 0,
    shadow_image->width, shadow_image->height);
  XRenderComposite(dpy, PictOpSrc, cshadow_picture, shadow_picture,
      shadow_picture_argb, 0, 0, 0, 0, 0, 0,
      shadow_image->width, shadow_image->height);

  XFreeGC(dpy, gc);
  XDestroyImage(shadow_image);
  XFreePixmap(dpy, shadow_pixmap);
  XFreePixmap(dpy, shadow_pixmap_argb);
  XRenderFreePicture(dpy, shadow_picture);

  return shadow_picture_argb;

shadow_picture_err:
  if (shadow_image)
    XDestroyImage(shadow_image);
  if (shadow_pixmap)
    XFreePixmap(dpy, shadow_pixmap);
  if (shadow_pixmap_argb)
    XFreePixmap(dpy, shadow_pixmap_argb);
  if (shadow_picture)
    XRenderFreePicture(dpy, shadow_picture);
  if (shadow_picture_argb)
    XRenderFreePicture(dpy, shadow_picture_argb);
  if (gc)
    XFreeGC(dpy, gc);
  return None;
}

static Picture
solid_picture(Display *dpy, Bool argb, double a,
              double r, double g, double b) {
  Pixmap pixmap;
  Picture picture;
  XRenderPictureAttributes pa;
  XRenderColor c;

  pixmap = XCreatePixmap(dpy, root, 1, 1, argb ? 32 : 8);

  if (!pixmap) return None;

  pa.repeat = True;
  picture = XRenderCreatePicture(dpy, pixmap,
    XRenderFindStandardFormat(dpy, argb
      ? PictStandardARGB32 : PictStandardA8),
    CPRepeat,
    &pa);

  if (!picture) {
    XFreePixmap(dpy, pixmap);
    return None;
  }

  c.alpha = a * 0xffff;
  c.red = r * 0xffff;
  c.green = g * 0xffff;
  c.blue = b * 0xffff;

  XRenderFillRectangle(dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);
  XFreePixmap(dpy, pixmap);

  return picture;
}

/**
 * Errors
 */

static void
discard_ignore(Display *dpy, unsigned long sequence) {
  while (ignore_head) {
    if ((long) (sequence - ignore_head->sequence) > 0) {
      ignore  *next = ignore_head->next;
      free(ignore_head);
      ignore_head = next;
      if (!ignore_head) {
        ignore_tail = &ignore_head;
      }
    } else {
      break;
    }
  }
}

static void
set_ignore(Display *dpy, unsigned long sequence) {
  ignore *i = malloc(sizeof(ignore));
  if (!i) return;

  i->sequence = sequence;
  i->next = 0;
  *ignore_tail = i;
  ignore_tail = &i->next;
}

static int
should_ignore(Display *dpy, unsigned long sequence) {
  discard_ignore(dpy, sequence);
  return ignore_head && ignore_head->sequence == sequence;
}

/**
 * Windows
 */

/**
 * Check if a window has rounded corners.
 */
static void
win_rounded_corners(Display *dpy, win *w) {
  if (!w->bounding_shaped)
    return;

  // Fetch its bounding region
  if (!w->border_size)
    w->border_size = border_size(dpy, w);

  // Quit if border_size() returns None
  if (!w->border_size)
    return;

  // Determine the minimum width/height of a rectangle that could mark
  // a window as having rounded corners
  unsigned short minwidth = max_i(w->widthb * (1 - ROUNDED_PERCENT),
      w->widthb - ROUNDED_PIXELS);
  unsigned short minheight = max_i(w->heightb * (1 - ROUNDED_PERCENT),
      w->heightb - ROUNDED_PIXELS);

  // Get the rectangles in the bounding region
  int nrects = 0, i;
  XRectangle *rects = XFixesFetchRegion(dpy, w->border_size, &nrects);
  if (!rects)
    return;

  // Look for a rectangle large enough for this window be considered
  // having rounded corners
  for (i = 0; i < nrects; ++i)
    if (rects[i].width >= minwidth && rects[i].height >= minheight) {
      w->rounded_corners = True;
      XFree(rects);
      return;
    }

  w->rounded_corners = False;
  XFree(rects);
}

/**
 * Match a window against a single window condition.
 *
 * @return true if matched, false otherwise.
 */
static bool
win_match_once(win *w, const wincond *cond) {
  const char *target;
  bool matched = false;

#ifdef DEBUG_WINMATCH
  printf("win_match_once(%#010lx \"%s\"): cond = %p", w->id, w->name,
      cond);
#endif

  if (InputOnly == w->a.class) {
#ifdef DEBUG_WINMATCH
  printf(": InputOnly\n");
#endif
    return false;
  }

  // Determine the target
  target = NULL;
  switch (cond->target) {
    case CONDTGT_NAME:
      target = w->name;
      break;
    case CONDTGT_CLASSI:
      target = w->class_instance;
      break;
    case CONDTGT_CLASSG:
      target = w->class_general;
      break;
  }

  if (!target) {
#ifdef DEBUG_WINMATCH
  printf(": Target not found\n");
#endif
    return false;
  }

  // Determine pattern type and match
  switch (cond->type) {
    case CONDTP_EXACT:
      if (cond->flags & CONDF_IGNORECASE)
        matched = !strcasecmp(target, cond->pattern);
      else
        matched = !strcmp(target, cond->pattern);
      break;
    case CONDTP_ANYWHERE:
      if (cond->flags & CONDF_IGNORECASE)
        matched = strcasestr(target, cond->pattern);
      else
        matched = strstr(target, cond->pattern);
      break;
    case CONDTP_FROMSTART:
      if (cond->flags & CONDF_IGNORECASE)
        matched = !strncasecmp(target, cond->pattern,
            strlen(cond->pattern));
      else
        matched = !strncmp(target, cond->pattern,
            strlen(cond->pattern));
      break;
    case CONDTP_WILDCARD:
      {
        int flags = 0;
        if (cond->flags & CONDF_IGNORECASE)
          flags = FNM_CASEFOLD;
        matched = !fnmatch(cond->pattern, target, flags);
      }
      break;
    case CONDTP_REGEX_PCRE:
#ifdef CONFIG_REGEX_PCRE
      matched = (pcre_exec(cond->regex_pcre, cond->regex_pcre_extra,
            target, strlen(target), 0, 0, NULL, 0) >= 0);
#endif
      break;
  }

#ifdef DEBUG_WINMATCH
  printf(", matched = %d\n", matched);
#endif

  return matched;
}

/**
 * Match a window against a condition linked list.
 *
 * @param cache a place to cache the last matched condition
 * @return true if matched, false otherwise.
 */
static bool
win_match(win *w, wincond *condlst, wincond **cache) {
  // Check if the cached entry matches firstly
  if (cache && *cache && win_match_once(w, *cache))
    return true;

  // Then go through the whole linked list
  for (; condlst; condlst = condlst->next) {
    if (win_match_once(w, condlst)) {
      *cache = condlst;
      return true;
    }
  }

  return false;
}

/**
 * Add a pattern to a condition linked list.
 */
static Bool
condlst_add(wincond **pcondlst, const char *pattern) {
  if (!pattern)
    return False;

  unsigned plen = strlen(pattern);
  wincond *cond;
  const char *pos;

  if (plen < 4 || ':' != pattern[1] || !strchr(pattern + 2, ':')) {
    printf("Pattern \"%s\": Format invalid.\n", pattern);
    return False;
  }

  // Allocate memory for the new condition
  cond = malloc(sizeof(wincond));

  // Determine the pattern target
  switch (pattern[0]) {
    case 'n':
      cond->target = CONDTGT_NAME;
      break;
    case 'i':
      cond->target = CONDTGT_CLASSI;
      break;
    case 'g':
      cond->target = CONDTGT_CLASSG;
      break;
    default:
      printf("Pattern \"%s\": Target \"%c\" invalid.\n",
          pattern, pattern[0]);
      free(cond);
      return False;
  }

  // Determine the pattern type
  switch (pattern[2]) {
    case 'e':
      cond->type = CONDTP_EXACT;
      break;
    case 'a':
      cond->type = CONDTP_ANYWHERE;
      break;
    case 's':
      cond->type = CONDTP_FROMSTART;
      break;
    case 'w':
      cond->type = CONDTP_WILDCARD;
      break;
#ifdef CONFIG_REGEX_PCRE
    case 'p':
      cond->type = CONDTP_REGEX_PCRE;
      break;
#endif
    default:
      printf("Pattern \"%s\": Type \"%c\" invalid.\n",
          pattern, pattern[2]);
      free(cond);
      return False;
  }

  // Determine the pattern flags
  pos = &pattern[3];
  cond->flags = 0;
  while (':' != *pos) {
    switch (*pos) {
      case 'i':
        cond->flags |= CONDF_IGNORECASE;
        break;
      default:
        printf("Pattern \"%s\": Flag \"%c\" invalid.\n",
            pattern, *pos);
        break;
    }
    ++pos;
  }

  // Copy the pattern
  ++pos;
  cond->pattern = NULL;
#ifdef CONFIG_REGEX_PCRE
  cond->regex_pcre = NULL;
  cond->regex_pcre_extra = NULL;
#endif
  if (CONDTP_REGEX_PCRE == cond->type) {
#ifdef CONFIG_REGEX_PCRE
    const char *error = NULL;
    int erroffset = 0;
    int options = 0;

    if (cond->flags & CONDF_IGNORECASE)
      options |= PCRE_CASELESS;

    cond->regex_pcre = pcre_compile(pos, options, &error, &erroffset,
        NULL);
    if (!cond->regex_pcre) {
      printf("Pattern \"%s\": PCRE regular expression parsing failed on "
          "offset %d: %s\n", pattern, erroffset, error);
      free(cond);
      return False;
    }
#ifdef CONFIG_REGEX_PCRE_JIT
    cond->regex_pcre_extra = pcre_study(cond->regex_pcre, PCRE_STUDY_JIT_COMPILE, &error);
    if (!cond->regex_pcre_extra) {
      printf("Pattern \"%s\": PCRE regular expression study failed: %s",
          pattern, error);
    }
#endif
#endif
  }
  else {
    cond->pattern = mstrcpy(pos);
  }

  // Insert it into the linked list
  cond->next = *pcondlst;
  *pcondlst = cond;

  return True;
}

static long
determine_evmask(Display *dpy, Window wid, win_evmode_t mode) {
  long evmask = NoEventMask;

  if (WIN_EVMODE_FRAME == mode || find_win(dpy, wid)) {
    evmask |= PropertyChangeMask;
    if (opts.track_focus) evmask |= FocusChangeMask;
  }

  if (WIN_EVMODE_CLIENT == mode || find_toplevel(dpy, wid)) {
    if (opts.frame_opacity || opts.track_wdata
        || opts.detect_client_opacity)
      evmask |= PropertyChangeMask;
  }

  return evmask;
}

static win *
find_win(Display *dpy, Window id) {
  win *w;

  for (w = list; w; w = w->next) {
    if (w->id == id && !w->destroyed)
      return w;
  }

  return 0;
}

/**
 * Find out the WM frame of a client window using existing data.
 *
 * @param dpy display to use
 * @param w window ID
 * @return struct _win object of the found window, NULL if not found
 */
static win *
find_toplevel(Display *dpy, Window id) {
  win *w;

  for (w = list; w; w = w->next) {
    if (w->client_win == id && !w->destroyed)
      return w;
  }

  return NULL;
}

/**
 * Find out the WM frame of a client window by querying X.
 *
 * @param dpy display to use
 * @param w window ID
 * @return struct _win object of the found window, NULL if not found
 */
static win *
find_toplevel2(Display *dpy, Window wid) {
  win *w = NULL;

  // We traverse through its ancestors to find out the frame
  while (wid && wid != root && !(w = find_win(dpy, wid))) {
    Window troot;
    Window parent;
    Window *tchildren;
    unsigned tnchildren;

    // XQueryTree probably fails if you run compton when X is somehow
    // initializing (like add it in .xinitrc). In this case
    // just leave it alone.
    if (!XQueryTree(dpy, wid, &troot, &parent, &tchildren,
          &tnchildren)) {
      parent = 0;
      break;
    }

    if (tchildren) XFree(tchildren);

    wid = parent;
  }

  return w;
}

/**
 * Recheck currently focused window and set its <code>w->focused</code>
 * to True.
 *
 * @param dpy display to use
 * @return struct _win of currently focused window, NULL if not found
 */
static win *
recheck_focus(Display *dpy) {
  // Determine the currently focused window so we can apply appropriate
  // opacity on it
  Window wid = 0;
  int revert_to;
  win *w = NULL;

  XGetInputFocus(dpy, &wid, &revert_to);

  // Fallback to the old method if find_toplevel() fails
  if (!(w = find_toplevel(dpy, wid))) {
    w = find_toplevel2(dpy, wid);
  }

  // And we set the focus state and opacity here
  if (w) {
    set_focused(dpy, w, True);
    return w;
  }

  return NULL;
}

static Picture
root_tile_f(Display *dpy) {
  /*
  if (opts.paint_on_overlay) {
    return root_picture;
  } */

  Picture picture;
  Atom actual_type;
  Pixmap pixmap;
  int actual_format;
  unsigned long nitems;
  unsigned long bytes_after;
  unsigned char *prop;
  Bool fill;
  XRenderPictureAttributes pa;
  int p;

  pixmap = None;

  for (p = 0; background_props[p]; p++) {
    prop = NULL;
    if (XGetWindowProperty(dpy, root,
          XInternAtom(dpy, background_props[p], False),
          0, 4, False, AnyPropertyType, &actual_type,
          &actual_format, &nitems, &bytes_after, &prop
        ) == Success
        && actual_type == XInternAtom(dpy, "PIXMAP", False)
        && actual_format == 32 && nitems == 1) {
      memcpy(&pixmap, prop, 4);
      XFree(prop);
      fill = False;
      break;
    } else if (prop)
      XFree(prop);
  }

  if (!pixmap) {
    pixmap = XCreatePixmap(dpy, root, 1, 1, DefaultDepth(dpy, scr));
    fill = True;
  }

  pa.repeat = True;
  picture = XRenderCreatePicture(
    dpy, pixmap, XRenderFindVisualFormat(dpy, DefaultVisual(dpy, scr)),
    CPRepeat, &pa);

  if (fill) {
    XRenderColor  c;

    c.red = c.green = c.blue = 0x8080;
    c.alpha = 0xffff;
    XRenderFillRectangle(
      dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);
  }

  return picture;
}

static void
paint_root(Display *dpy) {
  if (!root_tile) {
    root_tile = root_tile_f(dpy);
  }

  XRenderComposite(
    dpy, PictOpSrc, root_tile, None,
    tgt_buffer, 0, 0, 0, 0, 0, 0,
    root_width, root_height);
}

/**
 * Get a rectangular region a window occupies, excluding shadow.
 */
static XserverRegion
win_get_region(Display *dpy, win *w) {
  XRectangle r;

  r.x = w->a.x;
  r.y = w->a.y;
  r.width = w->widthb;
  r.height = w->heightb;

  return XFixesCreateRegion(dpy, &r, 1);
}

/**
 * Get a rectangular region a window occupies, excluding frame and shadow.
 */
static XserverRegion
win_get_region_noframe(Display *dpy, win *w) {
  XRectangle r;

  r.x = w->a.x + w->a.border_width + w->left_width;
  r.y = w->a.y + w->a.border_width + w->top_width;
  r.width = max_i(w->a.width - w->left_width - w->right_width, 0);
  r.height = max_i(w->a.height - w->top_width - w->bottom_width, 0);

  if (r.width > 0 && r.height > 0)
    return XFixesCreateRegion(dpy, &r, 1);
  else
    return XFixesCreateRegion(dpy, NULL, 0);
}

/**
 * Get a rectangular region a window (and possibly its shadow) occupies.
 *
 * Note w->shadow and shadow geometry must be correct before calling this
 * function.
 */
static XserverRegion
win_extents(Display *dpy, win *w) {
  XRectangle r;

  r.x = w->a.x;
  r.y = w->a.y;
  r.width = w->widthb;
  r.height = w->heightb;

  if (w->shadow) {
    XRectangle sr;

    sr.x = w->a.x + w->shadow_dx;
    sr.y = w->a.y + w->shadow_dy;
    sr.width = w->shadow_width;
    sr.height = w->shadow_height;

    if (sr.x < r.x) {
      r.width = (r.x + r.width) - sr.x;
      r.x = sr.x;
    }

    if (sr.y < r.y) {
      r.height = (r.y + r.height) - sr.y;
      r.y = sr.y;
    }

    if (sr.x + sr.width > r.x + r.width) {
      r.width = sr.x + sr.width - r.x;
    }

    if (sr.y + sr.height > r.y + r.height) {
      r.height = sr.y + sr.height - r.y;
    }
  }

  return XFixesCreateRegion(dpy, &r, 1);
}

static XserverRegion
border_size(Display *dpy, win *w) {
  // Start with the window rectangular region
  XserverRegion fin = win_get_region(dpy, w);

  // Only request for a bounding region if the window is shaped
  if (w->bounding_shaped) {
    /*
     * if window doesn't exist anymore,  this will generate an error
     * as well as not generate a region.  Perhaps a better XFixes
     * architecture would be to have a request that copies instead
     * of creates, that way you'd just end up with an empty region
     * instead of an invalid XID.
     */

    XserverRegion border = XFixesCreateRegionFromWindow(
      dpy, w->id, WindowRegionBounding);

    if (!border)
      return fin;

    // Translate the region to the correct place
    XFixesTranslateRegion(dpy, border,
      w->a.x + w->a.border_width,
      w->a.y + w->a.border_width);

    // Intersect the bounding region we got with the window rectangle, to
    // make sure the bounding region is not bigger than the window
    // rectangle
    XFixesIntersectRegion(dpy, fin, fin, border);
    XFixesDestroyRegion(dpy, border);
  }

  return fin;
}

static Window
find_client_win(Display *dpy, Window w) {
  if (wid_has_attr(dpy, w, client_atom)) {
    return w;
  }

  Window *children;
  unsigned int nchildren;
  unsigned int i;
  Window ret = 0;

  if (!wid_get_children(dpy, w, &children, &nchildren)) {
    return 0;
  }

  for (i = 0; i < nchildren; ++i) {
    if ((ret = find_client_win(dpy, children[i])))
      break;
  }

  XFree(children);

  return ret;
}

static void
get_frame_extents(Display *dpy, win *w, Window client) {
  long *extents;
  Atom type;
  int format;
  unsigned long nitems, after;
  unsigned char *data = NULL;
  int result;

  w->left_width = 0;
  w->right_width = 0;
  w->top_width = 0;
  w->bottom_width = 0;

  result = XGetWindowProperty(
    dpy, client, frame_extents_atom,
    0L, 4L, False, AnyPropertyType,
    &type, &format, &nitems, &after,
    &data);

  if (result == Success) {
    if (nitems == 4 && after == 0) {
      extents = (long *) data;
      w->left_width = extents[0];
      w->right_width = extents[1];
      w->top_width = extents[2];
      w->bottom_width = extents[3];

      if (opts.frame_opacity)
        update_reg_ignore_expire(w);
    }
    XFree(data);
  }
}

static inline Picture
get_alpha_pict_d(double o) {
  assert((lround(normalize_d(o) / opts.alpha_step)) <= lround(1.0 / opts.alpha_step));
  return alpha_picts[lround(normalize_d(o) / opts.alpha_step)];
}

static inline Picture
get_alpha_pict_o(opacity_t o) {
  return get_alpha_pict_d((double) o / OPAQUE);
}

static win *
paint_preprocess(Display *dpy, win *list) {
  win *w;
  win *t = NULL, *next = NULL;

  // Fading step calculation
  unsigned steps = (sub_unslong(get_time_ms(), fade_time)
      + FADE_DELTA_TOLERANCE * opts.fade_delta) / opts.fade_delta;
  fade_time += steps * opts.fade_delta;

  XserverRegion last_reg_ignore = None;

  for (w = list; w; w = next) {
    Bool to_paint = True;
    const winmode mode_old = w->mode;

    // In case calling the fade callback function destroys this window
    next = w->next;
    opacity_t opacity_old = w->opacity;

    // Destroy reg_ignore on all windows if they should expire
    if (reg_ignore_expire)
      free_region(dpy, &w->reg_ignore);

    // Run fading
    run_fade(dpy, w, steps);

    // Give up if it's not damaged or invisible, or it's unmapped and its
    // picture is gone (for example due to a ConfigureNotify)
    if (!w->damaged
        || w->a.x + w->a.width < 1 || w->a.y + w->a.height < 1
        || w->a.x >= root_width || w->a.y >= root_height
        || (IsUnmapped == w->a.map_state && !w->picture)) {
      to_paint = False;
    }

    if (to_paint) {
      // If opacity changes
      if (w->opacity != opacity_old) {
        determine_mode(dpy, w);
        add_damage_win(dpy, w);
      }

      w->alpha_pict = get_alpha_pict_o(w->opacity);

      // End the game if we are using the 0 opacity alpha_pict
      if (w->alpha_pict == alpha_picts[0]) {
        to_paint = False;
      }
    }

    if (to_paint) {
      // Fetch the picture and pixmap if needed
      if (!w->picture) {
        XRenderPictureAttributes pa;
        XRenderPictFormat *format;
        Drawable draw = w->id;

        if (has_name_pixmap && !w->pixmap) {
          set_ignore(dpy, NextRequest(dpy));
          w->pixmap = XCompositeNameWindowPixmap(dpy, w->id);
        }
        if (w->pixmap) draw = w->pixmap;

        format = XRenderFindVisualFormat(dpy, w->a.visual);
        pa.subwindow_mode = IncludeInferiors;
        w->picture = XRenderCreatePicture(
          dpy, draw, format, CPSubwindowMode, &pa);
      }

      // Fetch bounding region
      if (!w->border_size) {
        // Build a border_size ourselves if window is not shaped, to avoid
        // getting an invalid border_size region from X if the window is
        // unmapped/destroyed
        if (!w->bounding_shaped) {
          w->border_size = win_get_region(dpy, w);
        }
        else if (IsUnmapped != w->a.map_state) {
          w->border_size = border_size(dpy, w);
        }
      }

      // Fetch window extents
      if (!w->extents) {
        w->extents = win_extents(dpy, w);
        // If w->extents does not exist, the previous add_damage_win()
        // call when opacity changes has no effect, so redo it here.
        if (w->opacity != opacity_old)
          add_damage_win(dpy, w);
      }

      // Calculate frame_opacity
      {
        double frame_opacity_old = w->frame_opacity;

        if (opts.frame_opacity && 1.0 != opts.frame_opacity
            && win_has_frame(w))
          w->frame_opacity = get_opacity_percent(dpy, w) *
            opts.frame_opacity;
        else
          w->frame_opacity = 0.0;

        if (w->to_paint && WINDOW_SOLID == mode_old
            && (0.0 == frame_opacity_old) != (0.0 == w->frame_opacity))
          reg_ignore_expire = True;
      }

      w->frame_alpha_pict = get_alpha_pict_d(w->frame_opacity);

      // Calculate shadow opacity
      if (w->frame_opacity)
        w->shadow_opacity = opts.shadow_opacity * w->frame_opacity;
      else
        w->shadow_opacity = opts.shadow_opacity * get_opacity_percent(dpy, w);

      // Rebuild shadow_pict if necessary
      if (w->flags & WFLAG_SIZE_CHANGE)
        free_picture(dpy, &w->shadow_pict);

      if (w->shadow && !w->shadow_pict) {
        w->shadow_pict = shadow_picture(dpy, 1,
            w->widthb, w->heightb, False);
      }

      w->shadow_alpha_pict = get_alpha_pict_d(w->shadow_opacity);
    }

    if ((to_paint && WINDOW_SOLID == w->mode)
        != (w->to_paint && WINDOW_SOLID == mode_old))
      reg_ignore_expire = True;

    if (to_paint) {
      // Generate ignore region for painting to reduce GPU load
      if (reg_ignore_expire || !w->to_paint) {
        free_region(dpy, &w->reg_ignore);

        // If the window is solid, we add the window region to the
        // ignored region
        if (WINDOW_SOLID == w->mode) {
          if (!w->frame_opacity) {
            if (w->border_size)
              w->reg_ignore = copy_region(dpy, w->border_size);
            else
              w->reg_ignore = win_get_region(dpy, w);
          }
          else {
            w->reg_ignore = win_get_region_noframe(dpy, w);
            if (w->border_size)
              XFixesIntersectRegion(dpy, w->reg_ignore, w->reg_ignore,
                  w->border_size);
          }

          if (last_reg_ignore)
            XFixesUnionRegion(dpy, w->reg_ignore, w->reg_ignore,
                last_reg_ignore);
        }
        // Otherwise we copy the last region over
        else if (last_reg_ignore)
          w->reg_ignore = copy_region(dpy, last_reg_ignore);
        else
          w->reg_ignore = None;
      }

      last_reg_ignore = w->reg_ignore;

      // Reset flags
      w->flags = 0;
    }


    if (to_paint) {
      w->prev_trans = t;
      t = w;
    }
    else {
      check_fade_fin(dpy, w);
    }

    w->to_paint = to_paint;
  }

  return t;
}

/**
 * Paint the shadow of a window.
 */
static inline void
win_paint_shadow(Display *dpy, win *w, Picture tgt_buffer) {
  XRenderComposite(
    dpy, PictOpOver, w->shadow_pict, w->shadow_alpha_pict,
    tgt_buffer, 0, 0, 0, 0,
    w->a.x + w->shadow_dx, w->a.y + w->shadow_dy,
    w->shadow_width, w->shadow_height);
}

/**
 * Paint a window itself and dim it if asked.
 */
static inline void
win_paint_win(Display *dpy, win *w, Picture tgt_buffer) {
  int x = w->a.x;
  int y = w->a.y;
  int wid = w->widthb;
  int hei = w->heightb;

  Picture alpha_mask = (OPAQUE == w->opacity ? None: w->alpha_pict);
  int op = (w->mode == WINDOW_SOLID ? PictOpSrc: PictOpOver);

  if (!w->frame_opacity) {
    XRenderComposite(dpy, op, w->picture, alpha_mask,
        tgt_buffer, 0, 0, 0, 0, x, y, wid, hei);
  }
  else {
    int t = w->top_width;
    int l = w->left_width;
    int b = w->bottom_width;
    int r = w->right_width;

#define COMP_BDR(cx, cy, cwid, chei) \
    XRenderComposite(dpy, PictOpOver, w->picture, w->frame_alpha_pict, \
        tgt_buffer, (cx), (cy), 0, 0, x + (cx), y + (cy), (cwid), (chei))

    // The following complicated logic is requried because some broken
    // window managers (I'm talking about you, Openbox!) that makes
    // top_width + bottom_width > height in some cases.

    // top
    COMP_BDR(0, 0, wid, min_i(t, hei));

    if (hei > t) {
      int phei = min_i(hei - t, b);

      // bottom
      if (phei) {
        assert(phei > 0);
        COMP_BDR(0, hei - phei, wid, phei);

        phei = hei - t - phei;
        if (phei) {
          assert(phei > 0);
          // left
          COMP_BDR(0, t, min_i(l, wid), phei);

          if (wid > l) {
            int pwid = min_i(wid - l, r);

            if (pwid) {
              assert(pwid > 0);
              // right
              COMP_BDR(wid - pwid, t, pwid, phei);

              pwid = wid - l - pwid;
              if (pwid)
                assert(pwid > 0);
                // body
                XRenderComposite(dpy, op, w->picture, alpha_mask,
                    tgt_buffer, l, t, 0, 0, x + l, y + t, pwid, phei);
            }
          }
        }
      }
    }
  }

#undef COMP_BDR

  // Dimming the window if needed
  if (w->dim) {
    XRenderComposite(dpy, PictOpOver, dim_picture, None,
        tgt_buffer, 0, 0, 0, 0, x, y, wid, hei);
  }
}

/**
 * Rebuild cached <code>screen_reg</code>.
 */
static void
rebuild_screen_reg(Display *dpy) {
  if (screen_reg)
    XFixesDestroyRegion(dpy, screen_reg);
  screen_reg = get_screen_region(dpy);
}

static void
paint_all(Display *dpy, XserverRegion region, win *t) {
#ifdef DEBUG_REPAINT
  static struct timespec last_paint = { 0 };
#endif

  win *w;
  XserverRegion reg_paint = None, reg_tmp = None, reg_tmp2 = None;

  if (!region) {
    region = get_screen_region(dpy);
  }
  else {
    // Remove the damaged area out of screen
    XFixesIntersectRegion(dpy, region, region, screen_reg);
  }

#ifdef MONITOR_REPAINT
  // Note: MONITOR_REPAINT cannot work with DBE right now.
  tgt_buffer = tgt_picture;
#else
  if (!tgt_buffer) {
    // DBE painting mode: Directly paint to a Picture of the back buffer
    if (opts.dbe) {
      tgt_buffer = XRenderCreatePicture(dpy, root_dbe,
          XRenderFindVisualFormat(dpy, DefaultVisual(dpy, scr)),
          0, 0);
    }
    // No-DBE painting mode: Paint to an intermediate Picture then paint
    // the Picture to root window
    else {
      Pixmap root_pixmap = XCreatePixmap(
        dpy, root, root_width, root_height,
        DefaultDepth(dpy, scr));

      tgt_buffer = XRenderCreatePicture(dpy, root_pixmap,
        XRenderFindVisualFormat(dpy, DefaultVisual(dpy, scr)),
        0, 0);

      XFreePixmap(dpy, root_pixmap);
    }
  }
#endif

  XFixesSetPictureClipRegion(dpy, tgt_picture, 0, 0, region);

#ifdef MONITOR_REPAINT
  XRenderComposite(
    dpy, PictOpSrc, black_picture, None,
    tgt_picture, 0, 0, 0, 0, 0, 0,
    root_width, root_height);
#endif

  if (t && t->reg_ignore) {
    // Calculate the region upon which the root window is to be painted
    // based on the ignore region of the lowest window, if available
    reg_paint = reg_tmp = XFixesCreateRegion(dpy, NULL, 0);
    XFixesSubtractRegion(dpy, reg_paint, region, t->reg_ignore);
  }
  else {
    reg_paint = region;
  }

  XFixesSetPictureClipRegion(dpy, tgt_buffer, 0, 0, reg_paint);

  paint_root(dpy);

  // Create temporary regions for use during painting
  if (!reg_tmp)
    reg_tmp = XFixesCreateRegion(dpy, NULL, 0);
  reg_tmp2 = XFixesCreateRegion(dpy, NULL, 0);

  for (w = t; w; w = w->prev_trans) {
    // Painting shadow
    if (w->shadow) {
      // Shadow is to be painted based on the ignore region of current
      // window
      if (w->reg_ignore) {
        if (w == t) {
          // If it's the first cycle and reg_tmp2 is not ready, calculate
          // the paint region here
          reg_paint = reg_tmp;
          XFixesSubtractRegion(dpy, reg_paint, region, w->reg_ignore);
        }
        else {
          // Otherwise, used the cached region during last cycle
          reg_paint = reg_tmp2;
        }
        XFixesIntersectRegion(dpy, reg_paint, reg_paint, w->extents);
      }
      else {
        reg_paint = reg_tmp;
        XFixesIntersectRegion(dpy, reg_paint, region, w->extents);
      }
      // Clear the shadow here instead of in make_shadow() for saving GPU
      // power and handling shaped windows
      if (opts.clear_shadow && w->border_size)
        XFixesSubtractRegion(dpy, reg_paint, reg_paint, w->border_size);

      // Detect if the region is empty before painting
      if (region == reg_paint || !is_region_empty(dpy, reg_paint)) {
        XFixesSetPictureClipRegion(dpy, tgt_buffer, 0, 0, reg_paint);

        win_paint_shadow(dpy, w, tgt_buffer);
      }
    }

    // Calculate the region based on the reg_ignore of the next (higher)
    // window and the bounding region
    reg_paint = reg_tmp;
    if (w->prev_trans && w->prev_trans->reg_ignore) {
      XFixesSubtractRegion(dpy, reg_paint, region,
          w->prev_trans->reg_ignore);
      // Copy the subtracted region to be used for shadow painting in next
      // cycle
      XFixesCopyRegion(dpy, reg_tmp2, reg_paint);

      if (w->border_size)
        XFixesIntersectRegion(dpy, reg_paint, reg_paint, w->border_size);
    }
    else {
      if (w->border_size)
        XFixesIntersectRegion(dpy, reg_paint, region, w->border_size);
      else
        reg_paint = region;
    }

    if (!is_region_empty(dpy, reg_paint)) {
      XFixesSetPictureClipRegion(dpy, tgt_buffer, 0, 0, reg_paint);

      // Painting the window
      win_paint_win(dpy, w, tgt_buffer);
    }

    check_fade_fin(dpy, w);
  }

  // Free up all temporary regions
  XFixesDestroyRegion(dpy, region);
  XFixesDestroyRegion(dpy, reg_tmp);
  XFixesDestroyRegion(dpy, reg_tmp2);

  // Do this as early as possible
  if (!opts.dbe)
    XFixesSetPictureClipRegion(dpy, tgt_buffer, 0, 0, None);

  if (VSYNC_NONE != opts.vsync) {
    // Make sure all previous requests are processed to achieve best
    // effect
    XSync(dpy, False);
  }

  // Wait for VBlank. We could do it aggressively (send the painting
  // request and XFlush() on VBlank) or conservatively (send the request
  // only on VBlank).
  if (!opts.vsync_aggressive)
    vsync_wait();

  // DBE painting mode, only need to swap the buffer
  if (opts.dbe) {
    XdbeSwapInfo swap_info = {
      .swap_window = (opts.paint_on_overlay ? overlay: root),
      // Is it safe to use XdbeUndefined?
      .swap_action = XdbeCopied
    };
    XdbeSwapBuffers(dpy, &swap_info, 1);
  }
  // No-DBE painting mode
  else if (tgt_buffer != tgt_picture) {
    XRenderComposite(
      dpy, PictOpSrc, tgt_buffer, None,
      tgt_picture, 0, 0, 0, 0,
      0, 0, root_width, root_height);
  }

  if (opts.vsync_aggressive)
    vsync_wait();

  XFlush(dpy);

#ifdef DEBUG_REPAINT
  print_timestamp();
  struct timespec now = get_time_timespec();
  struct timespec diff = { 0 };
  timespec_subtract(&diff, &now, &last_paint);
  printf("[ %5ld:%09ld ] ", diff.tv_sec, diff.tv_nsec);
  last_paint = now;
  printf("paint:");
  for (w = t; w; w = w->prev_trans)
    printf(" %#010lx", w->id);
  putchar('\n');
  fflush(stdout);
#endif
}

static void
add_damage(Display *dpy, XserverRegion damage) {
  if (all_damage) {
    XFixesUnionRegion(dpy, all_damage, all_damage, damage);
    XFixesDestroyRegion(dpy, damage);
  } else {
    all_damage = damage;
  }
}

static void
repair_win(Display *dpy, win *w) {
  XserverRegion parts;

  if (!w->damaged) {
    parts = win_extents(dpy, w);
    set_ignore(dpy, NextRequest(dpy));
    XDamageSubtract(dpy, w->damage, None, None);
  } else {
    parts = XFixesCreateRegion(dpy, 0, 0);
    set_ignore(dpy, NextRequest(dpy));
    XDamageSubtract(dpy, w->damage, None, parts);
    XFixesTranslateRegion(dpy, parts,
      w->a.x + w->a.border_width,
      w->a.y + w->a.border_width);
  }

  // Remove the part in the damage area that could be ignored
  if (!reg_ignore_expire && w->prev_trans && w->prev_trans->reg_ignore)
    XFixesSubtractRegion(dpy, parts, parts, w->prev_trans->reg_ignore);

  add_damage(dpy, parts);
  w->damaged = 1;
}

static wintype
get_wintype_prop(Display *dpy, Window wid) {
  Atom actual;
  int format;
  unsigned long n = 0, left, i;
  long *data = NULL;
  int j;

  set_ignore(dpy, NextRequest(dpy));
  if (Success != XGetWindowProperty(
        dpy, wid, win_type_atom, 0L, 32L, False, XA_ATOM,
        &actual, &format, &n, &left, (unsigned char **) &data)
      || !data || !n) {
    if (data)
      XFree(data);
    return WINTYPE_UNKNOWN;
  }

  for (i = 0; i < n; ++i) {
    for (j = 1; j < NUM_WINTYPES; ++j) {
      if (win_type[j] == (Atom) data[i]) {
        XFree(data);
        return j;
      }
    }
  }

  XFree(data);

  return WINTYPE_UNKNOWN;
}

static void
map_win(Display *dpy, Window id,
        unsigned long sequence, Bool fade,
        Bool override_redirect) {
  win *w = find_win(dpy, id);

  // Don't care about window mapping if it's an InputOnly window
  if (!w || InputOnly == w->a.class) return;

  w->focused = False;
  w->a.map_state = IsViewable;

  // Call XSelectInput() before reading properties so that no property
  // changes are lost
  XSelectInput(dpy, id, determine_evmask(dpy, id, WIN_EVMODE_FRAME));

  // Notify compton when the shape of a window changes
  if (shape_exists) {
    XShapeSelectInput(dpy, id, ShapeNotifyMask);
  }

  // Detect client window here instead of in add_win() as the client
  // window should have been prepared at this point
  if (!w->client_win) {
    Window cw = 0;
    // Always recursively look for a window with WM_STATE, as Fluxbox
    // sets override-redirect flags on all frame windows.
    cw = find_client_win(dpy, w->id);
#ifdef DEBUG_CLIENTWIN
    printf("find_client_win(%#010lx): client %#010lx\n", w->id, cw);
#endif
    // Set a window's client window to itself only if we didn't find a
    // client window and the window has override-redirect flag
    if (!cw && w->a.override_redirect) {
      cw = w->id;
#ifdef DEBUG_CLIENTWIN
      printf("find_client_win(%#010lx): client self (override-redirected)\n", w->id);
#endif
    }
    if (cw) {
      mark_client_win(dpy, w, cw);
    }
  }
  else if (opts.frame_opacity) {
    // Refetch frame extents just in case it changes when the window is
    // unmapped
    get_frame_extents(dpy, w, w->client_win);
  }

  // Workaround for _NET_WM_WINDOW_TYPE for Openbox menus, which is
  // set on a non-override-redirect window with no WM_STATE either
  if (!w->client_win && WINTYPE_UNKNOWN == w->window_type)
    w->window_type = get_wintype_prop(dpy, w->id);

#ifdef DEBUG_WINTYPE
  printf("map_win(%#010lx): type %s\n",
    w->id, WINTYPES[w->window_type]);
#endif

  // Detect if the window is shaped or has rounded corners
  win_update_shape(dpy, w);

  // Get window name and class if we are tracking them
  if (opts.track_wdata) {
    win_get_name(dpy, w);
    win_get_class(dpy, w);
  }

  /*
   * Occasionally compton does not seem able to get a FocusIn event from a
   * window just mapped. I suspect it's a timing issue again when the
   * XSelectInput() is called too late. We have to recheck the focused
   * window here.
   */
  if (opts.track_focus) {
    recheck_focus(dpy);
    // Consider a window without client window a WM window and mark it
    // focused if mark_wmwin_focused is on, or it's over-redirected and
    // mark_ovredir_focused is on
    if ((opts.mark_wmwin_focused && !w->client_win)
        || (opts.mark_ovredir_focused && w->a.override_redirect))
      w->focused = True;
  }

  // Window type change and bounding shape state change could affect
  // shadow
  determine_shadow(dpy, w);

  // Fading in
  calc_opacity(dpy, w, True);

  // Set fading state
  if (opts.no_fading_openclose) {
    set_fade_callback(dpy, w, finish_map_win, True);
    // Must be set after we execute the old fade callback, in case we
    // receive two continuous MapNotify for the same window
    w->fade = False;
  }
  else {
    set_fade_callback(dpy, w, NULL, True);
    determine_fade(dpy, w);
  }

  calc_dim(dpy, w);

  w->damaged = 1;


  /* if any configure events happened while
     the window was unmapped, then configure
     the window to its correct place */
  if (w->need_configure) {
    configure_win(dpy, &w->queue_configure);
  }
}

static void
finish_map_win(Display *dpy, win *w) {
  if (opts.no_fading_openclose)
    determine_fade(dpy, w);
}

static void
finish_unmap_win(Display *dpy, win *w) {
  w->damaged = 0;

  update_reg_ignore_expire(w);

  if (w->extents != None) {
    /* destroys region */
    add_damage(dpy, w->extents);
    w->extents = None;
  }

  free_pixmap(dpy, &w->pixmap);
  free_picture(dpy, &w->picture);
  free_region(dpy, &w->border_size);
  free_picture(dpy, &w->shadow_pict);
}

static void
unmap_callback(Display *dpy, win *w) {
  finish_unmap_win(dpy, w);
}

static void
unmap_win(Display *dpy, Window id, Bool fade) {
  win *w = find_win(dpy, id);

  if (!w) return;

  w->a.map_state = IsUnmapped;

  // Fading out
  w->opacity_tgt = 0;
  set_fade_callback(dpy, w, unmap_callback, False);
  if (opts.no_fading_openclose)
    w->fade = False;

  // don't care about properties anymore
  // Will get BadWindow if the window is destroyed
  set_ignore(dpy, NextRequest(dpy));
  XSelectInput(dpy, w->id, 0);

  if (w->client_win) {
    set_ignore(dpy, NextRequest(dpy));
    XSelectInput(dpy, w->client_win, 0);
  }
}

static opacity_t
wid_get_opacity_prop(Display *dpy, Window wid, opacity_t def) {
  Atom actual;
  int format;
  unsigned long n, left;

  unsigned char *data;
  int result = XGetWindowProperty(
    dpy, wid, opacity_atom, 0L, 1L, False,
    XA_CARDINAL, &actual, &format, &n, &left, &data);

  if (result == Success && data != NULL) {
    opacity_t i = *((opacity_t *) data);
    XFree(data);
    return i;
  }

  return def;
}

static double
get_opacity_percent(Display *dpy, win *w) {
  return ((double) w->opacity) / OPAQUE;
}

static void
determine_mode(Display *dpy, win *w) {
  winmode mode;
  XRenderPictFormat *format;

  /* if trans prop == -1 fall back on previous tests */

  if (w->a.class == InputOnly) {
    format = 0;
  } else {
    format = XRenderFindVisualFormat(dpy, w->a.visual);
  }

  if (format && format->type == PictTypeDirect
      && format->direct.alphaMask) {
    mode = WINDOW_ARGB;
  } else if (w->opacity != OPAQUE) {
    mode = WINDOW_TRANS;
  } else {
    mode = WINDOW_SOLID;
  }

  w->mode = mode;
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
 * @param dpy X display to use
 * @param w struct _win object representing the window
 * @param refetch_prop whether _NET_WM_OPACITY of the window needs to be
 *    refetched
 */
static void
calc_opacity(Display *dpy, win *w, Bool refetch_prop) {
  opacity_t opacity;

  // Do nothing for unmapped window, calc_opacity() will be called
  // when it's mapped
  // I suppose I need not to check for IsUnviewable here?
  if (IsViewable != w->a.map_state) return;

  // Do not refetch the opacity window attribute unless necessary, this
  // is probably an expensive operation in some cases
  if (refetch_prop) {
    w->opacity_prop = wid_get_opacity_prop(dpy, w->id, OPAQUE);
    if (!opts.detect_client_opacity || !w->client_win
        || w->id == w->client_win)
      w->opacity_prop_client = OPAQUE;
    else
      w->opacity_prop_client = wid_get_opacity_prop(dpy, w->client_win,
            OPAQUE);
  }

  if (OPAQUE == (opacity = w->opacity_prop)
      && OPAQUE == (opacity = w->opacity_prop_client)) {
    opacity = opts.wintype_opacity[w->window_type] * OPAQUE;
  }

  // Respect inactive_opacity in some cases
  if (opts.inactive_opacity && is_normal_win(w) && False == w->focused
      && (OPAQUE == opacity || opts.inactive_opacity_override)) {
    opacity = opts.inactive_opacity;
  }

  w->opacity_tgt = opacity;
}

static void
calc_dim(Display *dpy, win *w) {
  Bool dim;

  if (opts.inactive_dim && is_normal_win(w) && !(w->focused)) {
    dim = True;
  } else {
    dim = False;
  }

  if (dim != w->dim) {
    w->dim = dim;
    add_damage_win(dpy, w);
  }
}

/**
 * Determine if a window should fade on opacity change.
 */
static void
determine_fade(Display *dpy, win *w) {
  w->fade = opts.wintype_fade[w->window_type];
}

/**
 * Update window-shape related information.
 */
static void
win_update_shape(Display *dpy, win *w) {
  if (shape_exists) {
    // Bool bounding_shaped_old = w->bounding_shaped;

    w->bounding_shaped = wid_bounding_shaped(dpy, w->id);
    if (w->bounding_shaped && opts.detect_rounded_corners)
      win_rounded_corners(dpy, w);

    // Shadow state could be changed
    determine_shadow(dpy, w);

    /*
    // If clear_shadow state on the window possibly changed, destroy the old
    // shadow_pict
    if (opts.clear_shadow && w->bounding_shaped != bounding_shaped_old)
      free_picture(dpy, &w->shadow_pict);
    */
  }
}

/**
 * Determine if a window should have shadow, and update things depending
 * on shadow state.
 */
static void
determine_shadow(Display *dpy, win *w) {
  Bool shadow_old = w->shadow;

  w->shadow = (opts.wintype_shadow[w->window_type]
      && !win_match(w, opts.shadow_blacklist, &w->cache_sblst)
      && !(opts.shadow_ignore_shaped && w->bounding_shaped
        && !w->rounded_corners));

  // Window extents need update on shadow state change
  if (w->shadow != shadow_old) {
    // Shadow geometry currently doesn't change on shadow state change
    // calc_shadow_geometry(dpy, w);
    if (w->extents) {
      // Mark the old extents as damaged if the shadow is removed
      if (!w->shadow)
        add_damage(dpy, w->extents);
      else
        free_region(dpy, &w->extents);
      w->extents = win_extents(dpy, w);
      // Mark the new extents as damaged if the shadow is added
      if (w->shadow)
        add_damage_win(dpy, w);
    }
  }
}

/**
 * Update cache data in struct _win that depends on window size.
 */

static void
calc_win_size(Display *dpy, win *w) {
  w->widthb = w->a.width + w->a.border_width * 2;
  w->heightb = w->a.height + w->a.border_width * 2;
  calc_shadow_geometry(dpy, w);
  w->flags |= WFLAG_SIZE_CHANGE;
}

/**
 * Calculate and update geometry of the shadow of a window.
 */
static void
calc_shadow_geometry(Display *dpy, win *w) {
  w->shadow_dx = opts.shadow_offset_x;
  w->shadow_dy = opts.shadow_offset_y;
  w->shadow_width = w->widthb + gaussian_map->size;
  w->shadow_height = w->heightb + gaussian_map->size;
}

/**
 * Mark a window as the client window of another.
 *
 * @param dpy display to use
 * @param w struct _win of the parent window
 * @param client window ID of the client window
 */
static void
mark_client_win(Display *dpy, win *w, Window client) {
  w->client_win = client;

  XSelectInput(dpy, client, determine_evmask(dpy, client, WIN_EVMODE_CLIENT));

  // Get the frame width and monitor further frame width changes on client
  // window if necessary
  if (opts.frame_opacity) {
    get_frame_extents(dpy, w, client);
  }

  // Detect window type here
  if (WINTYPE_UNKNOWN == w->window_type)
    w->window_type = get_wintype_prop(dpy, w->client_win);

  // Conform to EWMH standard, if _NET_WM_WINDOW_TYPE is not present, take
  // override-redirect windows or windows without WM_TRANSIENT_FOR as
  // _NET_WM_WINDOW_TYPE_NORMAL, otherwise as _NET_WM_WINDOW_TYPE_DIALOG.
  if (WINTYPE_UNKNOWN == w->window_type) {
    if (w->a.override_redirect
        || !wid_has_attr(dpy, client, transient_atom))
      w->window_type = WINTYPE_NORMAL;
    else
      w->window_type = WINTYPE_DIALOG;
  }
}

static void
add_win(Display *dpy, Window id, Window prev, Bool override_redirect) {
  if (find_win(dpy, id)) {
    return;
  }

  win *new = malloc(sizeof(win));
  win **p;

  if (!new) return;

  if (prev) {
    for (p = &list; *p; p = &(*p)->next) {
      if ((*p)->id == prev && !(*p)->destroyed)
        break;
    }
  } else {
    p = &list;
  }

  new->id = id;
  set_ignore(dpy, NextRequest(dpy));

  if (!XGetWindowAttributes(dpy, id, &new->a)) {
    free(new);
    return;
  }

  new->damaged = 0;
  new->to_paint = False;
  new->pixmap = None;
  new->picture = None;

  if (new->a.class == InputOnly) {
    new->damage_sequence = 0;
    new->damage = None;
  } else {
    new->damage_sequence = NextRequest(dpy);
    set_ignore(dpy, NextRequest(dpy));
    new->damage = XDamageCreate(dpy, id, XDamageReportNonEmpty);
  }

  new->name = NULL;
  new->class_instance = NULL;
  new->class_general = NULL;
  new->cache_sblst = NULL;
  new->cache_fblst = NULL;
  new->bounding_shaped = False;
  new->rounded_corners = False;

  new->border_size = None;
  new->reg_ignore = None;
  new->extents = None;
  new->shadow = False;
  new->shadow_opacity = 0.0;
  new->shadow_pict = None;
  new->shadow_alpha_pict = None;
  new->shadow_dx = 0;
  new->shadow_dy = 0;
  new->shadow_width = 0;
  new->shadow_height = 0;
  new->opacity = 0;
  new->opacity_tgt = 0;
  new->opacity_prop = OPAQUE;
  new->opacity_prop_client = OPAQUE;
  new->fade = False;
  new->fade_callback = NULL;
  new->alpha_pict = None;
  new->frame_opacity = 1.0;
  new->frame_alpha_pict = None;
  new->dim = False;
  new->focused = False;
  new->destroyed = False;
  new->need_configure = False;
  new->window_type = WINTYPE_UNKNOWN;
  new->mode = WINDOW_TRANS;

  new->prev_trans = NULL;

  new->left_width = 0;
  new->right_width = 0;
  new->top_width = 0;
  new->bottom_width = 0;

  new->client_win = 0;

  new->flags = 0;

  calc_win_size(dpy, new);

  new->next = *p;
  *p = new;

  if (new->a.map_state == IsViewable) {
    map_win(dpy, id, new->damage_sequence - 1, True, override_redirect);
  }
}

static void
restack_win(Display *dpy, win *w, Window new_above) {
  Window old_above;

  if (w->next) {
    old_above = w->next->id;
  } else {
    old_above = None;
  }

  if (old_above != new_above) {
    win **prev;

    /* unhook */
    for (prev = &list; *prev; prev = &(*prev)->next) {
      if ((*prev) == w) break;
    }

    *prev = w->next;

    /* rehook */
    for (prev = &list; *prev; prev = &(*prev)->next) {
      if ((*prev)->id == new_above && !(*prev)->destroyed)
        break;
    }

    w->next = *prev;
    *prev = w;

#ifdef DEBUG_RESTACK
    {
      const char *desc;
      char *window_name;
      Bool to_free;
      win* c = list;

      printf("restack_win(%#010lx, %#010lx): "
             "Window stack modified. Current stack:\n", w->id, new_above);

      for (; c; c = c->next) {
        window_name = "(Failed to get title)";

        if (root == c->id) {
          window_name = "(Root window)";
        } else {
          to_free = wid_get_name(dpy, c->id, &window_name);
        }

        desc = "";
        if (c->destroyed) desc = "(D) ";
        printf("%#010lx \"%s\" %s-> ", c->id, window_name, desc);

        if (to_free) {
          XFree(window_name);
          window_name = NULL;
        }
      }
      fputs("\n", stdout);
    }
#endif
  }
}

static void
configure_win(Display *dpy, XConfigureEvent *ce) {
  if (ce->window == root) {
    if (tgt_buffer) {
      XRenderFreePicture(dpy, tgt_buffer);
      tgt_buffer = None;
    }
    root_width = ce->width;
    root_height = ce->height;

    rebuild_screen_reg(dpy);

    return;
  }

  win *w = find_win(dpy, ce->window);
  XserverRegion damage = None;

  if (!w)
    return;

  if (w->a.map_state == IsUnmapped) {
    /* save the configure event for when the window maps */
    w->need_configure = True;
    w->queue_configure = *ce;
    restack_win(dpy, w, ce->above);
  } else {
    if (!(w->need_configure)) {
      restack_win(dpy, w, ce->above);
    }

    // Windows restack (including window restacks happened when this
    // window is not mapped) could mess up all reg_ignore
    reg_ignore_expire = True;

    w->need_configure = False;

    damage = XFixesCreateRegion(dpy, 0, 0);
    if (w->extents != None) {
      XFixesCopyRegion(dpy, damage, w->extents);
    }

    // If window geometry did not change, don't free extents here
    if (w->a.x != ce->x || w->a.y != ce->y
        || w->a.width != ce->width || w->a.height != ce->height) {
      free_region(dpy, &w->extents);
      free_region(dpy, &w->border_size);
    }

    w->a.x = ce->x;
    w->a.y = ce->y;

    if (w->a.width != ce->width || w->a.height != ce->height) {
      free_pixmap(dpy, &w->pixmap);
      free_picture(dpy, &w->picture);
    }

    if (w->a.width != ce->width || w->a.height != ce->height
        || w->a.border_width != ce->border_width) {
      w->a.width = ce->width;
      w->a.height = ce->height;
      w->a.border_width = ce->border_width;
      calc_win_size(dpy, w);

      // Rounded corner detection is affected by window size
      if (shape_exists && opts.shadow_ignore_shaped
          && opts.detect_rounded_corners && w->bounding_shaped)
        win_update_shape(dpy, w);
    }

    if (w->a.map_state != IsUnmapped && damage) {
      XserverRegion extents = win_extents(dpy, w);
      XFixesUnionRegion(dpy, damage, damage, extents);
      XFixesDestroyRegion(dpy, extents);
      add_damage(dpy, damage);
    }
  }

  w->a.override_redirect = ce->override_redirect;
}

static void
circulate_win(Display *dpy, XCirculateEvent *ce) {
  win *w = find_win(dpy, ce->window);
  Window new_above;

  if (!w) return;

  if (ce->place == PlaceOnTop) {
    new_above = list->id;
  } else {
    new_above = None;
  }

  restack_win(dpy, w, new_above);
}

static void
finish_destroy_win(Display *dpy, Window id) {
  win **prev, *w;

  for (prev = &list; (w = *prev); prev = &w->next) {
    if (w->id == id && w->destroyed) {
      finish_unmap_win(dpy, w);
      *prev = w->next;

      free_picture(dpy, &w->shadow_pict);
      free_damage(dpy, &w->damage);
      free_region(dpy, &w->reg_ignore);
      free(w->name);
      free(w->class_instance);
      free(w->class_general);

      free(w);
      break;
    }
  }
}

static void
destroy_callback(Display *dpy, win *w) {
  finish_destroy_win(dpy, w->id);
}

static void
destroy_win(Display *dpy, Window id, Bool fade) {
  win *w = find_win(dpy, id);

  if (w) {
    w->destroyed = True;

    // Fading out the window
    w->opacity_tgt = 0;
    set_fade_callback(dpy, w, destroy_callback, False);
  }
}

static inline void
root_damaged(void) {
  if (root_tile) {
    XClearArea(dpy, root, 0, 0, 0, 0, True);
    // if (root_picture != root_tile) {
      XRenderFreePicture(dpy, root_tile);
      root_tile = None;
    /* }
    if (root_damage) {
      XserverRegion parts = XFixesCreateRegion(dpy, 0, 0);
      XDamageSubtract(dpy, root_damage, None, parts);
      add_damage(dpy, parts);
    } */
  }
  // Mark screen damaged if we are painting on overlay
  if (opts.paint_on_overlay)
    add_damage(dpy, get_screen_region(dpy));
}

static void
damage_win(Display *dpy, XDamageNotifyEvent *de) {
  /*
  if (root == de->drawable) {
    root_damaged();
    return;
  } */

  win *w = find_win(dpy, de->drawable);

  if (!w) return;

  repair_win(dpy, w);
}

static int
error(Display *dpy, XErrorEvent *ev) {
  int o;
  const char *name = "Unknown";

  if (should_ignore(dpy, ev->serial)) {
    return 0;
  }

  if (ev->request_code == composite_opcode
      && ev->minor_code == X_CompositeRedirectSubwindows) {
    fprintf(stderr, "Another composite manager is already running\n");
    exit(1);
  }

#define CASESTRRET2(s)   case s: name = #s; break

  o = ev->error_code - xfixes_error;
  switch (o) {
    CASESTRRET2(BadRegion);
  }

  o = ev->error_code - damage_error;
  switch (o) {
    CASESTRRET2(BadDamage);
  }

  o = ev->error_code - render_error;
  switch (o) {
    CASESTRRET2(BadPictFormat);
    CASESTRRET2(BadPicture);
    CASESTRRET2(BadPictOp);
    CASESTRRET2(BadGlyphSet);
    CASESTRRET2(BadGlyph);
  }

  switch (ev->error_code) {
    CASESTRRET2(BadAccess);
    CASESTRRET2(BadAlloc);
    CASESTRRET2(BadAtom);
    CASESTRRET2(BadColor);
    CASESTRRET2(BadCursor);
    CASESTRRET2(BadDrawable);
    CASESTRRET2(BadFont);
    CASESTRRET2(BadGC);
    CASESTRRET2(BadIDChoice);
    CASESTRRET2(BadImplementation);
    CASESTRRET2(BadLength);
    CASESTRRET2(BadMatch);
    CASESTRRET2(BadName);
    CASESTRRET2(BadPixmap);
    CASESTRRET2(BadRequest);
    CASESTRRET2(BadValue);
    CASESTRRET2(BadWindow);
  }

#undef CASESTRRET2

  print_timestamp();
  printf("error %d (%s) request %d minor %d serial %lu\n",
    ev->error_code, name, ev->request_code,
    ev->minor_code, ev->serial);

  return 0;
}

static void
expose_root(Display *dpy, XRectangle *rects, int nrects) {
  XserverRegion region = XFixesCreateRegion(dpy, rects, nrects);
  add_damage(dpy, region);
}

static Bool
wid_get_text_prop(Display *dpy, Window wid, Atom prop,
    char ***pstrlst, int *pnstr) {
  XTextProperty text_prop;

  if (!(XGetTextProperty(dpy, wid, &text_prop, prop) && text_prop.value))
    return False;

  if (Success !=
      XmbTextPropertyToTextList(dpy, &text_prop, pstrlst, pnstr)
      || !*pnstr) {
    *pnstr = 0;
    if (*pstrlst)
      XFreeStringList(*pstrlst);
    return False;
  }

  return True;
}

static Bool
wid_get_name(Display *dpy, Window wid, char **name) {
  XTextProperty text_prop;
  char **strlst = NULL;
  int nstr = 0;

  // set_ignore(dpy, NextRequest(dpy));
  if (!(XGetTextProperty(dpy, wid, &text_prop, name_ewmh_atom)
      && text_prop.value)) {
    // set_ignore(dpy, NextRequest(dpy));
#ifdef DEBUG_WINDATA
    printf("wid_get_name(%#010lx): _NET_WM_NAME unset, falling back to WM_NAME.\n", wid);
#endif

    if (!(XGetWMName(dpy, wid, &text_prop) && text_prop.value)) {
      return False;
    }
  }
  if (Success !=
      XmbTextPropertyToTextList(dpy, &text_prop, &strlst, &nstr)
      || !nstr || !strlst) {
    if (strlst)
      XFreeStringList(strlst);
    return False;
  }
  *name = mstrcpy(strlst[0]);

  XFreeStringList(strlst);

  return True;
}

static int
win_get_name(Display *dpy, win *w) {
  Bool ret;
  char *name_old = w->name;

  // Can't do anything if there's no client window
  if (!w->client_win)
    return False;

  // Get the name
  ret = wid_get_name(dpy, w->client_win, &w->name);

  // Return -1 if wid_get_name() failed, 0 if name didn't change, 1 if
  // it changes
  if (!ret)
    ret = -1;
  else if (name_old && !strcmp(w->name, name_old))
    ret = 0;
  else
    ret = 1;

  // Keep the old name if there's no new one
  if (w->name != name_old)
    free(name_old);

#ifdef DEBUG_WINDATA
  printf("win_get_name(%#010lx): client = %#010lx, name = \"%s\", "
      "ret = %d\n", w->id, w->client_win, w->name, ret);
#endif

  return ret;
}

static Bool
win_get_class(Display *dpy, win *w) {
  char **strlst = NULL;
  int nstr = 0;

  // Can't do anything if there's no client window
  if (!w->client_win)
    return False;

  // Free and reset old strings
  free(w->class_instance);
  free(w->class_general);
  w->class_instance = NULL;
  w->class_general = NULL;

  // Retrieve the property string list
  if (!wid_get_text_prop(dpy, w->client_win, class_atom, &strlst, &nstr))
    return False;

  // Copy the strings if successful
  w->class_instance = mstrcpy(strlst[0]);

  if (nstr > 1)
    w->class_general = mstrcpy(strlst[1]);

  XFreeStringList(strlst);

#ifdef DEBUG_WINDATA
  printf("win_get_class(%#010lx): client = %#010lx, "
      "instance = \"%s\", general = \"%s\"\n",
      w->id, w->client_win, w->class_instance, w->class_general);
#endif

  return True;
}

#ifdef DEBUG_EVENTS
static int
ev_serial(XEvent *ev) {
  if ((ev->type & 0x7f) != KeymapNotify) {
    return ev->xany.serial;
  }
  return NextRequest(ev->xany.display);
}

static const char *
ev_name(XEvent *ev) {
  static char buf[128];
  switch (ev->type & 0x7f) {
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
    default:
      if (ev->type == damage_event + XDamageNotify) {
        return "Damage";
      }

      if (shape_exists && ev->type == shape_event) {
        return "ShapeNotify";
      }

      sprintf(buf, "Event %d", ev->type);

      return buf;
  }
}

static Window
ev_window(XEvent *ev) {
  switch (ev->type) {
    case FocusIn:
    case FocusOut:
      return ev->xfocus.window;
    case CreateNotify:
      return ev->xcreatewindow.window;
    case ConfigureNotify:
      return ev->xconfigure.window;
    case DestroyNotify:
      return ev->xdestroywindow.window;
    case MapNotify:
      return ev->xmap.window;
    case UnmapNotify:
      return ev->xunmap.window;
    case ReparentNotify:
      return ev->xreparent.window;
    case CirculateNotify:
      return ev->xcirculate.window;
    case Expose:
      return ev->xexpose.window;
    case PropertyNotify:
      return ev->xproperty.window;
    case ClientMessage:
      return ev->xclient.window;
    default:
      if (ev->type == damage_event + XDamageNotify) {
        return ((XDamageNotifyEvent *)ev)->drawable;
      }

      if (shape_exists && ev->type == shape_event) {
        return ((XShapeEvent *) ev)->window;
      }

      return 0;
  }
}

static inline const char *
ev_focus_mode_name(XFocusChangeEvent* ev) {
  switch (ev->mode) {
    CASESTRRET(NotifyNormal);
    CASESTRRET(NotifyWhileGrabbed);
    CASESTRRET(NotifyGrab);
    CASESTRRET(NotifyUngrab);
  }

  return "Unknown";
}

static inline const char *
ev_focus_detail_name(XFocusChangeEvent* ev) {
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

static inline void
ev_focus_report(XFocusChangeEvent* ev) {
  printf("  { mode: %s, detail: %s }\n", ev_focus_mode_name(ev),
      ev_focus_detail_name(ev));
}

#endif

/**
 * Events
 */

inline static bool
ev_focus_accept(XFocusChangeEvent *ev) {
  return ev->mode == NotifyGrab
    || (ev->mode == NotifyNormal
        && (ev->detail == NotifyNonlinear
          || ev->detail == NotifyNonlinearVirtual));
}

inline static void
ev_focus_in(XFocusChangeEvent *ev) {
#ifdef DEBUG_EVENTS
  ev_focus_report(ev);
#endif

  win *w = find_win(dpy, ev->window);

  // To deal with events sent from windows just destroyed
  if (!w) return;

  set_focused(dpy, w, True);
}

inline static void
ev_focus_out(XFocusChangeEvent *ev) {
#ifdef DEBUG_EVENTS
  ev_focus_report(ev);
#endif

  if (!ev_focus_accept(ev))
    return;

  win *w = find_win(dpy, ev->window);

  // To deal with events sent from windows just destroyed
  if (!w) return;

  set_focused(dpy, w, False);
}

inline static void
ev_create_notify(XCreateWindowEvent *ev) {
  assert(ev->parent == root);
  add_win(dpy, ev->window, 0, ev->override_redirect);
}

inline static void
ev_configure_notify(XConfigureEvent *ev) {
#ifdef DEBUG_EVENTS
  printf("  { send_event: %d, "
         " above: %#010lx, "
         " override_redirect: %d }\n",
         ev->send_event, ev->above, ev->override_redirect);
#endif
  configure_win(dpy, ev);
}

inline static void
ev_destroy_notify(XDestroyWindowEvent *ev) {
  destroy_win(dpy, ev->window, True);
}

inline static void
ev_map_notify(XMapEvent *ev) {
  map_win(dpy, ev->window, ev->serial, True, ev->override_redirect);
}

inline static void
ev_unmap_notify(XUnmapEvent *ev) {
  unmap_win(dpy, ev->window, True);
}

inline static void
ev_reparent_notify(XReparentEvent *ev) {
  if (ev->parent == root) {
    add_win(dpy, ev->window, 0, ev->override_redirect);
  } else {
    destroy_win(dpy, ev->window, True);
    // Reset event mask in case something wrong happens
    XSelectInput(dpy, ev->window,
        determine_evmask(dpy, ev->window, WIN_EVMODE_UNKNOWN));
    /*
    // Check if the window is a client window of another
    win *w_top = find_toplevel2(dpy, ev->window);
    if (w_top && !(w_top->client_win)) {
      mark_client_win(dpy, w_top, ev->window);
    } */
  }
}

inline static void
ev_circulate_notify(XCirculateEvent *ev) {
  circulate_win(dpy, ev);
}

inline static void
ev_expose(XExposeEvent *ev) {
  if (ev->window == root || (overlay && ev->window == overlay)) {
    int more = ev->count + 1;
    if (n_expose == size_expose) {
      if (expose_rects) {
        expose_rects = realloc(expose_rects,
          (size_expose + more) * sizeof(XRectangle));
        size_expose += more;
      } else {
        expose_rects = malloc(more * sizeof(XRectangle));
        size_expose = more;
      }
    }

    expose_rects[n_expose].x = ev->x;
    expose_rects[n_expose].y = ev->y;
    expose_rects[n_expose].width = ev->width;
    expose_rects[n_expose].height = ev->height;
    n_expose++;

    if (ev->count == 0) {
      expose_root(dpy, expose_rects, n_expose);
      n_expose = 0;
    }
  }
}

inline static void
ev_property_notify(XPropertyEvent *ev) {
  // Destroy the root "image" if the wallpaper probably changed
  if (root == ev->window) {
    for (int p = 0; background_props[p]; p++) {
      if (ev->atom == XInternAtom(dpy, background_props[p], False)) {
        root_damaged();
        break;
      }
    }
    // Unconcerned about any other proprties on root window
    return;
  }

  // If _NET_WM_OPACITY changes
  if (ev->atom == opacity_atom) {
    win *w = NULL;
    if ((w = find_win(dpy, ev->window)))
      w->opacity_prop = wid_get_opacity_prop(dpy, w->id, OPAQUE);
    else if (opts.detect_client_opacity
        && (w = find_toplevel(dpy, ev->window)))
      w->opacity_prop_client = wid_get_opacity_prop(dpy, w->client_win,
            OPAQUE);
    if (w) {
      calc_opacity(dpy, w, False);
    }
  }

  // If frame extents property changes
  if (opts.frame_opacity && ev->atom == extents_atom) {
    win *w = find_toplevel(dpy, ev->window);
    if (w) {
      get_frame_extents(dpy, w, ev->window);
      // If frame extents change, the window needs repaint
      add_damage_win(dpy, w);
    }
  }

  // If name changes
  if (opts.track_wdata
      && (name_atom == ev->atom || name_ewmh_atom == ev->atom)) {
    win *w = find_toplevel(dpy, ev->window);
    if (w && 1 == win_get_name(dpy, w))
      determine_shadow(dpy, w);
  }

  // If class changes
  if (opts.track_wdata && class_atom == ev->atom) {
    win *w = find_toplevel(dpy, ev->window);
    if (w) {
      win_get_class(dpy, w);
      determine_shadow(dpy, w);
    }
  }
}

inline static void
ev_damage_notify(XDamageNotifyEvent *ev) {
  damage_win(dpy, ev);
}

inline static void
ev_shape_notify(XShapeEvent *ev) {
  win *w = find_win(dpy, ev->window);
  if (!w || IsUnmapped == w->a.map_state) return;

  /*
   * Empty border_size may indicated an
   * unmapped/destroyed window, in which case
   * seemingly BadRegion errors would be triggered
   * if we attempt to rebuild border_size
   */
  if (w->border_size) {
    // Mark the old border_size as damaged
    add_damage(dpy, w->border_size);

    w->border_size = border_size(dpy, w);

    // Mark the new border_size as damaged
    add_damage(dpy, copy_region(dpy, w->border_size));
  }

  // Redo bounding shape detection and rounded corner detection
  win_update_shape(dpy, w);
}

/**
 * Handle ScreenChangeNotify events from X RandR extension.
 */
static void
ev_screen_change_notify(XRRScreenChangeNotifyEvent *ev) {
  if (!opts.refresh_rate) {
    update_refresh_rate(dpy);
    if (!refresh_rate) {
      fprintf(stderr, "ev_screen_change_notify(): Refresh rate detection "
          "failed, software VSync disabled.");
      opts.vsync = VSYNC_NONE;
    }
  }
}

static void
ev_handle(XEvent *ev) {
  if ((ev->type & 0x7f) != KeymapNotify) {
    discard_ignore(dpy, ev->xany.serial);
  }

#ifdef DEBUG_EVENTS
  if (ev->type != damage_event + XDamageNotify) {
    Window wid;
    char *window_name;
    Bool to_free = False;

    wid = ev_window(ev);
    window_name = "(Failed to get title)";

    if (wid) {
      if (root == wid)
        window_name = "(Root window)";
      else if (overlay == wid)
        window_name = "(Overlay)";
      else {
        win *w = find_win(dpy, wid);
        if (!w)
          w = find_toplevel(dpy, wid);

        if (w && w->name)
          window_name = w->name;
        else if (!(w && w->client_win
              && (to_free = (Bool) wid_get_name(dpy, w->client_win,
                  &window_name))))
            to_free = (Bool) wid_get_name(dpy, wid, &window_name);
      }
    }

    print_timestamp();
    printf("event %10.10s serial %#010x window %#010lx \"%s\"\n",
      ev_name(ev), ev_serial(ev), wid, window_name);

    if (to_free) {
      XFree(window_name);
      window_name = NULL;
    }
  }

#endif

  switch (ev->type) {
    case FocusIn:
      ev_focus_in((XFocusChangeEvent *)ev);
      break;
    case FocusOut:
      ev_focus_out((XFocusChangeEvent *)ev);
      break;
    case CreateNotify:
      ev_create_notify((XCreateWindowEvent *)ev);
      break;
    case ConfigureNotify:
      ev_configure_notify((XConfigureEvent *)ev);
      break;
    case DestroyNotify:
      ev_destroy_notify((XDestroyWindowEvent *)ev);
      break;
    case MapNotify:
      ev_map_notify((XMapEvent *)ev);
      break;
    case UnmapNotify:
      ev_unmap_notify((XUnmapEvent *)ev);
      break;
    case ReparentNotify:
      ev_reparent_notify((XReparentEvent *)ev);
      break;
    case CirculateNotify:
      ev_circulate_notify((XCirculateEvent *)ev);
      break;
    case Expose:
      ev_expose((XExposeEvent *)ev);
      break;
    case PropertyNotify:
      ev_property_notify((XPropertyEvent *)ev);
      break;
    default:
      if (shape_exists && ev->type == shape_event) {
        ev_shape_notify((XShapeEvent *) ev);
        break;
      }
      if (randr_exists && ev->type == (randr_event + RRScreenChangeNotify)) {
        ev_screen_change_notify((XRRScreenChangeNotifyEvent *) ev);
        break;
      }
      if (ev->type == damage_event + XDamageNotify) {
        ev_damage_notify((XDamageNotifyEvent *)ev);
      }
      break;
  }
}

/**
 * Main
 */

/**
 * Print usage text and exit.
 */
static void
usage(void) {
  fputs(
    "compton (development version)\n"
    "usage: compton [options]\n"
    "Options:\n"
    "\n"
    "-d display\n"
    "  Which display should be managed.\n"
    "-r radius\n"
    "  The blur radius for shadows. (default 12)\n"
    "-o opacity\n"
    "  The translucency for shadows. (default .75)\n"
    "-l left-offset\n"
    "  The left offset for shadows. (default -15)\n"
    "-t top-offset\n"
    "  The top offset for shadows. (default -15)\n"
    "-I fade-in-step\n"
    "  Opacity change between steps while fading in. (default 0.028)\n"
    "-O fade-out-step\n"
    "  Opacity change between steps while fading out. (default 0.03)\n"
    "-D fade-delta-time\n"
    "  The time between steps in a fade in milliseconds. (default 10)\n"
    "-m opacity\n"
    "  The opacity for menus. (default 1.0)\n"
    "-c\n"
    "  Enabled client-side shadows on windows.\n"
    "-C\n"
    "  Avoid drawing shadows on dock/panel windows.\n"
    "-z\n"
    "  Zero the part of the shadow's mask behind the window (experimental).\n"
    "-f\n"
    "  Fade windows in/out when opening/closing and when opacity\n"
    "  changes, unless --no-fading-openclose is used.\n"
    "-F\n"
    "  Equals -f. Deprecated.\n"
    "-i opacity\n"
    "  Opacity of inactive windows. (0.1 - 1.0)\n"
    "-e opacity\n"
    "  Opacity of window titlebars and borders. (0.1 - 1.0)\n"
    "-G\n"
    "  Don't draw shadows on DND windows\n"
    "-b\n"
    "  Daemonize process.\n"
    "-S\n"
    "  Enable synchronous operation (for debugging).\n"
    "--config path\n"
    "  Look for configuration file at the path.\n"
    "--shadow-red value\n"
    "  Red color value of shadow (0.0 - 1.0, defaults to 0).\n"
    "--shadow-green value\n"
    "  Green color value of shadow (0.0 - 1.0, defaults to 0).\n"
    "--shadow-blue value\n"
    "  Blue color value of shadow (0.0 - 1.0, defaults to 0).\n"
    "--inactive-opacity-override\n"
    "  Inactive opacity set by -i overrides value of _NET_WM_OPACITY.\n"
    "--inactive-dim value\n"
    "  Dim inactive windows. (0.0 - 1.0, defaults to 0)\n"
    "--mark-wmwin-focused\n"
    "  Try to detect WM windows and mark them as active.\n"
    "--shadow-exclude condition\n"
    "  Exclude conditions for shadows.\n"
    "--mark-ovredir-focused\n"
    "  Mark over-redirect windows as active.\n"
    "--no-fading-openclose\n"
    "  Do not fade on window open/close.\n"
    "--shadow-ignore-shaped\n"
    "  Do not paint shadows on shaped windows.\n"
    "--detect-rounded-corners\n"
    "  Try to detect windows with rounded corners and don't consider\n"
    "  them shaped windows.\n"
    "--detect-client-opacity\n"
    "  Detect _NET_WM_OPACITY on client windows, useful for window\n"
    "  managers not passing _NET_WM_OPACITY of client windows to frame\n"
    "  windows.\n"
    "\n"
    "--refresh-rate val\n"
    "  Specify refresh rate of the screen. If not specified or 0, compton\n"
    "  will try detecting this with X RandR extension.\n"
    "--vsync vsync-method\n"
    "  Set VSync method. There are 2 VSync methods currently available:\n"
    "    none = No VSync\n"
    "    drm = VSync with DRM_IOCTL_WAIT_VBLANK. May only work on some\n"
    "      drivers. Experimental.\n"
    "    opengl = Try to VSync with SGI_swap_control OpenGL extension. Only\n"
    "      work on some drivers. Experimental.\n"
    "  (Note some VSync methods may not be enabled at compile time.)\n"
    "--alpha-step val\n"
    "  Step for pregenerating alpha pictures. 0.01 - 1.0. Defaults to\n"
    "  0.03.\n"
    "--dbe\n"
    "  Enable DBE painting mode, intended to use with VSync to\n"
    "  (hopefully) eliminate tearing.\n"
    "--paint-on-overlay\n"
    "  Painting on X Composite overlay window.\n"
    "--sw-opti\n"
    "  Limit compton to repaint at most once every 1 / refresh_rate\n"
    "  second to boost performance. Experimental.\n"
    "--vsync-aggressive\n"
    "  Attempt to send painting request before VBlank and do XFlush()\n"
    "  during VBlank. This switch may be lifted out at any moment.\n"
    "\n"
    "Format of a condition:\n"
    "\n"
    "  condition = <target>:<type>[<flags>]:<pattern>\n"
    "\n"
    "  <target> is one of \"n\" (window name), \"i\" (window class\n"
    "  instance), and \"g\" (window general class)\n"
    "\n"
    "  <type> is one of \"e\" (exact match), \"a\" (match anywhere),\n"
    "  \"s\" (match from start), \"w\" (wildcard), and \"p\" (PCRE\n"
    "  regular expressions, if compiled with the support).\n"
    "\n"
    "  <flags> could be a series of flags. Currently the only defined\n"
    "  flag is \"i\" (ignore case).\n"
    "\n"
    "  <pattern> is the actual pattern string.\n"
    , stderr);

  exit(1);
}

/**
 * Register a window as symbol, and initialize GLX context if wanted.
 */
static void
register_cm(Bool want_glxct) {
  Atom a;
  char *buf;
  int len, s;

#ifdef CONFIG_VSYNC_OPENGL
  // Create a window with the wanted GLX visual
  if (want_glxct) {
    XVisualInfo *pvi = NULL;
    Bool ret = False;
    // Get visual for the window
    int attribs[] = { GLX_RGBA, GLX_RED_SIZE, 1, GLX_GREEN_SIZE, 1, GLX_BLUE_SIZE, 1, None };
    pvi = glXChooseVisual(dpy, scr, attribs);

    if (!pvi) {
      fprintf(stderr, "register_cm(): Failed to choose visual required "
          "by fake OpenGL VSync window. OpenGL VSync turned off.\n");
    }
    else {
      // Create the window
      XSetWindowAttributes swa = {
        .colormap = XCreateColormap(dpy, root, pvi->visual, AllocNone),
        .border_pixel = 0,
      };

      pvi->screen = scr;
      reg_win = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, pvi->depth,
          InputOutput, pvi->visual, CWBorderPixel | CWColormap, &swa);

      if (!reg_win)
        fprintf(stderr, "register_cm(): Failed to create window required "
            "by fake OpenGL VSync. OpenGL VSync turned off.\n");
      else {
        // Get GLX context
        glx_context = glXCreateContext(dpy, pvi, None, GL_TRUE);
        if (!glx_context) {
          fprintf(stderr, "register_cm(): Failed to get GLX context. "
              "OpenGL VSync turned off.\n");
          opts.vsync = VSYNC_NONE;
        }
        else {
          // Attach GLX context
          if (!(ret = glXMakeCurrent(dpy, reg_win, glx_context)))
            fprintf(stderr, "register_cm(): Failed to attach GLX context."
              " OpenGL VSync turned off.\n");
        }
      }
    }
    if (pvi)
      XFree(pvi);

    if (!ret)
      opts.vsync = VSYNC_NONE;
  }
#endif
  
  if (!reg_win)
    reg_win = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0,
        None, None);

  Xutf8SetWMProperties(
    dpy, reg_win, "xcompmgr", "xcompmgr",
    NULL, 0, NULL, NULL, NULL);

  len = strlen(REGISTER_PROP) + 2;
  s = scr;

  while (s >= 10) {
    ++len;
    s /= 10;
  }

  buf = malloc(len);
  snprintf(buf, len, REGISTER_PROP"%d", scr);

  a = XInternAtom(dpy, buf, False);
  free(buf);

  XSetSelectionOwner(dpy, a, reg_win, 0);
}

static void
fork_after(void) {
  if (getppid() == 1) return;

  int pid = fork();

  if (pid == -1) {
    fprintf(stderr, "Fork failed\n");
    return;
  }

  if (pid > 0) _exit(0);

  setsid();

  freopen("/dev/null", "r", stdin);
  freopen("/dev/null", "w", stdout);
  freopen("/dev/null", "w", stderr);
}

#ifdef CONFIG_LIBCONFIG
/**
 * Get a file stream of the configuration file to read.
 *
 * Follows the XDG specification to search for the configuration file.
 */
static FILE *
open_config_file(char *cpath, char **ppath) {
  const static char *config_filename = "/compton.conf";
  const static char *config_filename_legacy = "/.compton.conf";
  const static char *config_home_suffix = "/.config";
  const static char *config_system_dir = "/etc/xdg";

  char *dir = NULL, *home = NULL;
  char *path = cpath;
  FILE *f = NULL;

  if (path) {
    f = fopen(path, "r");
    if (f && ppath)
      *ppath = path;
    else
      free(path);
    return f;
  }

  // Check user configuration file in $XDG_CONFIG_HOME firstly
  if (!((dir = getenv("XDG_CONFIG_HOME")) && strlen(dir))) {
    if (!((home = getenv("HOME")) && strlen(home)))
      return NULL;

    path = mstrjoin3(home, config_home_suffix, config_filename);
  }
  else
    path = mstrjoin(dir, config_filename);

  f = fopen(path, "r");

  if (f && ppath)
    *ppath = path;
  else
    free(path);
  if (f)
    return f;

  // Then check user configuration file in $HOME
  if ((home = getenv("HOME")) && strlen(home)) {
    path = mstrjoin(home, config_filename_legacy);
    f = fopen(path, "r");
    if (f && ppath)
      *ppath = path;
    else
      free(path);
    if (f)
      return f;
  }

  // Check system configuration file in $XDG_CONFIG_DIRS at last
  if ((dir = getenv("XDG_CONFIG_DIRS")) && strlen(dir)) {
    char *part = strtok(dir, ":");
    while (part) {
      path = mstrjoin(part, config_filename);
      f = fopen(path, "r");
      if (f && ppath)
        *ppath = path;
      else
        free(path);
      if (f)
        return f;
      part = strtok(NULL, ":");
    }
  }
  else {
    path = mstrjoin(config_system_dir, config_filename);
    f = fopen(path, "r");
    if (f && ppath)
      *ppath = path;
    else
      free(path);
    if (f)
      return f;
  }

  return NULL;
}

/**
 * Parse a VSync option argument.
 */
static inline void
parse_vsync(const char *optarg) {
  const static char * const vsync_str[] = {
    "none",   // VSYNC_NONE
    "drm",    // VSYNC_DRM
    "opengl", // VSYNC_OPENGL
  };

  vsync_t i;

  for (i = 0; i < (sizeof(vsync_str) / sizeof(vsync_str[0])); ++i)
    if (!strcasecmp(optarg, vsync_str[i])) {
      opts.vsync = i;
      break;
    }
  if ((sizeof(vsync_str) / sizeof(vsync_str[0])) == i) {
    fputs("Invalid --vsync argument. Ignored.\n", stderr);
  }
}

/**
 * Parse a configuration file from default location.
 */
static void
parse_config(char *cpath, struct options_tmp *pcfgtmp) {
  char *path = NULL;
  FILE *f;
  config_t cfg;
  int ival = 0;
  double dval = 0.0;
  const char *sval = NULL;

  f = open_config_file(cpath, &path);
  if (!f) {
    if (cpath)
      printf("Failed to read the specified configuration file.\n");
    return;
  }

  config_init(&cfg);
#ifndef CONFIG_LIBCONFIG_LEGACY
  char *parent = dirname(path);
  if (parent)
    config_set_include_dir(&cfg, parent);
#endif

  if (CONFIG_FALSE == config_read(&cfg, f)) {
    printf("Error when reading configuration file \"%s\", line %d: %s\n",
        path, config_error_line(&cfg), config_error_text(&cfg));
    config_destroy(&cfg);
    free(path);
    return;
  }
  config_set_auto_convert(&cfg, 1);

  free(path);

  // Get options from the configuration file. We don't do range checking
  // right now. It will be done later

  // -D (fade_delta)
  if (lcfg_lookup_int(&cfg, "fade-delta", &ival))
    opts.fade_delta = ival;
  // -I (fade_in_step)
  if (config_lookup_float(&cfg, "fade-in-step", &dval))
    opts.fade_in_step = normalize_d(dval) * OPAQUE;
  // -O (fade_out_step)
  if (config_lookup_float(&cfg, "fade-out-step", &dval))
    opts.fade_out_step = normalize_d(dval) * OPAQUE;
  // -r (shadow_radius)
  lcfg_lookup_int(&cfg, "shadow-radius", &opts.shadow_radius);
  // -o (shadow_opacity)
  config_lookup_float(&cfg, "shadow-opacity", &opts.shadow_opacity);
  // -l (shadow_offset_x)
  lcfg_lookup_int(&cfg, "shadow-offset-x", &opts.shadow_offset_x);
  // -t (shadow_offset_y)
  lcfg_lookup_int(&cfg, "shadow-offset-y", &opts.shadow_offset_y);
  // -i (inactive_opacity)
  if (config_lookup_float(&cfg, "inactive-opacity", &dval))
    opts.inactive_opacity = normalize_d(dval) * OPAQUE;
  // -e (frame_opacity)
  config_lookup_float(&cfg, "frame-opacity", &opts.frame_opacity);
  // -z (clear_shadow)
  lcfg_lookup_bool(&cfg, "clear-shadow", &opts.clear_shadow);
  // -c (shadow_enable)
  if (config_lookup_bool(&cfg, "shadow", &ival) && ival)
    wintype_arr_enable(opts.wintype_shadow);
  // -C (no_dock_shadow)
  lcfg_lookup_bool(&cfg, "no-dock-shadow", &pcfgtmp->no_dock_shadow);
  // -G (no_dnd_shadow)
  lcfg_lookup_bool(&cfg, "no-dnd-shadow", &pcfgtmp->no_dnd_shadow);
  // -m (menu_opacity)
  config_lookup_float(&cfg, "menu-opacity", &pcfgtmp->menu_opacity);
  // -f (fading_enable)
  if (config_lookup_bool(&cfg, "fading", &ival) && ival)
    wintype_arr_enable(opts.wintype_fade);
  // --no-fading-open-close
  lcfg_lookup_bool(&cfg, "no-fading-openclose", &opts.no_fading_openclose);
  // --shadow-red
  config_lookup_float(&cfg, "shadow-red", &opts.shadow_red);
  // --shadow-green
  config_lookup_float(&cfg, "shadow-green", &opts.shadow_green);
  // --shadow-blue
  config_lookup_float(&cfg, "shadow-blue", &opts.shadow_blue);
  // --inactive-opacity-override
  lcfg_lookup_bool(&cfg, "inactive-opacity-override",
      &opts.inactive_opacity_override);
  // --inactive-dim
  config_lookup_float(&cfg, "inactive-dim", &opts.inactive_dim);
  // --mark-wmwin-focused
  lcfg_lookup_bool(&cfg, "mark-wmwin-focused", &opts.mark_wmwin_focused);
  // --mark-ovredir-focused
  lcfg_lookup_bool(&cfg, "mark-ovredir-focused",
      &opts.mark_ovredir_focused);
  // --shadow-ignore-shaped
  lcfg_lookup_bool(&cfg, "shadow-ignore-shaped",
      &opts.shadow_ignore_shaped);
  // --detect-rounded-corners
  lcfg_lookup_bool(&cfg, "detect-rounded-corners",
      &opts.detect_rounded_corners);
  // --detect-client-opacity
  lcfg_lookup_bool(&cfg, "detect-client-opacity",
      &opts.detect_client_opacity);
  // --refresh-rate
  lcfg_lookup_int(&cfg, "refresh-rate", &opts.refresh_rate);
  // --vsync
  if (config_lookup_string(&cfg, "vsync", &sval))
    parse_vsync(sval);
  // --alpha-step
  config_lookup_float(&cfg, "alpha-step", &opts.alpha_step);
  // --dbe
  lcfg_lookup_bool(&cfg, "dbe", &opts.dbe);
  // --paint-on-overlay
  lcfg_lookup_bool(&cfg, "paint-on-overlay", &opts.paint_on_overlay);
  // --sw-opti
  lcfg_lookup_bool(&cfg, "sw-opti", &opts.sw_opti);
  // --shadow-exclude
  {
    config_setting_t *setting =
      config_lookup(&cfg, "shadow-exclude");
    if (setting) {
      // Parse an array of shadow-exclude
      if (config_setting_is_array(setting)) {
        int i = config_setting_length(setting);
        while (i--) {
          condlst_add(&opts.shadow_blacklist,
              config_setting_get_string_elem(setting, i));
        }
      }
      // Treat it as a single pattern if it's a string
      else if (CONFIG_TYPE_STRING == config_setting_type(setting)) {
        condlst_add(&opts.shadow_blacklist,
            config_setting_get_string(setting));
      }
    }
  }
  // Wintype settings
  {
    wintype i;

    for (i = 0; i < NUM_WINTYPES; ++i) {
      char *str = mstrjoin("wintypes.", WINTYPES[i]);
      config_setting_t *setting = config_lookup(&cfg, str);
      free(str);
      if (setting) {
        if (config_setting_lookup_bool(setting, "shadow", &ival))
          opts.wintype_shadow[i] = (Bool) ival;
        if (config_setting_lookup_bool(setting, "fade", &ival))
          opts.wintype_fade[i] = (Bool) ival;
        config_setting_lookup_float(setting, "opacity",
            &opts.wintype_opacity[i]);
      }
    }
  }

  config_destroy(&cfg);
}
#endif

/**
 * Process arguments and configuration files.
 */
static void
get_cfg(int argc, char *const *argv) {
  const static char *shortopts = "D:I:O:d:r:o:m:l:t:i:e:scnfFCaSzGb";
  const static struct option longopts[] = {
    { "config", required_argument, NULL, 256 },
    { "shadow-red", required_argument, NULL, 257 },
    { "shadow-green", required_argument, NULL, 258 },
    { "shadow-blue", required_argument, NULL, 259 },
    { "inactive-opacity-override", no_argument, NULL, 260 },
    { "inactive-dim", required_argument, NULL, 261 },
    { "mark-wmwin-focused", no_argument, NULL, 262 },
    { "shadow-exclude", required_argument, NULL, 263 },
    { "mark-ovredir-focused", no_argument, NULL, 264 },
    { "no-fading-openclose", no_argument, NULL, 265 },
    { "shadow-ignore-shaped", no_argument, NULL, 266 },
    { "detect-rounded-corners", no_argument, NULL, 267 },
    { "detect-client-opacity", no_argument, NULL, 268 },
    { "refresh-rate", required_argument, NULL, 269 },
    { "vsync", required_argument, NULL, 270 },
    { "alpha-step", required_argument, NULL, 271 },
    { "dbe", no_argument, NULL, 272 },
    { "paint-on-overlay", no_argument, NULL, 273 },
    { "sw-opti", no_argument, NULL, 274 },
    { "vsync-aggressive", no_argument, NULL, 275 },
    // Must terminate with a NULL entry
    { NULL, 0, NULL, 0 },
  };

  struct options_tmp cfgtmp = {
    .no_dock_shadow = False,
    .no_dnd_shadow = False,
    .menu_opacity = 1.0,
  };
  Bool shadow_enable = False, fading_enable = False;
  int o, longopt_idx, i;
  char *config_file = NULL;
  char *lc_numeric_old = mstrcpy(setlocale(LC_NUMERIC, NULL));

  for (i = 0; i < NUM_WINTYPES; ++i) {
    opts.wintype_fade[i] = False;
    opts.wintype_shadow[i] = False;
    opts.wintype_opacity[i] = 1.0;
  }

  // Pre-parse the commandline arguments to check for --config and invalid
  // switches
  while (-1 !=
      (o = getopt_long(argc, argv, shortopts, longopts, &longopt_idx))) {
    if (256 == o)
      config_file = mstrcpy(optarg);
    else if ('?' == o || ':' == o)
      usage();
  }

#ifdef CONFIG_LIBCONFIG
  parse_config(config_file, &cfgtmp);
#endif

  // Parse commandline arguments. Range checking will be done later.

  // Enforce LC_NUMERIC locale "C" here to make sure dots are recognized
  // instead of commas in atof().
  setlocale(LC_NUMERIC, "C");

  optind = 1;
  while (-1 !=
      (o = getopt_long(argc, argv, shortopts, longopts, &longopt_idx))) {
    switch (o) {
      // Short options
      case 'd':
        opts.display = optarg;
        break;
      case 'D':
        opts.fade_delta = atoi(optarg);
        break;
      case 'I':
        opts.fade_in_step = normalize_d(atof(optarg)) * OPAQUE;
        break;
      case 'O':
        opts.fade_out_step = normalize_d(atof(optarg)) * OPAQUE;
        break;
      case 'c':
        shadow_enable = True;
        break;
      case 'C':
        cfgtmp.no_dock_shadow = True;
        break;
      case 'G':
        cfgtmp.no_dnd_shadow = True;
        break;
      case 'm':
        cfgtmp.menu_opacity = atof(optarg);
        break;
      case 'f':
      case 'F':
        fading_enable = True;
        break;
      case 'S':
        opts.synchronize = True;
        break;
      case 'r':
        opts.shadow_radius = atoi(optarg);
        break;
      case 'o':
        opts.shadow_opacity = atof(optarg);
        break;
      case 'l':
        opts.shadow_offset_x = atoi(optarg);
        break;
      case 't':
        opts.shadow_offset_y = atoi(optarg);
        break;
      case 'i':
        opts.inactive_opacity = (normalize_d(atof(optarg)) * OPAQUE);
        break;
      case 'e':
        opts.frame_opacity = atof(optarg);
        break;
      case 'z':
        opts.clear_shadow = True;
        break;
      case 'n':
      case 'a':
      case 's':
        fprintf(stderr, "Warning: "
          "-n, -a, and -s have been removed.\n");
        break;
      case 'b':
        opts.fork_after_register = True;
        break;
      // Long options
      case 256:
        // --config
        break;
      case 257:
        // --shadow-red
        opts.shadow_red = atof(optarg);
        break;
      case 258:
        // --shadow-green
        opts.shadow_green = atof(optarg);
        break;
      case 259:
        // --shadow-blue
        opts.shadow_blue = atof(optarg);
        break;
      case 260:
        // --inactive-opacity-override
        opts.inactive_opacity_override = True;
        break;
      case 261:
        // --inactive-dim
        opts.inactive_dim = atof(optarg);
        break;
      case 262:
        // --mark-wmwin-focused
        opts.mark_wmwin_focused = True;
        break;
      case 263:
        // --shadow-exclude
        condlst_add(&opts.shadow_blacklist, optarg);
        break;
      case 264:
        // --mark-ovredir-focused
        opts.mark_ovredir_focused = True;
        break;
      case 265:
        // --no-fading-openclose
        opts.no_fading_openclose = True;
        break;
      case 266:
        // --shadow-ignore-shaped
        opts.shadow_ignore_shaped = True;
        break;
      case 267:
        // --detect-rounded-corners
        opts.detect_rounded_corners = True;
        break;
      case 268:
        // --detect-client-opacity
        opts.detect_client_opacity = True;
        break;
      case 269:
        // --refresh-rate
        opts.refresh_rate = atoi(optarg);
        break;
      case 270:
        // --vsync
        parse_vsync(optarg);
        break;
      case 271:
        // --alpha-step
        opts.alpha_step = atof(optarg);
        break;
      case 272:
        // --dbe
        opts.dbe = True;
        break;
      case 273:
        // --paint-on-overlay
        opts.paint_on_overlay = True;
        break;
      case 274:
        // --sw-opti
        opts.sw_opti = True;
        break;
      case 275:
        // --vsync-aggressive
        opts.vsync_aggressive = True;
        break;
      default:
        usage();
        break;
    }
  }

  // Restore LC_NUMERIC
  setlocale(LC_NUMERIC, lc_numeric_old);
  free(lc_numeric_old);

  // Range checking and option assignments
  opts.fade_delta = max_i(opts.fade_delta, 1);
  opts.shadow_radius = max_i(opts.shadow_radius, 1);
  opts.shadow_red = normalize_d(opts.shadow_red);
  opts.shadow_green = normalize_d(opts.shadow_green);
  opts.shadow_blue = normalize_d(opts.shadow_blue);
  opts.inactive_dim = normalize_d(opts.inactive_dim);
  opts.frame_opacity = normalize_d(opts.frame_opacity);
  opts.shadow_opacity = normalize_d(opts.shadow_opacity);
  cfgtmp.menu_opacity = normalize_d(cfgtmp.menu_opacity);
  opts.refresh_rate = normalize_i_range(opts.refresh_rate, 0, 300);
  opts.alpha_step = normalize_d_range(opts.alpha_step, 0.01, 1.0);
  if (OPAQUE == opts.inactive_opacity) {
    opts.inactive_opacity = 0;
  }
  if (shadow_enable)
    wintype_arr_enable(opts.wintype_shadow);
  opts.wintype_shadow[WINTYPE_DESKTOP] = False;
  if (cfgtmp.no_dock_shadow)
    opts.wintype_shadow[WINTYPE_DOCK] = False;
  if (cfgtmp.no_dnd_shadow)
    opts.wintype_shadow[WINTYPE_DND] = False;
  if (fading_enable)
    wintype_arr_enable(opts.wintype_fade);
  if (1.0 != cfgtmp.menu_opacity) {
    opts.wintype_opacity[WINTYPE_DROPDOWN_MENU] = cfgtmp.menu_opacity;
    opts.wintype_opacity[WINTYPE_POPUP_MENU] = cfgtmp.menu_opacity;
  }

  // Other variables determined by options

  // Determine whether we need to track focus changes
  if (opts.inactive_opacity || opts.inactive_dim) {
    opts.track_focus = True;
  }

  // Determine whether we need to track window name and class
  if (opts.shadow_blacklist || opts.fade_blacklist)
    opts.track_wdata = True;
}

static void
get_atoms(void) {
  extents_atom = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
  opacity_atom = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
  frame_extents_atom = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
  client_atom = XInternAtom(dpy, "WM_STATE", False);
  name_atom = XA_WM_NAME;
  name_ewmh_atom = XInternAtom(dpy, "_NET_WM_NAME", False);
  class_atom = XA_WM_CLASS;
  transient_atom = XA_WM_TRANSIENT_FOR;

  win_type_atom = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE", False);
  win_type[WINTYPE_UNKNOWN] = 0;
  win_type[WINTYPE_DESKTOP] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DESKTOP", False);
  win_type[WINTYPE_DOCK] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DOCK", False);
  win_type[WINTYPE_TOOLBAR] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
  win_type[WINTYPE_MENU] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_MENU", False);
  win_type[WINTYPE_UTILITY] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_UTILITY", False);
  win_type[WINTYPE_SPLASH] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_SPLASH", False);
  win_type[WINTYPE_DIALOG] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DIALOG", False);
  win_type[WINTYPE_NORMAL] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_NORMAL", False);
  win_type[WINTYPE_DROPDOWN_MENU] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
  win_type[WINTYPE_POPUP_MENU] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
  win_type[WINTYPE_TOOLTIP] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
  win_type[WINTYPE_NOTIFY] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
  win_type[WINTYPE_COMBO] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_COMBO", False);
  win_type[WINTYPE_DND] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DND", False);
}

/**
 * Update refresh rate info with X Randr extension.
 */
static void
update_refresh_rate(Display *dpy) {
  XRRScreenConfiguration* randr_info;

  if (!(randr_info = XRRGetScreenInfo(dpy, root)))
    return;
  refresh_rate = XRRConfigCurrentRate(randr_info);

  XRRFreeScreenConfigInfo(randr_info);

  if (refresh_rate)
    refresh_intv = NS_PER_SEC / refresh_rate;
  else
    refresh_intv = 0;
}

/**
 * Initialize refresh-rated based software optimization.
 *
 * @return True for success, False otherwise
 */
static Bool
sw_opti_init(void) {
  // Prepare refresh rate
  // Check if user provides one
  refresh_rate = opts.refresh_rate;
  if (refresh_rate)
    refresh_intv = NS_PER_SEC / refresh_rate;

  // Auto-detect refresh rate otherwise
  if (!refresh_rate && randr_exists) {
    update_refresh_rate(dpy);
  }

  // Turn off vsync_sw if we can't get the refresh rate
  if (!refresh_rate)
    return False;

  // Monitor screen changes only if vsync_sw is enabled and we are using
  // an auto-detected refresh rate
  if (randr_exists && !opts.refresh_rate)
    XRRSelectInput(dpy, root, RRScreenChangeNotify);

  return True;
}

/**
 * Get the smaller number that is bigger than <code>dividend</code> and is
 * N times of <code>divisor</code>.
 */
static inline long
lceil_ntimes(long dividend, long divisor) {
  // It's possible to use the more beautiful expression here:
  // ret = ((dividend - 1) / divisor + 1) * divisor;
  // But it does not work well for negative values.
  long ret = dividend / divisor * divisor;
  if (ret < dividend)
    ret += divisor;

  return ret;
}

/**
 * Wait for events until next paint.
 *
 * Optionally use refresh-rate based optimization to reduce painting.
 *
 * @param fd struct pollfd used for poll()
 * @param timeout second timeout (fading timeout)
 * @return > 0 if we get some events, 0 if timeout is reached, < 0 on
 *     problems
 */
static int
evpoll(struct pollfd *fd, int timeout) {
  // Always wait infinitely if asked so, to minimize CPU usage
  if (timeout < 0) {
    int ret = poll(fd, 1, timeout);
    // Reset fade_time so the fading steps during idling are not counted
    fade_time = get_time_ms();
    return ret;
  }

  // Just do a poll() if we are not using optimization
  if (!opts.sw_opti)
    return poll(fd, 1, timeout);

  // Convert the old timeout to struct timespec
  struct timespec next_paint_tmout = {
    .tv_sec = timeout / MS_PER_SEC,
    .tv_nsec = timeout % MS_PER_SEC * (NS_PER_SEC / MS_PER_SEC)
  };

  // Get the nanosecond offset of the time when the we reach the timeout
  // I don't think a 32-bit long could overflow here.
  long target_relative_offset = (next_paint_tmout.tv_nsec + get_time_timespec().tv_nsec - paint_tm_offset) % NS_PER_SEC;
  if (target_relative_offset < 0)
    target_relative_offset += NS_PER_SEC;

  assert(target_relative_offset >= 0);

  // If the target time is sufficiently close to a refresh time, don't add
  // an offset, to avoid certain blocking conditions.
  if ((target_relative_offset % NS_PER_SEC) < SW_OPTI_TOLERANCE)
    return poll(fd, 1, timeout);

  // Add an offset so we wait until the next refresh after timeout
  next_paint_tmout.tv_nsec += lceil_ntimes(target_relative_offset, refresh_intv) - target_relative_offset;
  if (next_paint_tmout.tv_nsec > NS_PER_SEC) {
    next_paint_tmout.tv_nsec -= NS_PER_SEC;
    ++next_paint_tmout.tv_sec;
  }

  return ppoll(fd, 1, &next_paint_tmout, NULL);
}

/**
 * Initialize DRM VSync.
 *
 * @return True for success, False otherwise
 */
static Bool
vsync_drm_init(void) {
#ifdef CONFIG_VSYNC_DRM
  // Should we always open card0?
  if ((drm_fd = open("/dev/dri/card0", O_RDWR)) < 0) {
    fprintf(stderr, "vsync_drm_init(): Failed to open device.\n");
    return False;
  }

  if (vsync_drm_wait())
    return False;

  return True;
#else
  fprintf(stderr, "Program not compiled with DRM VSync support.\n");
  return False;
#endif
}

#ifdef CONFIG_VSYNC_DRM
/**
 * Wait for next VSync, DRM method.
 *
 * Stolen from: https://github.com/MythTV/mythtv/blob/master/mythtv/libs/libmythtv/vsync.cpp
 */
static int
vsync_drm_wait(void) {
  int ret = -1;
  drm_wait_vblank_t vbl;

  vbl.request.type = _DRM_VBLANK_RELATIVE,
  vbl.request.sequence = 1;

  do {
     ret = ioctl(drm_fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
     vbl.request.type &= ~_DRM_VBLANK_RELATIVE;
  } while (ret && errno == EINTR);

  if (ret)
    fprintf(stderr, "vsync_drm_wait(): VBlank ioctl did not work, "
        "unimplemented in this drmver?\n");

  return ret;
  
}
#endif

/**
 * Initialize OpenGL VSync.
 *
 * Stolen from: http://git.tuxfamily.org/?p=ccm/cairocompmgr.git;a=commitdiff;h=efa4ceb97da501e8630ca7f12c99b1dce853c73e
 * Possible original source: http://www.inb.uni-luebeck.de/~boehme/xvideo_sync.html
 *
 * @return True for success, False otherwise
 */
static Bool
vsync_opengl_init(void) {
#ifdef CONFIG_VSYNC_OPENGL
  // Get video sync functions
  glx_get_video_sync = (f_GetVideoSync)
    glXGetProcAddress ((const GLubyte *) "glXGetVideoSyncSGI");
  glx_wait_video_sync = (f_WaitVideoSync)
    glXGetProcAddress ((const GLubyte *) "glXWaitVideoSyncSGI");
  if (!glx_wait_video_sync || !glx_get_video_sync) {
    fprintf(stderr, "vsync_opengl_init(): "
        "Failed to get glXWait/GetVideoSyncSGI function.\n");
    return False;
  }
  
  return True;
#else
  fprintf(stderr, "Program not compiled with OpenGL VSync support.\n");
  return False;
#endif
}

#ifdef CONFIG_VSYNC_OPENGL
/**
 * Wait for next VSync, OpenGL method.
 */
static void
vsync_opengl_wait(void) {
  unsigned vblank_count;

  glx_get_video_sync(&vblank_count);
  glx_wait_video_sync(2, (vblank_count + 1) % 2, &vblank_count);
  // I see some code calling glXSwapIntervalSGI(1) afterwards, is it required?
}
#endif

/**
 * Wait for next VSync.
 */
static void
vsync_wait(void) {
  if (VSYNC_NONE == opts.vsync)
    return;

#ifdef CONFIG_VSYNC_DRM
  if (VSYNC_DRM == opts.vsync) {
    vsync_drm_wait();
    return;
  }
#endif

#ifdef CONFIG_VSYNC_OPENGL
  if (VSYNC_OPENGL == opts.vsync) {
    vsync_opengl_wait();
    return;
  }
#endif

  // This place should not reached!
  assert(0);

  return;
}

/**
 * Pregenerate alpha pictures.
 */
static void
init_alpha_picts(Display *dpy) {
  int i;
  int num = lround(1.0 / opts.alpha_step) + 1;

  alpha_picts = malloc(sizeof(Picture) * num);

  for (i = 0; i < num; ++i) {
    double o = i * opts.alpha_step;
    if ((1.0 - o) > opts.alpha_step)
      alpha_picts[i] = solid_picture(dpy, False, o, 0, 0, 0);
    else
      alpha_picts[i] = None;
  }
}

/**
 * Initialize double buffer.
 */
static void
init_dbe(void) {
  if (!(root_dbe = XdbeAllocateBackBufferName(dpy,
          (opts.paint_on_overlay ? overlay: root), XdbeCopied))) {
    fprintf(stderr, "Failed to create double buffer. Double buffering "
        "turned off.\n");
    opts.dbe = False;
    return;
  }
}

/**
 * Initialize X composite overlay window.
 */
static void
init_overlay(void) {
  overlay = XCompositeGetOverlayWindow(dpy, root);
  if (overlay) {
    // Set window region of the overlay window, code stolen from
    // compiz-0.8.8
    XserverRegion region = XFixesCreateRegion (dpy, NULL, 0);
    XFixesSetWindowShapeRegion(dpy, overlay, ShapeBounding, 0, 0, 0);
    XFixesSetWindowShapeRegion(dpy, overlay, ShapeInput, 0, 0, region);
    XFixesDestroyRegion (dpy, region);

    // Listen to Expose events on the overlay
    XSelectInput(dpy, overlay, ExposureMask);

    // Retrieve DamageNotify on root window if we are painting on an
    // overlay
    // root_damage = XDamageCreate(dpy, root, XDamageReportNonEmpty);
  }
  else {
    fprintf(stderr, "Cannot get X Composite overlay window. Falling "
        "back to painting on root window.\n");
    opts.paint_on_overlay = False;
  }
}

int
main(int argc, char **argv) {
  XEvent ev;
  Window root_return, parent_return;
  Window *children;
  unsigned int nchildren;
  int i;
  XRenderPictureAttributes pa;
  struct pollfd ufd;
  int composite_major, composite_minor;
  win *t;

  gettimeofday(&time_start, NULL);

  // Set locale so window names with special characters are interpreted
  // correctly
  setlocale (LC_ALL, "");

  get_cfg(argc, argv);

  fade_time = get_time_ms();

  dpy = XOpenDisplay(opts.display);
  if (!dpy) {
    fprintf(stderr, "Can't open display\n");
    exit(1);
  }

  XSetErrorHandler(error);
  if (opts.synchronize) {
    XSynchronize(dpy, 1);
  }

  scr = DefaultScreen(dpy);
  root = RootWindow(dpy, scr);

  if (!XRenderQueryExtension(dpy, &render_event, &render_error)) {
    fprintf(stderr, "No render extension\n");
    exit(1);
  }

  if (!XQueryExtension(dpy, COMPOSITE_NAME, &composite_opcode,
                       &composite_event, &composite_error)) {
    fprintf(stderr, "No composite extension\n");
    exit(1);
  }

  XCompositeQueryVersion(dpy, &composite_major, &composite_minor);

  if (composite_major > 0 || composite_minor >= 2) {
    has_name_pixmap = True;
  }

  if (!XDamageQueryExtension(dpy, &damage_event, &damage_error)) {
    fprintf(stderr, "No damage extension\n");
    exit(1);
  }

  if (!XFixesQueryExtension(dpy, &xfixes_event, &xfixes_error)) {
    fprintf(stderr, "No XFixes extension\n");
    exit(1);
  }

  // Query X Shape
  if (XShapeQueryExtension(dpy, &shape_event, &shape_error)) {
    shape_exists = True;
  }

  // Query X RandR
  if (opts.sw_opti && !opts.refresh_rate) {
    if (XRRQueryExtension(dpy, &randr_event, &randr_error))
      randr_exists = True;
    else
      fprintf(stderr, "No XRandR extension, automatic refresh rate "
          "detection impossible.\n");
  }

#ifdef CONFIG_VSYNC_OPENGL
  // Query X GLX extension
  if (VSYNC_OPENGL == opts.vsync) {
    if (glXQueryExtension(dpy, &glx_event, &glx_error))
      glx_exists = True;
    else {
      fprintf(stderr, "No GLX extension, OpenGL VSync impossible.\n");
      opts.vsync = VSYNC_NONE;
    }
  }
#endif

  // Query X DBE extension
  if (opts.dbe) {
    int dbe_ver_major = 0, dbe_ver_minor = 0;
    if (XdbeQueryExtension(dpy, &dbe_ver_major, &dbe_ver_minor))
      if (dbe_ver_major >= 1)
        dbe_exists = True;
      else
        fprintf(stderr, "DBE extension version too low. Double buffering "
            "impossible.\n");
    else {
      fprintf(stderr, "No DBE extension. Double buffering impossible.\n");
    }
    if (!dbe_exists)
      opts.dbe = False;
  }

  register_cm((VSYNC_OPENGL == opts.vsync));

  // Initialize software optimization
  if (opts.sw_opti)
    opts.sw_opti = sw_opti_init();

  // Initialize DRM/OpenGL VSync
  if ((VSYNC_DRM == opts.vsync && !vsync_drm_init())
      || (VSYNC_OPENGL == opts.vsync && !vsync_opengl_init()))
    opts.vsync = VSYNC_NONE;

  // Overlay must be initialized before double buffer
  if (opts.paint_on_overlay)
    init_overlay();

  if (opts.dbe)
    init_dbe();

  if (opts.fork_after_register) fork_after();

  get_atoms();
  init_alpha_picts(dpy);

  pa.subwindow_mode = IncludeInferiors;

  gaussian_map = make_gaussian_map(dpy, opts.shadow_radius);
  presum_gaussian(gaussian_map);

  root_width = DisplayWidth(dpy, scr);
  root_height = DisplayHeight(dpy, scr);

  rebuild_screen_reg(dpy);

  root_picture = XRenderCreatePicture(dpy, root,
      XRenderFindVisualFormat(dpy, DefaultVisual(dpy, scr)),
      CPSubwindowMode, &pa);
  if (opts.paint_on_overlay) {
    tgt_picture = XRenderCreatePicture(dpy, overlay,
        XRenderFindVisualFormat(dpy, DefaultVisual(dpy, scr)),
        CPSubwindowMode, &pa);
  }
  else {
    tgt_picture = root_picture;
  }

  black_picture = solid_picture(dpy, True, 1, 0, 0, 0);

  // Generates another Picture for shadows if the color is modified by
  // user
  if (!opts.shadow_red && !opts.shadow_green && !opts.shadow_blue) {
    cshadow_picture = black_picture;
  } else {
    cshadow_picture = solid_picture(dpy, True, 1,
        opts.shadow_red, opts.shadow_green, opts.shadow_blue);
  }

  // Generates a picture for inactive_dim
  if (opts.inactive_dim) {
    dim_picture = solid_picture(dpy, True, opts.inactive_dim, 0, 0, 0);
  }

  all_damage = None;
  XGrabServer(dpy);

  XCompositeRedirectSubwindows(
    dpy, root, CompositeRedirectManual);

  XSelectInput(dpy, root,
    SubstructureNotifyMask
    | ExposureMask
    | StructureNotifyMask
    | PropertyChangeMask);

  XQueryTree(dpy, root, &root_return,
    &parent_return, &children, &nchildren);

  for (i = 0; i < nchildren; i++) {
    add_win(dpy, children[i], i ? children[i-1] : None, False);
  }

  XFree(children);

  if (opts.track_focus) {
    recheck_focus(dpy);
  }

  XUngrabServer(dpy);

  ufd.fd = ConnectionNumber(dpy);
  ufd.events = POLLIN;

  if (opts.sw_opti)
    paint_tm_offset = get_time_timespec().tv_nsec;

  reg_ignore_expire = True;

  t = paint_preprocess(dpy, list);

  paint_all(dpy, None, t);

  // Initialize idling
  idling = False;

  // Main loop
  while (1) {
    Bool ev_received = False;

    while (XEventsQueued(dpy, QueuedAfterReading)
        || (evpoll(&ufd,
            (ev_received ? 0: (idling ? -1: fade_timeout()))) > 0)) {
      // Sometimes poll() returns 1 but no events are actually read, causing
      // XNextEvent() to block, I have no idea what's wrong, so we check for the
      // number of events here
      if (XEventsQueued(dpy, QueuedAfterReading)) {
        XNextEvent(dpy, &ev);
        ev_handle((XEvent *) &ev);
        ev_received = True;
      }
    }

    // idling will be turned off during paint_preprocess() if needed
    idling = True;

    t = paint_preprocess(dpy, list);

    if (all_damage && !is_region_empty(dpy, all_damage)) {
      static int paint;
      paint_all(dpy, all_damage, t);
      reg_ignore_expire = False;
      paint++;
      XSync(dpy, False);
      all_damage = None;
    }
  }
}
