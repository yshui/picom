// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

// Throw everything in here.
// !!! DON'T !!!

// === Includes ===

#include <X11/Xutil.h>
#include <locale.h>
#include <stdbool.h>
#include <stdlib.h>
#include <xcb/xproto.h>

#include <picom/types.h>

#include "c2.h"
#include "common.h"
#include "config.h"
#include "log.h"        // XXX clean up
#include "region.h"
#include "render.h"
#include "wm/win.h"
#include "x.h"

// == Functions ==
// TODO(yshui) move static inline functions that are only used in picom.c, into picom.c

void add_damage(session_t *ps, const region_t *damage);

void circulate_win(session_t *ps, xcb_circulate_notify_event_t *ce);

void root_damaged(session_t *ps);

void queue_redraw(session_t *ps);

void discard_pending(session_t *ps, uint32_t sequence);

void configure_root(session_t *ps);

void quit(session_t *ps);

xcb_window_t session_get_target_window(session_t *);

uint8_t session_redirection_mode(session_t *ps);

#ifdef CONFIG_DBUS
struct cdbus_data *session_get_cdbus(struct session *);
#else
static inline struct cdbus_data *session_get_cdbus(session_t *ps attr_unused) {
	return NULL;
}
#endif

/**
 * Set a <code>switch_t</code> array of all unset wintypes to true.
 */
static inline void wintype_arr_enable_unset(switch_t arr[]) {
	wintype_t i;

	for (i = 0; i < NUM_WINTYPES; ++i) {
		if (UNSET == arr[i]) {
			arr[i] = ON;
		}
	}
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

/**
 * Dump an drawable's info.
 */
static inline void dump_drawable(session_t *ps, xcb_drawable_t drawable) {
	auto r = xcb_get_geometry_reply(ps->c.c, xcb_get_geometry(ps->c.c, drawable), NULL);
	if (!r) {
		log_trace("Drawable %#010x: Failed", drawable);
		return;
	}
	log_trace("Drawable %#010x: x = %u, y = %u, wid = %u, hei = %d, b = %u, d = %u",
	          drawable, r->x, r->y, r->width, r->height, r->border_width, r->depth);
	free(r);
}
