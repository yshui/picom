/**
 * compton.h
 */

// Throw everything in here.

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

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

#if COMPOSITE_MAJOR > 0 || COMPOSITE_MINOR >= 2
#define HAS_NAME_WINDOW_PIXMAP 1
#endif

#define CAN_DO_USABLE 0

// Debug options, enable them using -D in CFLAGS
// #define DEBUG_REPAINT 1
// #define DEBUG_EVENTS 1
// #define DEBUG_RESTACK 1
// #define DEBUG_WINTYPE 1
// #define MONITOR_REPAINT 1

// For printing timestamps
#include <time.h>
extern struct timeval time_start;

#define OPAQUE 0xffffffff
#define REGISTER_PROP "_NET_WM_CM_S"

#define WINDOW_SOLID 0
#define WINDOW_TRANS 1
#define WINDOW_ARGB 2

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

typedef struct _win {
  struct _win *next;
  Window id;
  Window client_win;
#if HAS_NAME_WINDOW_PIXMAP
  Pixmap pixmap;
#endif
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
  Bool shadow;
  Picture shadow_pict;
  int shadow_dx;
  int shadow_dy;
  int shadow_width;
  int shadow_height;

  /// Current window opacity.
  opacity_t opacity;
  /// Opacity of current alpha_pict.
  opacity_t opacity_cur;
  /// Cached value of opacity window attribute.
  opacity_t opacity_prop;
  /// Alpha mask Picture to render window with opacity.
  Picture alpha_pict;

  /// Current window frame opacity.
  double frame_opacity;
  /// Opacity of current frame_alpha_pict.
  opacity_t frame_opacity_cur;
  /// Alpha mask Picture to render window frame with opacity.
  Picture frame_alpha_pict;

  /// Whether the window is to be dimmed.
  Bool dim;
  wintype window_type;
  /// Whether the window is focused.
  Bool focused;
  unsigned long damage_sequence; /* sequence when damage was created */
  Bool destroyed;
  unsigned int left_width;
  unsigned int right_width;
  unsigned int top_width;
  unsigned int bottom_width;

  Bool need_configure;
  XConfigureEvent queue_configure;

  struct _win *prev_trans;
} win;

typedef struct _conv {
  int size;
  double *data;
} conv;

typedef struct _fade {
  struct _fade *next;
  win *w;
  double cur;
  double finish;
  double step;
  void (*callback) (Display *dpy, win *w);
  Display *dpy;
} fade;

typedef enum {
  WIN_EVMODE_UNKNOWN,
  WIN_EVMODE_FRAME,
  WIN_EVMODE_CLIENT
} win_evmode_t;

extern int root_height, root_width;
extern Atom atom_client_attr;

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
 * Normalize an int value to a specific range.
 *
 * @param i int value to normalize
 * @param min minimal value
 * @param max maximum value
 * @return normalized value
 */
static inline double
normalize_i_range(int i, int min, int max) {
  if (i > max) return max;
  if (i < min) return min;
  return i;
}

/**
 * Normalize a double value to 0.\ 0 - 1.\ 0.
 *
 * @param d double value to normalize
 */
static inline double
normalize_d(double d) {
  if (d > 1.0) return 1.0;
  if (d < 0.0) return 0.0;
  return d;
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

/**
 * Determine if a window has a specific attribute.
 *
 * @param dpy Display to use
 * @param w window to check
 * @param atom atom of attribute to check
 * @return 1 if it has the attribute, 0 otherwise
 */
static inline Bool
win_has_attr(Display *dpy, Window w, Atom atom) {
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
win_get_children(Display *dpy, Window w,
    Window **children, unsigned *nchildren) {
  Window troot, tparent;

  if (!XQueryTree(dpy, w, &troot, &tparent, children, nchildren)) {
    *nchildren = 0;
    return False;
  }

  return True;
}

static int
get_time_in_milliseconds(void);

static fade *
find_fade(win *w);

static void
dequeue_fade(Display *dpy, fade *f);

static void
cleanup_fade(Display *dpy, win *w);

static void
enqueue_fade(Display *dpy, fade *f);

static void
set_fade(Display *dpy, win *w, double start,
         double finish, double step,
         void(*callback) (Display *dpy, win *w),
         Bool exec_callback, Bool override);

static int
fade_timeout(void);

static void
run_fades(Display *dpy);

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
shadow_picture(Display *dpy, double opacity,
               int width, int height, int *wp, int *hp);

static Picture
solid_picture(Display *dpy, Bool argb, double a,
              double r, double g, double b);

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
get_frame_extents(Display *dpy, Window w,
                  unsigned int *left,
                  unsigned int *right,
                  unsigned int *top,
                  unsigned int *bottom);

static void
paint_all(Display *dpy, XserverRegion region);

static void
add_damage(Display *dpy, XserverRegion damage);

static void
repair_win(Display *dpy, win *w);

static wintype
get_wintype_prop(Display * dpy, Window w);

static wintype
determine_wintype(Display *dpy, Window w, Window top);

static void
map_win(Display *dpy, Window id,
        unsigned long sequence, Bool fade,
        Bool override_redirect);

static void
finish_unmap_win(Display *dpy, win *w);

#if HAS_NAME_WINDOW_PIXMAP
static void
unmap_callback(Display *dpy, win *w);
#endif

static void
unmap_win(Display *dpy, Window id, Bool fade);

static opacity_t
get_opacity_prop(Display *dpy, win *w, opacity_t def);

static double
get_opacity_percent(Display *dpy, win *w);

static void
determine_mode(Display *dpy, win *w);

static void
set_opacity(Display *dpy, win *w, opacity_t opacity);

static void
calc_opacity(Display *dpy, win *w, Bool refetch_prop);

static void
calc_dim(Display *dpy, win *w);

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

#if HAS_NAME_WINDOW_PIXMAP
static void
destroy_callback(Display *dpy, win *w);
#endif

static void
destroy_win(Display *dpy, Window id, Bool fade);

static void
damage_win(Display *dpy, XDamageNotifyEvent *de);

static int
error(Display *dpy, XErrorEvent *ev);

static void
expose_root(Display *dpy, Window root, XRectangle *rects, int nrects);

#if defined(DEBUG_EVENTS) || defined(DEBUG_RESTACK)
static int
window_get_name(Window w, char **name);
#endif

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
register_cm(int scr);

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

static void
get_atoms(void);
