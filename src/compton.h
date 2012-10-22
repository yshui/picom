/**
 * compton.h
 */

// Throw everything in here.

// === Options ===

#define CAN_DO_USABLE 0

// Debug options, enable them using -D in CFLAGS
// #define DEBUG_REPAINT 1
// #define DEBUG_EVENTS 1
// #define DEBUG_RESTACK 1
// #define DEBUG_WINTYPE 1
// #define DEBUG_CLIENTWIN 1
// #define DEBUG_WINDATA 1
// #define DEBUG_WINMATCH 1
// #define MONITOR_REPAINT 1

// Whether to enable PCRE regular expression support in blacklists, enabled
// by default
// #define CONFIG_REGEX_PCRE 1
// Whether to enable JIT support of libpcre. This may cause problems on PaX
// kernels.
// #define CONFIG_REGEX_PCRE_JIT 1
// Whether to enable parsing of configuration files using libconfig
// #define CONFIG_LIBCONFIG 1
// Whether to enable DRM VSync support
// #define CONFIG_VSYNC_DRM 1
// Whether to enable OpenGL VSync support
// #define CONFIG_VSYNC_OPENGL 1

// === Includes ===

// For some special functions
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <locale.h>
#include <assert.h>

#include <fnmatch.h>

#ifdef CONFIG_REGEX_PCRE
#include <pcre.h>

// For compatiblity with <libpcre-8.20
#ifndef PCRE_STUDY_JIT_COMPILE
#define PCRE_STUDY_JIT_COMPILE 0
#endif

#endif

#ifdef CONFIG_LIBCONFIG
#include <libgen.h>
#include <libconfig.h>
#endif

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrandr.h>

#ifdef CONFIG_VSYNC_DRM
#include <fcntl.h>
// We references some definitions in drm.h, which could also be found in
// /usr/src/linux/include/drm/drm.h, but that path is probably even less
// reliable than libdrm
#include <libdrm/drm.h>
#include <sys/ioctl.h>
#include <errno.h>
#endif

#ifdef CONFIG_VSYNC_OPENGL
#include <GL/glx.h>
#endif

// === Constants ===
#if !(COMPOSITE_MAJOR > 0 || COMPOSITE_MINOR >= 2)
#error libXcomposite version unsupported
#endif

#define ROUNDED_PERCENT 0.05
#define ROUNDED_PIXELS  10

// For printing timestamps
#include <time.h>
extern struct timeval time_start;

#define OPAQUE 0xffffffff
#define REGISTER_PROP "_NET_WM_CM_S"

#define WINDOW_SOLID 0
#define WINDOW_TRANS 1
#define WINDOW_ARGB 2

#define FADE_DELTA_TOLERANCE 0.2
#define VSYNC_SW_TOLERANCE 1000

#define NS_PER_SEC 1000000000L
#define US_PER_SEC 1000000L
#define MS_PER_SEC 1000

// Window flags

// Window size is changed
#define WFLAG_SIZE_CHANGE   0x0001

/**
 * Types
 */

typedef uint32_t opacity_t;

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
} wintype;

typedef struct _ignore {
  struct _ignore *next;
  unsigned long sequence;
} ignore;

enum wincond_target {
  CONDTGT_NAME,
  CONDTGT_CLASSI,
  CONDTGT_CLASSG,
};

enum wincond_type {
  CONDTP_EXACT,
  CONDTP_ANYWHERE,
  CONDTP_FROMSTART,
  CONDTP_WILDCARD,
  CONDTP_REGEX_PCRE,
};

#define CONDF_IGNORECASE 0x0001

typedef struct _wincond {
  enum wincond_target target;
  enum wincond_type type;
  char *pattern;
#ifdef CONFIG_REGEX_PCRE
  pcre *regex_pcre;
  pcre_extra *regex_pcre_extra;
#endif
  int16_t flags;
  struct _wincond *next;
} wincond;

typedef struct _win {
  struct _win *next;
  Window id;
  Window client_win;
  Pixmap pixmap;
  XWindowAttributes a;
#if CAN_DO_USABLE
  Bool usable; /* mapped and all damaged at one point */
  XRectangle damage_bounds; /* bounds of damage */
#endif
  int mode;
  int damaged;
  Damage damage;
  Picture picture;
  XserverRegion border_size;
  XserverRegion extents;
  // Type of the window.
  wintype window_type;
  /// Whether the window is focused.
  Bool focused;
  Bool destroyed;
  /// Cached width/height of the window including border.
  int widthb, heightb;
  /// Whether the window is bounding-shaped.
  Bool bounding_shaped;
  /// Whether the window just have rounded corners.
  Bool rounded_corners;

  // Blacklist related members
  char *name;
  char *class_instance;
  char *class_general;
  wincond *cache_sblst;
  wincond *cache_fblst;

  // Opacity-related members
  /// Current window opacity.
  opacity_t opacity;
  /// Target window opacity.
  opacity_t opacity_tgt;
  /// Cached value of opacity window attribute.
  opacity_t opacity_prop;
  /// Cached value of opacity window attribute on client window. For
  /// broken window managers not transferring client window's
  /// _NET_WM_OPACITY value
  opacity_t opacity_prop_client;
  /// Alpha mask Picture to render window with opacity.
  Picture alpha_pict;

  // Fading-related members
  /// Do not fade if it's false. Change on window type change.
  /// Used by fading blacklist in the future.
  Bool fade;
  /// Callback to be called after fading completed.
  void (*fade_callback) (Display *dpy, struct _win *w);
  /// Whether fading is finished.
  Bool fade_fin;

  // Frame-opacity-related members
  /// Current window frame opacity. Affected by window opacity.
  double frame_opacity;
  /// Alpha mask Picture to render window frame with opacity.
  Picture frame_alpha_pict;
  /// Frame widths. Determined by client window attributes.
  unsigned int left_width, right_width, top_width, bottom_width;

  // Shadow-related members
  /// Whether a window has shadow. Affected by window type.
  Bool shadow;
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
  /// Picture to render shadow. Affected by window size.
  Picture shadow_pict;
  /// Alpha mask Picture to render shadow. Affected by shadow opacity.
  Picture shadow_alpha_pict;

  // Dim-related members
  /// Whether the window is to be dimmed.
  Bool dim;

  /// Window flags. Definitions above.
  int_fast16_t flags;

  unsigned long damage_sequence; /* sequence when damage was created */

  Bool need_configure;
  XConfigureEvent queue_configure;
  /// Region to be ignored when painting. Basically the region where
  /// higher opaque windows will paint upon. Depends on window frame
  /// opacity state, window geometry, window mapped/unmapped state,
  /// window mode, of this and all higher windows.
  XserverRegion reg_ignore;

  struct _win *prev_trans;
} win;

typedef enum _vsync_t {
  VSYNC_NONE,
  VSYNC_SW,
  VSYNC_DRM,
  VSYNC_OPENGL,
} vsync_t;

#ifdef CONFIG_VSYNC_OPENGL
typedef int (*f_WaitVideoSync) (int, int, unsigned *);
typedef int (*f_GetVideoSync) (unsigned *);
#endif

typedef struct _options {
  // General
  char *display;
  /// Whether to try to detect WM windows and mark them as focused.
  Bool mark_wmwin_focused;
  /// Whether to mark override-redirect windows as focused.
  Bool mark_ovredir_focused;
  /// Whether to fork to background.
  Bool fork_after_register;
  /// Whether to detect rounded corners.
  Bool detect_rounded_corners;
  /// Whether to work under synchronized mode for debugging.
  Bool synchronize;

  // VSync
  /// User-specified refresh rate.
  int refresh_rate;
  /// VSync method to use;
  vsync_t vsync;

  // Shadow
  Bool wintype_shadow[NUM_WINTYPES];
  /// Red, green and blue tone of the shadow.
  double shadow_red, shadow_green, shadow_blue;
  int shadow_radius;
  int shadow_offset_x, shadow_offset_y;
  double shadow_opacity;
  Bool clear_shadow;
  /// Shadow blacklist. A linked list of conditions.
  wincond *shadow_blacklist;
  /// Whether bounding-shaped window should be ignored.
  Bool shadow_ignore_shaped;

  // Fading
  Bool wintype_fade[NUM_WINTYPES];
  /// How much to fade in in a single fading step.
  opacity_t fade_in_step;
  /// How much to fade out in a single fading step.
  opacity_t fade_out_step;
  unsigned long fade_delta;
  Bool no_fading_openclose;
  /// Fading blacklist. A linked list of conditions.
  wincond *fade_blacklist;

  // Opacity
  double wintype_opacity[NUM_WINTYPES];
  /// Default opacity for inactive windows.
  /// 32-bit integer with the format of _NET_WM_OPACITY. 0 stands for
  /// not enabled, default.
  opacity_t inactive_opacity;
  /// Whether inactive_opacity overrides the opacity set by window
  /// attributes.
  Bool inactive_opacity_override;
  /// Frame opacity. Relative to window opacity, also affects shadow
  /// opacity.
  double frame_opacity;
  /// Whether to detect _NET_WM_OPACITY on client windows. Used on window
  /// managers that don't pass _NET_WM_OPACITY to frame windows.
  Bool detect_client_opacity;
  /// How much to dim an inactive window. 0.0 - 1.0, 0 to disable.
  double inactive_dim;
  /// Step for pregenerating alpha pictures. 0.01 - 1.0.
  double alpha_step;

  // Calculated
  /// Whether compton needs to track focus changes.
  Bool track_focus;
  /// Whether compton needs to track window name and class.
  Bool track_wdata;

} options_t;

struct options_tmp {
  Bool no_dock_shadow;
  Bool no_dnd_shadow;
  double menu_opacity;
};

typedef struct _conv {
  int size;
  double *data;
} conv;

typedef enum {
  WIN_EVMODE_UNKNOWN,
  WIN_EVMODE_FRAME,
  WIN_EVMODE_CLIENT
} win_evmode_t;

extern int root_height, root_width;
extern Atom atom_client_attr;
extern Bool idling;
extern Bool shape_exists;

/**
 * Functions
 */

// inline functions must be made static to compile correctly under clang:
// http://clang.llvm.org/compatibility.html#inline

// Helper functions

static void
discard_ignore(Display *dpy, unsigned long sequence);

static void
set_ignore(Display *dpy, unsigned long sequence);

static int
should_ignore(Display *dpy, unsigned long sequence);

/**
 * Subtract two unsigned long values.
 *
 * Truncate to 0 if the result is negative.
 */
static inline unsigned long
sub_unslong(unsigned long a, unsigned long b) {
  return (a > b) ? a - b : 0;
}

/**
 * Set a Bool array of all wintypes to true.
 */
static void
wintype_arr_enable(Bool arr[]) {
  wintype i;

  for (i = 0; i < NUM_WINTYPES; ++i) {
    arr[i] = True;
  }
}

/**
 * Allocate the space and copy a string.
 */
static inline char *
mstrcpy(const char *src) {
  char *str = malloc(sizeof(char) * (strlen(src) + 1));

  strcpy(str, src);

  return str;
}

/**
 * Allocate the space and join two strings.
 */
static inline char *
mstrjoin(const char *src1, const char *src2) {
  char *str = malloc(sizeof(char) * (strlen(src1) + strlen(src2) + 1));

  strcpy(str, src1);
  strcat(str, src2);

  return str;
}

/**
 * Allocate the space and join two strings;
 */
static inline char *
mstrjoin3(const char *src1, const char *src2, const char *src3) {
  char *str = malloc(sizeof(char) * (strlen(src1) + strlen(src2)
        + strlen(src3) + 1));

  strcpy(str, src1);
  strcat(str, src2);
  strcat(str, src3);

  return str;
}

/**
 * Normalize an int value to a specific range.
 *
 * @param i int value to normalize
 * @param min minimal value
 * @param max maximum value
 * @return normalized value
 */
static inline int
normalize_i_range(int i, int min, int max) {
  if (i > max) return max;
  if (i < min) return min;
  return i;
}

/**
 * Select the larger integer of two.
 */
static inline int
max_i(int a, int b) {
  return (a > b ? a : b);
}

/**
 * Select the smaller integer of two.
 */
static inline int
min_i(int a, int b) {
  return (a > b ? b : a);
}

/**
 * Normalize a double value to a specific range.
 *
 * @param d double value to normalize
 * @param min minimal value
 * @param max maximum value
 * @return normalized value
 */
static inline double
normalize_d_range(double d, double min, double max) {
  if (d > max) return max;
  if (d < min) return min;
  return d;
}

/**
 * Normalize a double value to 0.\ 0 - 1.\ 0.
 *
 * @param d double value to normalize
 * @return normalized value
 */
static inline double
normalize_d(double d) {
  return normalize_d_range(d, 0.0, 1.0);
}

/**
 * Check if a window ID exists in an array of window IDs.
 *
 * @param arr the array of window IDs
 * @param count amount of elements in the array
 * @param wid window ID to search for
 */
static inline Bool
array_wid_exists(const Window *arr, int count, Window wid) {
  while (count--) {
    if (arr[count] == wid) {
      return True;
    }
  }

  return False;
}

/*
 * Subtracting two struct timeval values.
 *
 * Taken from glibc manual.
 *
 * Subtract the `struct timeval' values X and Y,
 * storing the result in RESULT.
 * Return 1 if the difference is negative, otherwise 0. */
static int
timeval_subtract(struct timeval *result,
                 struct timeval *x,
                 struct timeval *y) {
  /* Perform the carry for the later subtraction by updating y. */
  if (x->tv_usec < y->tv_usec) {
    int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
    y->tv_usec -= 1000000 * nsec;
    y->tv_sec += nsec;
  }

  if (x->tv_usec - y->tv_usec > 1000000) {
    int nsec = (x->tv_usec - y->tv_usec) / 1000000;
    y->tv_usec += 1000000 * nsec;
    y->tv_sec -= nsec;
  }

  /* Compute the time remaining to wait.
     tv_usec is certainly positive. */
  result->tv_sec = x->tv_sec - y->tv_sec;
  result->tv_usec = x->tv_usec - y->tv_usec;

  /* Return 1 if result is negative. */
  return x->tv_sec < y->tv_sec;
}

/*
 * Subtracting two struct timespec values.
 *
 * Taken from glibc manual.
 *
 * Subtract the `struct timespec' values X and Y,
 * storing the result in RESULT.
 * Return 1 if the difference is negative, otherwise 0.
 */
static inline int
timespec_subtract(struct timespec *result,
                 struct timespec *x,
                 struct timespec *y) {
  /* Perform the carry for the later subtraction by updating y. */
  if (x->tv_nsec < y->tv_nsec) {
    int nsec = (y->tv_nsec - x->tv_nsec) / NS_PER_SEC + 1;
    y->tv_nsec -= NS_PER_SEC * nsec;
    y->tv_sec += nsec;
  }

  if (x->tv_nsec - y->tv_nsec > NS_PER_SEC) {
    int nsec = (x->tv_nsec - y->tv_nsec) / NS_PER_SEC;
    y->tv_nsec += NS_PER_SEC * nsec;
    y->tv_sec -= nsec;
  }

  /* Compute the time remaining to wait.
     tv_nsec is certainly positive. */
  result->tv_sec = x->tv_sec - y->tv_sec;
  result->tv_nsec = x->tv_nsec - y->tv_nsec;

  /* Return 1 if result is negative. */
  return x->tv_sec < y->tv_sec;
}

/**
 * Print time passed since program starts execution.
 *
 * Used for debugging.
 */
static inline void
print_timestamp(void) {
  struct timeval tm, diff;

  if (gettimeofday(&tm, NULL)) return;

  timeval_subtract(&diff, &tm, &time_start);
  printf("[ %5ld.%02ld ] ", diff.tv_sec, diff.tv_usec / 10000);
}

/**
 * Destroy a <code>XserverRegion</code>.
 */
inline static void
free_region(Display *dpy, XserverRegion *p) {
  if (*p) {
    XFixesDestroyRegion(dpy, *p);
    *p = None;
  }
}

/**
 * Destroy a <code>Picture</code>.
 */
inline static void
free_picture(Display *dpy, Picture *p) {
  if (*p) {
    XRenderFreePicture(dpy, *p);
    *p = None;
  }
}

/**
 * Destroy a <code>Pixmap</code>.
 */
inline static void
free_pixmap(Display *dpy, Pixmap *p) {
  if (*p) {
    XFreePixmap(dpy, *p);
    *p = None;
  }
}

/**
 * Destroy a <code>Damage</code>.
 */
inline static void
free_damage(Display *dpy, Damage *p) {
  if (*p) {
    // BadDamage will be thrown if the window is destroyed
    set_ignore(dpy, NextRequest(dpy));
    XDamageDestroy(dpy, *p);
    *p = None;
  }
}

static unsigned long
get_time_ms(void);

static int
fade_timeout(void);

static void
run_fade(Display *dpy, win *w, unsigned steps);

static void
set_fade_callback(Display *dpy, win *w,
    void (*callback) (Display *dpy, win *w), Bool exec_callback);

/**
 * Execute fade callback of a window if fading finished.
 */
static inline void
check_fade_fin(Display *dpy, win *w) {
  if (w->fade_fin) {
    w->fade_fin = False;
    // Must be the last line as the callback could destroy w!
    set_fade_callback(dpy, w, NULL, True);
  }
}

static void
set_fade_callback(Display *dpy, win *w,
    void (*callback) (Display *dpy, win *w), Bool exec_callback);

static double
gaussian(double r, double x, double y);

static conv *
make_gaussian_map(Display *dpy, double r);

static unsigned char
sum_gaussian(conv *map, double opacity,
             int x, int y, int width, int height);

static void
presum_gaussian(conv *map);

static XImage *
make_shadow(Display *dpy, double opacity,
            int width, int height);

static Picture
shadow_picture(Display *dpy, double opacity, int width, int height);

static Picture
solid_picture(Display *dpy, Bool argb, double a,
              double r, double g, double b);

static inline bool is_normal_win(const win *w) {
  return (WINTYPE_NORMAL == w->window_type
      || WINTYPE_UTILITY == w->window_type);
}

/**
 * Determine if a window has a specific attribute.
 *
 * @param dpy Display to use
 * @param w window to check
 * @param atom atom of attribute to check
 * @return 1 if it has the attribute, 0 otherwise
 */
static inline Bool
wid_has_attr(Display *dpy, Window w, Atom atom) {
  Atom type = None;
  int format;
  unsigned long nitems, after;
  unsigned char *data;

  if (Success == XGetWindowProperty(dpy, w, atom, 0, 0, False,
        AnyPropertyType, &type, &format, &nitems, &after, &data)) {
    XFree(data);
    if (type) return True;
  }

  return False;
}

/**
 * Get the children of a window.
 *
 * @param dpy Display to use
 * @param w window to check
 * @param children [out] an array of child window IDs
 * @param nchildren [out] number of children
 * @return 1 if successful, 0 otherwise
 */
static inline Bool
wid_get_children(Display *dpy, Window w,
    Window **children, unsigned *nchildren) {
  Window troot, tparent;

  if (!XQueryTree(dpy, w, &troot, &tparent, children, nchildren)) {
    *nchildren = 0;
    return False;
  }

  return True;
}

/**
 * Check if a window is bounding-shaped.
 */
static inline Bool
wid_bounding_shaped(Display *dpy, Window wid) {
  if (shape_exists) {
    Bool bounding_shaped = False;
    Bool clip_shaped;
    int x_bounding, y_bounding, x_clip, y_clip;
    unsigned int w_bounding, h_bounding, w_clip, h_clip;

    XShapeQueryExtents(dpy, wid, &bounding_shaped,
        &x_bounding, &y_bounding, &w_bounding, &h_bounding,
        &clip_shaped, &x_clip, &y_clip, &w_clip, &h_clip);
    return bounding_shaped;
  }
  
  return False;
}

static void
win_rounded_corners(Display *dpy, win *w);

static bool
win_match_once(win *w, const wincond *cond);

static bool
win_match(win *w, wincond *condlst, wincond * *cache);

static Bool
condlst_add(wincond **pcondlst, const char *pattern);

static long
determine_evmask(Display *dpy, Window wid, win_evmode_t mode);

static win *
find_win(Display *dpy, Window id);

static win *
find_toplevel(Display *dpy, Window id);

static win *
find_toplevel2(Display *dpy, Window wid);

static win *
recheck_focus(Display *dpy);

static Picture
root_tile_f(Display *dpy);

static void
paint_root(Display *dpy);

static XserverRegion
win_extents(Display *dpy, win *w);

static XserverRegion
border_size(Display *dpy, win *w);

static Window
find_client_win(Display *dpy, Window w);

static void
get_frame_extents(Display *dpy, win *w, Window client);

static win *
paint_preprocess(Display *dpy, win *list);

static void
paint_all(Display *dpy, XserverRegion region, win *t);

static void
add_damage(Display *dpy, XserverRegion damage);

static void
repair_win(Display *dpy, win *w);

static wintype
get_wintype_prop(Display * dpy, Window w);

static void
map_win(Display *dpy, Window id,
        unsigned long sequence, Bool fade,
        Bool override_redirect);

static void
finish_map_win(Display *dpy, win *w);

static void
finish_unmap_win(Display *dpy, win *w);

static void
unmap_callback(Display *dpy, win *w);

static void
unmap_win(Display *dpy, Window id, Bool fade);

static opacity_t
wid_get_opacity_prop(Display *dpy, Window wid, opacity_t def);

static double
get_opacity_percent(Display *dpy, win *w);

static void
determine_mode(Display *dpy, win *w);

static void
calc_opacity(Display *dpy, win *w, Bool refetch_prop);

static void
calc_dim(Display *dpy, win *w);

static inline void
set_focused(Display *dpy, win *w, Bool focused) {
  w->focused = focused;
  calc_opacity(dpy, w, False);
  calc_dim(dpy, w);
}

static void
determine_fade(Display *dpy, win *w);

static void
determine_shadow(Display *dpy, win *w);

static void
calc_win_size(Display *dpy, win *w);

static void
calc_shadow_geometry(Display *dpy, win *w);

static void
mark_client_win(Display *dpy, win *w, Window client);

static void
add_win(Display *dpy, Window id, Window prev, Bool override_redirect);

static void
restack_win(Display *dpy, win *w, Window new_above);

static void
configure_win(Display *dpy, XConfigureEvent *ce);

static void
circulate_win(Display *dpy, XCirculateEvent *ce);

static void
finish_destroy_win(Display *dpy, Window id);

static void
destroy_callback(Display *dpy, win *w);

static void
destroy_win(Display *dpy, Window id, Bool fade);

static void
damage_win(Display *dpy, XDamageNotifyEvent *de);

static int
error(Display *dpy, XErrorEvent *ev);

static void
expose_root(Display *dpy, Window root, XRectangle *rects, int nrects);

static Bool
wid_get_text_prop(Display *dpy, Window wid, Atom prop,
    char ***pstrlst, int *pnstr);

static Bool
wid_get_name(Display *dpy, Window w, char **name);

static int
win_get_name(Display *dpy, win *w);

static Bool
win_get_class(Display *dpy, win *w);

#ifdef DEBUG_EVENTS
static int
ev_serial(XEvent *ev);

static char *
ev_name(XEvent *ev);

static Window
ev_window(XEvent *ev);
#endif

static void
usage(void);

static void
register_cm(Bool want_glxct);

inline static void
ev_focus_in(XFocusChangeEvent *ev);

inline static void
ev_focus_out(XFocusChangeEvent *ev);

inline static void
ev_create_notify(XCreateWindowEvent *ev);

inline static void
ev_configure_notify(XConfigureEvent *ev);

inline static void
ev_destroy_notify(XDestroyWindowEvent *ev);

inline static void
ev_map_notify(XMapEvent *ev);

inline static void
ev_unmap_notify(XUnmapEvent *ev);

inline static void
ev_reparent_notify(XReparentEvent *ev);

inline static void
ev_circulate_notify(XCirculateEvent *ev);

inline static void
ev_expose(XExposeEvent *ev);

inline static void
ev_property_notify(XPropertyEvent *ev);

inline static void
ev_damage_notify(XDamageNotifyEvent *ev);

inline static void
ev_shape_notify(XShapeEvent *ev);

/**
 * Get a region of the screen size.
 */
inline static XserverRegion
get_screen_region(Display *dpy) {
  XRectangle r;

  r.x = 0;
  r.y = 0;
  r.width = root_width;
  r.height = root_height;
  return XFixesCreateRegion(dpy, &r, 1);
}

/**
 * Copies a region
 */
inline static XserverRegion
copy_region(Display *dpy, XserverRegion oldregion) {
  XserverRegion region = XFixesCreateRegion(dpy, NULL, 0);

  XFixesCopyRegion(dpy, region, oldregion);

  return region;
}

/**
 * Dump a region.
 */
static inline void
dump_region(Display *dpy, XserverRegion region) {
  int nrects = 0, i;
  XRectangle *rects = XFixesFetchRegion(dpy, region, &nrects);
  if (!rects)
    return;

  for (i = 0; i < nrects; ++i)
    printf("Rect #%d: %8d, %8d, %8d, %8d\n", i, rects[i].x, rects[i].y,
        rects[i].width, rects[i].height);

  XFree(rects);
}

/**
 * Check if a region is empty.
 *
 * Keith Packard said this is slow:
 * http://lists.freedesktop.org/archives/xorg/2007-November/030467.html
 */
static inline Bool
is_region_empty(Display *dpy, XserverRegion region) {
  int nrects = 0;
  XRectangle *rects = XFixesFetchRegion(dpy, region, &nrects);

  XFree(rects);

  return !nrects;
}

/**
 * Add a window to damaged area.
 *
 * @param dpy display in use
 * @param w struct _win element representing the window
 */
static inline void
add_damage_win(Display *dpy, win *w) {
  if (w->extents) {
    add_damage(dpy, copy_region(dpy, w->extents));
  }
}

inline static void
ev_handle(XEvent *ev);

static void
fork_after(void);

#ifdef CONFIG_LIBCONFIG
static inline void
lcfg_lookup_bool(const config_t *config, const char *path, Bool *value) {
  int ival;

  if (config_lookup_bool(config, path, &ival))
    *value = ival;
}

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
parse_config(char *cpath, struct options_tmp *pcfgtmp);
#endif

static void
get_cfg(int argc, char *const *argv);

static void
get_atoms(void);

static void
update_refresh_rate(Display *dpy);

static Bool
vsync_sw_init(void);

static struct timespec
vsync_sw_ntimeout(int timeout);

static Bool
vsync_drm_init(void);

#ifdef CONFIG_VSYNC_DRM
static int
vsync_drm_wait(void);
#endif

static Bool
vsync_opengl_init(void);

#ifdef CONFIG_VSYNC_OPENGL
static void
vsync_opengl_wait(void);
#endif

static Bool
vsync_wait(Display *dpy, struct pollfd *fd, int timeout);

static void
init_alpha_picts(Display *dpy);
