// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <X11/Xlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <xcb/render.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xfixes.h>

#include "atom.h"
#include "compiler.h"
#include "log.h"
#include "region.h"
#include "utils/kernel.h"
#include "utils/list.h"

typedef struct session session_t;
struct atom;

/// Structure representing Window property value.
typedef struct winprop {
	union {
		void *ptr;
		int8_t *p8;
		int16_t *p16;
		int32_t *p32;
		uint32_t *c32;        // 32bit cardinal
		xcb_atom_t *atom;
	};
	unsigned long nitems;
	xcb_atom_t type;
	int format;

	xcb_get_property_reply_t *r;
} winprop_t;

enum x_error_action {
	PENDING_REPLY_ACTION_IGNORE,
	PENDING_REPLY_ACTION_ABORT,
	PENDING_REPLY_ACTION_DEBUG_ABORT,
};

struct x_extensions {
	/// The X Damage extension's base event number.
	int damage_event;
	/// The X Damage extension's base error number.
	int damage_error;
	/// The X Fixes extension's base error number.
	int fixes_error;
	/// MIT-SHM's base error number.
	int shm_error;
	/// MIT-SHM's base event number.
	int shm_event;
	/// The X GLX extension's presence.
	bool has_glx;
	/// The X GLX extension's base error number.
	int glx_error;
	/// The X Present extension's presence.
	bool has_present;
	/// The X RandR extension's presence.
	bool has_randr;
	/// The X RandR extension's base event number.
	int randr_event;
	/// The X Render extension's base error number.
	int render_error;
	/// The X Shape extension's presence.
	bool has_shape;
	/// The X Shape extension's base event number.
	int shape_event;
	/// The X Sync extension's presence.
	bool has_sync;
	/// The X Sync extension's base event number.
	int sync_event;
	/// The X Sync extension's base error number.
	int sync_error;
};

struct x_connection {
	// Public fields
	// These are part of the public ABI, changing these
	// requires bumping PICOM_API_MAJOR.
	/// XCB connection.
	xcb_connection_t *c;
	/// Display in use.
	Display *dpy;
	/// Default screen
	int screen;
	/// Information about the X extensions.
	struct x_extensions e;

	// Private fields
	/// The list of pending async requests that we have
	/// yet to receive a reply for.
	struct list_node pending_x_requests;
	/// Previous handler of X errors
	XErrorHandler previous_xerror_handler;
	/// Information about the default screen
	xcb_screen_t *screen_info;
	/// The sequence number of the last message returned by
	/// `x_poll_for_message`. Used for sequence number overflow
	/// detection.
	uint32_t last_sequence;
	/// The sequence number of the last completed request.
	uint32_t latest_completed_request;
	/// The sequence number of the "event sync" request we sent. This is
	/// a request we sent that is guaranteed to error, so we can be sure
	/// `xcb_poll_for_event` will return something. This is akin to `xcb_aux_sync`,
	/// except that guarantees a reply, this one guarantees an error.
	///
	/// # Why do we need this?
	///
	/// To understand why we need this, first notice we need a way to fetch replies
	/// that are already in xcb's buffer, without reading from the X connection.
	/// Because otherwise we can't going into sleep while being confident that there
	/// is no buffered events we haven't handled.
	///
	/// For events or unchecked errors (we will refer to both of them as events
	/// without distinction), this is possible with `xcb_poll_for_queued_event`, but
	/// for replies, there is no `xcb_poll_for_queued_reply` (ridiculous, if you
	/// ask me). Luckily, if there is a reply already in the buffer,
	/// `xcb_poll_for_reply` will return it without reading from X. And we can deduce
	/// whether a reply is already received from the sequence number of received
	/// events. The only problem, if no events are coming, we will be stuck
	/// indefinitely, so we have to make our own events.
	uint32_t event_sync;
};

/// Monitor info
struct x_monitors {
	int count;
	region_t *regions;
};

#define XCB_AWAIT_VOID(func, conn, ...)                                                   \
	/* NOLINTBEGIN(bugprone-assignment-in-if-condition) */                            \
	({                                                                                \
		bool __success = true;                                                    \
		__auto_type __e =                                                         \
		    xcb_request_check((conn)->c, func##_checked((conn)->c, __VA_ARGS__)); \
		if (__e) {                                                                \
			x_print_error(conn, __e->sequence, __e->major_code,               \
			              __e->minor_code, __e->error_code);                  \
			free(__e);                                                        \
			__success = false;                                                \
		}                                                                         \
		__success;                                                                \
	}) /* NOLINTEND(bugprone-assignment-in-if-condition) */

#define XCB_AWAIT(func, conn, ...)                                                       \
	({                                                                               \
		xcb_generic_error_t *__e = NULL;                                         \
		__auto_type __r =                                                        \
		    func##_reply((conn)->c, func((conn)->c, ##__VA_ARGS__), &__e);       \
		if (__e) {                                                               \
			x_print_error(conn, __e->sequence, __e->major_code,              \
			              __e->minor_code, __e->error_code);                 \
			free(__e);                                                       \
		}                                                                        \
		__r;                                                                     \
	})

#define log_debug_x_error(c, e, fmt, ...)                                                \
	LOG(DEBUG, fmt " (%s)", ##__VA_ARGS__, x_strerror(c, e))
#define log_error_x_error(c, e, fmt, ...)                                                \
	LOG(ERROR, fmt " (%s)", ##__VA_ARGS__, x_strerror(c, e))
#define log_fatal_x_error(c, e, fmt, ...)                                                \
	LOG(FATAL, fmt " (%s)", ##__VA_ARGS__, x_strerror(c, e))

// xcb-render specific macros
#define XFIXED_TO_DOUBLE(value) (((double)(value)) / 65536)
#define DOUBLE_TO_XFIXED(value) ((xcb_render_fixed_t)(((double)(value)) * 65536))

/// Wraps x_new_id. abort the program if x_new_id returns error
static inline uint32_t x_new_id(struct x_connection *c) {
	auto ret = xcb_generate_id(c->c);
	if (ret == (uint32_t)-1) {
		log_fatal("We seems to have run of XIDs. This is either a bug in the X "
		          "server, or a resource leakage in the compositor. Please open "
		          "an issue about this problem. The compositor will die.");
		abort();
	}
	return ret;
}

/// Set error handler for a specific X request.
///
/// @param c X connection
/// @param sequence sequence number of the X request to set error handler for
/// @param action action to take when error occurs
void x_set_error_action(struct x_connection *c, uint32_t sequence, enum x_error_action action,
                        const char *func, const char *file, int line);

/// Convenience wrapper for x_set_error_action with action `PENDING_REPLY_ACTION_IGNORE`
#define x_set_error_action_ignore(c, cookie)                                             \
	x_set_error_action(c, (cookie).sequence, PENDING_REPLY_ACTION_IGNORE, __func__,  \
	                   __FILE__, __LINE__)

/// Convenience wrapper for x_set_error_action with action `PENDING_REPLY_ACTION_ABORT`
#define x_set_error_action_abort(c, cookie)                                              \
	x_set_error_action(c, (cookie).sequence, PENDING_REPLY_ACTION_ABORT, __func__,   \
	                   __FILE__, __LINE__)

/// Convenience wrapper for x_set_error_action with action
/// `PENDING_REPLY_ACTION_DEBUG_ABORT`
#ifndef NDEBUG
#define x_set_error_action_debug_abort(c, cookie)                                        \
	x_set_error_action(c, (cookie).sequence, PENDING_REPLY_ACTION_DEBUG_ABORT,       \
	                   __func__, __FILE__, __LINE__)
#else
#define x_set_error_action_debug_abort(c, cookie)                                        \
	((void)(c));                                                                     \
	((void)(cookie))
#endif

struct x_async_request_base {
	struct list_node siblings;
	/// The callback function to call when the reply is received. If `reply_or_error`
	/// is NULL, it means the X connection is closed while waiting for the reply.
	void (*callback)(struct x_connection *, struct x_async_request_base *,
	                 const xcb_raw_generic_event_t *reply_or_error);
	/// The sequence number of the X request.
	unsigned int sequence;
	/// This request doesn't expect a reply. If this is true, in the success case,
	/// `callback` will be called with a dummy reply whose `response_type` is 1.
	bool no_reply;
};

static inline void attr_unused free_x_connection(struct x_connection *c) {
	list_foreach_safe(struct x_async_request_base, i, &c->pending_x_requests, siblings) {
		list_remove(&i->siblings);
		i->callback(c, i, NULL);
	}

	XSetErrorHandler(c->previous_xerror_handler);
}

/// Initialize the used X extensions and populate the x_extensions structure in an
/// x_connection structure with the information about them.
///
/// Returns false if the X server doesn't have or support the required version of at least
/// one required X extension, true otherwise.
bool x_extensions_init(struct x_connection *c);

/// Initialize x_connection struct from an Xlib Display.
///
/// Note this function doesn't take ownership of the Display, the caller is still
/// responsible for closing it after `free_x_connection` is called.
void x_connection_init(struct x_connection *c, Display *dpy);
void x_connection_init_xcb(struct x_connection *c, xcb_connection_t *conn, int screen);

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
                                 int offset, int length, xcb_atom_t rtype, int rformat);

/**
 * Wrapper of wid_get_prop_adv().
 */
static inline winprop_t
x_get_prop(const struct x_connection *c, xcb_window_t wid, xcb_atom_t atom, int length,
           xcb_atom_t rtype, int rformat) {
	return x_get_prop_with_offset(c, wid, atom, 0L, length, rtype, rformat);
}

/**
 * Get the value of a type-<code>xcb_window_t</code> property of a window.
 *
 * @return the value if successful, 0 otherwise
 */
xcb_window_t wid_get_prop_window(struct x_connection *c, xcb_window_t wid,
                                 xcb_atom_t aprop, bool *exists);
bool wid_get_opacity_prop(struct x_connection *c, struct atom *atoms, xcb_window_t wid,
                          opacity_t def, opacity_t *out);

/**
 * Get the value of a text property of a window.
 *
 * @param[out] pstrlst Out parameter for an array of strings, caller needs to free this
 *                     array
 * @param[out] pnstr   Number of strings in the array
 */
bool wid_get_text_prop(struct x_connection *c, struct atom *atoms, xcb_window_t wid,
                       xcb_atom_t prop, char ***pstrlst, int *pnstr);

static inline bool x_is_type_string(struct atom *atoms, xcb_atom_t type) {
	return type == XCB_ATOM_STRING || type == atoms->aUTF8_STRING ||
	       type == atoms->aC_STRING;
}

const xcb_render_pictforminfo_t *
x_get_pictform_for_visual(struct x_connection *, xcb_visualid_t);

xcb_render_picture_t
x_create_picture_with_pictfmt_and_pixmap(struct x_connection *, xcb_render_pictformat_t pictfmt,
                                         xcb_pixmap_t pixmap, uint32_t valuemask,
                                         const xcb_render_create_picture_value_list_t *attr)
    attr_nonnull(1);

xcb_render_picture_t
x_create_picture_with_visual_and_pixmap(struct x_connection *, xcb_visualid_t visual,
                                        xcb_pixmap_t pixmap, uint32_t valuemask,
                                        const xcb_render_create_picture_value_list_t *attr)
    attr_nonnull(1);

xcb_render_picture_t
x_create_picture_with_standard_and_pixmap(struct x_connection *, xcb_pict_standard_t standard,
                                          xcb_pixmap_t pixmap, uint32_t valuemask,
                                          const xcb_render_create_picture_value_list_t *attr)
    attr_nonnull(1);

xcb_render_picture_t
x_create_picture_with_standard(struct x_connection *c, int w, int h,
                               xcb_pict_standard_t standard, uint32_t valuemask,
                               const xcb_render_create_picture_value_list_t *attr)
    attr_nonnull(1);

/**
 * Create an picture.
 */
xcb_render_picture_t
x_create_picture_with_pictfmt(struct x_connection *, int w, int h,
                              xcb_render_pictformat_t pictfmt, uint8_t depth, uint32_t valuemask,
                              const xcb_render_create_picture_value_list_t *attr)
    attr_nonnull(1);

xcb_render_picture_t
x_create_picture_with_visual(struct x_connection *, int w, int h, xcb_visualid_t visual,
                             uint32_t valuemask,
                             const xcb_render_create_picture_value_list_t *attr)
    attr_nonnull(1);

/// Fetch a X region and store it in a pixman region
bool x_fetch_region(struct x_connection *, xcb_xfixes_region_t r, region_t *res);

/// Set an X region to a pixman region
bool x_set_region(struct x_connection *c, xcb_xfixes_region_t dst, const region_t *src);

/// Create a X region from a pixman region
uint32_t x_create_region(struct x_connection *c, const region_t *reg);

void x_async_change_window_attributes(struct x_connection *c, xcb_window_t wid,
                                      uint32_t mask, const uint32_t *values,
                                      struct x_async_request_base *req);
void x_async_query_tree(struct x_connection *c, xcb_window_t wid,
                        struct x_async_request_base *req);
void x_async_get_property(struct x_connection *c, xcb_window_t wid, xcb_atom_t atom,
                          xcb_atom_t type, uint32_t long_offset, uint32_t long_length,
                          struct x_async_request_base *req);

/// Destroy a X region
void x_destroy_region(struct x_connection *c, uint32_t region);

void x_set_picture_clip_region(struct x_connection *, xcb_render_picture_t,
                               int16_t clip_x_origin, int16_t clip_y_origin, const region_t *);

void x_clear_picture_clip_region(struct x_connection *, xcb_render_picture_t pict);

/**
 * Destroy a <code>Picture</code>.
 *
 * Picture must be valid.
 */
void x_free_picture(struct x_connection *c, xcb_render_picture_t p);

/**
 * Log a X11 error
 */
void x_print_error_impl(struct x_connection *c, unsigned long serial, uint8_t major,
                        uint16_t minor, uint8_t error_code, const char *func);
#define x_print_error(c, serial, major, minor, error_code)                               \
	x_print_error_impl(c, serial, major, minor, error_code, __func__)

/*
 * Convert a xcb_generic_error_t to a string that describes the error
 *
 * @return a pointer to a string. this pointer shouldn NOT be freed, same buffer is used
 *         for multiple calls to this function,
 */
const char *x_strerror(struct x_connection *c, const xcb_generic_error_t *e);

xcb_pixmap_t x_create_pixmap(struct x_connection *, uint8_t depth, int width, int height);

/**
 * Free a <code>winprop_t</code>.
 *
 * @param pprop pointer to the <code>winprop_t</code> to free.
 */
static inline void free_winprop(winprop_t *pprop) {
	// Empty the whole structure to avoid possible issues
	if (pprop->r) {
		free(pprop->r);
	}
	pprop->ptr = NULL;
	pprop->r = NULL;
	pprop->nitems = 0;
}

/// Get the back pixmap of the root window
xcb_pixmap_t x_get_root_back_pixmap(struct x_connection *c, struct atom *atoms);

/// Return true if the atom refers to a property name that is used for the
/// root window background pixmap
bool x_is_root_back_pixmap_atom(struct atom *atoms, xcb_atom_t atom);

struct x_convolution_kernel {
	int size;
	int capacity;
	xcb_render_fixed_t kernel[];
};

/**
 * Convert a struct conv to a X picture convolution filter, normalizing the kernel
 * in the process. Allow the caller to specify the element at the center of the kernel,
 * for compatibility with legacy code.
 *
 * @param[in] kernel the convolution kernel
 * @param[in] center the element to put at the center of the matrix
 * @param[inout] ret pointer to an array of `size`, if `size` is too small, more space
 *                   will be allocated, and `*ret` will be updated.
 * @param[inout] size size of the array pointed to by `ret`.
 */
void attr_nonnull(1, 3) x_create_convolution_kernel(const conv *kernel, double center,
                                                    struct x_convolution_kernel **ret);

/// Generate a search criteria for fbconfig from a X visual.
/// Returns {-1, -1, -1, -1, -1, -1} on failure
struct xvisual_info x_get_visual_info(struct x_connection *c, xcb_visualid_t visual);

xcb_visualid_t x_get_visual_for_standard(struct x_connection *c, xcb_pict_standard_t std);

xcb_visualid_t x_get_visual_for_depth(xcb_screen_t *screen, uint8_t depth);

xcb_render_pictformat_t
x_get_pictfmt_for_standard(struct x_connection *c, xcb_pict_standard_t std);

/// Populates a `struct x_monitors` with the current monitor configuration asynchronously.
void x_update_monitors_async(struct x_connection *, struct x_monitors *);
/// Free memory allocated for a `struct x_monitors`.
void x_free_monitor_info(struct x_monitors *);

uint32_t attr_deprecated xcb_generate_id(xcb_connection_t *c);        // NOLINT(readability-redundant-declaration)

/// Ask X server to send us a notification for the next end of vblank.
void x_request_vblank_event(struct x_connection *c, xcb_window_t window, uint64_t msc);

/// Register an X request as async request. Its reply will be processed as part of the
/// event stream. i.e. the registered callback will only be called when all preceding
/// events have been retrieved via `x_poll_for_event`.
/// `req` store information about the request, including the callback. The callback is
/// responsible for freeing `req`.
static inline void x_await_request(struct x_connection *c, struct x_async_request_base *req) {
	list_insert_before(&c->pending_x_requests, &req->siblings);
}

/// Flush all X buffers to ensure we don't sleep with outgoing messages not sent.
///
/// If there are requests pending replies, an event sync request will
/// be sent if necessary. See comments on `event_sync` for more information. MUST be
/// called before sleep to ensure we can handle replies/events in a timely manner. This
/// function MIGHT read data from X into xcb buffer (because `xcb_flush` might read,
/// ridiculous, I know), so `x_poll_for_event(queued = true)` MUST be called after this to
/// drain the buffer.
bool x_prepare_for_sleep(struct x_connection *c);

/// Poll for the next X event. This is like `xcb_poll_for_event`, but also includes
/// machinery for handling async replies. Calling `xcb_poll_for_event` directly will
/// cause replies to async requests to be lost, so that should never be called.
///
/// @param[out] queued if true, only return events that are already in the queue, don't
///                    attempt to read from the X connection.
xcb_generic_event_t *x_poll_for_event(struct x_connection *c, bool queued);
