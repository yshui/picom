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
Display *dpy;
int scr;

Window root;
Picture root_picture;
Picture root_buffer;
Picture black_picture;
Picture cshadow_picture;
/// Picture used for dimming inactive windows.
Picture dim_picture = 0;
Picture root_tile;
XserverRegion all_damage;
#if HAS_NAME_WINDOW_PIXMAP
Bool has_name_pixmap;
#endif
int root_height, root_width;
/// Whether the program is idling. I.e. no fading, no potential window
/// changes.
Bool idling;

/* errors */
ignore *ignore_head = NULL, **ignore_tail = &ignore_head;
int xfixes_event, xfixes_error;
int damage_event, damage_error;
int composite_event, composite_error;
/// Whether X Shape extension exists.
Bool shape_exists = True;
/// Event base number and error base number for X Shape extension.
int shape_event, shape_error;
int render_event, render_error;
int composite_opcode;

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

Atom win_type_atom;
Atom win_type[NUM_WINTYPES];

unsigned long fade_time;

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
  .inactive_dim = 0.0,

  .track_focus = False,
  .track_wdata = False,
};

/**
 * Fades
 */

/**
 * Get current system clock in milliseconds.
 *
 * The return type must be unsigned long because so many milliseconds have
 * passed since the epoch.
 */
static unsigned long
get_time_in_milliseconds() {
  struct timeval tv;

  gettimeofday(&tv, NULL);

  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/**
 * Get the time left before next fading point.
 *
 * In milliseconds.
 */
static int
fade_timeout(void) {
  int diff = opts.fade_delta - get_time_in_milliseconds() + fade_time;

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
  // If we reach target opacity, set fade_fin so the callback gets
  // executed
  if (w->opacity == w->opacity_tgt) {
    w->fade_fin = True;
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

  if (w->opacity == w->opacity_tgt) {
    w->fade_fin = True;
    return;
  }
  else {
    idling = False;
  }

  w->fade_fin = False;
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
            int width, int height) {
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
  if (!(opts.clear_shadow && opts.shadow_offset_x <= 0 && opts.shadow_offset_x >= -cgsize
        && opts.shadow_offset_y <= 0 && opts.shadow_offset_y >= -cgsize)) {
    if (cgsize > 0) {
      d = shadow_top[opacity_int * (cgsize + 1) + cgsize];
    } else {
      d = sum_gaussian(gaussian_map,
        opacity, center, center, width, height);
    }
    memset(data, d, sheight * swidth);
  }

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

  if (opts.clear_shadow) {
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

  return ximage;
}

static Picture
shadow_picture(Display *dpy, double opacity, int width, int height) {
  XImage *shadow_image = NULL;
  Pixmap shadow_pixmap = None, shadow_pixmap_argb = None;
  Picture shadow_picture = None, shadow_picture_argb = None;
  GC gc = None;

  shadow_image = make_shadow(dpy, opacity, width, height);
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
    if (opts.frame_opacity || opts.track_wdata)
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
    root_buffer, 0, 0, 0, 0, 0, 0,
    root_width, root_height);
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
  XserverRegion border;

  /*
   * if window doesn't exist anymore,  this will generate an error
   * as well as not generate a region.  Perhaps a better XFixes
   * architecture would be to have a request that copies instead
   * of creates, that way you'd just end up with an empty region
   * instead of an invalid XID.
   */

  border = XFixesCreateRegionFromWindow(
    dpy, w->id, WindowRegionBounding);

  /* translate this */
  XFixesTranslateRegion(dpy, border,
    w->a.x + w->a.border_width,
    w->a.y + w->a.border_width);

  return border;
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
    }
    XFree(data);
  }
}

static win *
paint_preprocess(Display *dpy, win *list) {
  win *w;
  win *t = NULL, *next = NULL;
  // Sounds like the timeout in poll() frequently does not work
  // accurately, asking it to wait to 20ms, and often it would wait for
  // 19ms, so the step value has to be rounded.
  unsigned steps = roundl((double) (get_time_in_milliseconds() - fade_time) / opts.fade_delta);

  // Reset fade_time
  fade_time = get_time_in_milliseconds();

  for (w = list; w; w = next) {
    // In case calling the fade callback function destroys this window
    next = w->next;
    opacity_t opacity_old = w->opacity;

#if CAN_DO_USABLE
    if (!w->usable) continue;
#endif

    // Run fading
    run_fade(dpy, w, steps);

    // Give up if it's not damaged or invisible
    if (!w->damaged
        || w->a.x + w->a.width < 1 || w->a.y + w->a.height < 1
        || w->a.x >= root_width || w->a.y >= root_height) {
      check_fade_fin(dpy, w);
      continue;
    }

    // If opacity changes
    if (w->opacity != opacity_old) {
      determine_mode(dpy, w);
      add_damage_win(dpy, w);
    }

    if (!w->opacity) {
      check_fade_fin(dpy, w);
      continue;
    }

    // Fetch the picture and pixmap if needed
    if (!w->picture) {
      XRenderPictureAttributes pa;
      XRenderPictFormat *format;
      Drawable draw = w->id;

#if HAS_NAME_WINDOW_PIXMAP
      if (has_name_pixmap && !w->pixmap) {
        set_ignore(dpy, NextRequest(dpy));
        w->pixmap = XCompositeNameWindowPixmap(dpy, w->id);
      }
      if (w->pixmap) draw = w->pixmap;
#endif

      format = XRenderFindVisualFormat(dpy, w->a.visual);
      pa.subwindow_mode = IncludeInferiors;
      w->picture = XRenderCreatePicture(
        dpy, draw, format, CPSubwindowMode, &pa);
    }

    // Fetch bounding region and extents if needed
    if (!w->border_size) {
      w->border_size = border_size(dpy, w);
    }

    if (!w->extents) {
      w->extents = win_extents(dpy, w);
      // If w->extents does not exist, the previous add_damage_win()
      // call when opacity changes has no effect, so redo it here.
      if (w->opacity != opacity_old)
        add_damage_win(dpy, w);
    }

    // Rebuild alpha_pict only if necessary
    if (OPAQUE != w->opacity
        && (!w->alpha_pict || w->opacity != w->opacity_cur)) {
      free_picture(dpy, &w->alpha_pict);
      w->alpha_pict = solid_picture(
        dpy, False, get_opacity_percent(dpy, w), 0, 0, 0);
      w->opacity_cur = w->opacity;
    }

    // Calculate frame_opacity
    if (opts.frame_opacity && 1.0 != opts.frame_opacity && w->top_width)
      w->frame_opacity = get_opacity_percent(dpy, w) * opts.frame_opacity;
    else
      w->frame_opacity = 0.0;

    // Rebuild frame_alpha_pict only if necessary
    if (w->frame_opacity
        && (!w->frame_alpha_pict
          || w->frame_opacity != w->frame_opacity_cur)) {
      free_picture(dpy, &w->frame_alpha_pict);
      w->frame_alpha_pict = solid_picture(
        dpy, False, w->frame_opacity, 0, 0, 0);
      w->frame_opacity_cur = w->frame_opacity;
    }

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
          w->widthb, w->heightb);
    }

    // Rebuild shadow_alpha_pict if necessary
    if (w->shadow
        && (!w->shadow_alpha_pict
          || w->shadow_opacity != w->shadow_opacity_cur)) {
      free_picture(dpy, &w->shadow_alpha_pict);
      w->shadow_alpha_pict = solid_picture(
        dpy, False, w->shadow_opacity, 0, 0, 0);
      w->shadow_opacity_cur = w->shadow_opacity;
    }

    // Reset flags
    w->flags = 0;

    w->prev_trans = t;
    t = w;
  }

  return t;
}

static void
paint_all(Display *dpy, XserverRegion region, win *t) {
  win *w;

  if (!region) {
    region = get_screen_region(dpy);
  }

#ifdef MONITOR_REPAINT
  root_buffer = root_picture;
#else
  if (!root_buffer) {
    Pixmap root_pixmap = XCreatePixmap(
      dpy, root, root_width, root_height,
      DefaultDepth(dpy, scr));

    root_buffer = XRenderCreatePicture(dpy, root_pixmap,
      XRenderFindVisualFormat(dpy, DefaultVisual(dpy, scr)),
      0, 0);

    XFreePixmap(dpy, root_pixmap);
  }
#endif

  XFixesSetPictureClipRegion(dpy, root_picture, 0, 0, region);

#ifdef MONITOR_REPAINT
  XRenderComposite(
    dpy, PictOpSrc, black_picture, None,
    root_picture, 0, 0, 0, 0, 0, 0,
    root_width, root_height);
#endif

  paint_root(dpy);

#ifdef DEBUG_REPAINT
  printf("paint:");
#endif

  for (w = t; w; w = w->prev_trans) {
    int x, y, wid, hei;

#if HAS_NAME_WINDOW_PIXMAP
    x = w->a.x;
    y = w->a.y;
    wid = w->widthb;
    hei = w->heightb;
#else
    x = w->a.x + w->a.border_width;
    y = w->a.y + w->a.border_width;
    wid = w->a.width;
    hei = w->a.height;
#endif

#ifdef DEBUG_REPAINT
    printf(" %#010lx", w->id);
#endif

    // Allow shadow to be painted anywhere in the damaged region
    XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, region);

    // Painting shadow
    if (w->shadow) {
      XRenderComposite(
        dpy, PictOpOver, w->shadow_pict, w->shadow_alpha_pict,
        root_buffer, 0, 0, 0, 0,
        w->a.x + w->shadow_dx, w->a.y + w->shadow_dy,
        w->shadow_width, w->shadow_height);
    }

    // The window only could be painted in its bounding region
    XserverRegion paint_reg = XFixesCreateRegion(dpy, NULL, 0);
    XFixesIntersectRegion(dpy, paint_reg, region, w->border_size);
    XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, paint_reg);

    Picture alpha_mask = (OPAQUE == w->opacity ? None: w->alpha_pict);
    int op = (w->mode == WINDOW_SOLID ? PictOpSrc: PictOpOver);

    // Painting the window
    if (!w->frame_opacity) {
      XRenderComposite(dpy, op, w->picture, alpha_mask,
          root_buffer, 0, 0, 0, 0, x, y, wid, hei);
    }
    else {
      unsigned int t = w->top_width;
      unsigned int l = w->left_width;
      unsigned int b = w->bottom_width;
      unsigned int r = w->right_width;

      /* top */
      XRenderComposite(
        dpy, PictOpOver, w->picture, w->frame_alpha_pict, root_buffer,
        0, 0, 0, 0, x, y, wid, t);

      /* left */
      XRenderComposite(
        dpy, PictOpOver, w->picture, w->frame_alpha_pict, root_buffer,
        0, t, 0, t, x, y + t, l, hei - t);

      /* bottom */
      XRenderComposite(
        dpy, PictOpOver, w->picture, w->frame_alpha_pict, root_buffer,
        l, hei - b, l, hei - b, x + l, y + hei - b, wid - l - r, b);

      /* right */
      XRenderComposite(
        dpy, PictOpOver, w->picture, w->frame_alpha_pict, root_buffer,
        wid - r, t, wid - r, t, x + wid - r, y + t, r, hei - t);

      /* body */
      XRenderComposite(
        dpy, op, w->picture, alpha_mask, root_buffer,
        l, t, l, t, x + l, y + t, wid - l - r, hei - t - b);

    }

    // Dimming the window if needed
    if (w->dim) {
      XRenderComposite(dpy, PictOpOver, dim_picture, None,
          root_buffer, 0, 0, 0, 0, x, y, wid, hei);
    }

    XFixesDestroyRegion(dpy, paint_reg);

    check_fade_fin(dpy, w);
  }

#ifdef DEBUG_REPAINT
  printf("\n");
  fflush(stdout);
#endif

  XFixesDestroyRegion(dpy, region);

  if (root_buffer != root_picture) {
    XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, None);
    XRenderComposite(
      dpy, PictOpSrc, root_buffer, None,
      root_picture, 0, 0, 0, 0,
      0, 0, root_width, root_height);
  }
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

static wintype
determine_wintype(Display *dpy, Window w) {
  Window *children = NULL;
  unsigned int nchildren, i;
  wintype type;

  type = get_wintype_prop(dpy, w);
  if (type != WINTYPE_UNKNOWN) return type;

  if (!wid_get_children(dpy, w, &children, &nchildren))
    return WINTYPE_UNKNOWN;

  for (i = 0; i < nchildren; i++) {
    type = determine_wintype(dpy, children[i]);
    if (type != WINTYPE_UNKNOWN) return type;
  }

  if (children) {
    XFree((void *)children);
  }

  return WINTYPE_UNKNOWN;
}

static void
map_win(Display *dpy, Window id,
        unsigned long sequence, Bool fade,
        Bool override_redirect) {
  win *w = find_win(dpy, id);

  if (!w) return;

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
    Window cw = find_client_win(dpy, w->id);
#ifdef DEBUG_CLIENTWIN
    printf("find_client_win(%#010lx): client %#010lx\n", w->id, cw);
#endif
    if (cw) {
      mark_client_win(dpy, w, cw);
    }
  }
  else if (opts.frame_opacity) {
    // Refetch frame extents just in case it changes when the window is
    // unmapped
    get_frame_extents(dpy, w, w->client_win);
  }

  if (WINTYPE_UNKNOWN == w->window_type)
    w->window_type = determine_wintype(dpy, w->id);

#ifdef DEBUG_WINTYPE
  printf("map_win(%#010lx): type %s\n",
    w->id, WINTYPES[w->window_type]);
#endif

  // Detect if the window is shaped or has rounded corners
  if (opts.shadow_ignore_shaped) {
    w->bounding_shaped = wid_bounding_shaped(dpy, w->id);
    if (w->bounding_shaped && opts.detect_rounded_corners)
      win_rounded_corners(dpy, w);
  }

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

  // Determine mode here just in case the colormap changes
  determine_mode(dpy, w);

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

#if CAN_DO_USABLE
  w->damage_bounds.x = w->damage_bounds.y = 0;
  w->damage_bounds.width = w->damage_bounds.height = 0;
#endif
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
#if CAN_DO_USABLE
  w->usable = False;
#endif

  if (w->extents != None) {
    /* destroys region */
    add_damage(dpy, w->extents);
    w->extents = None;
  }

#if HAS_NAME_WINDOW_PIXMAP
  free_pixmap(dpy, &w->pixmap);
#endif

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
get_opacity_prop(Display *dpy, win *w, opacity_t def) {
  Atom actual;
  int format;
  unsigned long n, left;

  unsigned char *data;
  int result = XGetWindowProperty(
    dpy, w->id, opacity_atom, 0L, 1L, False,
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
  int mode;
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
    w->opacity_prop = get_opacity_prop(dpy, w, OPAQUE);
  }

  if (OPAQUE == (opacity = w->opacity_prop)) {
    if (1.0 != opts.wintype_opacity[w->window_type]) {
      opacity = opts.wintype_opacity[w->window_type] * OPAQUE;
    }
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

  // Get the frame width and monitor further frame width changes on client
  // window if necessary
  if (opts.frame_opacity) {
    get_frame_extents(dpy, w, client);
  }
  XSelectInput(dpy, client, determine_evmask(dpy, client, WIN_EVMODE_CLIENT));
  if (WINTYPE_UNKNOWN == w->window_type)
    w->window_type = get_wintype_prop(dpy, w->client_win);
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
#if CAN_DO_USABLE
  new->usable = False;
#endif
#if HAS_NAME_WINDOW_PIXMAP
  new->pixmap = None;
#endif
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
  new->extents = None;
  new->shadow = False;
  new->shadow_opacity = 0.0;
  new->shadow_opacity_cur = 0.0;
  new->shadow_pict = None;
  new->shadow_alpha_pict = None;
  new->shadow_dx = 0;
  new->shadow_dy = 0;
  new->shadow_width = 0;
  new->shadow_height = 0;
  new->opacity = 0;
  new->opacity_tgt = 0;
  new->opacity_cur = OPAQUE;
  new->opacity_prop = OPAQUE;
  new->fade = False;
  new->fade_callback = NULL;
  new->fade_fin = False;
  new->alpha_pict = None;
  new->frame_opacity = 1.0;
  new->frame_opacity_cur = 1.0;
  new->frame_alpha_pict = None;
  new->dim = False;
  new->focused = False;
  new->destroyed = False;
  new->need_configure = False;
  new->window_type = WINTYPE_UNKNOWN;

  new->prev_trans = 0;

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
  win *w = find_win(dpy, ce->window);
  XserverRegion damage = None;

  if (!w) {
    if (ce->window == root) {
      if (root_buffer) {
        XRenderFreePicture(dpy, root_buffer);
        root_buffer = None;
      }
      root_width = ce->width;
      root_height = ce->height;
    }
    return;
  }

  if (w->a.map_state == IsUnmapped) {
    /* save the configure event for when the window maps */
    w->need_configure = True;
    w->queue_configure = *ce;
    restack_win(dpy, w, ce->above);
  } else {
    if (!(w->need_configure)) {
      restack_win(dpy, w, ce->above);
    }

    w->need_configure = False;

#if CAN_DO_USABLE
    if (w->usable)
#endif
    {
      damage = XFixesCreateRegion(dpy, 0, 0);
      if (w->extents != None) {
        XFixesCopyRegion(dpy, damage, w->extents);
      }
    }

    w->a.x = ce->x;
    w->a.y = ce->y;

    if (w->a.width != ce->width || w->a.height != ce->height) {
#if HAS_NAME_WINDOW_PIXMAP
      free_pixmap(dpy, &w->pixmap);
      free_picture(dpy, &w->picture);
#endif
    }

    if (w->a.width != ce->width || w->a.height != ce->height
        || w->a.border_width != ce->border_width) {
      w->a.width = ce->width;
      w->a.height = ce->height;
      w->a.border_width = ce->border_width;
      calc_win_size(dpy, w);
    }

    if (w->a.map_state != IsUnmapped && damage) {
      XserverRegion extents = win_extents(dpy, w);
      XFixesUnionRegion(dpy, damage, damage, extents);
      XFixesDestroyRegion(dpy, extents);
      add_damage(dpy, damage);
    }

    // Window extents and border_size may have changed
    free_region(dpy, &w->extents);
    free_region(dpy, &w->border_size);
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

      free_picture(dpy, &w->alpha_pict);
      free_picture(dpy, &w->frame_alpha_pict);
      free_picture(dpy, &w->shadow_pict);
      free_damage(dpy, &w->damage);
      free(w->name);
      free(w->class_instance);
      free(w->class_general);

      free(w);
      break;
    }
  }
}

#if HAS_NAME_WINDOW_PIXMAP
static void
destroy_callback(Display *dpy, win *w) {
  finish_destroy_win(dpy, w->id);
}
#endif

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

static void
damage_win(Display *dpy, XDamageNotifyEvent *de) {
  win *w = find_win(dpy, de->drawable);

  if (!w) return;

#if CAN_DO_USABLE
  if (!w->usable) {
    if (w->damage_bounds.width == 0 || w->damage_bounds.height == 0) {
      w->damage_bounds = de->area;
    } else {
      if (de->area.x < w->damage_bounds.x) {
        w->damage_bounds.width += (w->damage_bounds.x - de->area.x);
        w->damage_bounds.x = de->area.x;
      }
      if (de->area.y < w->damage_bounds.y) {
        w->damage_bounds.height += (w->damage_bounds.y - de->area.y);
        w->damage_bounds.y = de->area.y;
      }
      if (de->area.x + de->area.width
          > w->damage_bounds.x + w->damage_bounds.width) {
        w->damage_bounds.width =
          de->area.x + de->area.width - w->damage_bounds.x;
      }
      if (de->area.y + de->area.height
          > w->damage_bounds.y + w->damage_bounds.height) {
        w->damage_bounds.height =
          de->area.y + de->area.height - w->damage_bounds.y;
      }
    }

    if (w->damage_bounds.x <= 0
        && w->damage_bounds.y <= 0
        && w->a.width <= w->damage_bounds.x + w->damage_bounds.width
        && w->a.height <= w->damage_bounds.y + w->damage_bounds.height) {
      if (opts.wintype_fade[w->window_type]) {
        set_fade(dpy, w, 0, get_opacity_percent(dpy, w),
                 opts.fade_in_step, 0, True, True);
      }
      w->usable = True;
    }
  }

  if (w->usable)
#endif
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

  o = ev->error_code - xfixes_error;
  switch (o) {
    case BadRegion:
      name = "BadRegion";
      break;
    default:
      break;
  }

  o = ev->error_code - damage_error;
  switch (o) {
    case BadDamage:
      name = "BadDamage";
      break;
    default:
      break;
  }

  o = ev->error_code - render_error;
  switch (o) {
    case BadPictFormat:
      name = "BadPictFormat";
      break;
    case BadPicture:
      name = "BadPicture";
      break;
    case BadPictOp:
      name = "BadPictOp";
      break;
    case BadGlyphSet:
      name = "BadGlyphSet";
      break;
    case BadGlyph:
      name = "BadGlyph";
      break;
    default:
      break;
  }

  switch (ev->error_code) {
    case BadAccess:
      name = "BadAccess";
      break;
    case BadAlloc:
      name = "BadAlloc";
      break;
    case BadAtom:
      name = "BadAtom";
      break;
    case BadColor:
      name = "BadColor";
      break;
    case BadCursor:
      name = "BadCursor";
      break;
    case BadDrawable:
      name = "BadDrawable";
      break;
    case BadFont:
      name = "BadFont";
      break;
    case BadGC:
      name = "BadGC";
      break;
    case BadIDChoice:
      name = "BadIDChoice";
      break;
    case BadImplementation:
      name = "BadImplementation";
      break;
    case BadLength:
      name = "BadLength";
      break;
    case BadMatch:
      name = "BadMatch";
      break;
    case BadName:
      name = "BadName";
      break;
    case BadPixmap:
      name = "BadPixmap";
      break;
    case BadRequest:
      name = "BadRequest";
      break;
    case BadValue:
      name = "BadValue";
      break;
    case BadWindow:
      name = "BadWindow";
      break;
  }

  print_timestamp();
  printf("error %d (%s) request %d minor %d serial %lu\n",
    ev->error_code, name, ev->request_code,
    ev->minor_code, ev->serial);

  return 0;
}

static void
expose_root(Display *dpy, Window root, XRectangle *rects, int nrects) {
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

static char *
ev_name(XEvent *ev) {
  static char buf[128];
  switch (ev->type & 0x7f) {
    case FocusIn:
      return "FocusIn";
    case FocusOut:
      return "FocusOut";
    case CreateNotify:
      return "CreateNotify";
    case ConfigureNotify:
      return "ConfigureNotify";
    case DestroyNotify:
      return "DestroyNotify";
    case MapNotify:
      return "Map";
    case UnmapNotify:
      return "Unmap";
    case ReparentNotify:
      return "Reparent";
    case CirculateNotify:
      return "Circulate";
    case Expose:
      return "Expose";
    case PropertyNotify:
      return "PropertyNotify";
    case ClientMessage:
      return "ClientMessage";
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
#endif

/**
 * Events
 */

inline static void
ev_focus_in(XFocusChangeEvent *ev) {
  win *w = find_win(dpy, ev->window);

  // To deal with events sent from windows just destroyed
  if (!w) return;

  set_focused(dpy, w, True);
}

inline static void
ev_focus_out(XFocusChangeEvent *ev) {
  if (ev->mode == NotifyGrab
      || (ev->mode == NotifyNormal
      && (ev->detail == NotifyNonlinear
      || ev->detail == NotifyNonlinearVirtual))) {
    ;
  } else {
    return;
  }

  win *w = find_win(dpy, ev->window);

  // To deal with events sent from windows just destroyed
  if (!w) return;

  set_focused(dpy, w, False);
}

inline static void
ev_create_notify(XCreateWindowEvent *ev) {
  add_win(dpy, ev->window, 0, ev->override_redirect);
}

inline static void
ev_configure_notify(XConfigureEvent *ev) {
#ifdef DEBUG_EVENTS
  printf("{ send_event: %d, "
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
  if (ev->window == root) {
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
      expose_root(dpy, root, expose_rects, n_expose);
      n_expose = 0;
    }
  }
}

inline static void
ev_property_notify(XPropertyEvent *ev) {
  int p;
  for (p = 0; background_props[p]; p++) {
    if (ev->atom == XInternAtom(dpy, background_props[p], False)) {
      if (root_tile) {
        XClearArea(dpy, root, 0, 0, 0, 0, True);
        XRenderFreePicture(dpy, root_tile);
        root_tile = None;
        break;
      }
    }
  }

  /* check if Trans property was changed */
  if (ev->atom == opacity_atom) {
    /* reset mode and redraw window */
    win *w = find_win(dpy, ev->window);
    if (w) {
      calc_opacity(dpy, w, True);
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
  if (!w) return;

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
  if (opts.shadow_ignore_shaped) {
    w->bounding_shaped = wid_bounding_shaped(dpy, w->id);
    if (w->bounding_shaped && opts.detect_rounded_corners)
      win_rounded_corners(dpy, w);

    // Shadow state could be changed
    determine_shadow(dpy, w);
  }
}

inline static void
ev_handle(XEvent *ev) {
  if ((ev->type & 0x7f) != KeymapNotify) {
    discard_ignore(dpy, ev->xany.serial);
  }

#ifdef DEBUG_EVENTS
  if (ev->type != damage_event + XDamageNotify) {
    Window w;
    char *window_name;
    Bool to_free = False;

    w = ev_window(ev);
    window_name = "(Failed to get title)";

    if (w) {
      if (root == w) {
        window_name = "(Root window)";
      } else {
        to_free = (Bool) wid_get_name(dpy, w, &window_name);
      }
    }

    print_timestamp();
    printf("event %10.10s serial %#010x window %#010lx \"%s\"\n",
      ev_name(ev), ev_serial(ev), w, window_name);

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
      if (ev->type == damage_event + XDamageNotify) {
        ev_damage_notify((XDamageNotifyEvent *)ev);
      }
      break;
  }
}

/**
 * Main
 */

static void
usage(void) {
  fprintf(stderr, "compton (development version)\n");
  fprintf(stderr, "usage: compton [options]\n");
  fprintf(stderr,
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
    "-b daemonize\n"
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
    );

  exit(1);
}

static void
register_cm(int scr) {
  Window w;
  Atom a;
  char *buf;
  int len, s;

  if (scr < 0) return;

  w = XCreateSimpleWindow(
    dpy, RootWindow(dpy, 0),
    0, 0, 1, 1, 0, None, None);

  Xutf8SetWMProperties(
    dpy, w, "xcompmgr", "xcompmgr",
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

  XSetSelectionOwner(dpy, a, w, 0);
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
 * Parse a configuration file from default location.
 */
static void
parse_config(char *cpath, struct options_tmp *pcfgtmp) {
  char *path = NULL;
  FILE *f;
  config_t cfg;
  int ival = 0;
  double dval = 0.0;

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
  client_atom = XA_WM_CLASS;
  name_atom = XA_WM_NAME;
  name_ewmh_atom = XInternAtom(dpy, "_NET_WM_NAME", False);
  class_atom = XA_WM_CLASS;

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

  fade_time = get_time_in_milliseconds();

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

#if HAS_NAME_WINDOW_PIXMAP
  if (composite_major > 0 || composite_minor >= 2) {
    has_name_pixmap = True;
  }
#endif

  if (!XDamageQueryExtension(dpy, &damage_event, &damage_error)) {
    fprintf(stderr, "No damage extension\n");
    exit(1);
  }

  if (!XFixesQueryExtension(dpy, &xfixes_event, &xfixes_error)) {
    fprintf(stderr, "No XFixes extension\n");
    exit(1);
  }

  if (!XShapeQueryExtension(dpy, &shape_event, &shape_error)) {
    shape_exists = False;
  }

  register_cm(scr);

  if (opts.fork_after_register) fork_after();

  get_atoms();

  pa.subwindow_mode = IncludeInferiors;

  gaussian_map = make_gaussian_map(dpy, opts.shadow_radius);
  presum_gaussian(gaussian_map);

  root_width = DisplayWidth(dpy, scr);
  root_height = DisplayHeight(dpy, scr);

  root_picture = XRenderCreatePicture(dpy, root,
    XRenderFindVisualFormat(dpy, DefaultVisual(dpy, scr)),
    CPSubwindowMode, &pa);

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

  t = paint_preprocess(dpy, list);
  paint_all(dpy, None, t);

  // Initialize idling
  idling = False;

  for (;;) {
    do {
      if (!QLength(dpy)) {
        if (poll(&ufd, 1, (idling ? -1: fade_timeout())) == 0) {
          break;
        }
      }

      XNextEvent(dpy, &ev);
      ev_handle((XEvent *)&ev);
    } while (QLength(dpy));

    // idling will be turned off during paint_preprocess() if needed
    idling = True;

    t = paint_preprocess(dpy, list);
    if (all_damage) {
      static int paint;
      paint_all(dpy, all_damage, t);
      paint++;
      XSync(dpy, False);
      all_damage = None;
    }
  }
}
