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

// FIXME This list of includes should get shorter
#include "backend/driver.h"
#include "config.h"
#include "region.h"
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
struct atom;
struct conv;

struct shader_source {
	const char *path;
	const char *source;
	UT_hash_handle hh;
};

struct shader_info {
	const struct shader_specification *spec;
	const char *source;
	shader_handle backend_shader;
	uint64_t attributes;
	UT_hash_handle hh;
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
	struct shader_info *root_pixmap_shader;
	/// Shader sources
	struct shader_source *shader_sources;
	/// Shaders
	struct shader_info *shaders;

	// === Display related ===
	/// X connection
	struct x_connection c;
	/// Width of root window.
	int root_width;
	/// Height of root window.
	int root_height;
	/// X Composite overlay window.
	xcb_window_t overlay;
	/// The target window for debug mode
	xcb_window_t debug_window;
	/// The backend data the root pixmap bound to
	image_handle root_image;
	/// The geometry of the root image
	rect_t root_image_extent;
	/// The root pixmap generation, incremented every time
	/// the root pixmap changes
	uint64_t root_image_generation;
	/// A region of the size of the screen.
	region_t screen_reg;
	/// Window ID of the window we register as a symbol.
	xcb_window_t reg_win;
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
	/// A X region used for various operations. Kept to avoid repeated allocation.
	xcb_xfixes_region_t x_region;
	// TODO(yshui) move render related fields into separate struct
	/// Render planner
	struct layout_manager *layout_manager;
	/// Render command builder
	struct command_builder *command_builder;
	struct renderer *renderer;
	/// Whether all windows are currently redirected.
	bool redirected;
	/// Time of last fading. In milliseconds.
	long long fade_time;
	/// If we should quit
	bool quit : 1;
	// TODO(yshui) use separate flags for different kinds of updates so we don't
	// waste our time.
	/// Whether there are pending updates, like window creation, etc.
	bool pending_updates : 1;

	struct wm *wm;

	struct window_options window_options_default;

	// === X extension related ===
	/// Information about monitors.
	struct x_monitors monitors;

	// === Atoms ===
	struct atom *atoms;

#ifdef CONFIG_DBUS
	// === DBus related ===
	struct cdbus_data *dbus_data;
#endif
} session_t;

struct wintype_info {
	const char *name;
	const char *atom;
};
extern const struct wintype_info WINTYPES[NUM_WINTYPES];
extern session_t *ps_g;

// === Functions ===

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
