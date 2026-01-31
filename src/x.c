// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#include <stdalign.h>
#include <stdbool.h>
#include <stdlib.h>

#include <X11/Xlib-xcb.h>
#include <X11/Xutil.h>
#include <X11/extensions/syncconst.h>
#include <pixman.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/glx.h>
#include <xcb/present.h>
#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/shm.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xcb_util.h>
#include <xcb/xcbext.h>
#include <xcb/xfixes.h>
#include <xcb/xproto.h>

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

/// Convert a X11 error to string
///
/// @return a pointer to a string. this pointer shouldn NOT be freed, same buffer is used
///         for multiple calls to this function,
static const char *x_error_code_to_string(struct x_connection *c, unsigned long serial,
                                          uint8_t major, uint16_t minor, uint8_t error_code) {
	// NOLINTBEGIN(bugprone-switch-missing-default-case)
	int o = 0;
	const char *name = "Unknown";

#define CASESTRRET(s)                                                                    \
	case s: name = #s; break

#define CASESTRRET2(s)                                                                   \
	case XCB_##s: name = #s; break

	o = error_code - c->e.fixes_error;
	switch (o) { CASESTRRET2(XFIXES_BAD_REGION); }

	o = error_code - c->e.damage_error;
	switch (o) { CASESTRRET2(DAMAGE_BAD_DAMAGE); }

	o = error_code - c->e.shm_error;
	switch (o) { CASESTRRET2(SHM_BAD_SEG); }

	o = error_code - c->e.render_error;
	switch (o) {
		CASESTRRET2(RENDER_PICT_FORMAT);
		CASESTRRET2(RENDER_PICTURE);
		CASESTRRET2(RENDER_PICT_OP);
		CASESTRRET2(RENDER_GLYPH_SET);
		CASESTRRET2(RENDER_GLYPH);
	}

	if (c->e.has_glx) {
		o = error_code - c->e.glx_error;
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

	if (c->e.has_sync) {
		o = error_code - c->e.sync_error;
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
	// NOLINTEND(bugprone-switch-missing-default-case)
}

void x_print_error_impl(struct x_connection *c, unsigned long serial, uint8_t major,
                        uint16_t minor, uint8_t error_code, const char *func) {
	if (unlikely(LOG_LEVEL_DEBUG >= log_get_level_tls())) {
		log_printf(tls_logger, LOG_LEVEL_DEBUG, func, "%s",
		           x_error_code_to_string(c, serial, major, minor, error_code));
	}
}

struct x_generic_async_request {
	struct x_async_request_base base;
	enum x_error_action error_action;
	const char *func;
	const char *file;
	int line;
};

static void
x_generic_async_callback(struct x_connection *c, struct x_async_request_base *req_base,
                         const xcb_raw_generic_event_t *reply_or_error) {
	auto req = (struct x_generic_async_request *)req_base;
	auto error_action = req->error_action;
	auto func = req->func == NULL ? "(unknown)" : req->func;
	auto file = req->file == NULL ? "(unknown)" : req->file;
	auto line = req->line;
	free(req);

	if (reply_or_error == NULL || reply_or_error->response_type != 0) {
		return;
	}

	auto error = (xcb_generic_error_t *)reply_or_error;
	if (error_action != PENDING_REPLY_ACTION_IGNORE) {
		log_error("X error for request in %s at %s:%d: %s", func, file, line,
		          x_error_code_to_string(c, error->full_sequence, error->major_code,
		                                 error->minor_code, error->error_code));
	} else {
		log_debug("Expected X error for request in %s at %s:%d: %s", func, file, line,
		          x_error_code_to_string(c, error->full_sequence, error->major_code,
		                                 error->minor_code, error->error_code));
	}
	switch (error_action) {
	case PENDING_REPLY_ACTION_ABORT:
		log_fatal("An unrecoverable X error occurred, "
		          "aborting...");
		abort();
	case PENDING_REPLY_ACTION_DEBUG_ABORT: assert(false); break;
	case PENDING_REPLY_ACTION_IGNORE: break;
	}
}

void x_set_error_action(struct x_connection *c, uint32_t sequence, enum x_error_action action,
                        const char *func, const char *file, int line) {
	auto req = ccalloc(1, struct x_generic_async_request);
	req->func = func;
	req->file = file;
	req->line = line;
	req->error_action = action;
	req->base.sequence = sequence;
	req->base.callback = x_generic_async_callback;
	req->base.no_reply = true;
	x_await_request(c, &req->base);
}

static bool x_feed_event(struct x_connection *c, xcb_generic_event_t *e);

/**
 * Xlib error handler function.
 */
static int xerror(Display attr_unused *dpy, XErrorEvent *ev) {
	if (!ps_g) {
		// Do not ignore errors until the session has been initialized
		return 0;
	}

	// Fake a xcb error, fill in just enough information
	xcb_generic_error_t xcb_err = {};
	xcb_err.full_sequence = (uint32_t)ev->serial;
	xcb_err.major_code = ev->request_code;
	xcb_err.minor_code = ev->minor_code;
	xcb_err.error_code = ev->error_code;
	x_feed_event(&ps_g->c, (xcb_generic_event_t *)&xcb_err);
	return 0;
}

/// Initialize the used X extensions and populate the x_extensions structure in an
/// x_connection structure with the information about them.
///
/// Returns false if the X server doesn't have or support the required version of at least
/// one required X extension, true otherwise.
bool x_extensions_init(struct x_connection *c) {
	xcb_prefetch_extension_data(c->c, &xcb_composite_id);
	xcb_prefetch_extension_data(c->c, &xcb_damage_id);
	xcb_prefetch_extension_data(c->c, &xcb_xfixes_id);
	xcb_prefetch_extension_data(c->c, &xcb_glx_id);
	xcb_prefetch_extension_data(c->c, &xcb_present_id);
	xcb_prefetch_extension_data(c->c, &xcb_randr_id);
	xcb_prefetch_extension_data(c->c, &xcb_render_id);
	xcb_prefetch_extension_data(c->c, &xcb_shape_id);
	xcb_prefetch_extension_data(c->c, &xcb_sync_id);
	xcb_prefetch_extension_data(c->c, &xcb_shm_id);

	// Initialize the X Composite extension.
	auto extension = xcb_get_extension_data(c->c, &xcb_composite_id);
	if (!extension || !extension->present) {
		log_fatal("The X server doesn't have the X Composite extension.");

		return false;
	}

	// The NameWindowPixmap request was introduced in the X Composite extension v0.2.
	auto composite = xcb_composite_query_version_reply(
	    c->c,
	    xcb_composite_query_version(c->c, XCB_COMPOSITE_MAJOR_VERSION,
	                                XCB_COMPOSITE_MINOR_VERSION),
	    NULL);
	if (!composite || (composite->major_version == 0 && composite->minor_version < 2)) {
		log_fatal("The X server doesn't support the X Composite extension v0.2.");

		if (composite) {
			free(composite);
		}

		return false;
	}

	free(composite);

	// Initialize the X Damage extension.
	extension = xcb_get_extension_data(c->c, &xcb_damage_id);
	if (!extension || !extension->present) {
		log_fatal("The X server doesn't have the X Damage extension.");

		return false;
	}

	c->e.damage_event = extension->first_event;
	c->e.damage_error = extension->first_error;

	// According to the X Damage extension's specification:
	// "The client must negotiate the version of the extension before executing
	// extension requests. Otherwise, the server will return BadRequest for any
	// operations other than QueryVersion."
	xcb_discard_reply(c->c, xcb_damage_query_version(c->c, XCB_DAMAGE_MAJOR_VERSION,
	                                                 XCB_DAMAGE_MINOR_VERSION)
	                            .sequence);

	// Initialize the X Fixes extension.
	extension = xcb_get_extension_data(c->c, &xcb_xfixes_id);
	if (!extension || !extension->present) {
		log_fatal("The X server doesn't have the X Fixes extension.");

		return false;
	}

	c->e.fixes_error = extension->first_error;

	// According to the X Fixes extension's specification:
	// "The client must negotiate the version of the extension before executing
	// extension requests. Behavior of the server is undefined otherwise."
	xcb_discard_reply(c->c, xcb_xfixes_query_version(c->c, XCB_XFIXES_MAJOR_VERSION,
	                                                 XCB_XFIXES_MINOR_VERSION)
	                            .sequence);

	// MIT-SHM
	extension = xcb_get_extension_data(c->c, &xcb_shm_id);
	if (!extension || !extension->present) {
		log_fatal("This X server is missing the MIT-SHM extension.");
		return false;
	}

	c->e.shm_event = extension->first_event;
	c->e.shm_error = extension->first_error;

	auto shm_info = XCB_AWAIT(xcb_shm_query_version, c);
	if (!shm_info) {
		log_fatal("Failed to query version of the MIT-SHM.");
		return false;
	}
	if (shm_info->major_version < 1 ||
	    (shm_info->major_version == 1 && shm_info->minor_version < 2)) {
		log_fatal("This X server's MIT-SHM extension is too old (expected at "
		          "least 1.2, got %d.%d).",
		          shm_info->major_version, shm_info->minor_version);
		return false;
	}
	free(shm_info);

	// Initialize the X GLX extension.
	extension = xcb_get_extension_data(c->c, &xcb_glx_id);
	if (extension && extension->present) {
		c->e.has_glx = true;
		c->e.glx_error = extension->first_error;
	}

	// Initialize the X Present extension.
	extension = xcb_get_extension_data(c->c, &xcb_present_id);
	if (extension && extension->present) {
		c->e.has_present = true;
	}

	// Initialize the X RandR extension.
	extension = xcb_get_extension_data(c->c, &xcb_randr_id);
	if (extension && extension->present) {
		c->e.has_randr = true;
		c->e.randr_event = extension->first_event;
	}

	// Initialize the X Render extension.
	extension = xcb_get_extension_data(c->c, &xcb_render_id);
	if (!extension || !extension->present) {
		log_fatal("The X server doesn't have the X Render extension.");

		return false;
	}

	c->e.render_error = extension->first_error;

	// Initialize the X Shape extension.
	extension = xcb_get_extension_data(c->c, &xcb_shape_id);
	if (extension && extension->present) {
		c->e.has_shape = true;
		c->e.shape_event = extension->first_event;
	}

	// Initialize the X Sync extension.
	extension = xcb_get_extension_data(c->c, &xcb_sync_id);
	if (extension && extension->present) {
		// Fences were introduced in the X Sync extension v3.1.
		auto sync = xcb_sync_initialize_reply(
		    c->c,
		    xcb_sync_initialize(c->c, XCB_SYNC_MAJOR_VERSION, XCB_SYNC_MINOR_VERSION),
		    NULL);
		if (sync && (sync->major_version > 3 ||
		             (sync->major_version == 3 && sync->minor_version >= 1))) {
			c->e.has_sync = true;
			c->e.sync_event = extension->first_event;
			c->e.sync_error = extension->first_error;
		}

		if (sync) {
			free(sync);
		}
	}

	return true;
}

static void x_connection_init_inner(struct x_connection *c) {
	list_init_head(&c->pending_x_requests);
	c->previous_xerror_handler = XSetErrorHandler(xerror);

	c->screen_info = xcb_aux_get_screen(c->c, c->screen);

	// Do a round trip to fetch the current sequence number
	auto cookie = xcb_get_input_focus(c->c);
	free(xcb_get_input_focus_reply(c->c, cookie, NULL));
	c->last_sequence = cookie.sequence;
}

void x_connection_init(struct x_connection *c, Display *dpy) {
	c->dpy = dpy;
	c->c = XGetXCBConnection(dpy);
	c->screen = DefaultScreen(dpy);
	x_connection_init_inner(c);
}

void x_connection_init_xcb(struct x_connection *c, xcb_connection_t *conn, int screen) {
	c->c = conn;
	c->dpy = NULL;
	c->screen = screen;
	x_connection_init_inner(c);
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
		log_debug_x_error(c, e, "Failed to get window property for %#010x", wid);
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
		log_error_x_error(c, e, "failed to create picture");
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
		log_error_x_error(c, e, "Failed to fetch rectangles");
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
	    XCB_AWAIT_VOID(xcb_xfixes_set_region, c, dst, to_u32_checked(nrects), xrects);

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
	bool success =
	    XCB_AWAIT_VOID(xcb_xfixes_create_region, c, ret, to_u32_checked(nrects), xrects);
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
		log_error_x_error(c, e, "Failed to set clip region");
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
		log_error_x_error(c, e, "failed to clear clip region");
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
const char *x_strerror(struct x_connection *c, const xcb_generic_error_t *e) {
	if (!e) {
		return "No error";
	}
	return x_error_code_to_string(c, e->full_sequence, e->major_code, e->minor_code,
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

	log_error_x_error(c, err, "Failed to create pixmap");
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

static void x_handle_update_monitors_reply(struct x_connection *c,
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
		         x_strerror(c, (xcb_generic_error_t *)reply_or_error));
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

static inline void x_ingest_event(struct x_connection *c, xcb_generic_event_t *event) {
	if (event != NULL) {
		assert(event->response_type != 1);
		c->last_sequence = event->full_sequence;
	}
}

static const xcb_raw_generic_event_t no_reply_success = {.response_type = 1};

/// Complete all pending async requests that "come before" the given event.
static void x_complete_async_requests(struct x_connection *c, xcb_generic_event_t *e) {
	auto seq = x_widen_sequence(c, e->full_sequence);
	list_foreach_safe(struct x_async_request_base, i, &c->pending_x_requests, siblings) {
		auto head_seq = x_widen_sequence(c, i->sequence);
		if (head_seq > seq) {
			break;
		}
		if (head_seq == seq && e->response_type == 0) {
			// Error replies are handled in `x_poll_for_event`.
			break;
		}
		auto reply_or_error = &no_reply_success;
		if (!i->no_reply) {
			// We have received something with sequence number `seq >=
			// head_seq`, so we are sure that a reply for `i` is available in
			// xcb's buffer, so we can safely call `xcb_poll_for_reply`
			// without reading from X.
			xcb_generic_error_t *err = NULL;
			auto has_reply = xcb_poll_for_reply(
			    c->c, i->sequence, (void **)&reply_or_error, &err);
			BUG_ON(has_reply == 0);
			if (reply_or_error == NULL) {
				reply_or_error = (xcb_raw_generic_event_t *)err;
			}
		}
		c->latest_completed_request = i->sequence;
		list_remove(&i->siblings);
		i->callback(c, i, reply_or_error);
		if (reply_or_error != &no_reply_success) {
			free((void *)reply_or_error);
		}
	}
}

static bool x_feed_event(struct x_connection *c, xcb_generic_event_t *e) {
	x_complete_async_requests(c, e);
	x_ingest_event(c, e);

	if (e->response_type != 0) {
		return true;
	}

	// We received an error, handle it and return NULL so we try again to see if there
	// are real events.
	struct x_async_request_base *head = NULL;
	xcb_generic_error_t *error = (xcb_generic_error_t *)e;
	if (!list_is_empty(&c->pending_x_requests)) {
		head = list_entry(c->pending_x_requests.next, struct x_async_request_base,
		                  siblings);
	}
	if (head != NULL && error->full_sequence == head->sequence) {
		// This is an error response to the head of pending requests.
		c->latest_completed_request = head->sequence;
		list_remove(&head->siblings);
		head->callback(c, head, (xcb_raw_generic_event_t *)e);
	} else {
		log_warn("Stray X error: %s",
		         x_error_code_to_string(c, error->full_sequence, error->major_code,
		                                error->minor_code, error->error_code));
	}
	return false;
}

bool x_prepare_for_sleep(struct x_connection *c) {
	if (!list_is_empty(&c->pending_x_requests)) {
		auto last = list_entry(c->pending_x_requests.prev,
		                       struct x_async_request_base, siblings);
		if (c->event_sync != last->sequence) {
			// Send an async request that is guaranteed to error, see comments
			// on `event_sync` for why.
			auto cookie = xcb_free_pixmap(c->c, XCB_NONE);
			c->event_sync = cookie.sequence;
			x_set_error_action_ignore(c, cookie);
			log_trace("Sending event sync request to catch response to "
			          "pending request, last sequence: %u, event sync: %u",
			          last->sequence, c->event_sync);
		}
	}
	XFlush(c->dpy);
	xcb_flush(c->c);
	return true;
}

xcb_generic_event_t *x_poll_for_event(struct x_connection *c, bool queued) {
	while (true) {
		auto e = queued ? xcb_poll_for_queued_event(c->c) : xcb_poll_for_event(c->c);
		if (e == NULL) {
			break;
		}
		if (x_feed_event(c, e)) {
			return e;
		}
		free(e);
	}
	return NULL;
}
