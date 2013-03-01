/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE for more information.
 *
 */

#ifndef COMPTON_COMMON_H
#define COMPTON_COMMON_H

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
// #define DEBUG_LEADER     1
// #define DEBUG_C2         1
// #define MONITOR_REPAINT  1

// Whether to enable PCRE regular expression support in blacklists, enabled
// by default
// #define CONFIG_REGEX_PCRE 1
// Whether to enable JIT support of libpcre. This may cause problems on PaX
// kernels.
// #define CONFIG_REGEX_PCRE_JIT 1
// Whether to enable parsing of configuration files using libconfig.
// #define CONFIG_LIBCONFIG 1
// Whether we are using a legacy version of libconfig (1.3.x).
// #define CONFIG_LIBCONFIG_LEGACY 1
// Whether to enable DRM VSync support
// #define CONFIG_VSYNC_DRM 1
// Whether to enable OpenGL VSync support
// #define CONFIG_VSYNC_OPENGL 1
// Whether to enable DBus support with libdbus.
// #define CONFIG_DBUS 1
// Whether to enable condition support.
// #define CONFIG_C2 1

#if !defined(CONFIG_C2) && defined(DEBUG_C2)
#error Cannot enable c2 debugging without c2 support.
#endif

// === Includes ===

// For some special functions
#define _GNU_SOURCE

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/poll.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xdbe.h>

// libconfig
#ifdef CONFIG_LIBCONFIG
#include <libgen.h>
#include <libconfig.h>
#endif

// libdbus
#ifdef CONFIG_DBUS
#include <dbus/dbus.h>
#endif

// libGL
#ifdef CONFIG_VSYNC_OPENGL
#include <GL/glx.h>
#endif

// === Macros ===

#define MSTR_(s)        #s
#define MSTR(s)         MSTR_(s)

/// Print out an error message.
#define printf_err(format, ...) \
  fprintf(stderr, format "\n", ## __VA_ARGS__)

/// Print out an error message with function name.
#define printf_errf(format, ...) \
  printf_err("%s" format,  __func__, ## __VA_ARGS__)

/// Print out an error message with function name, and quit with a
/// specific exit code.
#define printf_errfq(code, format, ...) { \
  printf_err("%s" format,  __func__, ## __VA_ARGS__); \
  exit(code); \
}

/// Print out a debug message.
#define printf_dbg(format, ...) \
  printf(format, ## __VA_ARGS__); \
  fflush(stdout)

/// Print out a debug message with function name.
#define printf_dbgf(format, ...) \
  printf_dbg("%s" format, __func__, ## __VA_ARGS__)

// Use #s here to prevent macro expansion
/// Macro used for shortening some debugging code.
#define CASESTRRET(s)   case s: return #s

// === Constants ===
#if !(COMPOSITE_MAJOR > 0 || COMPOSITE_MINOR >= 2)
#error libXcomposite version unsupported
#endif

#define ROUNDED_PERCENT 0.05
#define ROUNDED_PIXELS  10

#define OPAQUE 0xffffffff
#define REGISTER_PROP "_NET_WM_CM_S"

#define TIME_MS_MAX LONG_MAX
#define FADE_DELTA_TOLERANCE 0.2
#define SWOPTI_TOLERANCE 3000
#define TIMEOUT_RUN_TOLERANCE 0.05
#define WIN_GET_LEADER_MAX_RECURSION 20

#define SEC_WRAP (15L * 24L * 60L * 60L)

#define NS_PER_SEC 1000000000L
#define US_PER_SEC 1000000L
#define MS_PER_SEC 1000

#define XRFILTER_CONVOLUTION  "convolution"
#define XRFILTER_GUASSIAN     "gaussian"
#define XRFILTER_BINOMIAL     "binomial"

// Window flags

// Window size is changed
#define WFLAG_SIZE_CHANGE   0x0001
// Window size/position is changed
#define WFLAG_POS_CHANGE    0x0002
// Window opacity / dim state changed
#define WFLAG_OPCT_CHANGE   0x0004

// === Types ===

typedef uint32_t opacity_t;
typedef long time_ms_t;

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

/// Enumeration type to represent switches.
typedef enum {
  OFF,    // false
  ON,     // true
  UNSET
} switch_t;

/// Enumeration type of window painting mode.
typedef enum {
  WMODE_TRANS,
  WMODE_SOLID,
  WMODE_ARGB
} winmode_t;

/// Structure representing needed window updates.
typedef struct {
  bool shadow       : 1;
  bool fade         : 1;
  bool focus        : 1;
  bool invert_color : 1;
} win_upd_t;

/// Structure representing Window property value.
typedef struct {
  // All pointers have the same length, right?
  // I wanted to use anonymous union but it's a GNU extension...
  union {
    unsigned char *p8;
    short *p16;
    long *p32;
  } data;
  unsigned long nitems;
  Atom type;
  int format;
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

/// VSync modes.
typedef enum {
  VSYNC_NONE,
  VSYNC_DRM,
  VSYNC_OPENGL,
  VSYNC_OPENGL_OML,
  NUM_VSYNC,
} vsync_t;

#ifdef CONFIG_VSYNC_OPENGL
typedef int (*f_WaitVideoSync) (int, int, unsigned *);
typedef int (*f_GetVideoSync) (unsigned *);

typedef Bool (*f_GetSyncValuesOML) (Display* dpy, GLXDrawable drawable, int64_t* ust, int64_t* msc, int64_t* sbc);
typedef Bool (*f_WaitForMscOML) (Display* dpy, GLXDrawable drawable, int64_t target_msc, int64_t divisor, int64_t remainder, int64_t* ust, int64_t* msc, int64_t* sbc);

typedef void (*f_BindTexImageEXT) (Display *display, GLXDrawable drawable, int buffer, const int *attrib_list);
typedef void (*f_ReleaseTexImageEXT) (Display *display, GLXDrawable drawable, int buffer);

struct glx_fbconfig {
  GLXFBConfig cfg;
  bool y_inverted;
};
#endif

typedef struct {
  int size;
  double *data;
} conv;

/// Linked list type of atoms.
typedef struct _latom {
  Atom atom;
  struct _latom *next;
} latom_t;

struct _timeout_t;

struct _win;

typedef struct _c2_lptr c2_lptr_t;

/// Structure representing all options.
typedef struct {
  // === General ===
  /// The configuration file we used.
  char *config_file;
  /// The display name we used. NULL means we are using the value of the
  /// <code>DISPLAY</code> environment variable.
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
  /// Whether to enable D-Bus support.
  bool dbus;
  /// Path to log file.
  char *logpath;
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
  /// Enable/disable shadow for specific window types.
  bool wintype_shadow[NUM_WINTYPES];
  /// Red, green and blue tone of the shadow.
  double shadow_red, shadow_green, shadow_blue;
  int shadow_radius;
  int shadow_offset_x, shadow_offset_y;
  double shadow_opacity;
  bool clear_shadow;
  /// Shadow blacklist. A linked list of conditions.
  c2_lptr_t *shadow_blacklist;
  /// Whether bounding-shaped window should be ignored.
  bool shadow_ignore_shaped;
  /// Whether to respect _COMPTON_SHADOW.
  bool respect_prop_shadow;

  // === Fading ===
  /// Enable/disable fading for specific window types.
  bool wintype_fade[NUM_WINTYPES];
  /// How much to fade in in a single fading step.
  opacity_t fade_in_step;
  /// How much to fade out in a single fading step.
  opacity_t fade_out_step;
  /// Fading time delta. In milliseconds.
  time_ms_t fade_delta;
  /// Whether to disable fading on window open/close.
  bool no_fading_openclose;
  /// Fading blacklist. A linked list of conditions.
  c2_lptr_t *fade_blacklist;

  // === Opacity ===
  /// Default opacity for specific window types
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
  /// Step for pregenerating alpha pictures. 0.01 - 1.0.
  double alpha_step;

  // === Other window processing ===
  /// Whether to blur background of semi-transparent / ARGB windows.
  bool blur_background;
  /// Whether to blur background when the window frame is not opaque.
  /// Implies blur_background.
  bool blur_background_frame;
  /// Whether to use fixed blur strength instead of adjusting according
  /// to window opacity.
  bool blur_background_fixed;
  /// How much to dim an inactive window. 0.0 - 1.0, 0 to disable.
  double inactive_dim;
  /// Whether to use fixed inactive dim opacity, instead of deciding
  /// based on window opacity.
  bool inactive_dim_fixed;
  /// Conditions of windows to have inverted colors.
  c2_lptr_t *invert_color_list;

  // === Focus related ===
  /// Consider windows of specific types to be always focused.
  bool wintype_focus[NUM_WINTYPES];
  /// Whether to use EWMH _NET_ACTIVE_WINDOW to find active window.
  bool use_ewmh_active_win;
  /// A list of windows always to be considered focused.
  c2_lptr_t *focus_blacklist;
  /// Whether to do window grouping with <code>WM_TRANSIENT_FOR</code>.
  bool detect_transient;
  /// Whether to do window grouping with <code>WM_CLIENT_LEADER</code>.
  bool detect_client_leader;

  // === Calculated ===
  /// Whether compton needs to track focus changes.
  bool track_focus;
  /// Whether compton needs to track window name and class.
  bool track_wdata;
  /// Whether compton needs to track window leaders.
  bool track_leader;
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
  /// File descriptors to check for reading.
  fd_set *pfds_read;
  /// File descriptors to check for writing.
  fd_set *pfds_write;
  /// File descriptors to check for exceptions.
  fd_set *pfds_except;
  /// Largest file descriptor in fd_set-s above.
  int nfds_max;
  /// Linked list of all timeouts.
  struct _timeout_t *tmout_lst;
  /// Whether we have received an event in this cycle.
  bool ev_received;
  /// Whether the program is idling. I.e. no fading, no potential window
  /// changes.
  bool idling;
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
  /// Time of last fading. In milliseconds.
  time_ms_t fade_time;
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
  /// Window ID of leader window of currently active window. Used for
  /// subsidiary window detection.
  Window active_leader;

  // === Shadow/dimming related ===
  /// 1x1 black Picture.
  Picture black_picture;
  /// 1x1 Picture of the shadow color.
  Picture cshadow_picture;
  /// 1x1 white Picture.
  Picture white_picture;
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
  // === OpenGL related ===
  /// GLX context.
  GLXContext glx_context;
  /// Pointer to glXGetVideoSyncSGI function.
  f_GetVideoSync glXGetVideoSyncSGI;
  /// Pointer to glXWaitVideoSyncSGI function.
  f_WaitVideoSync glXWaitVideoSyncSGI;
  /// Pointer to glXGetSyncValuesOML function.
  f_GetSyncValuesOML glXGetSyncValuesOML;
  /// Pointer to glXWaitForMscOML function.
  f_WaitForMscOML glXWaitForMscOML;
  /// Pointer to glXBindTexImageEXT function.
  f_BindTexImageEXT glXBindTexImageEXT;
  /// Pointer to glXReleaseTexImageEXT function.
  f_ReleaseTexImageEXT glXReleaseTexImageEXT;
  /// FBConfig for RGB GLX pixmap.
  struct glx_fbconfig *glx_fbconfig_rgb;
  /// FBConfig for RGBA GLX pixmap.
  struct glx_fbconfig *glx_fbconfig_rgba;
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
  /// Whether X Render convolution filter exists.
  bool xrfilter_convolution_exists;

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
  /// Atom of property <code>WM_CLIENT_LEADER</code>.
  Atom atom_client_leader;
  /// Atom of property <code>_NET_ACTIVE_WINDOW</code>.
  Atom atom_ewmh_active_win;
  /// Atom of property <code>_COMPTON_SHADOW</code>.
  Atom atom_compton_shadow;
  /// Atom of property <code>_NET_WM_WINDOW_TYPE</code>.
  Atom atom_win_type;
  /// Array of atoms of all possible window types.
  Atom atoms_wintypes[NUM_WINTYPES];
  /// Linked list of additional atoms to track.
  latom_t *track_atom_lst;

#ifdef CONFIG_DBUS
  // === DBus related ===
  // DBus connection.
  DBusConnection *dbus_conn;
  // DBus service name.
  char *dbus_service;
#endif
} session_t;

/// Structure representing a top-level window compton manages.
typedef struct _win {
  /// Pointer to the next structure in the linked list.
  struct _win *next;
  /// Pointer to the next higher window to paint.
  struct _win *prev_trans;

  // Core members
  /// ID of the top-level frame window.
  Window id;
  /// Window attributes.
  XWindowAttributes a;
  /// Window painting mode.
  winmode_t mode;
  /// Whether the window has been damaged at least once.
  bool damaged;
  /// Damage of the window.
  Damage damage;
  /// NameWindowPixmap of the window.
  Pixmap pixmap;
  /// Picture of the window.
  Picture picture;
  /// Bounding shape of the window.
  XserverRegion border_size;
  /// Region of the whole window, shadow region included.
  XserverRegion extents;
  /// Window flags. Definitions above.
  int_fast16_t flags;
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
  /// Cached width/height of the window including border.
  int widthb, heightb;
  /// Whether the window has been destroyed.
  bool destroyed;
  /// Whether the window is bounding-shaped.
  bool bounding_shaped;
  /// Whether the window just have rounded corners.
  bool rounded_corners;
  /// Whether this window is to be painted.
  bool to_paint;
  /// Whether this window is in open/close state.
  bool in_openclose;

  // Client window related members
  /// ID of the top-level client window of the window.
  Window client_win;
  /// Type of the window.
  wintype_t window_type;
  /// Whether it looks like a WM window. We consider a window WM window if
  /// it does not have a decedent with WM_STATE and it is not override-
  /// redirected itself.
  bool wmwin;
  /// Leader window ID of the window.
  Window leader;
  /// Cached topmost window ID of the window.
  Window cache_leader;

  // Focus-related members
  /// Whether the window is to be considered focused.
  bool focused;
  /// Override value of window focus state. Set by D-Bus method calls.
  switch_t focused_force;
  /// Whether the window is actually focused.
  bool focused_real;

  // Blacklist related members
  /// Name of the window.
  char *name;
  /// Window instance class of the window.
  char *class_instance;
  /// Window general class of the window.
  char *class_general;
  /// <code>WM_WINDOW_ROLE</code> value of the window.
  char *role;
  const c2_lptr_t *cache_sblst;
  const c2_lptr_t *cache_fblst;
  const c2_lptr_t *cache_fcblst;
  const c2_lptr_t *cache_ivclst;

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
  /// Whether a window has shadow. Calculated.
  bool shadow;
  /// Override value of window shadow state. Set by D-Bus method calls.
  switch_t shadow_force;
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
  long prop_shadow;

  // Dim-related members
  /// Whether the window is to be dimmed.
  bool dim;
  /// Picture for dimming. Affected by user-specified inactive dim
  /// opacity and window opacity.
  Picture dim_alpha_pict;

  /// Whether to invert window color.
  bool invert_color;
  /// Override value of window color inversion state. Set by D-Bus method
  /// calls.
  switch_t invert_color_force;
} win;

/// Temporary structure used for communication between
/// <code>get_cfg()</code> and <code>parse_config()</code>.
struct options_tmp {
  bool no_dock_shadow;
  bool no_dnd_shadow;
  double menu_opacity;
};

/// Structure for a recorded timeout.
typedef struct _timeout_t {
  bool enabled;
  void *data;
  bool (*callback)(session_t *ps, struct _timeout_t *ptmout);
  time_ms_t interval;
  time_ms_t firstrun;
  time_ms_t lastrun;
  struct _timeout_t *next;
} timeout_t;

/// Enumeration for window event hints.
typedef enum {
  WIN_EVMODE_UNKNOWN,
  WIN_EVMODE_FRAME,
  WIN_EVMODE_CLIENT
} win_evmode_t;

extern const char * const WINTYPES[NUM_WINTYPES];
extern const char * const VSYNC_STRS[NUM_VSYNC];
extern session_t *ps_g;

// == Debugging code ==
static inline void
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

// === Functions ===

/**
 * Return whether a struct timeval value is empty.
 */
static inline bool
timeval_isempty(struct timeval *ptv) {
  if (!ptv)
    return false;

  return ptv->tv_sec <= 0 && ptv->tv_usec <= 0;
}

/**
 * Compare a struct timeval with a time in milliseconds.
 *
 * @return > 0 if ptv > ms, 0 if ptv == 0, -1 if ptv < ms
 */
static inline int
timeval_ms_cmp(struct timeval *ptv, time_ms_t ms) {
  assert(ptv);

  // We use those if statement instead of a - expression because of possible
  // truncation problem from long to int.
  {
    long sec = ms / MS_PER_SEC;
    if (ptv->tv_sec > sec)
      return 1;
    if (ptv->tv_sec < sec)
      return -1;
  }

  {
    long usec = ms % MS_PER_SEC * (US_PER_SEC / MS_PER_SEC);
    if (ptv->tv_usec > usec)
      return 1;
    if (ptv->tv_usec < usec)
      return -1;
  }

  return 0;
}

/**
 * Subtracting two struct timeval values.
 *
 * Taken from glibc manual.
 *
 * Subtract the `struct timeval' values X and Y,
 * storing the result in RESULT.
 * Return 1 if the difference is negative, otherwise 0.
 */
static inline int
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

/**
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
 * Get current time in struct timeval.
 */
static inline struct timeval __attribute__((const))
get_time_timeval(void) {
  struct timeval tv = { 0, 0 };

  gettimeofday(&tv, NULL);

  // Return a time of all 0 if the call fails
  return tv;
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
static inline void
print_timestamp(session_t *ps) {
  struct timeval tm, diff;

  if (gettimeofday(&tm, NULL)) return;

  timeval_subtract(&diff, &tm, &ps->time_start);
  printf("[ %5ld.%02ld ] ", diff.tv_sec, diff.tv_usec / 10000);
}

/**
 * Allocate the space and copy a string.
 */
static inline char *
mstrcpy(const char *src) {
  char *str = malloc(sizeof(char) * (strlen(src) + 1));

  if (!str)
    printf_errfq(1, "(): Failed to allocate memory.");

  strcpy(str, src);

  return str;
}

/**
 * Allocate the space and copy a string.
 */
static inline char *
mstrncpy(const char *src, unsigned len) {
  char *str = malloc(sizeof(char) * (len + 1));

  if (!str)
    printf_errfq(1, "(): Failed to allocate memory.");

  strncpy(str, src, len);
  str[len] = '\0';

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
 * Select the larger long integer of two.
 */
static inline long __attribute__((const))
max_l(long a, long b) {
  return (a > b ? a : b);
}

/**
 * Select the smaller long integer of two.
 */
static inline long __attribute__((const))
min_l(long a, long b) {
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
 * Parse a VSync option argument.
 */
static inline bool
parse_vsync(session_t *ps, const char *str) {
  for (vsync_t i = 0; i < (sizeof(VSYNC_STRS) / sizeof(VSYNC_STRS[0])); ++i)
    if (!strcasecmp(str, VSYNC_STRS[i])) {
      ps->o.vsync = i;
      return true;
    }
  printf_errf("(\"%s\"): Invalid vsync argument.", str);
  return false;
}

timeout_t *
timeout_insert(session_t *ps, time_ms_t interval,
    bool (*callback)(session_t *ps, timeout_t *ptmout), void *data);

void
timeout_invoke(session_t *ps, timeout_t *ptmout);

bool
timeout_drop(session_t *ps, timeout_t *prm);

/**
 * Add a file descriptor to a select() fd_set.
 */
static inline bool
fds_insert_select(fd_set **ppfds, int fd) {
  assert(fd <= FD_SETSIZE);

  if (!*ppfds) {
    if ((*ppfds = malloc(sizeof(fd_set)))) {
      FD_ZERO(*ppfds);
    }
    else {
      fprintf(stderr, "Failed to allocate memory for select() fdset.\n");
      exit(1);
    }
  }

  FD_SET(fd, *ppfds);

  return true;
}

/**
 * Add a new file descriptor to wait for.
 */
static inline bool
fds_insert(session_t *ps, int fd, short events) {
  bool result = true;

  ps->nfds_max = max_i(fd + 1, ps->nfds_max);

  if (POLLIN & events)
    result = fds_insert_select(&ps->pfds_read, fd) && result;
  if (POLLOUT & events)
    result = fds_insert_select(&ps->pfds_write, fd) && result;
  if (POLLPRI & events)
    result = fds_insert_select(&ps->pfds_except, fd) && result;

  return result;
}

/**
 * Delete a file descriptor to wait for.
 */
static inline void
fds_drop(session_t *ps, int fd, short events) {
  // Drop fd from respective fd_set-s
  if (POLLIN & events && ps->pfds_read)
    FD_CLR(fd, ps->pfds_read);
  if (POLLOUT & events && ps->pfds_write)
    FD_CLR(fd, ps->pfds_write);
  if (POLLPRI & events && ps->pfds_except)
    FD_CLR(fd, ps->pfds_except);
}

#define CPY_FDS(key) \
  fd_set * key = NULL; \
  if (ps->key) { \
    key = malloc(sizeof(fd_set)); \
    memcpy(key, ps->key, sizeof(fd_set)); \
    if (!key) { \
      fprintf(stderr, "Failed to allocate memory for copying select() fdset.\n"); \
      exit(1); \
    } \
  } \

/**
 * Poll for changes.
 *
 * poll() is much better than select(), but ppoll() does not exist on
 * *BSD.
 */
static inline int
fds_poll(session_t *ps, struct timeval *ptv) {
  // Copy fds
  CPY_FDS(pfds_read);
  CPY_FDS(pfds_write);
  CPY_FDS(pfds_except);

  int ret = select(ps->nfds_max, pfds_read, pfds_write, pfds_except, ptv);

  free(pfds_read);
  free(pfds_write);
  free(pfds_except);

  return ret;
}
#undef CPY_FDS

/**
 * Wrapper of XInternAtom() for convenience.
 */
static inline Atom
get_atom(session_t *ps, const char *atom_name) {
  return XInternAtom(ps->dpy, atom_name, False);
}

/**
 * Find a window from window id in window linked list of the session.
 */
static inline win *
find_win(session_t *ps, Window id) {
  if (!id)
    return NULL;

  win *w;

  for (w = ps->list; w; w = w->next) {
    if (w->id == id && !w->destroyed)
      return w;
  }

  return 0;
}

/**
 * Find out the WM frame of a client window using existing data.
 *
 * @param id window ID
 * @return struct _win object of the found window, NULL if not found
 */
static inline win *
find_toplevel(session_t *ps, Window id) {
  if (!id)
    return NULL;

  for (win *w = ps->list; w; w = w->next) {
    if (w->client_win == id && !w->destroyed)
      return w;
  }

  return NULL;
}

/**
 * Find out the currently focused window.
 *
 * @return struct _win object of the found window, NULL if not found
 */
static inline win *
find_focused(session_t *ps) {
  if (!ps->o.track_focus)
    return NULL;

  for (win *w = ps->list; w; w = w->next) {
    if (w->focused_real && !w->destroyed)
      return w;
  }

  return NULL;
}

/**
 * Copies a region
 */
static inline XserverRegion
copy_region(const session_t *ps, XserverRegion oldregion) {
  XserverRegion region = XFixesCreateRegion(ps->dpy, NULL, 0);

  XFixesCopyRegion(ps->dpy, region, oldregion);

  return region;
}

/**
 * Determine if a window has a specific property.
 *
 * @param ps current session
 * @param w window to check
 * @param atom atom of property to check
 * @return 1 if it has the attribute, 0 otherwise
 */
static inline bool
wid_has_prop(const session_t *ps, Window w, Atom atom) {
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

winprop_t
wid_get_prop_adv(const session_t *ps, Window w, Atom atom, long offset,
    long length, Atom rtype, int rformat);

/**
 * Wrapper of wid_get_prop_adv().
 */
static inline winprop_t
wid_get_prop(const session_t *ps, Window wid, Atom atom, long length,
    Atom rtype, int rformat) {
  return wid_get_prop_adv(ps, wid, atom, 0L, length, rtype, rformat);
}

/**
 * Get the numeric property value from a win_prop_t.
 */
static inline long
winprop_get_int(winprop_t prop) {
  long tgt = 0;

  if (!prop.nitems)
    return 0;

  switch (prop.format) {
    case 8:   tgt = *(prop.data.p8);    break;
    case 16:  tgt = *(prop.data.p16);   break;
    case 32:  tgt = *(prop.data.p32);   break;
    default:  assert(0);
              break;
  }

  return tgt;
}

bool
wid_get_text_prop(session_t *ps, Window wid, Atom prop,
    char ***pstrlst, int *pnstr);

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

void
force_repaint(session_t *ps);

bool
vsync_init(session_t *ps);

#ifdef CONFIG_DBUS
/** @name DBus handling
 */
///@{
bool
cdbus_init(session_t *ps);

void
cdbus_destroy(session_t *ps);

void
cdbus_loop(session_t *ps);

void
cdbus_ev_win_added(session_t *ps, win *w);

void
cdbus_ev_win_destroyed(session_t *ps, win *w);

void
cdbus_ev_win_mapped(session_t *ps, win *w);

void
cdbus_ev_win_unmapped(session_t *ps, win *w);
//!@}

/** @name DBus hooks
 */
///@{
void
win_set_shadow_force(session_t *ps, win *w, switch_t val);

void
win_set_focused_force(session_t *ps, win *w, switch_t val);

void
win_set_invert_color_force(session_t *ps, win *w, switch_t val);
//!@}
#endif

#ifdef CONFIG_C2
/** @name c2
 */
///@{

c2_lptr_t *
c2_parse(session_t *ps, c2_lptr_t **pcondlst, const char *pattern);

c2_lptr_t *
c2_free_lptr(c2_lptr_t *lp);

bool
c2_match(session_t *ps, win *w, const c2_lptr_t *condlst,
    const c2_lptr_t **cache);
#endif

///@}

#endif
