// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#include <stdalign.h>
#include <stdbool.h>
#include <stdlib.h>

#include <X11/Xlib-xcb.h>
#include <X11/Xutil.h>
#include <pixman.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/glx.h>
#include <xcb/present.h>
#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xcb_util.h>
#include <xcb/xcbext.h>
#include <xcb/xfixes.h>

#include "atom.h"
#include "common.h"
#include "compiler.h"
#include "log.h"
#include "region.h"
#include "utils/kernel.h"
#include "utils/misc.h"
#include "x.h"

// === Error handling ===

/// Discard pending error handlers.
///
/// We have received reply with sequence number `sequence`, which means all pending
/// replies with sequence number strictly less than `sequence` will never be received. So
/// discard them.
static void x_discard_pending_errors(struct x_connection *c, uint32_t sequence) {
	if (sequence < c->last_sequence) {
		// Overflown, drop from `pending_x_errors` until its sequence number
		// decreases.
		log_debug("X sequence number overflown, %u -> %u", c->last_sequence, sequence);
		list_foreach_safe(struct pending_x_error, i, &c->pending_x_errors, siblings) {
			if (sequence >= i->sequence) {
				break;
			}
			list_remove(&i->siblings);
			free(i);
		}
	}
	list_foreach_safe(struct pending_x_error, i, &c->pending_x_errors, siblings) {
		if (sequence <= i->sequence) {
			break;
		}
		list_remove(&i->siblings);
		free(i);
	}
}

enum {
	XSyncBadCounter = 0,
	XSyncBadAlarm = 1,
	XSyncBadFence = 2,
};

/// Convert a X11 error to string
///
/// @return a pointer to a string. this pointer shouldn NOT be freed, same buffer is used
///         for multiple calls to this function,
static const char *x_error_code_to_string(unsigned long serial, uint8_t major,
                                          uint16_t minor, uint8_t error_code) {
	session_t *const ps = ps_g;

	int o = 0;
	const char *name = "Unknown";

#define CASESTRRET(s)                                                                    \
	case s: name = #s; break

#define CASESTRRET2(s)                                                                   \
	case XCB_##s: name = #s; break

	// TODO(yshui) separate error code out from session_t
	o = error_code - ps->xfixes_error;
	switch (o) { CASESTRRET2(XFIXES_BAD_REGION); }

	o = error_code - ps->damage_error;
	switch (o) { CASESTRRET2(DAMAGE_BAD_DAMAGE); }

	o = error_code - ps->render_error;
	switch (o) {
		CASESTRRET2(RENDER_PICT_FORMAT);
		CASESTRRET2(RENDER_PICTURE);
		CASESTRRET2(RENDER_PICT_OP);
		CASESTRRET2(RENDER_GLYPH_SET);
		CASESTRRET2(RENDER_GLYPH);
	}

	if (ps->glx_exists) {
		o = error_code - ps->glx_error;
		switch (o) {
			CASESTRRET2(GLX_BAD_CONTEXT);
			CASESTRRET2(GLX_BAD_CONTEXT_STATE);
			CASESTRRET2(GLX_BAD_DRAWABLE);
			CASESTRRET2(GLX_BAD_PIXMAP);
			CASESTRRET2(GLX_BAD_CONTEXT_TAG);
			CASESTRRET2(GLX_BAD_CURRENT_WINDOW);
			CASESTRRET2(GLX_BAD_RENDER_REQUEST);
			CASESTRRET2(GLX_BAD_LARGE_REQUEST);
			CASESTRRET2(GLX_UNSUPPORTED_PRIVATE_REQUEST);
			CASESTRRET2(GLX_BAD_FB_CONFIG);
			CASESTRRET2(GLX_BAD_PBUFFER);
			CASESTRRET2(GLX_BAD_CURRENT_DRAWABLE);
			CASESTRRET2(GLX_BAD_WINDOW);
			CASESTRRET2(GLX_GLX_BAD_PROFILE_ARB);
		}
	}

	if (ps->xsync_exists) {
		o = error_code - ps->xsync_error;
		switch (o) {
			CASESTRRET(XSyncBadCounter);
			CASESTRRET(XSyncBadAlarm);
			CASESTRRET(XSyncBadFence);
		}
	}

	switch (error_code) {
		CASESTRRET2(ACCESS);
		CASESTRRET2(ALLOC);
		CASESTRRET2(ATOM);
		CASESTRRET2(COLORMAP);
		CASESTRRET2(CURSOR);
		CASESTRRET2(DRAWABLE);
		CASESTRRET2(FONT);
		CASESTRRET2(G_CONTEXT);
		CASESTRRET2(ID_CHOICE);
		CASESTRRET2(IMPLEMENTATION);
		CASESTRRET2(LENGTH);
		CASESTRRET2(MATCH);
		CASESTRRET2(NAME);
		CASESTRRET2(PIXMAP);
		CASESTRRET2(REQUEST);
		CASESTRRET2(VALUE);
		CASESTRRET2(WINDOW);
	}

#undef CASESTRRET
#undef CASESTRRET2

	thread_local static char buffer[256];
	snprintf(buffer, sizeof(buffer), "X error %d %s request %d minor %d serial %lu",
	         error_code, name, major, minor, serial);
	return buffer;
}

void x_print_error_impl(unsigned long serial, uint8_t major, uint16_t minor,
                        uint8_t error_code, const char *func) {
	if (unlikely(LOG_LEVEL_DEBUG >= log_get_level_tls())) {
		log_printf(tls_logger, LOG_LEVEL_DEBUG, func, "%s",
		           x_error_code_to_string(serial, major, minor, error_code));
	}
}

/// Handle X errors.
///
/// This function logs X errors, or aborts the program based on severity of the error.
static void x_handle_error(struct x_connection *c, xcb_generic_error_t *ev) {
	x_discard_pending_errors(c, ev->full_sequence);
	struct pending_x_error *first_error_action = NULL;
	if (!list_is_empty(&c->pending_x_errors)) {
		first_error_action =
		    list_entry(c->pending_x_errors.next, struct pending_x_error, siblings);
	}
	if (first_error_action != NULL && first_error_action->sequence == ev->full_sequence) {
		if (first_error_action->action != PENDING_REPLY_ACTION_IGNORE) {
			log_error("X error for request in %s at %s:%d: %s",
			          first_error_action->func, first_error_action->file,
			          first_error_action->line,
			          x_error_code_to_string(ev->full_sequence, ev->major_code,
			                                 ev->minor_code, ev->error_code));
		} else {
			log_debug("Expected X error for request in %s at %s:%d: %s",
			          first_error_action->func, first_error_action->file,
			          first_error_action->line,
			          x_error_code_to_string(ev->full_sequence, ev->major_code,
			                                 ev->minor_code, ev->error_code));
		}
		switch (first_error_action->action) {
		case PENDING_REPLY_ACTION_ABORT:
			log_fatal("An unrecoverable X error occurred, "
			          "aborting...");
			abort();
		case PENDING_REPLY_ACTION_DEBUG_ABORT: assert(false); break;
		case PENDING_REPLY_ACTION_IGNORE: break;
		}
		return;
	}
	log_warn("Stray X error: %s",
	         x_error_code_to_string(ev->full_sequence, ev->major_code, ev->minor_code,
	                                ev->error_code));
}

/**
 * Xlib error handler function.
 */
static int xerror(Display attr_unused *dpy, XErrorEvent *ev) {
	if (!ps_g) {
		// Do not ignore errors until the session has been initialized
		return 0;
	}

	// Fake a xcb error, fill in just enough information
	xcb_generic_error_t xcb_err;
	xcb_err.full_sequence = (uint32_t)ev->serial;
	xcb_err.major_code = ev->request_code;
	xcb_err.minor_code = ev->minor_code;
	xcb_err.error_code = ev->error_code;
	x_handle_error(&ps_g->c, &xcb_err);
	return 0;
}

/// Initialize x_connection struct from an Xlib Display.
///
/// Note this function doesn't take ownership of the Display, the caller is still
/// responsible for closing it after `free_x_connection` is called.
void x_connection_init(struct x_connection *c, Display *dpy) {
	c->dpy = dpy;
	c->c = XGetXCBConnection(dpy);
	list_init_head(&c->pending_x_errors);
	list_init_head(&c->pending_x_requests);
	c->previous_xerror_handler = XSetErrorHandler(xerror);

	c->screen = DefaultScreen(dpy);
	c->screen_info = xcb_aux_get_screen(c->c, c->screen);
	c->message_on_hold = NULL;

	// Do a round trip to fetch the current sequence number
	auto cookie = xcb_get_input_focus(c->c);
	free(xcb_get_input_focus_reply(c->c, cookie, NULL));
	c->last_sequence = cookie.sequence;
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
winprop_t x_get_prop_with_offset(const struct x_connection *c, xcb_window_t w, xcb_atom_t atom,
                                 int offset, int length, xcb_atom_t rtype, int rformat) {
	xcb_get_property_reply_t *r = xcb_get_property_reply(
	    c->c,
	    xcb_get_property(c->c, 0, w, atom, rtype, to_u32_checked(offset),
	                     to_u32_checked(length)),
	    NULL);

	if (r && xcb_get_property_value_length(r) &&
	    (rtype == XCB_GET_PROPERTY_TYPE_ANY || r->type == rtype) &&
	    (!rformat || r->format == rformat) &&
	    (r->format == 8 || r->format == 16 || r->format == 32)) {
		auto len = xcb_get_property_value_length(r);
		return (winprop_t){
		    .ptr = xcb_get_property_value(r),
		    .nitems = (ulong)(len / (r->format / 8)),
		    .type = r->type,
		    .format = r->format,
		    .r = r,
		};
	}

	free(r);
	return (winprop_t){
	    .ptr = NULL, .nitems = 0, .type = XCB_GET_PROPERTY_TYPE_ANY, .format = 0};
}

/// Get the type, format and size in bytes of a window's specific attribute.
winprop_info_t x_get_prop_info(const struct x_connection *c, xcb_window_t w, xcb_atom_t atom) {
	xcb_generic_error_t *e = NULL;
	auto r = xcb_get_property_reply(
	    c->c, xcb_get_property(c->c, 0, w, atom, XCB_ATOM_ANY, 0, 0), &e);
	if (!r) {
		log_debug_x_error(e, "Failed to get property info for window %#010x", w);
		free(e);
		return (winprop_info_t){
		    .type = XCB_GET_PROPERTY_TYPE_ANY, .format = 0, .length = 0};
	}

	winprop_info_t winprop_info = {
	    .type = r->type, .format = r->format, .length = r->bytes_after};
	free(r);

	return winprop_info;
}

/**
 * Get the value of a type-<code>xcb_window_t</code> property of a window.
 *
 * @return the value if successful, 0 otherwise
 */
xcb_window_t wid_get_prop_window(struct x_connection *c, xcb_window_t wid, xcb_atom_t aprop) {
	// Get the attribute
	xcb_window_t p = XCB_NONE;
	winprop_t prop = x_get_prop(c, wid, aprop, 1L, XCB_ATOM_WINDOW, 32);

	// Return it
	if (prop.nitems) {
		p = (xcb_window_t)*prop.p32;
	}

	free_winprop(&prop);

	return p;
}

/**
 * Get the value of a text property of a window.
 */
bool wid_get_text_prop(struct x_connection *c, struct atom *atoms, xcb_window_t wid,
                       xcb_atom_t prop, char ***pstrlst, int *pnstr) {
	auto prop_info = x_get_prop_info(c, wid, prop);
	auto type = prop_info.type;
	auto format = prop_info.format;
	auto length = prop_info.length;

	if (type == XCB_ATOM_NONE) {
		return false;
	}

	if (!x_is_type_string(atoms, type)) {
		log_warn("Text property %d of window %#010x has unsupported type: %d",
		         prop, wid, type);
		return false;
	}

	if (format != 8) {
		log_warn("Text property %d of window %#010x has unexpected format: %d",
		         prop, wid, format);
		return false;
	}

	xcb_generic_error_t *e = NULL;
	auto word_count = (length + 4 - 1) / 4;
	auto r = xcb_get_property_reply(
	    c->c, xcb_get_property(c->c, 0, wid, prop, type, 0, word_count), &e);
	if (!r) {
		log_debug_x_error(e, "Failed to get window property for %#010x", wid);
		free(e);
		return false;
	}

	assert(length == (uint32_t)xcb_get_property_value_length(r));

	void *data = xcb_get_property_value(r);
	unsigned int nstr = 0;
	uint32_t current_offset = 0;
	while (current_offset < length) {
		current_offset +=
		    (uint32_t)strnlen(data + current_offset, length - current_offset) + 1;
		nstr += 1;
	}

	if (nstr == 0) {
		// The property is set to an empty string, in that case, we return one
		// string
		char **strlst = malloc(sizeof(char *));
		strlst[0] = "";
		*pnstr = 1;
		*pstrlst = strlst;
		free(r);
		return true;
	}

	// Allocate the pointers and the strings together
	void *buf = NULL;
	if (posix_memalign(&buf, alignof(char *), length + sizeof(char *) * nstr + 1) != 0) {
		abort();
	}

	char *strlst = buf + sizeof(char *) * nstr;
	memcpy(strlst, xcb_get_property_value(r), length);
	strlst[length] = '\0';        // X strings aren't guaranteed to be null terminated

	char **ret = buf;
	current_offset = 0;
	nstr = 0;
	while (current_offset < length) {
		ret[nstr] = strlst + current_offset;
		current_offset += (uint32_t)strlen(strlst + current_offset) + 1;
		nstr += 1;
	}

	*pnstr = to_int_checked(nstr);
	*pstrlst = ret;
	free(r);
	return true;
}

// A cache of pict formats. We assume they don't change during the lifetime
// of this program
static thread_local xcb_render_query_pict_formats_reply_t *g_pictfmts = NULL;

static inline void x_get_server_pictfmts(struct x_connection *c) {
	if (g_pictfmts) {
		return;
	}
	xcb_generic_error_t *e = NULL;
	// Get window picture format
	g_pictfmts = xcb_render_query_pict_formats_reply(
	    c->c, xcb_render_query_pict_formats(c->c), &e);
	if (e || !g_pictfmts) {
		log_fatal("failed to get pict formats\n");
		abort();
	}
}

const xcb_render_pictforminfo_t *
x_get_pictform_for_visual(struct x_connection *c, xcb_visualid_t visual) {
	x_get_server_pictfmts(c);

	xcb_render_pictvisual_t *pv = xcb_render_util_find_visual_format(g_pictfmts, visual);
	for (xcb_render_pictforminfo_iterator_t i =
	         xcb_render_query_pict_formats_formats_iterator(g_pictfmts);
	     i.rem; xcb_render_pictforminfo_next(&i)) {
		if (i.data->id == pv->format) {
			return i.data;
		}
	}
	return NULL;
}

static xcb_visualid_t attr_pure x_get_visual_for_pictfmt(xcb_render_query_pict_formats_reply_t *r,
                                                         xcb_render_pictformat_t fmt) {
	for (auto screen = xcb_render_query_pict_formats_screens_iterator(r); screen.rem;
	     xcb_render_pictscreen_next(&screen)) {
		for (auto depth = xcb_render_pictscreen_depths_iterator(screen.data);
		     depth.rem; xcb_render_pictdepth_next(&depth)) {
			for (auto pv = xcb_render_pictdepth_visuals_iterator(depth.data);
			     pv.rem; xcb_render_pictvisual_next(&pv)) {
				if (pv.data->format == fmt) {
					return pv.data->visual;
				}
			}
		}
	}
	return XCB_NONE;
}

xcb_visualid_t x_get_visual_for_standard(struct x_connection *c, xcb_pict_standard_t std) {
	x_get_server_pictfmts(c);

	auto pictfmt = xcb_render_util_find_standard_format(g_pictfmts, std);

	return x_get_visual_for_pictfmt(g_pictfmts, pictfmt->id);
}

xcb_visualid_t x_get_visual_for_depth(xcb_screen_t *screen, uint8_t depth) {
	xcb_depth_iterator_t depth_it = xcb_screen_allowed_depths_iterator(screen);
	for (; depth_it.rem; xcb_depth_next(&depth_it)) {
		if (depth_it.data->depth == depth) {
			return xcb_depth_visuals_iterator(depth_it.data).data->visual_id;
		}
	}

	return XCB_NONE;
}

xcb_render_pictformat_t
x_get_pictfmt_for_standard(struct x_connection *c, xcb_pict_standard_t std) {
	x_get_server_pictfmts(c);

	auto pictfmt = xcb_render_util_find_standard_format(g_pictfmts, std);

	return pictfmt->id;
}

xcb_render_picture_t
x_create_picture_with_pictfmt_and_pixmap(struct x_connection *c, xcb_render_pictformat_t pictfmt,
                                         xcb_pixmap_t pixmap, uint32_t valuemask,
                                         const xcb_render_create_picture_value_list_t *attr) {
	void *buf = NULL;
	if (attr) {
		xcb_render_create_picture_value_list_serialize(&buf, valuemask, attr);
		if (!buf) {
			log_error("failed to serialize picture attributes");
			return XCB_NONE;
		}
	}

	xcb_render_picture_t tmp_picture = x_new_id(c);
	xcb_generic_error_t *e =
	    xcb_request_check(c->c, xcb_render_create_picture_checked(
	                                c->c, tmp_picture, pixmap, pictfmt, valuemask, buf));
	free(buf);
	if (e) {
		log_error_x_error(e, "failed to create picture");
		free(e);
		abort();
		return XCB_NONE;
	}
	return tmp_picture;
}

xcb_render_picture_t
x_create_picture_with_visual_and_pixmap(struct x_connection *c, xcb_visualid_t visual,
                                        xcb_pixmap_t pixmap, uint32_t valuemask,
                                        const xcb_render_create_picture_value_list_t *attr) {
	const xcb_render_pictforminfo_t *pictfmt = x_get_pictform_for_visual(c, visual);
	return x_create_picture_with_pictfmt_and_pixmap(c, pictfmt->id, pixmap, valuemask, attr);
}

xcb_render_picture_t
x_create_picture_with_standard_and_pixmap(struct x_connection *c, xcb_pict_standard_t standard,
                                          xcb_pixmap_t pixmap, uint32_t valuemask,
                                          const xcb_render_create_picture_value_list_t *attr) {
	x_get_server_pictfmts(c);

	auto pictfmt = xcb_render_util_find_standard_format(g_pictfmts, standard);
	assert(pictfmt);
	return x_create_picture_with_pictfmt_and_pixmap(c, pictfmt->id, pixmap, valuemask, attr);
}

xcb_render_picture_t
x_create_picture_with_standard(struct x_connection *c, int w, int h,
                               xcb_pict_standard_t standard, uint32_t valuemask,
                               const xcb_render_create_picture_value_list_t *attr) {
	x_get_server_pictfmts(c);

	auto pictfmt = xcb_render_util_find_standard_format(g_pictfmts, standard);
	assert(pictfmt);
	return x_create_picture_with_pictfmt(c, w, h, pictfmt->id, pictfmt->depth,
	                                     valuemask, attr);
}

/**
 * Create an picture.
 */
xcb_render_picture_t
x_create_picture_with_pictfmt(struct x_connection *c, int w, int h,
                              xcb_render_pictformat_t pictfmt, uint8_t depth, uint32_t valuemask,
                              const xcb_render_create_picture_value_list_t *attr) {

	xcb_pixmap_t tmp_pixmap = x_create_pixmap(c, depth, w, h);
	if (!tmp_pixmap) {
		return XCB_NONE;
	}

	xcb_render_picture_t picture = x_create_picture_with_pictfmt_and_pixmap(
	    c, pictfmt, tmp_pixmap, valuemask, attr);

	x_set_error_action_abort(c, xcb_free_pixmap(c->c, tmp_pixmap));

	return picture;
}

xcb_render_picture_t
x_create_picture_with_visual(struct x_connection *c, int w, int h, xcb_visualid_t visual,
                             uint32_t valuemask,
                             const xcb_render_create_picture_value_list_t *attr) {
	auto pictfmt = x_get_pictform_for_visual(c, visual);
	return x_create_picture_with_pictfmt(c, w, h, pictfmt->id, pictfmt->depth,
	                                     valuemask, attr);
}

bool x_fetch_region(struct x_connection *c, xcb_xfixes_region_t r, pixman_region32_t *res) {
	xcb_generic_error_t *e = NULL;
	xcb_xfixes_fetch_region_reply_t *xr =
	    xcb_xfixes_fetch_region_reply(c->c, xcb_xfixes_fetch_region(c->c, r), &e);
	if (!xr) {
		log_error_x_error(e, "Failed to fetch rectangles");
		return false;
	}

	int nrect = xcb_xfixes_fetch_region_rectangles_length(xr);
	auto b = ccalloc(nrect, pixman_box32_t);
	xcb_rectangle_t *xrect = xcb_xfixes_fetch_region_rectangles(xr);
	for (int i = 0; i < nrect; i++) {
		b[i] = (pixman_box32_t){.x1 = xrect[i].x,
		                        .y1 = xrect[i].y,
		                        .x2 = xrect[i].x + xrect[i].width,
		                        .y2 = xrect[i].y + xrect[i].height};
	}
	bool ret = pixman_region32_init_rects(res, b, nrect);
	free(b);
	free(xr);
	return ret;
}

bool x_set_region(struct x_connection *c, xcb_xfixes_region_t dst, const region_t *src) {
	if (!src || dst == XCB_NONE) {
		return false;
	}

	int32_t nrects = 0;
	const rect_t *rects = pixman_region32_rectangles((region_t *)src, &nrects);
	if (!rects || nrects < 1) {
		return false;
	}

	xcb_rectangle_t *xrects = ccalloc(nrects, xcb_rectangle_t);
	for (int32_t i = 0; i < nrects; i++) {
		xrects[i] =
		    (xcb_rectangle_t){.x = to_i16_checked(rects[i].x1),
		                      .y = to_i16_checked(rects[i].y1),
		                      .width = to_u16_checked(rects[i].x2 - rects[i].x1),
		                      .height = to_u16_checked(rects[i].y2 - rects[i].y1)};
	}

	bool success =
	    XCB_AWAIT_VOID(xcb_xfixes_set_region, c->c, dst, to_u32_checked(nrects), xrects);

	free(xrects);

	return success;
}

uint32_t x_create_region(struct x_connection *c, const region_t *reg) {
	if (!reg) {
		return XCB_NONE;
	}

	int nrects;
	// In older pixman versions, pixman_region32_rectangles doesn't take const
	// region_t, instead of dealing with this version difference, just suppress the
	// warning.
	const pixman_box32_t *rects = pixman_region32_rectangles((region_t *)reg, &nrects);
	auto xrects = ccalloc(nrects, xcb_rectangle_t);
	for (int i = 0; i < nrects; i++) {
		xrects[i] =
		    (xcb_rectangle_t){.x = to_i16_checked(rects[i].x1),
		                      .y = to_i16_checked(rects[i].y1),
		                      .width = to_u16_checked(rects[i].x2 - rects[i].x1),
		                      .height = to_u16_checked(rects[i].y2 - rects[i].y1)};
	}

	xcb_xfixes_region_t ret = x_new_id(c);
	bool success = XCB_AWAIT_VOID(xcb_xfixes_create_region, c->c, ret,
	                              to_u32_checked(nrects), xrects);
	free(xrects);
	if (!success) {
		return XCB_NONE;
	}
	return ret;
}

void x_destroy_region(struct x_connection *c, xcb_xfixes_region_t r) {
	if (r != XCB_NONE) {
		x_set_error_action_debug_abort(c, xcb_xfixes_destroy_region(c->c, r));
	}
}

void x_set_picture_clip_region(struct x_connection *c, xcb_render_picture_t pict,
                               int16_t clip_x_origin, int16_t clip_y_origin,
                               const region_t *reg) {
	int nrects;
	const rect_t *rects = pixman_region32_rectangles((region_t *)reg, &nrects);
	auto xrects = ccalloc(nrects, xcb_rectangle_t);
	for (int i = 0; i < nrects; i++) {
		xrects[i] = (xcb_rectangle_t){
		    .x = to_i16_checked(rects[i].x1),
		    .y = to_i16_checked(rects[i].y1),
		    .width = to_u16_checked(rects[i].x2 - rects[i].x1),
		    .height = to_u16_checked(rects[i].y2 - rects[i].y1),
		};
	}

	xcb_generic_error_t *e =
	    xcb_request_check(c->c, xcb_render_set_picture_clip_rectangles_checked(
	                                c->c, pict, clip_x_origin, clip_y_origin,
	                                to_u32_checked(nrects), xrects));
	if (e) {
		log_error_x_error(e, "Failed to set clip region");
		free(e);
	}
	free(xrects);
}

void x_clear_picture_clip_region(struct x_connection *c, xcb_render_picture_t pict) {
	assert(pict != XCB_NONE);
	xcb_render_change_picture_value_list_t v = {.clipmask = XCB_NONE};
	xcb_generic_error_t *e = xcb_request_check(
	    c->c, xcb_render_change_picture_checked(c->c, pict, XCB_RENDER_CP_CLIP_MASK, &v));
	if (e) {
		log_error_x_error(e, "failed to clear clip region");
		free(e);
	}
}

/**
 * Destroy a <code>Picture</code>.
 *
 * Picture must be valid.
 */
void x_free_picture(struct x_connection *c, xcb_render_picture_t p) {
	assert(p != XCB_NONE);
	auto cookie = xcb_render_free_picture(c->c, p);
	x_set_error_action_debug_abort(c, cookie);
}

/*
 * Convert a xcb_generic_error_t to a string that describes the error
 *
 * @return a pointer to a string. this pointer shouldn NOT be freed, same buffer is used
 *         for multiple calls to this function,
 */
const char *x_strerror(xcb_generic_error_t *e) {
	if (!e) {
		return "No error";
	}
	return x_error_code_to_string(e->full_sequence, e->major_code, e->minor_code,
	                              e->error_code);
}

/**
 * Create a pixmap and check that creation succeeded.
 */
xcb_pixmap_t x_create_pixmap(struct x_connection *c, uint8_t depth, int width, int height) {
	xcb_pixmap_t pix = x_new_id(c);
	xcb_void_cookie_t cookie =
	    xcb_create_pixmap_checked(c->c, depth, pix, c->screen_info->root,
	                              to_u16_checked(width), to_u16_checked(height));
	xcb_generic_error_t *err = xcb_request_check(c->c, cookie);
	if (err == NULL) {
		return pix;
	}

	log_error_x_error(err, "Failed to create pixmap");
	free(err);
	return XCB_NONE;
}

/// We don't use the _XSETROOT_ID root window property as a source of the background
/// pixmap because it most likely points to a dummy pixmap used to keep the colormap
/// associated with the background pixmap alive but we listen for it's changes and update
/// the background pixmap accordingly.
///
/// For details on the _XSETROOT_ID root window property and it's usage see:
/// https://metacpan.org/pod/X11::Protocol::XSetRoot#_XSETROOT_ID
/// https://gitlab.freedesktop.org/xorg/app/xsetroot/-/blob/435d35409768de7cbc2c47a6322192dd4b480545/xsetroot.c#L318-352
/// https://github.com/ImageMagick/ImageMagick/blob/d04a47227637dbb3af9231b0107ccf9677bf985e/MagickCore/xwindow.c#L9203-L9260
/// https://github.com/ImageMagick/ImageMagick/blob/d04a47227637dbb3af9231b0107ccf9677bf985e/MagickCore/xwindow.c#L1853-L1922
/// https://www.fvwm.org/Archive/Manpages/fvwm-root.html

xcb_pixmap_t x_get_root_back_pixmap(struct x_connection *c, struct atom *atoms) {
	xcb_pixmap_t pixmap = XCB_NONE;

	xcb_atom_t root_back_pixmap_atoms[] = {atoms->a_XROOTPMAP_ID, atoms->aESETROOT_PMAP_ID};
	for (size_t i = 0; i < ARR_SIZE(root_back_pixmap_atoms); i++) {
		winprop_t prop =
		    x_get_prop(c, c->screen_info->root, root_back_pixmap_atoms[i], 1,
		               XCB_ATOM_PIXMAP, 32);
		if (prop.nitems) {
			pixmap = (xcb_pixmap_t)*prop.p32;
			free_winprop(&prop);
			break;
		}
		free_winprop(&prop);
	}

	return pixmap;
}

bool x_is_root_back_pixmap_atom(struct atom *atoms, xcb_atom_t atom) {
	return atom == atoms->a_XROOTPMAP_ID || atom == atoms->aESETROOT_PMAP_ID ||
	       atom == atoms->a_XSETROOT_ID;
}

/**
 * Synchronizes a X Render drawable to ensure all pending painting requests
 * are completed.
 */
bool x_fence_sync(struct x_connection *c, xcb_sync_fence_t f) {
	// TODO(richardgv): If everybody just follows the rules stated in X Sync
	// prototype, we need only one fence per screen, but let's stay a bit
	// cautious right now

	auto e = xcb_request_check(c->c, xcb_sync_trigger_fence_checked(c->c, f));
	if (e) {
		log_error_x_error(e, "Failed to trigger the fence");
		goto err;
	}

	e = xcb_request_check(c->c, xcb_sync_await_fence_checked(c->c, 1, &f));
	if (e) {
		log_error_x_error(e, "Failed to await on a fence");
		goto err;
	}

	e = xcb_request_check(c->c, xcb_sync_reset_fence_checked(c->c, f));
	if (e) {
		log_error_x_error(e, "Failed to reset the fence");
		goto err;
	}
	return true;

err:
	free(e);
	return false;
}

void x_request_vblank_event(struct x_connection *c, xcb_window_t window, uint64_t msc) {
	auto cookie = xcb_present_notify_msc(c->c, window, 0, msc, 1, 0);
	x_set_error_action_abort(c, cookie);
}

/**
 * Convert a struct conv to a X picture convolution filter, normalizing the kernel
 * in the process. Allow the caller to specify the element at the center of the kernel,
 * for compatibility with legacy code.
 *
 * @param[in] kernel the convolution kernel
 * @param[in] center the element to put at the center of the matrix
 * @param[inout] ret pointer to an array of `size`, if `size` is too small, more space
 *                   will be allocated, and `*ret` will be updated
 * @param[inout] size size of the array pointed to by `ret`, in number of elements
 * @return number of elements filled into `*ret`
 */
void x_create_convolution_kernel(const conv *kernel, double center,
                                 struct x_convolution_kernel **ret) {
	assert(ret);
	if (!*ret || (*ret)->capacity < kernel->w * kernel->h + 2) {
		free(*ret);
		*ret =
		    cvalloc(sizeof(struct x_convolution_kernel) +
		            (size_t)(kernel->w * kernel->h + 2) * sizeof(xcb_render_fixed_t));
		(*ret)->capacity = kernel->w * kernel->h + 2;
	}

	(*ret)->size = kernel->w * kernel->h + 2;

	auto buf = (*ret)->kernel;
	buf[0] = DOUBLE_TO_XFIXED(kernel->w);
	buf[1] = DOUBLE_TO_XFIXED(kernel->h);

	double sum = center;
	bool has_neg = false;
	for (int i = 0; i < kernel->w * kernel->h; i++) {
		if (i == kernel->w * kernel->h / 2) {
			// Ignore center
			continue;
		}
		sum += kernel->data[i];
		if (kernel->data[i] < 0 && !has_neg) {
			has_neg = true;
			log_warn("A X convolution kernel with negative values may not "
			         "work properly.");
		}
	}

	// Note for floating points a / b != a * (1 / b), but this shouldn't have any real
	// impact on the result
	double factor = sum != 0 ? 1.0 / sum : 1;
	for (int i = 0; i < kernel->w * kernel->h; i++) {
		buf[i + 2] = DOUBLE_TO_XFIXED(kernel->data[i] * factor);
	}

	buf[kernel->h / 2 * kernel->w + kernel->w / 2 + 2] =
	    DOUBLE_TO_XFIXED(center * factor);
}

/// Generate a search criteria for fbconfig from a X visual.
/// Returns {-1, -1, -1, -1, -1, 0} on failure
struct xvisual_info x_get_visual_info(struct x_connection *c, xcb_visualid_t visual) {
	auto pictfmt = x_get_pictform_for_visual(c, visual);
	auto depth = xcb_aux_get_depth_of_visual(c->screen_info, visual);
	if (!pictfmt || depth == 0) {
		log_error("Invalid visual %#03x", visual);
		return (struct xvisual_info){-1, -1, -1, -1, -1, 0};
	}
	if (pictfmt->type != XCB_RENDER_PICT_TYPE_DIRECT) {
		log_error("We cannot handle non-DirectColor visuals. Report an "
		          "issue if you see this error message.");
		return (struct xvisual_info){-1, -1, -1, -1, -1, 0};
	}

	int red_size = popcntul(pictfmt->direct.red_mask),
	    blue_size = popcntul(pictfmt->direct.blue_mask),
	    green_size = popcntul(pictfmt->direct.green_mask),
	    alpha_size = popcntul(pictfmt->direct.alpha_mask);

	return (struct xvisual_info){
	    .red_size = red_size,
	    .green_size = green_size,
	    .blue_size = blue_size,
	    .alpha_size = alpha_size,
	    .visual_depth = depth,
	    .visual = visual,
	};
}

void x_update_monitors(struct x_connection *c, struct x_monitors *m) {
	x_free_monitor_info(m);

	xcb_randr_get_monitors_reply_t *r = xcb_randr_get_monitors_reply(
	    c->c, xcb_randr_get_monitors(c->c, c->screen_info->root, true), NULL);
	if (!r) {
		return;
	}

	m->count = xcb_randr_get_monitors_monitors_length(r);
	m->regions = ccalloc(m->count, region_t);
	xcb_randr_monitor_info_iterator_t monitor_info_it =
	    xcb_randr_get_monitors_monitors_iterator(r);
	for (int i = 0; monitor_info_it.rem; xcb_randr_monitor_info_next(&monitor_info_it)) {
		xcb_randr_monitor_info_t *mi = monitor_info_it.data;
		pixman_region32_init_rect(&m->regions[i++], mi->x, mi->y, mi->width, mi->height);
	}

	free(r);
}

void x_free_monitor_info(struct x_monitors *m) {
	if (m->regions) {
		for (int i = 0; i < m->count; i++) {
			pixman_region32_fini(&m->regions[i]);
		}
		free(m->regions);
		m->regions = NULL;
	}
	m->count = 0;
}

static uint32_t x_get_full_sequence(struct x_connection *c, uint16_t sequence) {
	auto last_sequence_low = c->last_sequence & 0xffff;
	// sequence < last_sequence16 means the lower 16 bits overflowed, which should
	// carry to the higher 16 bits
	auto sequence_high = c->last_sequence & 0xffff0000;
	if (sequence < last_sequence_low) {
		sequence_high += 0x10000;
	}
	return sequence_high | sequence;
}

static int64_t x_compare_sequence(struct x_connection *c, uint32_t a, uint32_t b) {
	bool a_overflown = a < c->last_sequence, b_overflown = b < c->last_sequence;
	if (a_overflown == b_overflown) {
		return (int64_t)a - (int64_t)b;
	}
	return a_overflown ? 1 : -1;
}

static xcb_raw_generic_event_t *
x_poll_for_event_impl(struct x_connection *c, struct x_async_request_base **out_req) {
	struct x_async_request_base *first_pending_request = NULL;
	if (!list_is_empty(&c->pending_x_requests)) {
		first_pending_request = list_entry(c->pending_x_requests.next,
		                                   struct x_async_request_base, siblings);
	}

	bool on_hold_is_reply;
	if (c->message_on_hold == NULL) {
		// Nothing on hold, we need to read new information from the X connection.
		// We must only read from the X connection once in this function to keep
		// things consistent. The only way to do  that is reading the connection
		// with `xcb_poll_for_reply`, and then check for events with
		// `xcb_poll_for_queued_event`, because there is no
		// `xcb_poll_for_queued_reply`. Unless we are not waiting for any replies,
		// in which case a simple `xcb_poll_for_event` is enough.
		if (first_pending_request != NULL) {
			xcb_generic_error_t *err = NULL;
			on_hold_is_reply =
			    xcb_poll_for_reply(c->c, first_pending_request->sequence,
			                       (void **)&c->message_on_hold, &err) == 1;
			if (err != NULL) {
				c->message_on_hold = (xcb_raw_generic_event_t *)err;
			}
			if (!on_hold_is_reply) {
				// We didn't get a reply, but did we get an event?
				c->message_on_hold =
				    (xcb_raw_generic_event_t *)xcb_poll_for_queued_event(c->c);
			}
		} else {
			c->message_on_hold =
			    (xcb_raw_generic_event_t *)xcb_poll_for_event(c->c);
			on_hold_is_reply = false;
		}
	} else if (first_pending_request != NULL) {
		// response_type 0 is error, 1 is reply.
		on_hold_is_reply = c->message_on_hold->response_type < 2 &&
		                   x_get_full_sequence(c, c->message_on_hold->sequence) ==
		                       first_pending_request->sequence;
	} else {
		on_hold_is_reply = false;
	}
	if (c->message_on_hold == NULL) {
		// Didn't get any new information from the X connection, nothing to
		// return.
		return NULL;
	}

	// From this point, no more reading from the X connection is allowed.
	xcb_generic_event_t *next_event = NULL;
	if (on_hold_is_reply) {
		next_event = xcb_poll_for_queued_event(c->c);
		assert(next_event == NULL || next_event->response_type != 1);
	} else {
		next_event = (xcb_generic_event_t *)c->message_on_hold;
	}

	// `next_event == c->message_on_hold` iff `on_hold_is_reply` is false.

	bool should_return_event = false;
	if (first_pending_request == NULL) {
		// Here `on_hold_is_reply` must be false, therefore `next_event ==
		// c->message_on_hold` must be true, therefore `next_event` cannot be
		// NULL.
		should_return_event = true;
	} else if (next_event != NULL) {
		auto ordering = x_compare_sequence(c, next_event->full_sequence,
		                                   first_pending_request->sequence);
		// If next_event is a true event, it might share a sequence number with a
		// reply. But if it's an error (i.e. response_type == 0), its sequence
		// number must be different from any reply.
		assert(next_event->response_type != 0 || ordering != 0);
		should_return_event = ordering < 0;
	}

	if (should_return_event) {
		x_discard_pending_errors(c, next_event->full_sequence);
		c->last_sequence = next_event->full_sequence;
		if (!on_hold_is_reply) {
			c->message_on_hold = NULL;
		}
		return (xcb_raw_generic_event_t *)next_event;
	}

	// We should return the reply to the first pending request.
	xcb_raw_generic_event_t *ret = NULL;
	if (!on_hold_is_reply) {
		xcb_generic_error_t *err = NULL;
		// This is a very special case. Because we have already received an event
		// with a greater or equal sequence number than the reply, we _know_ the
		// reply must also have already arrived. We can safely call
		// `xcb_poll_for_reply` here because we know it will not read from the X
		// connection again.
		BUG_ON(xcb_poll_for_reply(c->c, first_pending_request->sequence,
		                          (void **)&ret, &err) == 0);
		if (err != NULL) {
			ret = (xcb_raw_generic_event_t *)err;
		}
	} else {
		ret = c->message_on_hold;
		c->message_on_hold = (xcb_raw_generic_event_t *)next_event;
	}

	x_discard_pending_errors(c, first_pending_request->sequence + 1);
	c->last_sequence = first_pending_request->sequence;
	*out_req = first_pending_request;
	list_remove(&first_pending_request->siblings);
	return ret;
}

xcb_generic_event_t *x_poll_for_event(struct x_connection *c) {
	xcb_raw_generic_event_t *ret = NULL;
	while (true) {
		struct x_async_request_base *req = NULL;
		ret = x_poll_for_event_impl(c, &req);
		if (ret == NULL) {
			break;
		}

		if (req != NULL) {
			req->callback(c, req, ret);
		} else if (ret->response_type == 0) {
			x_handle_error(c, (xcb_generic_error_t *)ret);
		} else {
			break;
		}
		free(ret);
	}
	return (xcb_generic_event_t *)ret;
}

void x_cancel_request(struct x_connection *c, struct x_async_request_base *req) {
	list_remove(&req->siblings);
	if (c->message_on_hold == NULL) {
		return;
	}
	if (c->message_on_hold->response_type >= 2 ||
	    x_get_full_sequence(c, c->message_on_hold->sequence) != req->sequence) {
		return;
	}
	free(c->message_on_hold);
	c->message_on_hold = NULL;
}
