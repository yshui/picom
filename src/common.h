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
// #define DEBUG_BACKTRACE  1
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
// #define DEBUG_GLX        1
// #define DEBUG_GLX_GLSL   1
// #define DEBUG_GLX_ERR    1
// #define DEBUG_GLX_MARK   1
// #define DEBUG_GLX_PAINTREG 1
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
// Whether to enable OpenGL support
// #define CONFIG_VSYNC_OPENGL 1
// Whether to enable GLX GLSL support
// #define CONFIG_VSYNC_OPENGL_GLSL 1
// Whether to enable GLX FBO support
// #define CONFIG_VSYNC_OPENGL_FBO 1
// Whether to enable DBus support with libdbus.
// #define CONFIG_DBUS 1
// Whether to enable condition support.
// #define CONFIG_C2 1
// Whether to enable X Sync support.
// #define CONFIG_XSYNC 1
// Whether to enable GLX Sync support.
// #define CONFIG_GLX_XSYNC 1

#if !defined(CONFIG_C2) && defined(DEBUG_C2)
#error Cannot enable c2 debugging without c2 support.
#endif

#if (!defined(CONFIG_XSYNC) || !defined(CONFIG_VSYNC_OPENGL)) && defined(CONFIG_GLX_SYNC)
#error Cannot enable GL sync without X Sync / OpenGL support.
#endif

#ifndef COMPTON_VERSION
#define COMPTON_VERSION "unknown"
#endif

#if defined(DEBUG_ALLOC_REG)
#define DEBUG_BACKTRACE 1
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
#include <ctype.h>
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
#ifdef CONFIG_XSYNC
#include <X11/extensions/sync.h>
#endif

#ifdef CONFIG_XINERAMA
#include <X11/extensions/Xinerama.h>
#endif

// Workarounds for missing definitions in very old versions of X headers,
// thanks to consolers for reporting
#ifndef PictOpDifference
#define PictOpDifference 0x39
#endif

// libconfig
#ifdef CONFIG_LIBCONFIG
#include <libgen.h>
#include <libconfig.h>
#endif

// libdbus
#ifdef CONFIG_DBUS
#include <dbus/dbus.h>
#endif

#ifdef CONFIG_VSYNC_OPENGL

// libGL
#if defined(CONFIG_VSYNC_OPENGL_GLSL) || defined(CONFIG_VSYNC_OPENGL_FBO)
#define GL_GLEXT_PROTOTYPES
#endif

#include <GL/glx.h>

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

/// @brief Wrapper for gcc branch prediction builtin, for likely branch.
#define likely(x)    __builtin_expect(!!(x), 1)

/// @brief Wrapper for gcc branch prediction builtin, for unlikely branch.
#define unlikely(x)  __builtin_expect(!!(x), 0)

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

// X resource checker
#ifdef DEBUG_XRC
#include "xrescheck.h"
#endif

// === Constants ===
#if !(COMPOSITE_MAJOR > 0 || COMPOSITE_MINOR >= 2)
#error libXcomposite version unsupported
#endif

/// @brief Length of generic buffers.
#define BUF_LEN 80

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
#define XRFILTER_GAUSSIAN     "gaussian"
#define XRFILTER_BINOMIAL     "binomial"

/// @brief Maximum OpenGL FBConfig depth.
#define OPENGL_MAX_DEPTH 32

/// @brief Maximum OpenGL buffer age.
#define CGLX_MAX_BUFFER_AGE 5

/// @brief Maximum passes for blur.
#define MAX_BLUR_PASS 5

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

/// Structure representing a X geometry.
typedef struct {
  int wid;
  int hei;
  int x;
  int y;
} geometry_t;

/// A structure representing margins around a rectangle.
typedef struct {
  int top;
  int left;
  int bottom;
  int right;
} margin_t;

// Or use cmemzero().
#define MARGIN_INIT { 0, 0, 0, 0 }

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
  VSYNC_OPENGL_SWC,
  VSYNC_OPENGL_MSWC,
  NUM_VSYNC,
} vsync_t;

/// @brief Possible backends of compton.
enum backend {
  BKEND_XRENDER,
  BKEND_GLX,
  BKEND_XR_GLX_HYBRID,
  NUM_BKEND,
};

/// @brief Possible swap methods.
enum {
  SWAPM_BUFFER_AGE = -1,
  SWAPM_UNDEFINED = 0,
  SWAPM_COPY = 1,
  SWAPM_EXCHANGE = 2,
};

typedef struct _glx_texture glx_texture_t;

#ifdef CONFIG_VSYNC_OPENGL
#ifdef DEBUG_GLX_DEBUG_CONTEXT
typedef GLXContext (*f_glXCreateContextAttribsARB) (Display *dpy,
    GLXFBConfig config, GLXContext share_context, Bool direct,
    const int *attrib_list);
typedef void (*GLDEBUGPROC) (GLenum source, GLenum type,
    GLuint id, GLenum severity, GLsizei length, const GLchar* message,
    GLvoid* userParam);
typedef void (*f_DebugMessageCallback) (GLDEBUGPROC, void *userParam);
#endif

typedef int (*f_WaitVideoSync) (int, int, unsigned *);
typedef int (*f_GetVideoSync) (unsigned *);

typedef Bool (*f_GetSyncValuesOML) (Display* dpy, GLXDrawable drawable, int64_t* ust, int64_t* msc, int64_t* sbc);
typedef Bool (*f_WaitForMscOML) (Display* dpy, GLXDrawable drawable, int64_t target_msc, int64_t divisor, int64_t remainder, int64_t* ust, int64_t* msc, int64_t* sbc);

typedef int (*f_SwapIntervalSGI) (int interval);
typedef int (*f_SwapIntervalMESA) (unsigned int interval);

typedef void (*f_BindTexImageEXT) (Display *display, GLXDrawable drawable, int buffer, const int *attrib_list);
typedef void (*f_ReleaseTexImageEXT) (Display *display, GLXDrawable drawable, int buffer);

typedef void (*f_CopySubBuffer) (Display *dpy, GLXDrawable drawable, int x, int y, int width, int height);

#ifdef CONFIG_GLX_SYNC
// Looks like duplicate typedef of the same type is safe?
typedef int64_t GLint64;
typedef uint64_t GLuint64;
typedef struct __GLsync *GLsync;

#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif

#ifndef GL_TIMEOUT_IGNORED
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull
#endif

#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif

#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED 0x911B
#endif

#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif

#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED 0x911D
#endif

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

#ifdef DEBUG_GLX_MARK
typedef void (*f_StringMarkerGREMEDY) (GLsizei len, const void *string);
typedef void (*f_FrameTerminatorGREMEDY) (void);
#endif

/// @brief Wrapper of a GLX FBConfig.
typedef struct {
  GLXFBConfig cfg;
  GLint texture_fmt;
  GLint texture_tgts;
  bool y_inverted;
} glx_fbconfig_t;

/// @brief Wrapper of a binded GLX texture.
struct _glx_texture {
  GLuint texture;
  GLXPixmap glpixmap;
  Pixmap pixmap;
  GLenum target;
  unsigned width;
  unsigned height;
  unsigned depth;
  bool y_inverted;
};

#ifdef CONFIG_VSYNC_OPENGL_GLSL
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

typedef struct {
  /// Framebuffer used for blurring.
  GLuint fbo;
  /// Textures used for blurring.
  GLuint textures[2];
  /// Width of the textures.
  int width;
  /// Height of the textures.
  int height;
} glx_blur_cache_t;

typedef struct {
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
#endif

typedef struct {
  Pixmap pixmap;
  Picture pict;
  glx_texture_t *ptex;
} paint_t;

#define PAINT_INIT { .pixmap = None, .pict = None }

typedef struct {
  int size;
  double *data;
} conv;

/// Linked list type of atoms.
typedef struct _latom {
  Atom atom;
  struct _latom *next;
} latom_t;

/// A representation of raw region data
typedef struct {
  XRectangle *rects;
  int nrects;
} reg_data_t;

#define REG_DATA_INIT { NULL, 0 }

struct _timeout_t;

struct _win;

typedef struct _c2_lptr c2_lptr_t;

/// Structure representing all options.
typedef struct _options_t {
  // === General ===
  /// The configuration file we used.
  char *config_file;
  /// Path to write PID to.
  char *write_pid_path;
  /// The display name we used. NULL means we are using the value of the
  /// <code>DISPLAY</code> environment variable.
  char *display;
  /// Safe representation of display name.
  char *display_repr;
  /// The backend in use.
  enum backend backend;
  /// Whether to sync X drawing to avoid certain delay issues with
  /// GLX backend.
  bool xrender_sync;
  /// Whether to sync X drawing with X Sync fence.
  bool xrender_sync_fence;
  /// Whether to avoid using stencil buffer under GLX backend. Might be
  /// unsafe.
  bool glx_no_stencil;
  /// Whether to copy unmodified regions from front buffer.
  bool glx_copy_from_front;
  /// Whether to use glXCopySubBufferMESA() to update screen.
  bool glx_use_copysubbuffermesa;
  /// Whether to avoid rebinding pixmap on window damage.
  bool glx_no_rebind_pixmap;
  /// GLX swap method we assume OpenGL uses.
  int glx_swap_method;
  /// Whether to use GL_EXT_gpu_shader4 to (hopefully) accelerates blurring.
  bool glx_use_gpushader4;
  /// Custom fragment shader for painting windows, as a string.
  char *glx_fshader_win_str;
#ifdef CONFIG_VSYNC_OPENGL_GLSL
  /// Custom GLX program used for painting window.
  glx_prog_main_t glx_prog_win;
#endif
  /// Whether to fork to background.
  bool fork_after_register;
  /// Whether to detect rounded corners.
  bool detect_rounded_corners;
  /// Whether to paint on X Composite overlay window instead of root
  /// window.
  bool paint_on_overlay;
  /// Force painting of window content with blending.
  bool force_win_blend;
  /// Resize damage for a specific number of pixels.
  int resize_damage;
  /// Whether to unredirect all windows if a full-screen opaque window
  /// is detected.
  bool unredir_if_possible;
  /// List of conditions of windows to ignore as a full-screen window
  /// when determining if a window could be unredirected.
  c2_lptr_t *unredir_if_possible_blacklist;
  /// Delay before unredirecting screen.
  time_ms_t unredir_if_possible_delay;
  /// Forced redirection setting through D-Bus.
  switch_t redirected_force;
  /// Whether to stop painting. Controlled through D-Bus.
  switch_t stoppaint_force;
  /// Whether to re-redirect screen on root size change.
  bool reredir_on_root_change;
  /// Whether to reinitialize GLX on root size change.
  bool glx_reinit_on_root_change;
  /// Whether to enable D-Bus support.
  bool dbus;
  /// Path to log file.
  char *logpath;
  /// Number of cycles to paint in benchmark mode. 0 for disabled.
  int benchmark;
  /// Window to constantly repaint in benchmark mode. 0 for full-screen.
  Window benchmark_wid;
  /// A list of conditions of windows not to paint.
  c2_lptr_t *paint_blacklist;
  /// Whether to avoid using XCompositeNameWindowPixmap(), for debugging.
  bool no_name_pixmap;
  /// Whether to work under synchronized mode for debugging.
  bool synchronize;
  /// Whether to show all X errors.
  bool show_all_xerrors;
  /// Whether to avoid acquiring X Selection.
  bool no_x_selection;

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
  /// Whether to use glFinish() instead of glFlush() for (possibly) better
  /// VSync yet probably higher CPU usage.
  bool vsync_use_glfinish;

  // === Shadow ===
  /// Enable/disable shadow for specific window types.
  bool wintype_shadow[NUM_WINTYPES];
  /// Red, green and blue tone of the shadow.
  double shadow_red, shadow_green, shadow_blue;
  int shadow_radius;
  int shadow_offset_x, shadow_offset_y;
  double shadow_opacity;
  bool clear_shadow;
  /// Geometry of a region in which shadow is not painted on.
  geometry_t shadow_exclude_reg_geom;
  /// Shadow blacklist. A linked list of conditions.
  c2_lptr_t *shadow_blacklist;
  /// Whether bounding-shaped window should be ignored.
  bool shadow_ignore_shaped;
  /// Whether to respect _COMPTON_SHADOW.
  bool respect_prop_shadow;
  /// Whether to crop shadow to the very Xinerama screen.
  bool xinerama_shadow_crop;

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
  /// Whether to disable fading on ARGB managed destroyed windows.
  bool no_fading_destroyed_argb;
  /// Fading blacklist. A linked list of conditions.
  c2_lptr_t *fade_blacklist;

  // === Opacity ===
  /// Default opacity for specific window types
  double wintype_opacity[NUM_WINTYPES];
  /// Default opacity for inactive windows.
  /// 32-bit integer with the format of _NET_WM_OPACITY. 0 stands for
  /// not enabled, default.
  opacity_t inactive_opacity;
  /// Default opacity for inactive windows.
  opacity_t active_opacity;
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
  /// Background blur blacklist. A linked list of conditions.
  c2_lptr_t *blur_background_blacklist;
  /// Blur convolution kernel.
  XFixed *blur_kerns[MAX_BLUR_PASS];
  /// How much to dim an inactive window. 0.0 - 1.0, 0 to disable.
  double inactive_dim;
  /// Whether to use fixed inactive dim opacity, instead of deciding
  /// based on window opacity.
  bool inactive_dim_fixed;
  /// Conditions of windows to have inverted colors.
  c2_lptr_t *invert_color_list;
  /// Rules to change window opacity.
  c2_lptr_t *opacity_rules;

  // === Focus related ===
  /// Consider windows of specific types to be always focused.
  bool wintype_focus[NUM_WINTYPES];
  /// Whether to try to detect WM windows and mark them as focused.
  bool mark_wmwin_focused;
  /// Whether to mark override-redirect windows as focused.
  bool mark_ovredir_focused;
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

#ifdef CONFIG_VSYNC_OPENGL
/// Structure containing GLX-dependent data for a compton session.
typedef struct {
  // === OpenGL related ===
  /// GLX context.
  GLXContext context;
  /// Whether we have GL_ARB_texture_non_power_of_two.
  bool has_texture_non_power_of_two;
  /// Pointer to glXGetVideoSyncSGI function.
  f_GetVideoSync glXGetVideoSyncSGI;
  /// Pointer to glXWaitVideoSyncSGI function.
  f_WaitVideoSync glXWaitVideoSyncSGI;
   /// Pointer to glXGetSyncValuesOML function.
  f_GetSyncValuesOML glXGetSyncValuesOML;
  /// Pointer to glXWaitForMscOML function.
  f_WaitForMscOML glXWaitForMscOML;
  /// Pointer to glXSwapIntervalSGI function.
  f_SwapIntervalSGI glXSwapIntervalProc;
  /// Pointer to glXSwapIntervalMESA function.
  f_SwapIntervalMESA glXSwapIntervalMESAProc;
  /// Pointer to glXBindTexImageEXT function.
  f_BindTexImageEXT glXBindTexImageProc;
  /// Pointer to glXReleaseTexImageEXT function.
  f_ReleaseTexImageEXT glXReleaseTexImageProc;
  /// Pointer to glXCopySubBufferMESA function.
  f_CopySubBuffer glXCopySubBufferProc;
#ifdef CONFIG_GLX_SYNC
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
#endif
#ifdef DEBUG_GLX_MARK
  /// Pointer to StringMarkerGREMEDY function.
  f_StringMarkerGREMEDY glStringMarkerGREMEDY;
  /// Pointer to FrameTerminatorGREMEDY function.
  f_FrameTerminatorGREMEDY glFrameTerminatorGREMEDY;
#endif
  /// Current GLX Z value.
  int z;
  /// FBConfig-s for GLX pixmap of different depths.
  glx_fbconfig_t *fbconfigs[OPENGL_MAX_DEPTH + 1];
#ifdef CONFIG_VSYNC_OPENGL_GLSL
  glx_blur_pass_t blur_passes[MAX_BLUR_PASS];
#endif
} glx_session_t;

#define CGLX_SESSION_INIT { .context = NULL }

#endif

/// Structure containing all necessary data for a compton session.
typedef struct _session_t {
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
  /// Whether the root tile is filled by compton.
  bool root_tile_fill;
  /// Picture of the root window background.
  paint_t root_tile_paint;
  /// A region of the size of the screen.
  XserverRegion screen_reg;
  /// Picture of root window. Destination of painting in no-DBE painting
  /// mode.
  Picture root_picture;
  /// A Picture acting as the painting target.
  Picture tgt_picture;
  /// Temporary buffer to paint to before sending to display.
  paint_t tgt_buffer;
#ifdef CONFIG_XSYNC
  XSyncFence tgt_buffer_fence;
#endif
  /// DBE back buffer for root window. Used in DBE painting mode.
  XdbeBackBuffer root_dbe;
  /// Window ID of the window we register as a symbol.
  Window reg_win;
#ifdef CONFIG_VSYNC_OPENGL
  /// Pointer to GLX data.
  glx_session_t *psglx;
#endif

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
  /// Timeout for delayed unredirection.
  struct _timeout_t *tmout_unredir;
  /// Whether we have hit unredirection timeout.
  bool tmout_unredir_hit;
  /// Whether we have received an event in this cycle.
  bool ev_received;
  /// Whether the program is idling. I.e. no fading, no potential window
  /// changes.
  bool idling;
  /// Program start time.
  struct timeval time_start;
  /// The region needs to painted on next paint.
  XserverRegion all_damage;
  /// The region damaged on the last paint.
  XserverRegion all_damage_last[CGLX_MAX_BUFFER_AGE];
  /// Whether all windows are currently redirected.
  bool redirected;
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
  // Cached blur convolution kernels.
  XFixed *blur_kerns_cache[MAX_BLUR_PASS];
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
  /// A region in which shadow is not painted on.
  XserverRegion shadow_exclude_reg;

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
#ifdef CONFIG_XINERAMA
  /// Whether X Xinerama extension exists.
  bool xinerama_exists;
  /// Xinerama screen info.
  XineramaScreenInfo *xinerama_scrs;
  /// Xinerama screen regions.
  XserverRegion *xinerama_scr_regs;
  /// Number of Xinerama screens.
  int xinerama_nscrs;
#endif
#ifdef CONFIG_XSYNC
  /// Whether X Sync extension exists.
  bool xsync_exists;
  /// Event base number for X Sync extension.
  int xsync_event;
  /// Error base number for X Sync extension.
  int xsync_error;
#endif
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
#ifdef CONFIG_XINERAMA
  /// Xinerama screen this window is on.
  int xinerama_scr;
#endif
  /// Window visual pict format;
  XRenderPictFormat *pictfmt;
  /// Window painting mode.
  winmode_t mode;
  /// Whether the window has been damaged at least once.
  bool damaged;
#ifdef CONFIG_XSYNC
  /// X Sync fence of drawable.
  XSyncFence fence;
#endif
  /// Whether the window was damaged after last paint.
  bool pixmap_damaged;
  /// Damage of the window.
  Damage damage;
  /// Paint info of the window.
  paint_t paint;
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
  /// Whether the window is painting excluded.
  bool paint_excluded;
  /// Whether the window is unredirect-if-possible excluded.
  bool unredir_if_possible_excluded;
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
  const c2_lptr_t *cache_bbblst;
  const c2_lptr_t *cache_oparule;
  const c2_lptr_t *cache_pblst;
  const c2_lptr_t *cache_uipblst;

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
  /// Last window opacity value we set.
  opacity_t opacity_set;

  // Fading-related members
  /// Do not fade if it's false. Change on window type change.
  /// Used by fading blacklist in the future.
  bool fade;
  /// Fade state on last paint.
  bool fade_last;
  /// Override value of window fade state. Set by D-Bus method calls.
  switch_t fade_force;
  /// Callback to be called after fading completed.
  void (*fade_callback) (session_t *ps, struct _win *w);

  // Frame-opacity-related members
  /// Current window frame opacity. Affected by window opacity.
  double frame_opacity;
  /// Frame extents. Acquired from _NET_FRAME_EXTENTS.
  margin_t frame_extents;

  // Shadow-related members
  /// Whether a window has shadow. Calculated.
  bool shadow;
  /// Shadow state on last paint.
  bool shadow_last;
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
  paint_t shadow_paint;
  /// The value of _COMPTON_SHADOW attribute of the window. Below 0 for
  /// none.
  long prop_shadow;

  // Dim-related members
  /// Whether the window is to be dimmed.
  bool dim;

  /// Whether to invert window color.
  bool invert_color;
  /// Color inversion state on last paint.
  bool invert_color_last;
  /// Override value of window color inversion state. Set by D-Bus method
  /// calls.
  switch_t invert_color_force;

  /// Whether to blur window background.
  bool blur_background;
  /// Background state on last paint.
  bool blur_background_last;

#ifdef CONFIG_VSYNC_OPENGL_GLSL
  /// Textures and FBO background blur use.
  glx_blur_cache_t glx_blur_cache;
#endif
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
extern const char * const VSYNC_STRS[NUM_VSYNC + 1];
extern const char * const BACKEND_STRS[NUM_BKEND + 1];
extern session_t *ps_g;

// == Debugging code ==
static inline void
print_timestamp(session_t *ps);

#ifdef DEBUG_BACKTRACE

#include <execinfo.h>
#define BACKTRACE_SIZE  25

/**
 * Print current backtrace.
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

  for (size_t i = 0; i < size; i++)
     printf ("%s\n", strings[i]);

  free(strings);
}

#ifdef DEBUG_ALLOC_REG

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

#endif

// === Functions ===

/**
 * @brief Quit if the passed-in pointer is empty.
 */
static inline void *
allocchk_(const char *func_name, void *ptr) {
  if (!ptr) {
    printf_err("%s(): Failed to allocate memory.", func_name);
    exit(1);
  }
  return ptr;
}

/// @brief Wrapper of allocchk_().
#define allocchk(ptr) allocchk_(__func__, ptr)

/// @brief Wrapper of malloc().
#define cmalloc(nmemb, type) ((type *) allocchk(malloc((nmemb) * sizeof(type))))

/// @brief Wrapper of calloc().
#define ccalloc(nmemb, type) ((type *) allocchk(calloc((nmemb), sizeof(type))))

/// @brief Wrapper of ealloc().
#define crealloc(ptr, nmemb, type) ((type *) allocchk(realloc((ptr), (nmemb) * sizeof(type))))

/// @brief Zero out the given memory block.
#define cmemzero(ptr, size) memset((ptr), 0, (size))

/// @brief Wrapper of cmemzero() that handles a pointer to a single item, for
///        convenience.
#define cmemzero_one(ptr) cmemzero((ptr), sizeof(*(ptr)))

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
  printf("[ %5ld.%02ld ] ", diff.tv_sec, diff.tv_usec / 10000);
}

/**
 * Allocate the space and copy a string.
 */
static inline char *
mstrcpy(const char *src) {
  char *str = cmalloc(strlen(src) + 1, char);

  strcpy(str, src);

  return str;
}

/**
 * Allocate the space and copy a string.
 */
static inline char *
mstrncpy(const char *src, unsigned len) {
  char *str = cmalloc(len + 1, char);

  strncpy(str, src, len);
  str[len] = '\0';

  return str;
}

/**
 * Allocate the space and join two strings.
 */
static inline char *
mstrjoin(const char *src1, const char *src2) {
  char *str = cmalloc(strlen(src1) + strlen(src2) + 1, char);

  strcpy(str, src1);
  strcat(str, src2);

  return str;
}

/**
 * Allocate the space and join two strings;
 */
static inline char *
mstrjoin3(const char *src1, const char *src2, const char *src3) {
  char *str = cmalloc(strlen(src1) + strlen(src2)
        + strlen(src3) + 1, char);

  strcpy(str, src1);
  strcat(str, src2);
  strcat(str, src3);

  return str;
}

/**
 * Concatenate a string on heap with another string.
 */
static inline void
mstrextend(char **psrc1, const char *src2) {
  *psrc1 = crealloc(*psrc1, (*psrc1 ? strlen(*psrc1): 0) + strlen(src2) + 1,
      char);

  strcat(*psrc1, src2);
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
  for (vsync_t i = 0; VSYNC_STRS[i]; ++i)
    if (!strcasecmp(str, VSYNC_STRS[i])) {
      ps->o.vsync = i;
      return true;
    }

  printf_errf("(\"%s\"): Invalid vsync argument.", str);
  return false;
}

/**
 * Parse a backend option argument.
 */
static inline bool
parse_backend(session_t *ps, const char *str) {
  for (enum backend i = 0; BACKEND_STRS[i]; ++i)
    if (!strcasecmp(str, BACKEND_STRS[i])) {
      ps->o.backend = i;
      return true;
    }
  // Keep compatibility with an old revision containing a spelling mistake...
  if (!strcasecmp(str, "xr_glx_hybird")) {
    ps->o.backend = BKEND_XR_GLX_HYBRID;
    return true;
  }
  // cju wants to use dashes
  if (!strcasecmp(str, "xr-glx-hybrid")) {
    ps->o.backend = BKEND_XR_GLX_HYBRID;
    return true;
  }
  printf_errf("(\"%s\"): Invalid backend argument.", str);
  return false;
}

/**
 * Parse a glx_swap_method option argument.
 */
static inline bool
parse_glx_swap_method(session_t *ps, const char *str) {
  // Parse alias
  if (!strcmp("undefined", str)) {
    ps->o.glx_swap_method = 0;
    return true;
  }

  if (!strcmp("copy", str)) {
    ps->o.glx_swap_method = 1;
    return true;
  }

  if (!strcmp("exchange", str)) {
    ps->o.glx_swap_method = 2;
    return true;
  }

  if (!strcmp("buffer-age", str)) {
    ps->o.glx_swap_method = -1;
    return true;
  }

  // Parse number
  {
    char *pc = NULL;
    int age = strtol(str, &pc, 0);
    if (!pc || str == pc) {
      printf_errf("(\"%s\"): Invalid number.", str);
      return false;
    }

    for (; *pc; ++pc)
      if (!isspace(*pc)) {
        printf_errf("(\"%s\"): Trailing characters.", str);
        return false;
      }

    if (age > CGLX_MAX_BUFFER_AGE + 1 || age < -1) {
      printf_errf("(\"%s\"): Number too large / too small.", str);
      return false;
    }

    ps->o.glx_swap_method = age;
  }

  return true;
}

timeout_t *
timeout_insert(session_t *ps, time_ms_t interval,
    bool (*callback)(session_t *ps, timeout_t *ptmout), void *data);

void
timeout_invoke(session_t *ps, timeout_t *ptmout);

bool
timeout_drop(session_t *ps, timeout_t *prm);

void
timeout_reset(session_t *ps, timeout_t *ptmout);

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
 * Wrapper of XFree() for convenience.
 *
 * Because a NULL pointer cannot be passed to XFree(), its man page says.
 */
static inline void
cxfree(void *data) {
  if (data)
    XFree(data);
}

/**
 * Wrapper of XInternAtom() for convenience.
 */
static inline Atom
get_atom(session_t *ps, const char *atom_name) {
  return XInternAtom(ps->dpy, atom_name, False);
}

/**
 * Return the painting target window.
 */
static inline Window
get_tgt_window(session_t *ps) {
  return ps->o.paint_on_overlay ? ps->overlay: ps->root;
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
 * Check if current backend uses XRender for rendering.
 */
static inline bool
bkend_use_xrender(session_t *ps) {
  return BKEND_XRENDER == ps->o.backend
    || BKEND_XR_GLX_HYBRID == ps->o.backend;
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
 * Check if there's a GLX context.
 */
static inline bool
glx_has_context(session_t *ps) {
#ifdef CONFIG_VSYNC_OPENGL
  return ps->psglx && ps->psglx->context;
#else
  return false;
#endif
}

/**
 * Check if a window is really focused.
 */
static inline bool
win_is_focused_real(session_t *ps, const win *w) {
  return IsViewable == w->a.map_state && ps->active_win == w;
}

/**
 * Find out the currently focused window.
 *
 * @return struct _win object of the found window, NULL if not found
 */
static inline win *
find_focused(session_t *ps) {
  if (!ps->o.track_focus) return NULL;

  if (ps->active_win && win_is_focused_real(ps, ps->active_win))
    return ps->active_win;
  return NULL;
}

/**
 * Copies a region.
 */
static inline XserverRegion
copy_region(const session_t *ps, XserverRegion oldregion) {
  if (!oldregion)
    return None;

  XserverRegion region = XFixesCreateRegion(ps->dpy, NULL, 0);

  XFixesCopyRegion(ps->dpy, region, oldregion);

  return region;
}

/**
 * Destroy a <code>XserverRegion</code>.
 */
static inline void
free_region(session_t *ps, XserverRegion *p) {
  if (*p) {
    XFixesDestroyRegion(ps->dpy, *p);
    *p = None;
  }
}

/**
 * Free all regions in ps->all_damage_last .
 */
static inline void
free_all_damage_last(session_t *ps) {
  for (int i = 0; i < CGLX_MAX_BUFFER_AGE; ++i)
    free_region(ps, &ps->all_damage_last[i]);
}

#ifdef CONFIG_XSYNC
/**
 * Free a XSync fence.
 */
static inline void
free_fence(session_t *ps, XSyncFence *pfence) {
  if (*pfence)
    XSyncDestroyFence(ps->dpy, *pfence);
  *pfence = None;
}
#else
#define free_fence(ps, pfence) ((void) 0)
#endif

/**
 * Crop a rectangle by another rectangle.
 *
 * psrc and pdst cannot be the same.
 */
static inline void
rect_crop(XRectangle *pdst, const XRectangle *psrc, const XRectangle *pbound) {
  assert(psrc != pdst);
  pdst->x = max_i(psrc->x, pbound->x);
  pdst->y = max_i(psrc->y, pbound->y);
  pdst->width = max_i(0, min_i(psrc->x + psrc->width, pbound->x + pbound->width) - pdst->x);
  pdst->height = max_i(0, min_i(psrc->y + psrc->height, pbound->y + pbound->height) - pdst->y);
}

/**
 * Check if a rectangle includes the whole screen.
 */
static inline bool
rect_is_fullscreen(session_t *ps, int x, int y, unsigned wid, unsigned hei) {
  return (x <= 0 && y <= 0
      && (x + wid) >= ps->root_width && (y + hei) >= ps->root_height);
}

/**
 * Check if a window is a fullscreen window.
 *
 * It's not using w->border_size for performance measures.
 */
static inline bool
win_is_fullscreen(session_t *ps, const win *w) {
  return rect_is_fullscreen(ps, w->a.x, w->a.y, w->widthb, w->heightb)
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
    cxfree(data);
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
    cxfree(pprop->data.p8);
    pprop->data.p8 = NULL;
  }
  pprop->nitems = 0;
}

void
force_repaint(session_t *ps);

bool
vsync_init(session_t *ps);

void
vsync_deinit(session_t *ps);

#ifdef CONFIG_VSYNC_OPENGL
/** @name GLX
 */
///@{

#ifdef CONFIG_GLX_SYNC
void
xr_glx_sync(session_t *ps, Drawable d, XSyncFence *pfence);
#endif

bool
glx_init(session_t *ps, bool need_render);

void
glx_destroy(session_t *ps);

bool
glx_reinit(session_t *ps, bool need_render);

void
glx_on_root_change(session_t *ps);

bool
glx_init_blur(session_t *ps);

#ifdef CONFIG_VSYNC_OPENGL_GLSL
bool
glx_load_prog_main(session_t *ps,
    const char *vshader_str, const char *fshader_str,
    glx_prog_main_t *pprogram);
#endif

bool
glx_bind_pixmap(session_t *ps, glx_texture_t **pptex, Pixmap pixmap,
    unsigned width, unsigned height, unsigned depth);

void
glx_release_pixmap(session_t *ps, glx_texture_t *ptex);

void
glx_paint_pre(session_t *ps, XserverRegion *preg);

/**
 * Check if a texture is binded, or is binded to the given pixmap.
 */
static inline bool
glx_tex_binded(const glx_texture_t *ptex, Pixmap pixmap) {
  return ptex && ptex->glpixmap && ptex->texture
    && (!pixmap || pixmap == ptex->pixmap);
}

void
glx_set_clip(session_t *ps, XserverRegion reg, const reg_data_t *pcache_reg);

#ifdef CONFIG_VSYNC_OPENGL_GLSL
bool
glx_blur_dst(session_t *ps, int dx, int dy, int width, int height, float z,
    GLfloat factor_center,
    XserverRegion reg_tgt, const reg_data_t *pcache_reg,
    glx_blur_cache_t *pbc);
#endif

bool
glx_dim_dst(session_t *ps, int dx, int dy, int width, int height, float z,
    GLfloat factor, XserverRegion reg_tgt, const reg_data_t *pcache_reg);

bool
glx_render_(session_t *ps, const glx_texture_t *ptex,
    int x, int y, int dx, int dy, int width, int height, int z,
    double opacity, bool argb, bool neg,
    XserverRegion reg_tgt, const reg_data_t *pcache_reg
#ifdef CONFIG_VSYNC_OPENGL_GLSL
    , const glx_prog_main_t *pprogram
#endif
    );

#ifdef CONFIG_VSYNC_OPENGL_GLSL
#define \
   glx_render(ps, ptex, x, y, dx, dy, width, height, z, opacity, argb, neg, reg_tgt, pcache_reg, pprogram) \
  glx_render_(ps, ptex, x, y, dx, dy, width, height, z, opacity, argb, neg, reg_tgt, pcache_reg, pprogram)
#else
#define \
   glx_render(ps, ptex, x, y, dx, dy, width, height, z, opacity, argb, neg, reg_tgt, pcache_reg, pprogram) \
  glx_render_(ps, ptex, x, y, dx, dy, width, height, z, opacity, argb, neg, reg_tgt, pcache_reg)
#endif

void
glx_swap_copysubbuffermesa(session_t *ps, XserverRegion reg);

unsigned char *
glx_take_screenshot(session_t *ps, int *out_length);

#ifdef CONFIG_VSYNC_OPENGL_GLSL
GLuint
glx_create_shader(GLenum shader_type, const char *shader_str);

GLuint
glx_create_program(const GLuint * const shaders, int nshaders);

GLuint
glx_create_program_from_str(const char *vert_shader_str,
    const char *frag_shader_str);
#endif

/**
 * Free a GLX texture.
 */
static inline void
free_texture_r(session_t *ps, GLuint *ptexture) {
  if (*ptexture) {
    assert(glx_has_context(ps));
    glDeleteTextures(1, ptexture);
    *ptexture = 0;
  }
}

/**
 * Free a GLX Framebuffer object.
 */
static inline void
free_glx_fbo(session_t *ps, GLuint *pfbo) {
#ifdef CONFIG_VSYNC_OPENGL_FBO
  if (*pfbo) {
    glDeleteFramebuffers(1, pfbo);
    *pfbo = 0;
  }
#endif
  assert(!*pfbo);
}

#ifdef CONFIG_VSYNC_OPENGL_GLSL
/**
 * Free data in glx_blur_cache_t on resize.
 */
static inline void
free_glx_bc_resize(session_t *ps, glx_blur_cache_t *pbc) {
  free_texture_r(ps, &pbc->textures[0]);
  free_texture_r(ps, &pbc->textures[1]);
  pbc->width = 0;
  pbc->height = 0;
}

/**
 * Free a glx_blur_cache_t
 */
static inline void
free_glx_bc(session_t *ps, glx_blur_cache_t *pbc) {
  free_glx_fbo(ps, &pbc->fbo);
  free_glx_bc_resize(ps, pbc);
}
#endif
#endif

/**
 * Free a glx_texture_t.
 */
static inline void
free_texture(session_t *ps, glx_texture_t **pptex) {
  glx_texture_t *ptex = *pptex;

  // Quit if there's nothing
  if (!ptex)
    return;

#ifdef CONFIG_VSYNC_OPENGL
  glx_release_pixmap(ps, ptex);

  free_texture_r(ps, &ptex->texture);

  // Free structure itself
  free(ptex);
  *pptex = NULL;
#endif
  assert(!*pptex);
}

/**
 * Free GLX part of paint_t.
 */
static inline void
free_paint_glx(session_t *ps, paint_t *ppaint) {
  free_texture(ps, &ppaint->ptex);
}

/**
 * Free GLX part of win.
 */
static inline void
free_win_res_glx(session_t *ps, win *w) {
  free_paint_glx(ps, &w->paint);
  free_paint_glx(ps, &w->shadow_paint);
#ifdef CONFIG_VSYNC_OPENGL_GLSL
  free_glx_bc(ps, &w->glx_blur_cache);
#endif
}

/**
 * Add a OpenGL debugging marker.
 */
static inline void
glx_mark_(session_t *ps, const char *func, XID xid, bool start) {
#ifdef DEBUG_GLX_MARK
  if (glx_has_context(ps) && ps->psglx->glStringMarkerGREMEDY) {
    if (!func) func = "(unknown)";
    const char *postfix = (start ? " (start)": " (end)");
    char *str = malloc((strlen(func) + 12 + 2
          + strlen(postfix) + 5) * sizeof(char));
    strcpy(str, func);
    sprintf(str + strlen(str), "(%#010lx)%s", xid, postfix);
    ps->psglx->glStringMarkerGREMEDY(strlen(str), str);
    free(str);
  }
#endif
}

#define glx_mark(ps, xid, start) glx_mark_(ps, __func__, xid, start)

/**
 * Add a OpenGL debugging marker.
 */
static inline void
glx_mark_frame(session_t *ps) {
#ifdef DEBUG_GLX_MARK
  if (glx_has_context(ps) && ps->psglx->glFrameTerminatorGREMEDY)
    ps->psglx->glFrameTerminatorGREMEDY();
#endif
}

///@}

#ifdef CONFIG_XSYNC
#define xr_sync(ps, d, pfence) xr_sync_(ps, d, pfence)
#else
#define xr_sync(ps, d, pfence) xr_sync_(ps, d)
#endif

/**
 * Synchronizes a X Render drawable to ensure all pending painting requests
 * are completed.
 */
static inline void
xr_sync_(session_t *ps, Drawable d
#ifdef CONFIG_XSYNC
    , XSyncFence *pfence
#endif
    ) {
  if (!ps->o.xrender_sync)
    return;

  XSync(ps->dpy, False);
#ifdef CONFIG_XSYNC
  if (ps->o.xrender_sync_fence && ps->xsync_exists) {
    // TODO: If everybody just follows the rules stated in X Sync prototype,
    // we need only one fence per screen, but let's stay a bit cautious right
    // now
    XSyncFence tmp_fence = None;
    if (!pfence)
      pfence = &tmp_fence;
    assert(pfence);
    if (!*pfence)
      *pfence = XSyncCreateFence(ps->dpy, d, False);
    if (*pfence) {
      Bool triggered = False;
      /* if (XSyncQueryFence(ps->dpy, *pfence, &triggered) && triggered)
        XSyncResetFence(ps->dpy, *pfence); */
      // The fence may fail to be created (e.g. because of died drawable)
      assert(!XSyncQueryFence(ps->dpy, *pfence, &triggered) || !triggered);
      XSyncTriggerFence(ps->dpy, *pfence);
      XSyncAwaitFence(ps->dpy, pfence, 1);
      assert(!XSyncQueryFence(ps->dpy, *pfence, &triggered) || triggered);
    }
    else {
      printf_errf("(%#010lx): Failed to create X Sync fence.", d);
    }
    free_fence(ps, &tmp_fence);
    if (*pfence)
      XSyncResetFence(ps->dpy, *pfence);
  }
#endif
#ifdef CONFIG_GLX_SYNC
  xr_glx_sync(ps, d, pfence);
#endif
}

/** @name DBus handling
 */
///@{
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

void
cdbus_ev_win_focusout(session_t *ps, win *w);

void
cdbus_ev_win_focusin(session_t *ps, win *w);
//!@}

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

#ifdef CONFIG_C2
/** @name c2
 */
///@{

c2_lptr_t *
c2_parsed(session_t *ps, c2_lptr_t **pcondlst, const char *pattern,
    void *data);

#define c2_parse(ps, pcondlst, pattern) c2_parsed((ps), (pcondlst), (pattern), NULL)

c2_lptr_t *
c2_free_lptr(c2_lptr_t *lp);

bool
c2_matchd(session_t *ps, win *w, const c2_lptr_t *condlst,
    const c2_lptr_t **cache, void **pdata);

#define c2_match(ps, w, condlst, cache) c2_matchd((ps), (w), (condlst), \
    (cache), NULL)
#endif

///@}

/**
 * @brief Dump the given data to a file.
 */
static inline bool
write_binary_data(const char *path, const unsigned char *data, int length) {
  if (!data)
    return false;
  FILE *f = fopen(path, "wb");
  if (!f) {
    printf_errf("(\"%s\"): Failed to open file for writing.", path);
    return false;
  }
  int wrote_len = fwrite(data, sizeof(unsigned char), length, f);
  fclose(f);
  if (wrote_len != length) {
    printf_errf("(\"%s\"): Failed to write all blocks: %d / %d", path,
        wrote_len, length);
    return false;
  }
  return true;
}

/**
 * @brief Dump raw bytes in HEX format.
 *
 * @param data pointer to raw data
 * @param len length of data
 */
static inline void
hexdump(const char *data, int len) {
  static const int BYTE_PER_LN = 16;

  if (len <= 0)
    return;

  // Print header
  printf("%10s:", "Offset");
  for (int i = 0; i < BYTE_PER_LN; ++i)
    printf(" %2d", i);
  putchar('\n');

  // Dump content
  for (int offset = 0; offset < len; ++offset) {
    if (!(offset % BYTE_PER_LN))
      printf("0x%08x:", offset);

    printf(" %02hhx", data[offset]);

    if ((BYTE_PER_LN - 1) == offset % BYTE_PER_LN)
      putchar('\n');
  }
  if (len % BYTE_PER_LN)
    putchar('\n');

  fflush(stdout);
}

#endif
