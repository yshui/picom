// SPDX-License-Identifier: MIT
// Copyright (c) 2014 Richard Grenville <pyxlcy@gmail.com>
#pragma once

#include "common.h"
#include "uthash.h"

typedef struct {
	XID xid;
	const char *type;
	const char *file;
	const char *func;
	int line;
	UT_hash_handle hh;
} xrc_xid_record_t;

#define M_POS_DATA_PARAMS const char *file, int line, const char *func
#define M_POS_DATA_PASSTHROUGH file, line, func
#define M_POS_DATA __FILE__, __LINE__, __func__

void xrc_add_xid_(XID xid, const char *type, M_POS_DATA_PARAMS);

#define xrc_add_xid(xid, type) xrc_add_xid_(xid, type, M_POS_DATA)

void xrc_delete_xid_(XID xid, M_POS_DATA_PARAMS);

#define xrc_delete_xid(xid) xrc_delete_xid_(xid, M_POS_DATA)

void xrc_report_xid(void);

void xrc_clear_xid(void);

// Pixmap

static inline void xcb_create_pixmap_(xcb_connection_t *c, uint8_t depth,
                                      xcb_pixmap_t pixmap, xcb_drawable_t drawable,
                                      uint16_t width, uint16_t height, M_POS_DATA_PARAMS) {
	xcb_create_pixmap(c, depth, pixmap, drawable, width, height);
	xrc_add_xid_(pixmap, "Pixmap", M_POS_DATA_PASSTHROUGH);
}

#define xcb_create_pixmap(c, depth, pixmap, drawable, width, height)                     \
	xcb_create_pixmap_(c, depth, pixmap, drawable, width, height, M_POS_DATA)

static inline xcb_void_cookie_t
xcb_composite_name_window_pixmap_(xcb_connection_t *c, xcb_window_t window,
                                  xcb_pixmap_t pixmap, M_POS_DATA_PARAMS) {
	xcb_void_cookie_t ret = xcb_composite_name_window_pixmap(c, window, pixmap);
	xrc_add_xid_(pixmap, "PixmapC", M_POS_DATA_PASSTHROUGH);
	return ret;
}

#define xcb_composite_name_window_pixmap(dpy, window, pixmap)                            \
	xcb_composite_name_window_pixmap_(dpy, window, pixmap, M_POS_DATA)

static inline void
xcb_free_pixmap_(xcb_connection_t *c, xcb_pixmap_t pixmap, M_POS_DATA_PARAMS) {
	xcb_free_pixmap(c, pixmap);
	xrc_delete_xid_(pixmap, M_POS_DATA_PASSTHROUGH);
}

#define xcb_free_pixmap(c, pixmap) xcb_free_pixmap_(c, pixmap, M_POS_DATA);
