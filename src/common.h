// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2018, Yuxuan Shui <yshuiv7@gmail.com>

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
#include <sys/time.h>
#include <time.h>

#include <X11/Xlib.h>
#include <ev.h>
#include <pixman.h>
#include <uthash.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xproto.h>

#include <picom/backend.h>
#include <picom/types.h>

#ifdef CONFIG_OPENGL
#include "backend/gl/glx.h"
#endif

// X resource checker
#ifdef DEBUG_XRC
#include "xrescheck.h"
#endif

// FIXME This list of includes should get shorter
#include "backend/driver.h"
#include "config.h"
#include "region.h"
#include "render.h"
#include "utils/statistics.h"
#include "wm/defs.h"
#include "x.h"

// === Constants ===0

#define NS_PER_SEC 1000000000L
#define US_PER_SEC 1000000L
#define MS_PER_SEC 1000

/// @brief Maximum OpenGL buffer age.
#define CGLX_MAX_BUFFER_AGE 5

// Window flags

// === Types ===
typedef struct glx_fbconfig glx_fbconfig_t;
struct glx_session;
struct atom;
struct conv;

#ifdef CONFIG_OPENGL
#ifdef DEBUG_GLX_DEBUG_CONTEXT
typedef GLXContext (*f_glXCreateContextAttribsARB)(Display *dpy, GLXFBConfig config,
                                                   GLXContext share_context, Bool direct,
                                                   const int *attrib_list);
typedef void (*GLDEBUGPROC)(GLenum source, GLenum type, GLuint id, GLenum severity,
                            GLsizei length, const GLchar *message, GLvoid *userParam);
typedef void (*f_DebugMessageCallback)(GLDEBUGPROC, void *userParam);
#endif

typedef struct glx_prog_main {
	/// GLSL program.
	GLuint prog;
	/// Location of uniform "opacity" in window GLSL program.
	GLint unifm_opacity;
	/// Location of uniform "invert_color" in blur GLSL program.
	GLint unifm_invert_color;
	/// Location of uniform "tex" in window GLSL program.
	GLint unifm_tex;
	/// Location of uniform "time" in window GLSL program.
	GLint unifm_time;
} glx_prog_main_t;

#define GLX_PROG_MAIN_INIT                                                               \
	{                                                                                \
		.prog = 0, .unifm_opacity = -1, .unifm_invert_color = -1,                \
		.unifm_tex = -1, .unifm_time = -1                                        \
	}

#else
struct glx_prog_main {};
#endif

#define PAINT_INIT                                                                       \
	{ .pixmap = XCB_NONE, .pict = XCB_NONE }

/// Linked list type of atoms.
typedef struct _latom {
	xcb_atom_t atom;
	struct _latom *next;
} latom_t;

struct shader_info {
	char *key;
	char *source;
	void *backend_shader;
	uint64_t attributes;
	UT_hash_handle hh;
};

struct damage_ring {
	/// Cache a xfixes region so we don't need to allocate it every time.
	/// A workaround for yshui/picom#301
	xcb_xfixes_region_t x_region;
	/// The region needs to painted on next paint.
	int cursor;
	/// The region damaged on the last paint.
	region_t *damages;
	/// Number of damage regions we track
	int count;
};

/// Structure containing all necessary data for a session.
typedef struct session {
	// === Event handlers ===
	/// ev_io for X connection
	ev_io xiow;
	/// Timeout for delayed unredirection.
	ev_timer unredir_timer;
	/// Use an ev_timer callback for drawing
	ev_timer draw_timer;
	/// Called every time we have timeouts or new data on socket,
	/// so we can be sure if xcb read from X socket at anytime during event
	/// handling, we will not left any event unhandled in the queue
	ev_prepare event_check;
	/// Signal handler for SIGUSR1
	ev_signal usr1_signal;
	/// Signal handler for SIGINT
	ev_signal int_signal;

	// === Backend related ===
	/// backend data
	backend_t *backend_data;
	/// backend blur context
	void *backend_blur_context;
	/// graphic drivers used
	enum driver drivers;
	/// file watch handle
	void *file_watch_handle;
	/// libev mainloop
	struct ev_loop *loop;
	/// Shaders
	struct shader_info *shaders;

	// === Display related ===
	/// X connection
	struct x_connection c;
	/// Width of root window.
	int root_width;
	/// Height of root window.
	int root_height;
	/// Current desktop number of root window
	int root_desktop_num;
	/// Desktop switch direction
	int root_desktop_switch_direction;
	/// X Composite overlay window.
	xcb_window_t overlay;
	/// The target window for debug mode
	xcb_window_t debug_window;
	/// Whether the root tile is filled by us.
	bool root_tile_fill;
	/// Picture of the root window background.
	paint_t root_tile_paint;
	/// The backend data the root pixmap bound to
	image_handle root_image;
	/// The root pixmap generation, incremented everytime
	/// the root pixmap changes
	uint64_t root_image_generation;
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
	struct glx_session *psglx;
	/// Custom GLX program used for painting window.
	// XXX should be in struct glx_session
	glx_prog_main_t glx_prog_win;
	struct glx_fbconfig_info argb_fbconfig;
#endif
	/// Sync fence to sync draw operations
	xcb_sync_fence_t sync_fence;
	/// Whether we are rendering the first frame after screen is redirected
	bool first_frame;
	/// When last MSC event happened, in useconds.
	uint64_t last_msc_instant;
	/// The last MSC number
	uint64_t last_msc;
	/// The delay between when the last frame was scheduled to be rendered, and when
	/// the render actually started.
	uint64_t last_schedule_delay;
	/// When do we want our next frame to start rendering.
	uint64_t next_render;
	/// Whether we can perform frame pacing.
	bool frame_pacing;
	/// Vblank event scheduler
	struct vblank_scheduler *vblank_scheduler;

	/// Render statistics
	struct render_statistics render_stats;

	// === Operation related ===
	/// Whether there is a pending quest to get the focused window
	bool pending_focus_check;
	/// Flags related to the root window
	uint64_t root_flags;
	/// Program options.
	options_t o;
	/// State object for c2.
	struct c2_state *c2_state;
	/// Whether we have hit unredirection timeout.
	bool tmout_unredir_hit;
	/// If the backend is busy. This means two things:
	/// Either the backend is currently rendering a frame, or a frame has been
	/// rendered but has yet to be presented. In either case, we should not start
	/// another render right now. As if we start issuing rendering commands now, we
	/// will have to wait for either the current render to finish, or the current
	/// back buffer to become available again. In either case, we will be wasting
	/// time.
	bool backend_busy;
	/// Whether a render is queued. This generally means there are pending updates
	/// to the screen that's neither included in the current render, nor on the
	/// screen.
	bool render_queued;
	// TODO(yshui) remove this after we remove the legacy backends
	/// For tracking damage regions
	struct damage_ring damage_ring;
	// TODO(yshui) move render related fields into separate struct
	/// Render planner
	struct layout_manager *layout_manager;
	/// Render command builder
	struct command_builder *command_builder;
	struct renderer *renderer;
	/// Whether all windows are currently redirected.
	bool redirected;
	/// Pre-generated alpha pictures.
	xcb_render_picture_t *alpha_picts;
	/// Time of last fading. In milliseconds.
	long long fade_time;
	// Cached blur convolution kernels.
	struct x_convolution_kernel **blur_kerns_cache;
	/// If we should quit
	bool quit : 1;
	// TODO(yshui) use separate flags for different kinds of updates so we don't
	// waste our time.
	/// Whether there are pending updates, like window creation, etc.
	bool pending_updates : 1;

	// === Expose event related ===
	/// Pointer to an array of <code>XRectangle</code>-s of exposed region.
	/// This is a reuse temporary buffer for handling root expose events.
	/// This is a dynarr.
	rect_t *expose_rects;

	struct wm *wm;

	struct window_options window_options_default;

	// === Shadow/dimming related ===
	/// 1x1 black Picture.
	xcb_render_picture_t black_picture;
	/// 1x1 Picture of the shadow color.
	xcb_render_picture_t cshadow_picture;
	/// 1x1 white Picture.
	xcb_render_picture_t white_picture;
	/// Backend shadow context.
	struct backend_shadow_context *shadow_context;
	// for shadow precomputation

	// === Software-optimization-related ===
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
	/// Whether X GLX extension exists.
	bool glx_exists;
	/// Event base number for X GLX extension.
	int glx_event;
	/// Error base number for X GLX extension.
	int glx_error;
	/// Information about monitors.
	struct x_monitors monitors;
	/// Whether X Sync extension exists.
	bool xsync_exists;
	/// Event base number for X Sync extension.
	int xsync_event;
	/// Error base number for X Sync extension.
	int xsync_error;
	/// Whether X Render convolution filter exists.
	bool xrfilter_convolution_exists;

	// === Atoms ===
	struct atom *atoms;

#ifdef CONFIG_DBUS
	// === DBus related ===
	struct cdbus_data *dbus_data;
#endif

	int (*vsync_wait)(session_t *);
} session_t;

/// Enumeration for window event hints.
typedef enum { WIN_EVMODE_UNKNOWN, WIN_EVMODE_FRAME, WIN_EVMODE_CLIENT } win_evmode_t;
struct wintype_info {
	const char *name;
	const char *atom;
};
extern const struct wintype_info WINTYPES[NUM_WINTYPES];
extern session_t *ps_g;

void ev_xcb_error(session_t *ps, xcb_generic_error_t *err);

// === Functions ===

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
timespec_subtract(struct timespec *result, struct timespec *x, struct timespec *y) {
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
static inline struct timeval get_time_timeval(void) {
	struct timeval tv = {0, 0};

	gettimeofday(&tv, NULL);

	// Return a time of all 0 if the call fails
	return tv;
}

/**
 * Get current time in struct timespec.
 *
 * Note its starting time is unspecified.
 */
static inline struct timespec get_time_timespec(void) {
	struct timespec tm = {0, 0};

	clock_gettime(CLOCK_MONOTONIC, &tm);

	// Return a time of all 0 if the call fails
	return tm;
}

/**
 * Return the painting target window.
 */
static inline xcb_window_t get_tgt_window(session_t *ps) {
	return ps->overlay != XCB_NONE ? ps->overlay : ps->c.screen_info->root;
}

/**
 * Check if current backend uses GLX.
 */
static inline bool bkend_use_glx(session_t *ps) {
	return BKEND_GLX == ps->o.legacy_backend || BKEND_XR_GLX_HYBRID == ps->o.legacy_backend;
}

/**
 * Determine if a window has a specific property.
 *
 * @param ps current session
 * @param w window to check
 * @param atom atom of property to check
 * @return true if it has the attribute, false otherwise
 */
static inline bool wid_has_prop(xcb_connection_t *c, xcb_window_t w, xcb_atom_t atom) {
	auto r = xcb_get_property_reply(
	    c, xcb_get_property(c, 0, w, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 0), NULL);
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

void force_repaint(session_t *ps);

/**
 * Set a <code>bool</code> array of all wintypes to true.
 */
static inline void wintype_arr_enable(bool arr[]) {
	wintype_t i;

	for (i = 0; i < NUM_WINTYPES; ++i) {
		arr[i] = true;
	}
}

static inline void damage_ring_advance(struct damage_ring *ring) {
	ring->cursor--;
	if (ring->cursor < 0) {
		ring->cursor += ring->count;
	}
	pixman_region32_clear(&ring->damages[ring->cursor]);
}

static inline void damage_ring_collect(struct damage_ring *ring, region_t *all_region,
                                       region_t *region, int buffer_age) {
	if (buffer_age == -1 || buffer_age > ring->count) {
		pixman_region32_copy(region, all_region);
	} else {
		for (int i = 0; i < buffer_age; i++) {
			auto curr = (ring->cursor + i) % ring->count;
			pixman_region32_union(region, region, &ring->damages[curr]);
		}
		pixman_region32_intersect(region, region, all_region);
	}
}
