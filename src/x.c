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

static inline uint64_t x_widen_sequence(struct x_connection *c, uint32_t sequence) {
	if (sequence < c->last_sequence) {
		// The sequence number has wrapped around
		return (uint64_t)sequence + UINT32_MAX + 1;
	}
	return (uint64_t)sequence;
}

// === Error handling ===

/// Discard pending error handlers.
///
/// We have received reply with sequence number `sequence`, which means all pending
/// replies with sequence number strictly less than `sequence` will never be received. So
/// discard them.
static void x_discard_pending_errors(struct x_connection *c, uint64_t sequence) {
	list_foreach_safe(struct pending_x_error, i, &c->pending_x_errors, siblings) {
		if (x_widen_sequence(c, i->sequence) >= sequence) {
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
xcb_window_t wid_get_prop_window(struct x_connection *c, xcb_window_t wid,
                                 xcb_atom_t aprop, bool *exists) {
	// Get the attribute
	xcb_window_t p = XCB_NONE;
	winprop_t prop = x_get_prop(c, wid, aprop, 1L, XCB_ATOM_WINDOW, 32);

	// Return it
	if (prop.nitems) {
		*exists = true;
		p = (xcb_window_t)*prop.p32;
	} else {
		*exists = false;
	}

	free_winprop(&prop);

	return p;
}

/**
 * Get the value of a text property of a window.
 */
bool wid_get_text_prop(struct x_connection *c, struct atom *atoms, xcb_window_t wid,
                       xcb_atom_t prop, char ***pstrlst, int *pnstr) {
	xcb_generic_error_t *e = NULL;
	auto r = xcb_get_property_reply(
	    c->c, xcb_get_property(c->c, 0, wid, prop, XCB_ATOM_ANY, 0, UINT_MAX), &e);
	if (!r) {
		log_debug_x_error(e, "Failed to get window property for %#010x", wid);
		free(e);
		return false;
	}

	if (r->type == XCB_ATOM_NONE) {
		free(r);
		return false;
	}

	if (!x_is_type_string(atoms, r->type)) {
		log_warn("Text property %d of window %#010x has unsupported type: %d",
		         prop, wid, r->type);
		free(r);
		return false;
	}

	if (r->format != 8) {
		log_warn("Text property %d of window %#010x has unexpected format: %d",
		         prop, wid, r->format);
		free(r);
		return false;
	}

	uint32_t length = to_u32_checked(xcb_get_property_value_length(r));
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

bool wid_get_opacity_prop(struct x_connection *c, struct atom *atoms, xcb_window_t wid,
                          opacity_t def, opacity_t *out) {
	bool ret = false;
	*out = def;

	winprop_t prop =
	    x_get_prop(c, wid, atoms->a_NET_WM_WINDOW_OPACITY, 1L, XCB_ATOM_CARDINAL, 32);

	if (prop.nitems) {
		*out = *prop.c32;
		ret = true;
	}

	free_winprop(&prop);

	return ret;
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

void x_async_change_window_attributes(struct x_connection *c, xcb_window_t wid,
                                      uint32_t mask, const uint32_t *values,
                                      struct x_async_request_base *req) {
	req->sequence = xcb_change_window_attributes(c->c, wid, mask, values).sequence;
	req->no_reply = true;
	x_await_request(c, req);
}

void x_async_query_tree(struct x_connection *c, xcb_window_t wid,
                        struct x_async_request_base *req) {
	req->sequence = xcb_query_tree(c->c, wid).sequence;
	x_await_request(c, req);
}

void x_async_get_property(struct x_connection *c, xcb_window_t wid, xcb_atom_t atom,
                          xcb_atom_t type, uint32_t long_offset, uint32_t long_length,
                          struct x_async_request_base *req) {
	req->sequence =
	    xcb_get_property(c->c, 0, wid, atom, type, long_offset, long_length).sequence;
	x_await_request(c, req);
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
const char *x_strerror(const xcb_generic_error_t *e) {
	if (!e) {
		return "No error";
	}
	return x_error_code_to_string(e->full_sequence, e->major_code, e->minor_code,
	                              e->error_code);
}

void x_flush(struct x_connection *c) {
	xcb_flush(c->c);
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

struct x_update_monitors_request {
	struct x_async_request_base base;
	struct x_monitors *monitors;
};

static void x_handle_update_monitors_reply(struct x_connection * /*c*/,
                                           struct x_async_request_base *req_base,
                                           const xcb_raw_generic_event_t *reply_or_error) {
	auto m = ((struct x_update_monitors_request *)req_base)->monitors;
	free(req_base);

	if (reply_or_error == NULL) {
		// Shutting down
		return;
	}

	if (reply_or_error->response_type == 0) {
		log_warn("Failed to get monitor information using RandR: %s",
		         x_strerror((xcb_generic_error_t *)reply_or_error));
		return;
	}

	x_free_monitor_info(m);

	auto reply = (const xcb_randr_get_monitors_reply_t *)reply_or_error;

	m->count = xcb_randr_get_monitors_monitors_length(reply);
	m->regions = ccalloc(m->count, region_t);
	xcb_randr_monitor_info_iterator_t monitor_info_it =
	    xcb_randr_get_monitors_monitors_iterator(reply);
	for (int i = 0; monitor_info_it.rem; xcb_randr_monitor_info_next(&monitor_info_it)) {
		xcb_randr_monitor_info_t *mi = monitor_info_it.data;
		pixman_region32_init_rect(&m->regions[i++], mi->x, mi->y, mi->width, mi->height);
	}
}

void x_update_monitors_async(struct x_connection *c, struct x_monitors *m) {
	auto req = ccalloc(1, struct x_update_monitors_request);
	req->base.callback = x_handle_update_monitors_reply;
	req->base.sequence = xcb_randr_get_monitors(c->c, c->screen_info->root, 1).sequence;
	req->monitors = m;
	x_await_request(c, &req->base);
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

static inline xcb_raw_generic_event_t *
x_ingest_event(struct x_connection *c, xcb_generic_event_t *event) {
	if (event != NULL) {
		assert(event->response_type != 1);
		uint64_t seq = x_widen_sequence(c, event->full_sequence);
		if (event->response_type != 0) {
			// For true events, we can discard pending errors with a lower or
			// equal sequence number. This is because only a reply or an error
			// increments the sequence number.
			seq += 1;
		}
		x_discard_pending_errors(c, seq);
		c->last_sequence = event->full_sequence;
	}
	return (xcb_raw_generic_event_t *)event;
}

static const xcb_raw_generic_event_t no_reply_success = {.response_type = 1};

/// Read the X connection once, and return the first event or unchecked error. If any
/// replies to pending X requests are read, they will be processed and callbacks will be
/// called.
static const xcb_raw_generic_event_t *
x_poll_for_event_impl(struct x_connection *c, bool *retry) {
	// This whole thing, this whole complexity arises from libxcb's idiotic API.
	// What we need is a call to give us the next message from X, regardless if
	// it's a reply, an error, or an event. But what we get is two different calls:
	// `xcb_poll_for_event` and `xcb_poll_for_reply`. And there is this weird
	// asymmetry: `xcb_poll_for_event` has a version that doesn't read from X, but
	// `xcb_poll_for_reply` doesn't.
	//
	// This whole thing we are doing here, is trying to emulate the desired behavior
	// with what we got.
	if (c->first_request_with_reply == NULL) {
		// No reply to wait for, just poll for events.
		BUG_ON(!list_is_empty(&c->pending_x_requests));
		return x_ingest_event(c, xcb_poll_for_event(c->c));
	}

	// Now we need to first check if a reply to `first_request_with_reply` is
	// available.
	if (c->message_on_hold == NULL) {
		xcb_generic_error_t *err = NULL;
		auto has_reply = xcb_poll_for_reply(c->c, c->first_request_with_reply->sequence,
		                                    (void *)&c->message_on_hold, &err);
		if (has_reply != 0 && c->message_on_hold == NULL) {
			c->message_on_hold = (xcb_raw_generic_event_t *)err;
		}
		c->sequence_on_hold = c->first_request_with_reply->sequence;
	}

	// Even if we don't get a reply for `first_request_with_reply`, some of the
	// `no_reply` requests might have completed. We will see if we got an event, and
	// deduce that information from that.
	auto event = xcb_poll_for_queued_event(c->c);

	// As long as we have read one reply, we could have read an indefinite number of
	// replies, so after we returned we might need to retry this function.
	*retry = c->message_on_hold != NULL;

	while (!list_is_empty(&c->pending_x_requests) &&
	       (event != NULL || c->message_on_hold != NULL)) {
		auto head = list_entry(c->pending_x_requests.next,
		                       struct x_async_request_base, siblings);
		auto head_seq = x_widen_sequence(c, head->sequence);
		auto event_seq = UINT64_MAX;
		if (event != NULL) {
			event_seq = x_widen_sequence(c, event->full_sequence);
			BUG_ON(event->response_type == 1);
			if (head_seq > event_seq) {
				// The event comes before the first pending request, we
				// can return it first.
				break;
			}
		}

		// event == NULL || head_seq <= event_seq
		// If event == NULL, we have message_on_hold != NULL. And message_on_hold
		// has a sequence number equals to first_request_with_reply, which must be
		// greater or equal to head_seq; If event != NULL, we have head_seq <=
		// event_seq. Either case indicates `head` has completed, so we can remove
		// it from the list.
		list_remove(&head->siblings);
		if (c->first_request_with_reply == head) {
			BUG_ON(head->no_reply);
			c->first_request_with_reply = NULL;
			list_foreach(struct x_async_request_base, i,
			             &c->pending_x_requests, siblings) {
				if (!i->no_reply) {
					c->first_request_with_reply = i;
					break;
				}
			}
		}
		x_discard_pending_errors(c, head_seq);
		c->last_sequence = head->sequence;

		if (event != NULL) {
			if (event->response_type == 0 && head_seq == event_seq) {
				// `event` is an error response to `head`. `head` must be
				// `no_reply`, otherwise its error will be returned by
				// `xcb_poll_for_reply`.
				BUG_ON(!head->no_reply);
				head->callback(c, head, (xcb_raw_generic_event_t *)event);
				free(event);

				event = xcb_poll_for_queued_event(c->c);
				continue;
			}

			// Here, we have 2 cases:
			//    a. event is an error && head_seq < event_seq
			//    b. event is a true event && head_seq <= event_seq
			// In either case, we know `head` has completed. If it has a
			// reply, the reply should be in xcb's queue. So we can call
			// `xcb_poll_for_reply` safely without reading from X.
			if (head->no_reply) {
				head->callback(c, head, &no_reply_success);
				continue;
			}
			if (c->message_on_hold == NULL) {
				xcb_generic_error_t *err = NULL;
				BUG_ON(xcb_poll_for_reply(c->c, head->sequence,
				                          (void *)&c->message_on_hold,
				                          NULL) == 0);
				if (c->message_on_hold == NULL) {
					c->message_on_hold = (xcb_raw_generic_event_t *)err;
				}
				c->sequence_on_hold = head->sequence;
			}
			BUG_ON(c->sequence_on_hold != head->sequence);
			head->callback(c, head, c->message_on_hold);
			free(c->message_on_hold);
			c->message_on_hold = NULL;
		}
		// event == NULL => c->message_on_hold != NULL
		else if (x_widen_sequence(c, c->sequence_on_hold) > head_seq) {
			BUG_ON(!head->no_reply);
			head->callback(c, head, &no_reply_success);
		} else {
			BUG_ON(c->sequence_on_hold != head->sequence);
			BUG_ON(head->no_reply);
			head->callback(c, head, c->message_on_hold);
			free(c->message_on_hold);
			c->message_on_hold = NULL;
		}
	}

	return x_ingest_event(c, event);
}

static void
x_dummy_async_callback(struct x_connection * /*c*/, struct x_async_request_base *req_base,
                       const xcb_raw_generic_event_t * /*reply_or_error*/) {
	free(req_base);
}

xcb_generic_event_t *x_poll_for_event(struct x_connection *c) {
	const xcb_raw_generic_event_t *ret = NULL;
	while (true) {
		if (c->first_request_with_reply == NULL &&
		    !list_is_empty(&c->pending_x_requests)) {
			// All requests we are waiting for are no_reply. We would have no
			// idea if any of them completed, until a subsequent event is
			// received, which can take an indefinite amount of time. So we
			// insert a GetInputFocus request to ensure we get a reply.
			auto req = ccalloc(1, struct x_async_request_base);
			req->sequence = xcb_get_input_focus(c->c).sequence;
			req->callback = x_dummy_async_callback;
			x_await_request(c, req);
		}
		bool retry = false;
		ret = x_poll_for_event_impl(c, &retry);
		if (ret == NULL && !list_is_empty(&c->pending_x_requests) && retry) {
			continue;
		}
		if (ret == NULL || ret->response_type != 0) {
			break;
		}

		x_handle_error(c, (xcb_generic_error_t *)ret);
		free((void *)ret);
	}
	return (xcb_generic_event_t *)ret;
}
