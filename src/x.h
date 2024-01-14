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

#include "compiler.h"
#include "kernel.h"
#include "log.h"
#include "region.h"

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
	};
	unsigned long nitems;
	xcb_atom_t type;
	int format;

	xcb_get_property_reply_t *r;
} winprop_t;

typedef struct winprop_info {
	xcb_atom_t type;
	uint8_t format;
	uint32_t length;
} winprop_info_t;

struct xvisual_info {
	/// Bit depth of the red component
	int red_size;
	/// Bit depth of the green component
	int green_size;
	/// Bit depth of the blue component
	int blue_size;
	/// Bit depth of the alpha component
	int alpha_size;
	/// The depth of X visual
	int visual_depth;

	xcb_visualid_t visual;
};

enum pending_reply_action {
	PENDING_REPLY_ACTION_IGNORE,
	PENDING_REPLY_ACTION_ABORT,
	PENDING_REPLY_ACTION_DEBUG_ABORT,
};

typedef struct pending_reply {
	struct pending_reply *next;
	unsigned long sequence;
	enum pending_reply_action action;
} pending_reply_t;

struct x_connection {
	/// XCB connection.
	xcb_connection_t *c;
	/// Display in use.
	Display *dpy;
	/// Head pointer of the error ignore linked list.
	pending_reply_t *pending_reply_head;
	/// Pointer to the <code>next</code> member of tail element of the error
	/// ignore linked list.
	pending_reply_t **pending_reply_tail;
	/// Previous handler of X errors
	XErrorHandler previous_xerror_handler;
	/// Default screen
	int screen;
	/// Information about the default screen
	xcb_screen_t *screen_info;
};

/// Monitor info
struct x_monitors {
	int count;
	region_t *regions;
};

#define XCB_AWAIT_VOID(func, c, ...)                                                     \
	({                                                                               \
		bool __success = true;                                                   \
		__auto_type __e = xcb_request_check(c, func##_checked(c, __VA_ARGS__));  \
		if (__e) {                                                               \
			x_print_error(__e->sequence, __e->major_code, __e->minor_code,   \
			              __e->error_code);                                  \
			free(__e);                                                       \
			__success = false;                                               \
		}                                                                        \
		__success;                                                               \
	})

#define XCB_AWAIT(func, c, ...)                                                          \
	({                                                                               \
		xcb_generic_error_t *__e = NULL;                                         \
		__auto_type __r = func##_reply(c, func(c, __VA_ARGS__), &__e);           \
		if (__e) {                                                               \
			x_print_error(__e->sequence, __e->major_code, __e->minor_code,   \
			              __e->error_code);                                  \
			free(__e);                                                       \
		}                                                                        \
		__r;                                                                     \
	})

#define log_debug_x_error(e, fmt, ...)                                                   \
	LOG(DEBUG, fmt " (%s)", ##__VA_ARGS__, x_strerror(e))
#define log_error_x_error(e, fmt, ...)                                                   \
	LOG(ERROR, fmt " (%s)", ##__VA_ARGS__, x_strerror(e))
#define log_fatal_x_error(e, fmt, ...)                                                   \
	LOG(FATAL, fmt " (%s)", ##__VA_ARGS__, x_strerror(e))

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

static void set_reply_action(struct x_connection *c, uint32_t sequence,
                             enum pending_reply_action action) {
	auto i = cmalloc(pending_reply_t);

	i->sequence = sequence;
	i->next = 0;
	i->action = action;
	*c->pending_reply_tail = i;
	c->pending_reply_tail = &i->next;
}

/**
 * Ignore X errors caused by given X request.
 */
static inline void attr_unused set_ignore_cookie(struct x_connection *c,
                                                 xcb_void_cookie_t cookie) {
	set_reply_action(c, cookie.sequence, PENDING_REPLY_ACTION_IGNORE);
}

static inline void attr_unused set_cant_fail_cookie(struct x_connection *c,
                                                    xcb_void_cookie_t cookie) {
	set_reply_action(c, cookie.sequence, PENDING_REPLY_ACTION_ABORT);
}

static inline void attr_unused set_debug_cant_fail_cookie(struct x_connection *c,
                                                          xcb_void_cookie_t cookie) {
#ifndef NDEBUG
	set_reply_action(c, cookie.sequence, PENDING_REPLY_ACTION_DEBUG_ABORT);
#else
	(void)c;
	(void)cookie;
#endif
}

static inline void attr_unused free_x_connection(struct x_connection *c) {
	pending_reply_t *next = NULL;
	for (auto ign = c->pending_reply_head; ign; ign = next) {
		next = ign->next;

		free(ign);
	}

	// Reset head and tail
	c->pending_reply_head = NULL;
	c->pending_reply_tail = &c->pending_reply_head;

	XSetErrorHandler(c->previous_xerror_handler);
}

/// Initialize x_connection struct from an Xlib Display.
///
/// Note this function doesn't take ownership of the Display, the caller is still
/// responsible for closing it after `free_x_connection` is called.
void x_connection_init(struct x_connection *c, Display *dpy);

/// Discard queued pending replies.
///
/// We have received reply with sequence number `sequence`, which means all pending
/// replies with sequence number less than `sequence` will never be received. So discard
/// them.
void x_discard_pending(struct x_connection *c, uint32_t sequence);

/// Handle X errors.
///
/// This function logs X errors, or aborts the program based on severity of the error.
void x_handle_error(struct x_connection *c, xcb_generic_error_t *ev);

/**
 * Send a request to X server and get the reply to make sure all previous
 * requests are processed, and their replies received
 *
 * xcb_get_input_focus is used here because it is the same request used by
 * libX11
 */
static inline void x_sync(struct x_connection *c) {
	free(xcb_get_input_focus_reply(c->c, xcb_get_input_focus(c->c), NULL));
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
                                 int offset, int length, xcb_atom_t rtype, int rformat);

/**
 * Wrapper of wid_get_prop_adv().
 */
static inline winprop_t
x_get_prop(const struct x_connection *c, xcb_window_t wid, xcb_atom_t atom, int length,
           xcb_atom_t rtype, int rformat) {
	return x_get_prop_with_offset(c, wid, atom, 0L, length, rtype, rformat);
}

/// Get the type, format and size in bytes of a window's specific attribute.
winprop_info_t x_get_prop_info(const struct x_connection *c, xcb_window_t w, xcb_atom_t atom);

/// Discard all X events in queue or in flight. Should only be used when the server is
/// grabbed
static inline void x_discard_events(struct x_connection *c) {
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c->c))) {
		free(e);
	}
}

/**
 * Get the value of a type-<code>xcb_window_t</code> property of a window.
 *
 * @return the value if successful, 0 otherwise
 */
xcb_window_t wid_get_prop_window(struct x_connection *c, xcb_window_t wid, xcb_atom_t aprop);

/**
 * Get the value of a text property of a window.
 *
 * @param[out] pstrlst Out parameter for an array of strings, caller needs to free this
 *                     array
 * @param[out] pnstr   Number of strings in the array
 */
bool wid_get_text_prop(session_t *ps, xcb_window_t wid, xcb_atom_t prop, char ***pstrlst,
                       int *pnstr);

const xcb_render_pictforminfo_t *
x_get_pictform_for_visual(struct x_connection *, xcb_visualid_t);
int x_get_visual_depth(struct x_connection *, xcb_visualid_t);

xcb_render_picture_t
x_create_picture_with_pictfmt_and_pixmap(struct x_connection *,
                                         const xcb_render_pictforminfo_t *pictfmt,
                                         xcb_pixmap_t pixmap, uint32_t valuemask,
                                         const xcb_render_create_picture_value_list_t *attr)
    attr_nonnull(1, 2);

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
                              const xcb_render_pictforminfo_t *pictfmt, uint32_t valuemask,
                              const xcb_render_create_picture_value_list_t *attr)
    attr_nonnull(1, 4);

xcb_render_picture_t
x_create_picture_with_visual(struct x_connection *, int w, int h, xcb_visualid_t visual,
                             uint32_t valuemask,
                             const xcb_render_create_picture_value_list_t *attr)
    attr_nonnull(1);

/// Fetch a X region and store it in a pixman region
bool x_fetch_region(struct x_connection *, xcb_xfixes_region_t r, region_t *res);

/// Create a X region from a pixman region
uint32_t x_create_region(struct x_connection *c, const region_t *reg);

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
void x_print_error(unsigned long serial, uint8_t major, uint16_t minor, uint8_t error_code);
void x_log_error(enum log_level level, unsigned long serial, uint8_t major,
                 uint16_t minor, uint8_t error_code);

/*
 * Convert a xcb_generic_error_t to a string that describes the error
 *
 * @return a pointer to a string. this pointer shouldn NOT be freed, same buffer is used
 *         for multiple calls to this function,
 */
const char *x_strerror(xcb_generic_error_t *e);

xcb_pixmap_t x_create_pixmap(struct x_connection *, uint8_t depth, int width, int height);

bool x_validate_pixmap(struct x_connection *, xcb_pixmap_t pxmap);

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

bool x_fence_sync(struct x_connection *, xcb_sync_fence_t);

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

xcb_render_pictformat_t
x_get_pictfmt_for_standard(struct x_connection *c, xcb_pict_standard_t std);

xcb_screen_t *x_screen_of_display(struct x_connection *c, int screen);

/// Populates a `struct x_monitors` with the current monitor configuration.
void x_update_monitors(struct x_connection *, struct x_monitors *);
/// Free memory allocated for a `struct x_monitors`.
void x_free_monitor_info(struct x_monitors *);

uint32_t attr_deprecated xcb_generate_id(xcb_connection_t *c);

/// Ask X server to send us a notification for the next end of vblank.
void x_request_vblank_event(struct x_connection *c, xcb_window_t window, uint64_t msc);

/// Update screen_is_off to reflect the current DPMS state.
///
/// Returns true if the DPMS state was successfully queried, false otherwise.
bool x_check_dpms_status(struct x_connection *c, bool *screen_is_off);
