// SPDX-License-Identifier: MIT
// Copyright (c)

// Throw everything in here.
// !!! DON'T !!!

// === Includes ===

#include <locale.h>
#include <stdbool.h>
#include <stdlib.h>
#include <xcb/xproto.h>

#include <X11/Xutil.h>
#include "backend/backend.h"
#include "c2.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"        // XXX clean up
#include "region.h"
#include "render.h"
#include "types.h"
#include "utils.h"
#include "win.h"
#include "x.h"

enum root_flags {
	ROOT_FLAGS_SCREEN_CHANGE = 1,        // Received RandR screen change notify, we
	                                     // use this to track refresh rate changes
	ROOT_FLAGS_CONFIGURED = 2        // Received configure notify on the root window
};

// == Functions ==
// TODO(yshui) move static inline functions that are only used in picom.c, into picom.c

void add_damage(session_t *ps, const region_t *damage);

uint32_t determine_evmask(session_t *ps, xcb_window_t wid, win_evmode_t mode);

void circulate_win(session_t *ps, xcb_circulate_notify_event_t *ce);

void root_damaged(session_t *ps);

void cxinerama_upd_scrs(session_t *ps);

void queue_redraw(session_t *ps);

void discard_ignore(session_t *ps, unsigned long sequence);

void set_root_flags(session_t *ps, uint64_t flags);

void quit(session_t *ps);

xcb_window_t session_get_target_window(session_t *);

uint8_t session_redirection_mode(session_t *ps);

/**
 * Set a <code>switch_t</code> array of all unset wintypes to true.
 */
static inline void wintype_arr_enable_unset(switch_t arr[]) {
	wintype_t i;

	for (i = 0; i < NUM_WINTYPES; ++i)
		if (UNSET == arr[i])
			arr[i] = ON;
}

/**
 * Check if a window ID exists in an array of window IDs.
 *
 * @param arr the array of window IDs
 * @param count amount of elements in the array
 * @param wid window ID to search for
 */
static inline bool array_wid_exists(const xcb_window_t *arr, int count, xcb_window_t wid) {
	while (count--) {
		if (arr[count] == wid) {
			return true;
		}
	}

	return false;
}

#ifndef CONFIG_OPENGL
static inline void free_paint_glx(session_t *ps attr_unused, paint_t *p attr_unused) {
}
static inline void
free_win_res_glx(session_t *ps attr_unused, struct managed_win *w attr_unused) {
}
#endif

/**
 * Dump an drawable's info.
 */
static inline void dump_drawable(session_t *ps, xcb_drawable_t drawable) {
	auto r = xcb_get_geometry_reply(ps->c, xcb_get_geometry(ps->c, drawable), NULL);
	if (!r) {
		log_trace("Drawable %#010x: Failed", drawable);
		return;
	}
	log_trace("Drawable %#010x: x = %u, y = %u, wid = %u, hei = %d, b = %u, d = %u",
	          drawable, r->x, r->y, r->width, r->height, r->border_width, r->depth);
	free(r);
}
