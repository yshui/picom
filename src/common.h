// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * Copyright (c) 2018, Yuxuan Shui <yshuiv7@gmail.com>
 *
 * See LICENSE-mit for more information.
 *
 */

#pragma once

// === Options ===

// Debug options, enable them using -D in CFLAGS
// #define DEBUG_REPAINT    1
// #define DEBUG_EVENTS     1
// #define DEBUG_RESTACK    1
// #define DEBUG_WINMATCH   1
// #define DEBUG_C2         1
// #define DEBUG_GLX_DEBUG_CONTEXT        1

#define MAX_ALPHA (255)

// === Includes ===

// For some special functions
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <time.h>
#include <ctype.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <xcb/composite.h>
#include <xcb/render.h>
#include <xcb/damage.h>
#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/sync.h>

#ifdef CONFIG_XINERAMA
#include <xcb/xinerama.h>
#endif
#include <ev.h>
#include <pixman.h>

#ifdef CONFIG_OPENGL
// libGL
#include "backend/gl/glx.h"

// Workarounds for missing definitions in some broken GL drivers, thanks to
// douglasp and consolers for reporting
#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE 0x84F5
#endif

#ifndef GLX_BACK_BUFFER_AGE_EXT
#define GLX_BACK_BUFFER_AGE_EXT 0x20F4
#endif

#endif

// === Macros ===

#define MSTR_(s)        #s
#define MSTR(s)         MSTR_(s)

// X resource checker
#ifdef DEBUG_XRC
#include "xrescheck.h"
#endif

// FIXME This list of includes should get shorter
#include "types.h"
#include "win.h"
#include "region.h"
#include "kernel.h"
#include "render.h"
#include "config.h"
#include "log.h"
#include "compiler.h"
#include "utils.h"
#include "x.h"

// === Constants ===

/// @brief Length of generic buffers.
#define BUF_LEN 80

#define ROUNDED_PERCENT 0.05
#define ROUNDED_PIXELS  10

#define REGISTER_PROP "_NET_WM_CM_S"

#define TIME_MS_MAX LONG_MAX
#define FADE_DELTA_TOLERANCE 0.2
#define SWOPTI_TOLERANCE 3000
#define WIN_GET_LEADER_MAX_RECURSION 20

#define SEC_WRAP (15L * 24L * 60L * 60L)

#define NS_PER_SEC 1000000000L
#define US_PER_SEC 1000000L
#define MS_PER_SEC 1000

#define XRFILTER_CONVOLUTION  "convolution"
#define XRFILTER_GAUSSIAN     "gaussian"
#define XRFILTER_BINOMIAL     "binomial"

/// @brief Maximum OpenGL FBConfig depth.
#define OPENGL_MAX_DEPTH 32

/// @brief Maximum OpenGL buffer age.
#define CGLX_MAX_BUFFER_AGE 5

// Window flags

// Window size is changed
#define WFLAG_SIZE_CHANGE   0x0001
// Window size/position is changed
#define WFLAG_POS_CHANGE    0x0002
// Window opacity / dim state changed
#define WFLAG_OPCT_CHANGE   0x0004

// xcb-render specific macros
#define XFIXED_TO_DOUBLE(value) (((double) (value)) / 65536)
#define DOUBLE_TO_XFIXED(value) ((xcb_render_fixed_t) (((double) (value)) * 65536))

// === Types ===
typedef struct glx_fbconfig glx_fbconfig_t;

/// Structure representing needed window updates.
typedef struct {
  bool shadow       : 1;
  bool fade         : 1;
  bool focus        : 1;
  bool invert_color : 1;
} win_upd_t;

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

/// @brief Possible swap methods.
enum {
  SWAPM_BUFFER_AGE = -1,
  SWAPM_UNDEFINED = 0,
  SWAPM_COPY = 1,
  SWAPM_EXCHANGE = 2,
};

typedef struct _glx_texture glx_texture_t;

#ifdef CONFIG_OPENGL
#ifdef DEBUG_GLX_DEBUG_CONTEXT
typedef GLXContext (*f_glXCreateContextAttribsARB) (Display *dpy,
    GLXFBConfig config, GLXContext share_context, Bool direct,
    const int *attrib_list);
typedef void (*GLDEBUGPROC) (GLenum source, GLenum type,
    GLuint id, GLenum severity, GLsizei length, const GLchar* message,
    GLvoid* userParam);
typedef void (*f_DebugMessageCallback) (GLDEBUGPROC, void *userParam);
#endif

#ifdef CONFIG_OPENGL
typedef GLsync (*f_FenceSync) (GLenum condition, GLbitfield flags);
typedef GLboolean (*f_IsSync) (GLsync sync);
typedef void (*f_DeleteSync) (GLsync sync);
typedef GLenum (*f_ClientWaitSync) (GLsync sync, GLbitfield flags,
    GLuint64 timeout);
typedef void (*f_WaitSync) (GLsync sync, GLbitfield flags,
    GLuint64 timeout);
typedef GLsync (*f_ImportSyncEXT) (GLenum external_sync_type,
    GLintptr external_sync, GLbitfield flags);
#endif

/// @brief Wrapper of a binded GLX texture.
struct _glx_texture {
  GLuint texture;
  GLXPixmap glpixmap;
  xcb_pixmap_t pixmap;
  GLenum target;
  unsigned width;
  unsigned height;
  bool y_inverted;
};

#ifdef CONFIG_OPENGL
typedef struct {
  /// Fragment shader for blur.
  GLuint frag_shader;
  /// GLSL program for blur.
  GLuint prog;
  /// Location of uniform "offset_x" in blur GLSL program.
  GLint unifm_offset_x;
  /// Location of uniform "offset_y" in blur GLSL program.
  GLint unifm_offset_y;
  /// Location of uniform "factor_center" in blur GLSL program.
  GLint unifm_factor_center;
} glx_blur_pass_t;

typedef struct glx_prog_main {
  /// GLSL program.
  GLuint prog;
  /// Location of uniform "opacity" in window GLSL program.
  GLint unifm_opacity;
  /// Location of uniform "invert_color" in blur GLSL program.
  GLint unifm_invert_color;
  /// Location of uniform "tex" in window GLSL program.
  GLint unifm_tex;
} glx_prog_main_t;

#define GLX_PROG_MAIN_INIT { \
  .prog = 0, \
  .unifm_opacity = -1, \
  .unifm_invert_color = -1, \
  .unifm_tex = -1, \
}

#endif
#else
struct glx_prog_main { };
#endif

#define PAINT_INIT { .pixmap = XCB_NONE, .pict = XCB_NONE }

/// Linked list type of atoms.
typedef struct _latom {
  xcb_atom_t atom;
  struct _latom *next;
} latom_t;

#define REG_DATA_INIT { NULL, 0 }

#ifdef CONFIG_OPENGL
/// Structure containing GLX-dependent data for a compton session.
typedef struct {
  // === OpenGL related ===
  /// GLX context.
  GLXContext context;
  /// Whether we have GL_ARB_texture_non_power_of_two.
  bool has_texture_non_power_of_two;
  /// Pointer to the glFenceSync() function.
  f_FenceSync glFenceSyncProc;
  /// Pointer to the glIsSync() function.
  f_IsSync glIsSyncProc;
  /// Pointer to the glDeleteSync() function.
  f_DeleteSync glDeleteSyncProc;
  /// Pointer to the glClientWaitSync() function.
  f_ClientWaitSync glClientWaitSyncProc;
  /// Pointer to the glWaitSync() function.
  f_WaitSync glWaitSyncProc;
  /// Pointer to the glImportSyncEXT() function.
  f_ImportSyncEXT glImportSyncEXT;
  /// Current GLX Z value.
  int z;
#ifdef CONFIG_OPENGL
  glx_blur_pass_t blur_passes[MAX_BLUR_PASS];
#endif
} glx_session_t;

#define CGLX_SESSION_INIT { .context = NULL }

#endif

/// Structure containing all necessary data for a compton session.
typedef struct session {
  // === Event handlers ===
  /// ev_io for X connection
  ev_io xiow;
  /// Timeout for delayed unredirection.
  ev_timer unredir_timer;
  /// Timer for fading
  ev_timer fade_timer;
  /// Timer for delayed drawing, right now only used by
  /// swopti
  ev_timer delayed_draw_timer;
  /// Use an ev_idle callback for drawing
  /// So we only start drawing when events are processed
  ev_idle draw_idle;
  /// Called everytime we have timeouts or new data on socket,
  /// so we can be sure if xcb read from X socket at anytime during event
  /// handling, we will not left any event unhandled in the queue
  ev_prepare event_check;
  /// Signal handler for SIGUSR1
  ev_signal usr1_signal;
  /// Signal handler for SIGINT
  ev_signal int_signal;
  /// backend data
  void *backend_data;
  /// libev mainloop
  struct ev_loop *loop;

  // === Display related ===
  /// Display in use.
  Display *dpy;
  /// Default screen.
  int scr;
  /// XCB connection.
  xcb_connection_t *c;
  /// Default visual.
  xcb_visualid_t vis;
  /// Default depth.
  int depth;
  /// Root window.
  xcb_window_t root;
  /// Height of root window.
  int root_height;
  /// Width of root window.
  int root_width;
  // Damage of root window.
  // Damage root_damage;
  /// X Composite overlay window. Used if <code>--paint-on-overlay</code>.
  xcb_window_t overlay;
  /// Whether the root tile is filled by compton.
  bool root_tile_fill;
  /// Picture of the root window background.
  paint_t root_tile_paint;
  /// A region of the size of the screen.
  region_t screen_reg;
  /// Picture of root window. Destination of painting in no-DBE painting
  /// mode.
  xcb_render_picture_t root_picture;
  /// A Picture acting as the painting target.
  xcb_render_picture_t tgt_picture;
  /// Temporary buffer to paint to before sending to display.
  paint_t tgt_buffer;
  /// Window ID of the window we register as a symbol.
  xcb_window_t reg_win;
#ifdef CONFIG_OPENGL
  /// Pointer to GLX data.
  glx_session_t *psglx;
  /// Custom GLX program used for painting window.
  // XXX should be in glx_session_t
  glx_prog_main_t glx_prog_win;
#endif
  /// Sync fence to sync draw operations
  xcb_sync_fence_t sync_fence;

  // === Operation related ===
  /// Program options.
  options_t o;
  /// Whether we have hit unredirection timeout.
  bool tmout_unredir_hit;
  /// Whether we need to redraw the screen
  bool redraw_needed;
  /// Whether the program is idling. I.e. no fading, no potential window
  /// changes.
  bool fade_running;
  /// Program start time.
  struct timeval time_start;
  /// The region needs to painted on next paint.
  region_t *damage;
  /// The region damaged on the last paint.
  region_t *damage_ring;
  /// Number of damage regions we track
  int ndamage;
  /// Whether all windows are currently redirected.
  bool redirected;
  /// Pre-generated alpha pictures.
  xcb_render_picture_t *alpha_picts;
  /// Time of last fading. In milliseconds.
  unsigned long fade_time;
  /// Head pointer of the error ignore linked list.
  ignore_t *ignore_head;
  /// Pointer to the <code>next</code> member of tail element of the error
  /// ignore linked list.
  ignore_t **ignore_tail;
  // Cached blur convolution kernels.
  xcb_render_fixed_t *blur_kerns_cache[MAX_BLUR_PASS];
  /// Reset program after next paint.
  bool reset;
  /// If compton should quit
  bool quit;

  // === Expose event related ===
  /// Pointer to an array of <code>XRectangle</code>-s of exposed region.
  /// XXX why do we need this array?
  rect_t *expose_rects;
  /// Number of <code>XRectangle</code>-s in <code>expose_rects</code>.
  int size_expose;
  /// Index of the next free slot in <code>expose_rects</code>.
  int n_expose;

  // === Window related ===
  /// Linked list of all windows.
  win *list;
  /// Pointer to <code>win</code> of current active window. Used by
  /// EWMH <code>_NET_ACTIVE_WINDOW</code> focus detection. In theory,
  /// it's more reliable to store the window ID directly here, just in
  /// case the WM does something extraordinary, but caching the pointer
  /// means another layer of complexity.
  win *active_win;
  /// Window ID of leader window of currently active window. Used for
  /// subsidiary window detection.
  xcb_window_t active_leader;

  // === Shadow/dimming related ===
  /// 1x1 black Picture.
  xcb_render_picture_t black_picture;
  /// 1x1 Picture of the shadow color.
  xcb_render_picture_t cshadow_picture;
  /// 1x1 white Picture.
  xcb_render_picture_t white_picture;
  /// Gaussian map of shadow.
  conv *gaussian_map;
  // for shadow precomputation
  /// A region in which shadow is not painted on.
  region_t shadow_exclude_reg;

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
  /// Whether X Present extension exists.
  bool present_exists;
#ifdef CONFIG_OPENGL
  /// Whether X GLX extension exists.
  bool glx_exists;
  /// Event base number for X GLX extension.
  int glx_event;
  /// Error base number for X GLX extension.
  int glx_error;
#endif
#ifdef CONFIG_XINERAMA
  /// Whether X Xinerama extension exists.
  bool xinerama_exists;
  /// Xinerama screen info.
  xcb_xinerama_query_screens_reply_t *xinerama_scrs;
  /// Xinerama screen regions.
  region_t *xinerama_scr_regs;
  /// Number of Xinerama screens.
  int xinerama_nscrs;
#endif
  /// Whether X Sync extension exists.
  bool xsync_exists;
  /// Event base number for X Sync extension.
  int xsync_event;
  /// Error base number for X Sync extension.
  int xsync_error;
  /// Whether X Render convolution filter exists.
  bool xrfilter_convolution_exists;

  // === Atoms ===
  /// Atom of property <code>_NET_WM_OPACITY</code>.
  xcb_atom_t atom_opacity;
  /// Atom of <code>_NET_FRAME_EXTENTS</code>.
  xcb_atom_t atom_frame_extents;
  /// Property atom to identify top-level frame window. Currently
  /// <code>WM_STATE</code>.
  xcb_atom_t atom_client;
  /// Atom of property <code>WM_NAME</code>.
  xcb_atom_t atom_name;
  /// Atom of property <code>_NET_WM_NAME</code>.
  xcb_atom_t atom_name_ewmh;
  /// Atom of property <code>WM_CLASS</code>.
  xcb_atom_t atom_class;
  /// Atom of property <code>WM_WINDOW_ROLE</code>.
  xcb_atom_t atom_role;
  /// Atom of property <code>WM_TRANSIENT_FOR</code>.
  xcb_atom_t atom_transient;
  /// Atom of property <code>WM_CLIENT_LEADER</code>.
  xcb_atom_t atom_client_leader;
  /// Atom of property <code>_NET_ACTIVE_WINDOW</code>.
  xcb_atom_t atom_ewmh_active_win;
  /// Atom of property <code>_COMPTON_SHADOW</code>.
  xcb_atom_t atom_compton_shadow;
  /// Atom of property <code>_NET_WM_WINDOW_TYPE</code>.
  xcb_atom_t atom_win_type;
  /// Array of atoms of all possible window types.
  xcb_atom_t atoms_wintypes[NUM_WINTYPES];
  /// Linked list of additional atoms to track.
  latom_t *track_atom_lst;

#ifdef CONFIG_DBUS
  // === DBus related ===
  void *dbus_data;
#endif
} session_t;

/// Temporary structure used for communication between
/// <code>get_cfg()</code> and <code>parse_config()</code>.
struct options_tmp {
  bool no_dock_shadow;
  bool no_dnd_shadow;
  double menu_opacity;
};

/// Enumeration for window event hints.
typedef enum {
  WIN_EVMODE_UNKNOWN,
  WIN_EVMODE_FRAME,
  WIN_EVMODE_CLIENT
} win_evmode_t;

extern const char * const WINTYPES[NUM_WINTYPES];
extern session_t *ps_g;

// == Debugging code ==
static inline void
print_timestamp(session_t *ps);

void
ev_xcb_error(session_t *ps, xcb_generic_error_t *err);

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
timeval_ms_cmp(struct timeval *ptv, unsigned long ms) {
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
static inline struct timeval
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
static inline struct timespec
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
  fprintf(stderr, "[ %5ld.%06ld ] ", diff.tv_sec, diff.tv_usec);
}

/**
 * Wrapper of XFree() for convenience.
 *
 * Because a NULL pointer cannot be passed to XFree(), its man page says.
 */
static inline void
cxfree(void *data) {
  if (data)
    XFree(data);
}

_Noreturn static inline void
die(const char *msg) {
  puts(msg);
  exit(1);
}

/**
 * Wrapper of XInternAtom() for convenience.
 */
static inline xcb_atom_t
get_atom(session_t *ps, const char *atom_name) {
  xcb_intern_atom_reply_t *reply =
    xcb_intern_atom_reply(ps->c,
        xcb_intern_atom(ps->c, 0, strlen(atom_name), atom_name),
        NULL);

  xcb_atom_t atom = XCB_NONE;
  if (reply) {
    log_debug("Atom %s is %d", atom_name, reply->atom);
    atom = reply->atom;
    free(reply);
  } else
    die("Failed to intern atoms, bail out");
  return atom;
}

/**
 * Return the painting target window.
 */
static inline xcb_window_t
get_tgt_window(session_t *ps) {
  return ps->overlay != XCB_NONE ? ps->overlay: ps->root;
}

/**
 * Find a window from window id in window linked list of the session.
 */
static inline win *
find_win(session_t *ps, xcb_window_t id) {
  if (!id)
    return NULL;

  win *w;

  for (w = ps->list; w; w = w->next) {
    if (w->id == id && w->state != WSTATE_DESTROYING) {
      return w;
    }
  }

  return 0;
}

/**
 * Find out the WM frame of a client window using existing data.
 *
 * @param id window ID
 * @return struct win object of the found window, NULL if not found
 */
static inline win *
find_toplevel(session_t *ps, xcb_window_t id) {
  if (!id)
    return NULL;

  for (win *w = ps->list; w; w = w->next) {
    if (w->client_win == id && w->state != WSTATE_DESTROYING) {
      return w;
    }
  }

  return NULL;
}

/**
 * Check if current backend uses GLX.
 */
static inline bool
bkend_use_glx(session_t *ps) {
  return BKEND_GLX == ps->o.backend
    || BKEND_XR_GLX_HYBRID == ps->o.backend;
}

/**
 * Check if a window is really focused.
 */
static inline bool
win_is_focused_real(session_t *ps, const win *w) {
  return w->a.map_state == XCB_MAP_STATE_VIEWABLE && ps->active_win == w;
}

/**
 * Find out the currently focused window.
 *
 * @return struct win object of the found window, NULL if not found
 */
static inline win *
find_focused(session_t *ps) {
  if (!ps->o.track_focus) return NULL;

  if (ps->active_win && win_is_focused_real(ps, ps->active_win))
    return ps->active_win;
  return NULL;
}

/**
 * Check if a rectangle includes the whole screen.
 */
static inline bool
rect_is_fullscreen(session_t *ps, int x, int y, int wid, int hei) {
  return (x <= 0 && y <= 0 &&
          (x + wid) >= ps->root_width &&
          (y + hei) >= ps->root_height);
}

static void
set_ignore(session_t *ps, unsigned long sequence) {
  if (ps->o.show_all_xerrors)
    return;

  auto i = cmalloc(ignore_t);
  if (!i) return;

  i->sequence = sequence;
  i->next = 0;
  *ps->ignore_tail = i;
  ps->ignore_tail = &i->next;
}

/**
 * Ignore X errors caused by given X request.
 */
static inline void
set_ignore_cookie(session_t *ps, xcb_void_cookie_t cookie) {
  set_ignore(ps, cookie.sequence);
}

/**
 * Check if a window is a fullscreen window.
 *
 * It's not using w->border_size for performance measures.
 */
static inline bool
win_is_fullscreen(session_t *ps, const win *w) {
  return rect_is_fullscreen(ps, w->g.x, w->g.y, w->widthb, w->heightb)
      && (!w->bounding_shaped || w->rounded_corners);
}

/**
 * Check if a window will be painted solid.
 */
static inline bool
win_is_solid(session_t *ps, const win *w) {
  return WMODE_SOLID == w->mode && !ps->o.force_win_blend;
}

/**
 * Determine if a window has a specific property.
 *
 * @param ps current session
 * @param w window to check
 * @param atom atom of property to check
 * @return true if it has the attribute, false otherwise
 */
static inline bool
wid_has_prop(const session_t *ps, xcb_window_t w, xcb_atom_t atom) {
  auto r =
    xcb_get_property_reply(ps->c,
                           xcb_get_property(ps->c, 0, w, atom,
                                            XCB_GET_PROPERTY_TYPE_ANY, 0, 0),
                           NULL);
  if (!r) {
	  return false;
  }

  auto rtype = r->type;
  free(r);

  if (rtype != XCB_NONE) {
    return true;
  }
  return false;
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
    case 8:   tgt = *(prop.p8);    break;
    case 16:  tgt = *(prop.p16);   break;
    case 32:  tgt = *(prop.p32);   break;
    default:  assert(0);
              break;
  }

  return tgt;
}

void
force_repaint(session_t *ps);

bool
vsync_init(session_t *ps);

void
vsync_deinit(session_t *ps);

/** @name DBus handling
 */
///@{
#ifdef CONFIG_DBUS
/** @name DBus hooks
 */
///@{
void
win_set_shadow_force(session_t *ps, win *w, switch_t val);

void
win_set_fade_force(session_t *ps, win *w, switch_t val);

void
win_set_focused_force(session_t *ps, win *w, switch_t val);

void
win_set_invert_color_force(session_t *ps, win *w, switch_t val);

void
opts_init_track_focus(session_t *ps);

void
opts_set_no_fading_openclose(session_t *ps, bool newval);
//!@}
#endif

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
