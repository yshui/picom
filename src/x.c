// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>
#include <stdalign.h>
#include <stdbool.h>
#include <stdlib.h>

#include <X11/Xutil.h>
#include <pixman.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/glx.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xfixes.h>

#include "atom.h"
#ifdef CONFIG_OPENGL
#include "backend/gl/glx.h"
#endif
#include "common.h"
#include "compiler.h"
#include "kernel.h"
#include "log.h"
#include "region.h"
#include "utils.h"
#include "x.h"

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
winprop_t x_get_prop_with_offset(xcb_connection_t *c, xcb_window_t w, xcb_atom_t atom,
                                 int offset, int length, xcb_atom_t rtype, int rformat) {
	xcb_get_property_reply_t *r = xcb_get_property_reply(
	    c,
	    xcb_get_property(c, 0, w, atom, rtype, to_u32_checked(offset),
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
winprop_info_t x_get_prop_info(xcb_connection_t *c, xcb_window_t w, xcb_atom_t atom) {
	xcb_generic_error_t *e = NULL;
	auto r = xcb_get_property_reply(
	    c, xcb_get_property(c, 0, w, atom, XCB_ATOM_ANY, 0, 0), &e);
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
xcb_window_t wid_get_prop_window(xcb_connection_t *c, xcb_window_t wid, xcb_atom_t aprop) {
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
bool wid_get_text_prop(session_t *ps, xcb_window_t wid, xcb_atom_t prop, char ***pstrlst,
                       int *pnstr) {
	assert(ps->server_grabbed);
	auto prop_info = x_get_prop_info(ps->c, wid, prop);
	auto type = prop_info.type;
	auto format = prop_info.format;
	auto length = prop_info.length;

	if (type == XCB_ATOM_NONE) {
		return false;
	}

	if (type != XCB_ATOM_STRING && type != ps->atoms->aUTF8_STRING &&
	    type != ps->atoms->aC_STRING) {
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
	    ps->c, xcb_get_property(ps->c, 0, wid, prop, type, 0, word_count), &e);
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

static inline void x_get_server_pictfmts(xcb_connection_t *c) {
	if (g_pictfmts) {
		return;
	}
	xcb_generic_error_t *e = NULL;
	// Get window picture format
	g_pictfmts =
	    xcb_render_query_pict_formats_reply(c, xcb_render_query_pict_formats(c), &e);
	if (e || !g_pictfmts) {
		log_fatal("failed to get pict formats\n");
		abort();
	}
}

const xcb_render_pictforminfo_t *
x_get_pictform_for_visual(xcb_connection_t *c, xcb_visualid_t visual) {
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

xcb_visualid_t x_get_visual_for_standard(xcb_connection_t *c, xcb_pict_standard_t std) {
	x_get_server_pictfmts(c);

	auto pictfmt = xcb_render_util_find_standard_format(g_pictfmts, std);

	return x_get_visual_for_pictfmt(g_pictfmts, pictfmt->id);
}

xcb_render_pictformat_t
x_get_pictfmt_for_standard(xcb_connection_t *c, xcb_pict_standard_t std) {
	x_get_server_pictfmts(c);

	auto pictfmt = xcb_render_util_find_standard_format(g_pictfmts, std);

	return pictfmt->id;
}

int x_get_visual_depth(xcb_connection_t *c, xcb_visualid_t visual) {
	auto setup = xcb_get_setup(c);
	for (auto screen = xcb_setup_roots_iterator(setup); screen.rem;
	     xcb_screen_next(&screen)) {
		for (auto depth = xcb_screen_allowed_depths_iterator(screen.data);
		     depth.rem; xcb_depth_next(&depth)) {
			const int len = xcb_depth_visuals_length(depth.data);
			const xcb_visualtype_t *visuals = xcb_depth_visuals(depth.data);
			for (int i = 0; i < len; i++) {
				if (visual == visuals[i].visual_id) {
					return depth.data->depth;
				}
			}
		}
	}
	return -1;
}

xcb_render_picture_t
x_create_picture_with_pictfmt_and_pixmap(xcb_connection_t *c,
                                         const xcb_render_pictforminfo_t *pictfmt,
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
	    xcb_request_check(c, xcb_render_create_picture_checked(
	                             c, tmp_picture, pixmap, pictfmt->id, valuemask, buf));
	free(buf);
	if (e) {
		log_error_x_error(e, "failed to create picture");
		return XCB_NONE;
	}
	return tmp_picture;
}

xcb_render_picture_t
x_create_picture_with_visual_and_pixmap(xcb_connection_t *c, xcb_visualid_t visual,
                                        xcb_pixmap_t pixmap, uint32_t valuemask,
                                        const xcb_render_create_picture_value_list_t *attr) {
	const xcb_render_pictforminfo_t *pictfmt = x_get_pictform_for_visual(c, visual);
	return x_create_picture_with_pictfmt_and_pixmap(c, pictfmt, pixmap, valuemask, attr);
}

xcb_render_picture_t
x_create_picture_with_standard_and_pixmap(xcb_connection_t *c, xcb_pict_standard_t standard,
                                          xcb_pixmap_t pixmap, uint32_t valuemask,
                                          const xcb_render_create_picture_value_list_t *attr) {
	x_get_server_pictfmts(c);

	auto pictfmt = xcb_render_util_find_standard_format(g_pictfmts, standard);
	assert(pictfmt);
	return x_create_picture_with_pictfmt_and_pixmap(c, pictfmt, pixmap, valuemask, attr);
}

xcb_render_picture_t
x_create_picture_with_standard(xcb_connection_t *c, xcb_drawable_t d, int w, int h,
                               xcb_pict_standard_t standard, uint32_t valuemask,
                               const xcb_render_create_picture_value_list_t *attr) {
	x_get_server_pictfmts(c);

	auto pictfmt = xcb_render_util_find_standard_format(g_pictfmts, standard);
	assert(pictfmt);
	return x_create_picture_with_pictfmt(c, d, w, h, pictfmt, valuemask, attr);
}

/**
 * Create an picture.
 */
xcb_render_picture_t
x_create_picture_with_pictfmt(xcb_connection_t *c, xcb_drawable_t d, int w, int h,
                              const xcb_render_pictforminfo_t *pictfmt, uint32_t valuemask,
                              const xcb_render_create_picture_value_list_t *attr) {
	uint8_t depth = pictfmt->depth;

	xcb_pixmap_t tmp_pixmap = x_create_pixmap(c, depth, d, w, h);
	if (!tmp_pixmap) {
		return XCB_NONE;
	}

	xcb_render_picture_t picture = x_create_picture_with_pictfmt_and_pixmap(
	    c, pictfmt, tmp_pixmap, valuemask, attr);

	xcb_free_pixmap(c, tmp_pixmap);

	return picture;
}

xcb_render_picture_t
x_create_picture_with_visual(xcb_connection_t *c, xcb_drawable_t d, int w, int h,
                             xcb_visualid_t visual, uint32_t valuemask,
                             const xcb_render_create_picture_value_list_t *attr) {
	auto pictfmt = x_get_pictform_for_visual(c, visual);
	return x_create_picture_with_pictfmt(c, d, w, h, pictfmt, valuemask, attr);
}

bool x_fetch_region(xcb_connection_t *c, xcb_xfixes_region_t r, pixman_region32_t *res) {
	xcb_generic_error_t *e = NULL;
	xcb_xfixes_fetch_region_reply_t *xr =
	    xcb_xfixes_fetch_region_reply(c, xcb_xfixes_fetch_region(c, r), &e);
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

void x_set_picture_clip_region(xcb_connection_t *c, xcb_render_picture_t pict,
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

	xcb_generic_error_t *e = xcb_request_check(
	    c, xcb_render_set_picture_clip_rectangles_checked(
	           c, pict, clip_x_origin, clip_y_origin, to_u32_checked(nrects), xrects));
	if (e) {
		log_error_x_error(e, "Failed to set clip region");
		free(e);
	}
	free(xrects);
}

void x_clear_picture_clip_region(xcb_connection_t *c, xcb_render_picture_t pict) {
	xcb_render_change_picture_value_list_t v = {.clipmask = XCB_NONE};
	xcb_generic_error_t *e = xcb_request_check(
	    c, xcb_render_change_picture(c, pict, XCB_RENDER_CP_CLIP_MASK, &v));
	if (e) {
		log_error_x_error(e, "failed to clear clip region");
		free(e);
	}
}

enum {
	XSyncBadCounter = 0,
	XSyncBadAlarm = 1,
	XSyncBadFence = 2,
};

/**
 * Convert a X11 error to string
 *
 * @return a pointer to a string. this pointer shouldn NOT be freed, same buffer is used
 *         for multiple calls to this function,
 */
static const char *
_x_strerror(unsigned long serial, uint8_t major, uint16_t minor, uint8_t error_code) {
	session_t *const ps = ps_g;

	int o = 0;
	const char *name = "Unknown";

#define CASESTRRET(s)                                                                    \
	case s:                                                                          \
		name = #s;                                                               \
		break

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

/**
 * Log a X11 error
 */
void x_print_error(unsigned long serial, uint8_t major, uint16_t minor, uint8_t error_code) {
	log_debug("%s", _x_strerror(serial, major, minor, error_code));
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
	return _x_strerror(e->full_sequence, e->major_code, e->minor_code, e->error_code);
}

/**
 * Create a pixmap and check that creation succeeded.
 */
xcb_pixmap_t x_create_pixmap(xcb_connection_t *c, uint8_t depth, xcb_drawable_t drawable,
                             int width, int height) {
	xcb_pixmap_t pix = x_new_id(c);
	xcb_void_cookie_t cookie = xcb_create_pixmap_checked(
	    c, depth, pix, drawable, to_u16_checked(width), to_u16_checked(height));
	xcb_generic_error_t *err = xcb_request_check(c, cookie);
	if (err == NULL) {
		return pix;
	}

	log_error_x_error(err, "Failed to create pixmap");
	free(err);
	return XCB_NONE;
}

/**
 * Validate a pixmap.
 *
 * Detect whether the pixmap is valid with XGetGeometry. Well, maybe there
 * are better ways.
 */
bool x_validate_pixmap(xcb_connection_t *c, xcb_pixmap_t pixmap) {
	if (pixmap == XCB_NONE) {
		return false;
	}

	auto r = xcb_get_geometry_reply(c, xcb_get_geometry(c, pixmap), NULL);
	if (!r) {
		return false;
	}

	bool ret = r->width && r->height;
	free(r);
	return ret;
}
/// Names of root window properties that could point to a pixmap of
/// background.
static const char *background_props_str[] = {
    "_XROOTPMAP_ID",
    "_XSETROOT_ID",
    0,
};

xcb_pixmap_t
x_get_root_back_pixmap(xcb_connection_t *c, xcb_window_t root, struct atom *atoms) {
	xcb_pixmap_t pixmap = XCB_NONE;

	// Get the values of background attributes
	for (int p = 0; background_props_str[p]; p++) {
		xcb_atom_t prop_atom = get_atom(atoms, background_props_str[p]);
		winprop_t prop = x_get_prop(c, root, prop_atom, 1, XCB_ATOM_PIXMAP, 32);
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
	for (int p = 0; background_props_str[p]; p++) {
		xcb_atom_t prop_atom = get_atom(atoms, background_props_str[p]);
		if (prop_atom == atom) {
			return true;
		}
	}
	return false;
}

/**
 * Synchronizes a X Render drawable to ensure all pending painting requests
 * are completed.
 */
bool x_fence_sync(xcb_connection_t *c, xcb_sync_fence_t f) {
	// TODO(richardgv): If everybody just follows the rules stated in X Sync
	// prototype, we need only one fence per screen, but let's stay a bit
	// cautious right now

	auto e = xcb_request_check(c, xcb_sync_trigger_fence_checked(c, f));
	if (e) {
		log_error_x_error(e, "Failed to trigger the fence");
		goto err;
	}

	e = xcb_request_check(c, xcb_sync_await_fence_checked(c, 1, &f));
	if (e) {
		log_error_x_error(e, "Failed to await on a fence");
		goto err;
	}

	e = xcb_request_check(c, xcb_sync_reset_fence_checked(c, f));
	if (e) {
		log_error_x_error(e, "Failed to reset the fence");
		goto err;
	}
	return true;

err:
	free(e);
	return false;
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
	for (int i = 0; i < kernel->w * kernel->h; i++) {
		if (i == kernel->w * kernel->h / 2) {
			continue;
		}
		sum += kernel->data[i];
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
struct xvisual_info x_get_visual_info(xcb_connection_t *c, xcb_visualid_t visual) {
	auto pictfmt = x_get_pictform_for_visual(c, visual);
	auto depth = x_get_visual_depth(c, visual);
	if (!pictfmt || depth == -1) {
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

xcb_screen_t *x_screen_of_display(xcb_connection_t *c, int screen) {
	xcb_screen_iterator_t iter;

	iter = xcb_setup_roots_iterator(xcb_get_setup(c));
	for (; iter.rem; --screen, xcb_screen_next(&iter)) {
		if (screen == 0) {
			return iter.data;
		}
	}

	return NULL;
}
