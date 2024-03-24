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
#include <assert.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>

#include <X11/Xlib.h>
#include <ev.h>
#include <pixman.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xproto.h>

#include "uthash_extra.h"
#ifdef CONFIG_OPENGL
#include "backend/gl/glx.h"
#endif

// X resource checker
#ifdef DEBUG_XRC
#include "xrescheck.h"
#endif

// FIXME This list of includes should get shorter
#include "backend/backend.h"
#include "backend/driver.h"
#include "compiler.h"
#include "config.h"
#include "list.h"
#include "region.h"
#include "render.h"
#include "statistics.h"
#include "types.h"
#include "utils.h"
#include "win_defs.h"
#include "x.h"

// === Constants ===0

#define NS_PER_SEC 1000000000L
#define US_PER_SEC 1000000L
#define MS_PER_SEC 1000

/// @brief Maximum OpenGL FBConfig depth.
#define OPENGL_MAX_DEPTH 32

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
struct session;

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

struct options *session_get_options(session_t *ps);
/**
 * Check if current backend uses GLX.
 */
static inline bool bkend_use_glx(session_t *ps) {
	auto backend = session_get_options(ps)->backend;
	return backend == BKEND_GLX || backend == BKEND_XR_GLX_HYBRID;
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

static inline void damage_ring_collect(const struct damage_ring *ring, region_t *all_region,
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