// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xfixes.h>

#include "compiler.h"
#include "region.h"
#include "kernel.h"

typedef struct session session_t;

/// Structure representing Window property value.
typedef struct winprop {
	union {
		void *ptr;
		int8_t *p8;
		int16_t *p16;
		int32_t *p32;
		uint32_t *c32;        // 32bit cardinal
	};
	unsigned long nitems;
	xcb_atom_t type;
	int format;

	xcb_get_property_reply_t *r;
} winprop_t;

#define XCB_SYNCED_VOID(func, c, ...)                                                    \
	xcb_request_check(c, func##_checked(c, __VA_ARGS__));
#define XCB_SYNCED(func, c, ...)                                                                 \
	({                                                                                       \
		xcb_generic_error_t *e = NULL;                                                   \
		__auto_type r = func##_reply(c, func(c, __VA_ARGS__), &e);                       \
		if (e) {                                                                         \
			x_print_error(e->sequence, e->major_code, e->minor_code, e->error_code); \
			free(e);                                                                 \
		}                                                                                \
		r;                                                                               \
	})

/**
 * Send a request to X server and get the reply to make sure all previous
 * requests are processed, and their replies received
 *
 * xcb_get_input_focus is used here because it is the same request used by
 * libX11
 */
static inline void x_sync(xcb_connection_t *c) {
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
}

/**
 * Get a specific attribute of a window.
 *
 * Returns a blank structure if the returned type and format does not
 * match the requested type and format.
 *
 * @param ps current session
 * @param w window
 * @param atom atom of attribute to fetch
 * @param length length to read
 * @param rtype atom of the requested type
 * @param rformat requested format
 * @return a <code>winprop_t</code> structure containing the attribute
 *    and number of items. A blank one on failure.
 */
winprop_t wid_get_prop_adv(const session_t *ps, xcb_window_t w, xcb_atom_t atom,
                           long offset, long length, xcb_atom_t rtype, int rformat);

/**
 * Wrapper of wid_get_prop_adv().
 */
static inline winprop_t wid_get_prop(const session_t *ps, xcb_window_t wid, xcb_atom_t atom,
                                     long length, xcb_atom_t rtype, int rformat) {
	return wid_get_prop_adv(ps, wid, atom, 0L, length, rtype, rformat);
}

/**
 * Get the value of a type-<code>xcb_window_t</code> property of a window.
 *
 * @return the value if successful, 0 otherwise
 */
xcb_window_t wid_get_prop_window(session_t *ps, xcb_window_t wid, xcb_atom_t aprop);

/**
 * Get the value of a text property of a window.
 */
bool wid_get_text_prop(session_t *ps, xcb_window_t wid, xcb_atom_t prop, char ***pstrlst, int *pnstr);

const xcb_render_pictforminfo_t *x_get_pictform_for_visual(xcb_connection_t *, xcb_visualid_t);
int x_get_visual_depth(xcb_connection_t *, xcb_visualid_t);

xcb_render_picture_t
x_create_picture_with_pictfmt_and_pixmap(xcb_connection_t *, const xcb_render_pictforminfo_t *pictfmt,
                                         xcb_pixmap_t pixmap, unsigned long valuemask,
                                         const xcb_render_create_picture_value_list_t *attr)
    attr_nonnull(1, 2);

xcb_render_picture_t
x_create_picture_with_visual_and_pixmap(xcb_connection_t *, xcb_visualid_t visual,
                                        xcb_pixmap_t pixmap, unsigned long valuemask,
                                        const xcb_render_create_picture_value_list_t *attr)
    attr_nonnull(1);

xcb_render_picture_t
x_create_picture_with_standard_and_pixmap(xcb_connection_t *, xcb_pict_standard_t standard,
                                          xcb_pixmap_t pixmap, unsigned long valuemask,
                                          const xcb_render_create_picture_value_list_t *attr)
    attr_nonnull(1);

/**
 * Create an picture.
 */
xcb_render_picture_t
x_create_picture_with_pictfmt(session_t *ps, int wid, int hei,
                              const xcb_render_pictforminfo_t *pictfmt, unsigned long valuemask,
                              const xcb_render_create_picture_value_list_t *attr);

xcb_render_picture_t
x_create_picture_with_visual(session_t *ps, int w, int h, xcb_visualid_t visual, unsigned long valuemask,
                             const xcb_render_create_picture_value_list_t *attr);

/// Fetch a X region and store it in a pixman region
bool x_fetch_region(xcb_connection_t *, xcb_xfixes_region_t r, region_t *res);

void x_set_picture_clip_region(xcb_connection_t *, xcb_render_picture_t,
                               int clip_x_origin, int clip_y_origin, const region_t *);

void x_clear_picture_clip_region(xcb_connection_t *, xcb_render_picture_t pict);

/**
 * X11 error handler function.
 *
 * XXX consider making this error to string
 */
void x_print_error(unsigned long serial, uint8_t major, uint8_t minor, uint8_t error_code);

xcb_pixmap_t x_create_pixmap(xcb_connection_t *, uint8_t depth, xcb_drawable_t drawable,
                             uint16_t width, uint16_t height);

bool x_validate_pixmap(xcb_connection_t *, xcb_pixmap_t pxmap);

/**
 * Free a <code>winprop_t</code>.
 *
 * @param pprop pointer to the <code>winprop_t</code> to free.
 */
static inline void free_winprop(winprop_t *pprop) {
	// Empty the whole structure to avoid possible issues
	if (pprop->r)
		free(pprop->r);
	pprop->ptr = NULL;
	pprop->r = NULL;
	pprop->nitems = 0;
}
/// Get the back pixmap of the root window
xcb_pixmap_t x_get_root_back_pixmap(session_t *ps);

/// Return true if the atom refers to a property name that is used for the
/// root window background pixmap
bool x_is_root_back_pixmap_atom(session_t *ps, xcb_atom_t atom);

bool x_fence_sync(xcb_connection_t *, xcb_sync_fence_t);

/**
 * Set the picture filter of a xrender picture to a convolution
 * kernel.
 *
 * @param c   xcb connection
 * @param pict the picture
 * @param kern the convolution kernel
 */
void
x_set_picture_convolution_kernel(xcb_connection_t *c,
                                 xcb_render_picture_t pict, conv *kernel);
