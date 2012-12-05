/**
 * compton.h
 */

// Throw everything in here.

// === Options ===

// Debug options, enable them using -D in CFLAGS
// #define DEBUG_REPAINT    1
// #define DEBUG_EVENTS     1
// #define DEBUG_RESTACK    1
// #define DEBUG_WINTYPE    1
// #define DEBUG_CLIENTWIN  1
// #define DEBUG_WINDATA    1
// #define DEBUG_WINMATCH   1
// #define DEBUG_REDIR      1
// #define DEBUG_ALLOC_REG  1
// #define DEBUG_FRAME      1
// #define MONITOR_REPAINT  1

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
#include <signal.h>

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
#include <X11/extensions/Xdbe.h>
#include <time.h>

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

#define OPAQUE 0xffffffff
#define REGISTER_PROP "_NET_WM_CM_S"

#define FADE_DELTA_TOLERANCE 0.2
#define SW_OPTI_TOLERANCE 1000

#define NS_PER_SEC 1000000000L
#define US_PER_SEC 1000000L
#define MS_PER_SEC 1000

// Window flags

// Window size is changed
#define WFLAG_SIZE_CHANGE   0x0001
// Window size/position is changed
#define WFLAG_POS_CHANGE    0x0002
// Window opacity / dim state changed
#define WFLAG_OPCT_CHANGE   0x0004

// === Macros ===

// #define MSTR_(s)        #s
// #define MSTR(s)         MSTR_(s)

#define printf_dbg(format, ...) \
  printf(format, ## __VA_ARGS__); \
  fflush(stdout)

#define printf_dbgf(format, ...) \
  printf_dbg("%s" format, __func__, ## __VA_ARGS__)

// Use #s here to prevent macro expansion
/// Macro used for shortening some debugging code.
#define CASESTRRET(s)   case s: return #s

// === Types ===

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
} wintype_t;

typedef enum {
  WINDOW_SOLID,
  WINDOW_TRANS,
  WINDOW_ARGB
} winmode;

typedef struct {
  // All pointers have the same length, right?
  // I wanted to use anonymous union but it's a GNU extension...
  union {
    unsigned char *p8;
    short *p16;
    long *p32;
  } data;
  unsigned long nitems;
} winprop_t;

typedef struct _ignore {
  struct _ignore *next;
  unsigned long sequence;
} ignore_t;

enum wincond_target {
  CONDTGT_NAME,
  CONDTGT_CLASSI,
  CONDTGT_CLASSG,
  CONDTGT_ROLE,
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
} wincond_t;

/// VSync modes.
typedef enum {
  VSYNC_NONE,
  VSYNC_DRM,
  VSYNC_OPENGL,
} vsync_t;

#ifdef CONFIG_VSYNC_OPENGL
typedef int (*f_WaitVideoSync) (int, int, unsigned *);
typedef int (*f_GetVideoSync) (unsigned *);
#endif

typedef struct {
  int size;
  double *data;
} conv;

struct _win;

/// Structure representing all options.
typedef struct {
  // === General ===
  char *display;
  /// Whether to try to detect WM windows and mark them as focused.
  bool mark_wmwin_focused;
  /// Whether to mark override-redirect windows as focused.
  bool mark_ovredir_focused;
  /// Whether to fork to background.
  bool fork_after_register;
  /// Whether to detect rounded corners.
  bool detect_rounded_corners;
  /// Whether to paint on X Composite overlay window instead of root
  /// window.
  bool paint_on_overlay;
  /// Whether to unredirect all windows if a full-screen opaque window
  /// is detected.
  bool unredir_if_possible;
  /// Whether to work under synchronized mode for debugging.
  bool synchronize;

  // === VSync & software optimization ===
  /// User-specified refresh rate.
  int refresh_rate;
  /// Whether to enable refresh-rate-based software optimization.
  bool sw_opti;
  /// VSync method to use;
  vsync_t vsync;
  /// Whether to enable double buffer.
  bool dbe;
  /// Whether to do VSync aggressively.
  bool vsync_aggressive;

  // === Shadow ===
  bool wintype_shadow[NUM_WINTYPES];
  /// Red, green and blue tone of the shadow.
  double shadow_red, shadow_green, shadow_blue;
  int shadow_radius;
  int shadow_offset_x, shadow_offset_y;
  double shadow_opacity;
  bool clear_shadow;
  /// Shadow blacklist. A linked list of conditions.
  wincond_t *shadow_blacklist;
  /// Whether bounding-shaped window should be ignored.
  bool shadow_ignore_shaped;
  /// Whether to respect _COMPTON_SHADOW.
  bool respect_prop_shadow;

  // === Fading ===
  bool wintype_fade[NUM_WINTYPES];
  /// How much to fade in in a single fading step.
  opacity_t fade_in_step;
  /// How much to fade out in a single fading step.
  opacity_t fade_out_step;
  unsigned long fade_delta;
  /// Whether to disable fading on window open/close.
  bool no_fading_openclose;
  /// Fading blacklist. A linked list of conditions.
  wincond_t *fade_blacklist;

  // === Opacity ===
  double wintype_opacity[NUM_WINTYPES];
  /// Default opacity for inactive windows.
  /// 32-bit integer with the format of _NET_WM_OPACITY. 0 stands for
  /// not enabled, default.
  opacity_t inactive_opacity;
  /// Whether inactive_opacity overrides the opacity set by window
  /// attributes.
  bool inactive_opacity_override;
  /// Frame opacity. Relative to window opacity, also affects shadow
  /// opacity.
  double frame_opacity;
  /// Whether to detect _NET_WM_OPACITY on client windows. Used on window
  /// managers that don't pass _NET_WM_OPACITY to frame windows.
  bool detect_client_opacity;
  /// How much to dim an inactive window. 0.0 - 1.0, 0 to disable.
  double inactive_dim;
  /// Whether to use fixed inactive dim opacity, instead of deciding
  /// based on window opacity.
  bool inactive_dim_fixed;
  /// Step for pregenerating alpha pictures. 0.01 - 1.0.
  double alpha_step;

  // === Focus related ===
  /// Whether to use EWMH _NET_ACTIVE_WINDOW to find active window.
  bool use_ewmh_active_win;
  /// A list of windows always to be considered focused.
  wincond_t *focus_blacklist;

  // === Calculated ===
  /// Whether compton needs to track focus changes.
  bool track_focus;
  /// Whether compton needs to track window name and class.
  bool track_wdata;

} options_t;

/// Structure containing all necessary data for a compton session.
typedef struct {
  // === Display related ===
  /// Display in use.
  Display *dpy;
  /// Default screen.
  int scr;
  /// Default visual.
  Visual *vis;
  /// Default depth.
  int depth;
  /// Root window.
  Window root;
  /// Height of root window.
  int root_height;
  /// Width of root window.
  int root_width;
  // Damage of root window.
  // Damage root_damage;
  /// X Composite overlay window. Used if <code>--paint-on-overlay</code>.
  Window overlay;
  /// Picture of the root window background.
  Picture root_tile;
  /// A region of the size of the screen.
  XserverRegion screen_reg;
  /// Picture of root window. Destination of painting in no-DBE painting
  /// mode.
  Picture root_picture;
  /// A Picture acting as the painting target.
  Picture tgt_picture;
  /// Temporary buffer to paint to before sending to display.
  Picture tgt_buffer;
  /// DBE back buffer for root window. Used in DBE painting mode.
  XdbeBackBuffer root_dbe;
  /// Window ID of the window we register as a symbol.
  Window reg_win;

  // === Operation related ===
  /// Program options.
  options_t o;
  /// Program start time.
  struct timeval time_start;
  /// The region needs to painted on next paint.
  XserverRegion all_damage;
  /// Whether all windows are currently redirected.
  bool redirected;
  /// Whether there's a highest full-screen window, and all windows could
  /// be unredirected.
  bool unredir_possible;
  /// Pre-generated alpha pictures.
  Picture *alpha_picts;
  /// Whether all reg_ignore of windows should expire in this paint.
  bool reg_ignore_expire;
  /// Whether the program is idling. I.e. no fading, no potential window
  /// changes.
  bool idling;
  /// Time of last fading. In milliseconds.
  unsigned long fade_time;
  /// Head pointer of the error ignore linked list.
  ignore_t *ignore_head;
  /// Pointer to the <code>next</code> member of tail element of the error
  /// ignore linked list.
  ignore_t **ignore_tail;
  /// Reset program after next paint.
  bool reset;

  // === Expose event related ===
  /// Pointer to an array of <code>XRectangle</code>-s of exposed region.
  XRectangle *expose_rects;
  /// Number of <code>XRectangle</code>-s in <code>expose_rects</code>.
  int size_expose;
  /// Index of the next free slot in <code>expose_rects</code>.
  int n_expose;

  // === Window related ===
  /// Linked list of all windows.
  struct _win *list;
  /// Pointer to <code>win</code> of current active window. Used by
  /// EWMH <code>_NET_ACTIVE_WINDOW</code> focus detection. In theory,
  /// it's more reliable to store the window ID directly here, just in
  /// case the WM does something extraordinary, but caching the pointer
  /// means another layer of complexity.
  struct _win *active_win;

  // === Shadow/dimming related ===
  /// 1x1 black Picture.
  Picture black_picture;
  /// 1x1 Picture of the shadow color.
  Picture cshadow_picture;
  /// Gaussian map of shadow.
  conv *gaussian_map;
  // for shadow precomputation
  /// Shadow depth on one side.
  int cgsize;
  /// Pre-computed color table for corners of shadow.
  unsigned char *shadow_corner;
  /// Pre-computed color table for a side of shadow.
  unsigned char *shadow_top;

  // === Software-optimization-related ===
  /// Currently used refresh rate.
  short refresh_rate;
  /// Interval between refresh in nanoseconds.
  long refresh_intv;
  /// Nanosecond offset of the first painting.
  long paint_tm_offset;

  #ifdef CONFIG_VSYNC_DRM
  // === DRM VSync related ===
  /// File descriptor of DRI device file. Used for DRM VSync.
  int drm_fd;
  #endif

  #ifdef CONFIG_VSYNC_OPENGL
  // === OpenGL VSync related ===
  /// GLX context.
  GLXContext glx_context;
  /// Pointer to glXGetVideoSyncSGI function.
  f_GetVideoSync glx_get_video_sync;
  /// Pointer to glXWaitVideoSyncSGI function.
  f_WaitVideoSync glx_wait_video_sync;
  #endif

  // === X extension related ===
  /// Event base number for X Fixes extension.
  int xfixes_event;
  /// Error base number for X Fixes extension.
  int xfixes_error;
  /// Event base number for X Damage extension.
  int damage_event;
  /// Error base number for X Damage extension.
  int damage_error;
  /// Event base number for X Render extension.
  int render_event;
  /// Error base number for X Render extension.
  int render_error;
  /// Event base number for X Composite extension.
  int composite_event;
  /// Error base number for X Composite extension.
  int composite_error;
  /// Major opcode for X Composite extension.
  int composite_opcode;
  /// Whether X Composite NameWindowPixmap is available. Aka if X
  /// Composite version >= 0.2.
  bool has_name_pixmap;
  /// Whether X Shape extension exists.
  bool shape_exists;
  /// Event base number for X Shape extension.
  int shape_event;
  /// Error base number for X Shape extension.
  int shape_error;
  /// Whether X RandR extension exists.
  bool randr_exists;
  /// Event base number for X RandR extension.
  int randr_event;
  /// Error base number for X RandR extension.
  int randr_error;
  #ifdef CONFIG_VSYNC_OPENGL
  /// Whether X GLX extension exists.
  bool glx_exists;
  /// Event base number for X GLX extension.
  int glx_event;
  /// Error base number for X GLX extension.
  int glx_error;
  #endif
  /// Whether X DBE extension exists.
  bool dbe_exists;

  // === Atoms ===
  /// Atom of property <code>_NET_WM_OPACITY</code>.
  Atom atom_opacity;
  /// Atom of <code>_NET_FRAME_EXTENTS</code>.
  Atom atom_frame_extents;
  /// Property atom to identify top-level frame window. Currently
  /// <code>WM_STATE</code>.
  Atom atom_client;
  /// Atom of property <code>WM_NAME</code>.
  Atom atom_name;
  /// Atom of property <code>_NET_WM_NAME</code>.
  Atom atom_name_ewmh;
  /// Atom of property <code>WM_CLASS</code>.
  Atom atom_class;
  /// Atom of property <code>WM_WINDOW_ROLE</code>.
  Atom atom_role;
  /// Atom of property <code>WM_TRANSIENT_FOR</code>.
  Atom atom_transient;
  /// Atom of property <code>_NET_ACTIVE_WINDOW</code>.
  Atom atom_ewmh_active_win;
  /// Atom of property <code>_COMPTON_SHADOW</code>.
  Atom atom_compton_shadow;
  /// Atom of property <code>_NET_WM_WINDOW_TYPE</code>.
  Atom atom_win_type;
  /// Array of atoms of all possible window types.
  Atom atoms_wintypes[NUM_WINTYPES];
} session_t;

/// Structure representing a top-level window compton manages.
typedef struct _win {
  // Next structure in the linked list.
  struct _win *next;

  // ID of the top-level frame window.
  Window id;
  /// ID of the top-level client window of the window.
  Window client_win;
  /// Whether it looks like a WM window. We consider a window WM window if
  /// it does not have a decedent with WM_STATE and it is not override-
  /// redirected itself.
  bool wmwin;
  Pixmap pixmap;
  XWindowAttributes a;
  winmode mode;
  int damaged;
  Damage damage;
  Picture picture;
  XserverRegion border_size;
  XserverRegion extents;
  // Type of the window.
  wintype_t window_type;
  /// Whether the window is to be considered focused.
  bool focused;
  /// Whether the window is actually focused.
  bool focused_real;
  /// Leader window ID of the window.
  Window leader;
  /// Whether the window has been destroyed.
  bool destroyed;
  /// Cached width/height of the window including border.
  int widthb, heightb;
  /// Whether the window is bounding-shaped.
  bool bounding_shaped;
  /// Whether the window just have rounded corners.
  bool rounded_corners;
  /// Whether this window is to be painted.
  bool to_paint;

  // Blacklist related members
  /// Name of the window.
  char *name;
  /// Window instance class of the window.
  char *class_instance;
  /// Window general class of the window.
  char *class_general;
  /// <code>WM_WINDOW_ROLE</code> value of the window.
  char *role;
  wincond_t *cache_sblst;
  wincond_t *cache_fblst;
  wincond_t *cache_fcblst;

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
  bool fade;
  /// Callback to be called after fading completed.
  void (*fade_callback) (session_t *ps, struct _win *w);

  // Frame-opacity-related members
  /// Current window frame opacity. Affected by window opacity.
  double frame_opacity;
  /// Alpha mask Picture to render window frame with opacity.
  Picture frame_alpha_pict;
  /// Frame widths. Determined by client window attributes.
  unsigned int left_width, right_width, top_width, bottom_width;

  // Shadow-related members
  /// Whether a window has shadow. Affected by window type.
  bool shadow;
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
  /// The value of _COMPTON_SHADOW attribute of the window. Below 0 for
  /// none.
  long attr_shadow;

  // Dim-related members
  /// Whether the window is to be dimmed.
  bool dim;
  /// Picture for dimming. Affected by user-specified inactive dim
  /// opacity and window opacity.
  Picture dim_alpha_pict;

  /// Window flags. Definitions above.
  int_fast16_t flags;

  unsigned long damage_sequence; /* sequence when damage was created */

  /// Whether there's a pending <code>ConfigureNotify</code> happening
  /// when the window is unmapped.
  bool need_configure;
  /// Queued <code>ConfigureNotify</code> when the window is unmapped.
  XConfigureEvent queue_configure;
  /// Region to be ignored when painting. Basically the region where
  /// higher opaque windows will paint upon. Depends on window frame
  /// opacity state, window geometry, window mapped/unmapped state,
  /// window mode, of this and all higher windows.
  XserverRegion reg_ignore;

  struct _win *prev_trans;
} win;

/// Temporary structure used for communication between
/// <code>get_cfg()</code> and <code>parse_config()</code>.
struct options_tmp {
  bool no_dock_shadow;
  bool no_dnd_shadow;
  double menu_opacity;
};

typedef enum {
  WIN_EVMODE_UNKNOWN,
  WIN_EVMODE_FRAME,
  WIN_EVMODE_CLIENT
} win_evmode_t;

extern const char *WINTYPES[NUM_WINTYPES];
extern session_t *ps_g;

// == Debugging code ==
static void
print_timestamp(session_t *ps);

#ifdef DEBUG_ALLOC_REG

#include <execinfo.h>
#define BACKTRACE_SIZE  5

/**
 * Print current backtrace, excluding the first two items.
 *
 * Stolen from glibc manual.
 */
static inline void
print_backtrace(void) {
  void *array[BACKTRACE_SIZE];
  size_t size;
  char **strings;

  size = backtrace(array, BACKTRACE_SIZE);
  strings = backtrace_symbols(array, size);

  for (size_t i = 2; i < size; i++)
     printf ("%s\n", strings[i]);

  free(strings);
}

/**
 * Wrapper of <code>XFixesCreateRegion</code>, for debugging.
 */
static inline XserverRegion
XFixesCreateRegion_(Display *dpy, XRectangle *p, int n,
    const char *func, int line) {
  XserverRegion reg = XFixesCreateRegion(dpy, p, n);
  print_timestamp(ps_g);
  printf("%#010lx: XFixesCreateRegion() in %s():%d\n", reg, func, line);
  print_backtrace();
  fflush(stdout);
  return reg;
}

/**
 * Wrapper of <code>XFixesDestroyRegion</code>, for debugging.
 */
static inline void
XFixesDestroyRegion_(Display *dpy, XserverRegion reg,
    const char *func, int line) {
  XFixesDestroyRegion(dpy, reg);
  print_timestamp(ps_g);
  printf("%#010lx: XFixesDestroyRegion() in %s():%d\n", reg, func, line);
  fflush(stdout);
}

#define XFixesCreateRegion(dpy, p, n) XFixesCreateRegion_(dpy, p, n, __func__, __LINE__)
#define XFixesDestroyRegion(dpy, reg) XFixesDestroyRegion_(dpy, reg, __func__, __LINE__)
#endif

// == Functions ==

// inline functions must be made static to compile correctly under clang:
// http://clang.llvm.org/compatibility.html#inline

// Helper functions

static void
discard_ignore(session_t *ps, unsigned long sequence);

static void
set_ignore(session_t *ps, unsigned long sequence);

static inline void
set_ignore_next(session_t *ps) {
  set_ignore(ps, NextRequest(ps->dpy));
}

static int
should_ignore(session_t *ps, unsigned long sequence);

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
static void
wintype_arr_enable(bool arr[]) {
  wintype_t i;

  for (i = 0; i < NUM_WINTYPES; ++i) {
    arr[i] = true;
  }
}

/**
 * Allocate the space and copy a string.
 */
static inline char * __attribute__((const))
mstrcpy(const char *src) {
  char *str = malloc(sizeof(char) * (strlen(src) + 1));

  strcpy(str, src);

  return str;
}

/**
 * Allocate the space and join two strings.
 */
static inline char * __attribute__((const))
mstrjoin(const char *src1, const char *src2) {
  char *str = malloc(sizeof(char) * (strlen(src1) + strlen(src2) + 1));

  strcpy(str, src1);
  strcat(str, src2);

  return str;
}

/**
 * Allocate the space and join two strings;
 */
static inline char * __attribute__((const))
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
static inline int __attribute__((const))
normalize_i_range(int i, int min, int max) {
  if (i > max) return max;
  if (i < min) return min;
  return i;
}

/**
 * Select the larger integer of two.
 */
static inline int __attribute__((const))
max_i(int a, int b) {
  return (a > b ? a : b);
}

/**
 * Select the smaller integer of two.
 */
static inline int __attribute__((const))
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
static inline double __attribute__((const))
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
static inline double __attribute__((const))
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
static inline bool
array_wid_exists(const Window *arr, int count, Window wid) {
  while (count--) {
    if (arr[count] == wid) {
      return true;
    }
  }

  return false;
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
    long nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
    y->tv_usec -= 1000000 * nsec;
    y->tv_sec += nsec;
  }

  if (x->tv_usec - y->tv_usec > 1000000) {
    long nsec = (x->tv_usec - y->tv_usec) / 1000000;
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
    long nsec = (y->tv_nsec - x->tv_nsec) / NS_PER_SEC + 1;
    y->tv_nsec -= NS_PER_SEC * nsec;
    y->tv_sec += nsec;
  }

  if (x->tv_nsec - y->tv_nsec > NS_PER_SEC) {
    long nsec = (x->tv_nsec - y->tv_nsec) / NS_PER_SEC;
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
 * Get current time in struct timespec.
 *
 * Note its starting time is unspecified.
 */
static inline struct timespec __attribute__((const))
get_time_timespec(void) {
  struct timespec tm = { 0, 0 };

  clock_gettime(CLOCK_MONOTONIC, &tm);

  // Return a time of all 0 if the call fails
  return tm;
}

/**
 * Print time passed since program starts execution.
 *
 * Used for debugging.
 */
static void
print_timestamp(session_t *ps) {
  struct timeval tm, diff;

  if (gettimeofday(&tm, NULL)) return;

  timeval_subtract(&diff, &tm, &ps->time_start);
  printf("[ %5ld.%02ld ] ", diff.tv_sec, diff.tv_usec / 10000);
}

/**
 * Destroy a <code>XserverRegion</code>.
 */
inline static void
free_region(session_t *ps, XserverRegion *p) {
  if (*p) {
    XFixesDestroyRegion(ps->dpy, *p);
    *p = None;
  }
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
 * Destroy a <code>wincond_t</code>.
 */
inline static void
free_wincond(wincond_t *cond) {
  if (cond->pattern)
    free(cond->pattern);
#ifdef CONFIG_REGEX_PCRE
  if (cond->regex_pcre_extra)
    pcre_free_study(cond->regex_pcre_extra);
  if (cond->regex_pcre)
    pcre_free(cond->regex_pcre);
#endif
  free(cond);
}

/**
 * Destroy a linked list of <code>wincond_t</code>.
 */
inline static void
free_wincondlst(wincond_t **cond_lst) {
  wincond_t *next = NULL;

  for (wincond_t *cond = *cond_lst; cond; cond = next) {
    next = cond->next;

    free_wincond(cond);
  }

  *cond_lst = NULL;
}

/**
 * Destroy all resources in a <code>struct _win</code>.
 */
inline static void
free_win_res(session_t *ps, win *w) {
  free_region(ps, &w->extents);
  free_pixmap(ps, &w->pixmap);
  free_picture(ps, &w->picture);
  free_region(ps, &w->border_size);
  free_picture(ps, &w->shadow_pict);
  free_damage(ps, &w->damage);
  free_region(ps, &w->reg_ignore);
  free(w->name);
  free(w->class_instance);
  free(w->class_general);
}

/**
 * Get current system clock in milliseconds.
 *
 * The return type must be unsigned long because so many milliseconds have
 * passed since the epoch.
 */
static unsigned long
get_time_ms(void) {
  struct timeval tv;

  gettimeofday(&tv, NULL);

  return (unsigned long) tv.tv_sec * 1000
    + (unsigned long) tv.tv_usec / 1000;
}

static int
fade_timeout(session_t *ps);

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

static Picture
shadow_picture(session_t *ps, double opacity, int width, int height);

static Picture
solid_picture(session_t *ps, bool argb, double a,
              double r, double g, double b);

static inline bool is_normal_win(const win *w) {
  return (WINTYPE_NORMAL == w->window_type
      || WINTYPE_UTILITY == w->window_type);
}

/**
 * Determine if a window has a specific attribute.
 *
 * @param session_t current session
 * @param w window to check
 * @param atom atom of attribute to check
 * @return 1 if it has the attribute, 0 otherwise
 */
static inline bool
wid_has_attr(const session_t *ps, Window w, Atom atom) {
  Atom type = None;
  int format;
  unsigned long nitems, after;
  unsigned char *data;

  if (Success == XGetWindowProperty(ps->dpy, w, atom, 0, 0, False,
        AnyPropertyType, &type, &format, &nitems, &after, &data)) {
    XFree(data);
    if (type) return true;
  }

  return false;
}

/**
 * Get a specific attribute of a window.
 *
 * Returns a blank structure if the returned type and format does not
 * match the requested type and format.
 *
 * @param session_t current session
 * @param w window
 * @param atom atom of attribute to fetch
 * @param length length to read
 * @param rtype atom of the requested type
 * @param rformat requested format
 * @return a <code>winprop_t</code> structure containing the attribute
 *    and number of items. A blank one on failure.
 */
static winprop_t
wid_get_prop(const session_t *ps, Window w, Atom atom, long length,
    Atom rtype, int rformat) {
  Atom type = None;
  int format = 0;
  unsigned long nitems = 0, after = 0;
  unsigned char *data = NULL;

  // Use two if statements to deal with the sequence point issue.
  if (Success == XGetWindowProperty(ps->dpy, w, atom, 0L, length, False,
        rtype, &type, &format, &nitems, &after, &data)) {
    if (type == rtype && format == rformat) {
      return (winprop_t) {
        .data.p8 = data,
        .nitems = nitems
      };
    }
  }

  XFree(data);

  return (winprop_t) {
    .data.p8 = NULL,
    .nitems = 0
  };
}

/**
 * Free a <code>winprop_t</code>.
 *
 * @param pprop pointer to the <code>winprop_t</code> to free.
 */
static inline void
free_winprop(winprop_t *pprop) {
  // Empty the whole structure to avoid possible issues
  if (pprop->data.p8) {
    XFree(pprop->data.p8);
    pprop->data.p8 = NULL;
  }
  pprop->nitems = 0;
}

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
 * @param session_t current session
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
  if (w->to_paint && WINDOW_SOLID == w->mode)
    ps->reg_ignore_expire = true;
}

/**
 * Check whether a window has WM frames.
 */
static inline bool __attribute__((const))
win_has_frame(const win *w) {
  return w->top_width || w->left_width || w->right_width
    || w->bottom_width;
}

/**
 * Check if a window is a fullscreen window.
 *
 * It's not using w->border_size for performance measures.
 */
static inline bool
win_is_fullscreen(session_t *ps, const win *w) {
  return (w->a.x <= 0 && w->a.y <= 0
      && (w->a.x + w->widthb) >= ps->root_width
      && (w->a.y + w->heightb) >= ps->root_height
      && !w->bounding_shaped);
}

static void
win_rounded_corners(session_t *ps, win *w);

static bool
win_match_once(win *w, const wincond_t *cond);

static bool
win_match(win *w, wincond_t *condlst, wincond_t * *cache);

static bool
condlst_add(wincond_t **pcondlst, const char *pattern);

static long
determine_evmask(session_t *ps, Window wid, win_evmode_t mode);

static win *
find_win(session_t *ps, Window id);

static win *
find_toplevel(session_t *ps, Window id);

static win *
find_toplevel2(session_t *ps, Window wid);

static win *
recheck_focus(session_t *ps);

static Picture
root_tile_f(session_t *ps);

static void
paint_root(session_t *ps, Picture tgt_buffer);

static XserverRegion
win_extents(session_t *ps, win *w);

static XserverRegion
border_size(session_t *ps, win *w);

static Window
find_client_win(session_t *ps, Window w);

static void
get_frame_extents(session_t *ps, win *w, Window client);

static win *
paint_preprocess(session_t *ps, win *list);

static void
paint_all(session_t *ps, XserverRegion region, win *t);

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
unmap_win(session_t *ps, Window id);

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
determine_mode(session_t *ps, win *w);

static void
calc_opacity(session_t *ps, win *w);

static void
calc_dim(session_t *ps, win *w);

/**
 * Update focused state of a window.
 */
static inline void
win_update_focused(session_t *ps, win *w) {
  bool focused_old = w->focused;

  w->focused = w->focused_real;

  // Treat WM windows and override-redirected windows specially
  if ((ps->o.mark_wmwin_focused && w->wmwin)
      || (ps->o.mark_ovredir_focused
        && w->id == w->client_win && !w->wmwin)
      || win_match(w, ps->o.focus_blacklist, &w->cache_fcblst))
    w->focused = true;

  if (w->focused != focused_old)
    w->flags |= WFLAG_OPCT_CHANGE;
}

/**
 * Set real focused state of a window.
 */
static inline void
win_set_focused(session_t *ps, win *w, bool focused) {
  w->focused_real = focused;
  win_update_focused(ps, w);
}

static void
determine_fade(session_t *ps, win *w);

static void
win_update_shape_raw(session_t *ps, win *w);

static void
win_update_shape(session_t *ps, win *w);

static void
win_update_attr_shadow_raw(session_t *ps, win *w);

static void
win_update_attr_shadow(session_t *ps, win *w);

static void
determine_shadow(session_t *ps, win *w);

static void
calc_win_size(session_t *ps, win *w);

static void
calc_shadow_geometry(session_t *ps, win *w);

static void
mark_client_win(session_t *ps, win *w, Window client);

static void
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
error(Display *dpy, XErrorEvent *ev);

static void
expose_root(session_t *ps, XRectangle *rects, int nrects);

static bool
wid_get_text_prop(session_t *ps, Window wid, Atom prop,
    char ***pstrlst, int *pnstr);

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
usage(void);

static void
register_cm(session_t *ps, bool want_glxct);

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
 * Copies a region
 */
inline static XserverRegion
copy_region(const session_t *ps, XserverRegion oldregion) {
  XserverRegion region = XFixesCreateRegion(ps->dpy, NULL, 0);

  XFixesCopyRegion(ps->dpy, region, oldregion);

  return region;
}

/**
 * Dump a region.
 */
static inline void
dump_region(const session_t *ps, XserverRegion region) {
  int nrects = 0, i;
  XRectangle *rects = XFixesFetchRegion(ps->dpy, region, &nrects);
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
 *
 * @param ps current session
 * @param region region to check for
 */
static inline bool
is_region_empty(const session_t *ps, XserverRegion region) {
  int nrects = 0;
  XRectangle *rects = XFixesFetchRegion(ps->dpy, region, &nrects);

  XFree(rects);

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

static void
fork_after(void);

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
parse_cfg_condlst(const config_t *pcfg, wincond_t **pcondlst,
    const char *name);

static void
parse_config(session_t *ps, char *cpath, struct options_tmp *pcfgtmp);
#endif

static void
get_cfg(session_t *ps, int argc, char *const *argv);

static void
init_atoms(session_t *ps);

static void
update_refresh_rate(session_t *ps);

static bool
sw_opti_init(session_t *ps);

static int
evpoll(session_t *ps, int timeout);

static bool
vsync_drm_init(session_t *ps);

#ifdef CONFIG_VSYNC_DRM
static int
vsync_drm_wait(session_t *ps);
#endif

static bool
vsync_opengl_init(session_t *ps);

#ifdef CONFIG_VSYNC_OPENGL
static void
vsync_opengl_wait(session_t *ps);
#endif

static void
vsync_wait(session_t *ps);

static void
init_alpha_picts(session_t *ps);

static void
init_dbe(session_t *ps);

static void
init_overlay(session_t *ps);

static void
redir_start(session_t *ps);

static void
redir_stop(session_t *ps);

static session_t *
session_init(session_t *ps_old, int argc, char **argv);

static void
session_destroy(session_t *ps);

static void
session_run(session_t *ps);

static void
reset_enable(int __attribute__((unused)) signum);
