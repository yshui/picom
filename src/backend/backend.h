// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018, Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>

#include "compiler.h"
#include "kernel.h"
#include "region.h"
#include "x.h"

typedef struct session session_t;
typedef struct win win;

struct backend_operations;

typedef struct backend_base {
	struct backend_operations *ops;
	xcb_connection_t *c;
	xcb_window_t root;
	// ...
} backend_t;

enum image_operations {
	// Invert the color of the image
	IMAGE_OP_INVERT_COLOR,
	// Dim the image, argument is the percentage
	IMAGE_OP_DIM,
	// Multiply the alpha channel by the argument
	IMAGE_OP_APPLY_ALPHA,
	// Same as APPLY_ALPHA, but `reg_op` is ignored and the operation applies to the
	// full image
	IMAGE_OP_APPLY_ALPHA_ALL,
};

struct backend_operations {

	// ===========    Initialization    ===========

	/// Initialize the backend, prepare for rendering to the target window.
	/// Here is how you should choose target window:
	///    1) if ps->overlay is not XCB_NONE, use that
	///    2) use ps->root otherwise
	/// XXX make the target window a parameter
	backend_t *(*init)(session_t *) attr_nonnull(1);
	void (*deinit)(backend_t *backend_data) __attribute__((nonnull(1)));

	/// Called when rendering will be stopped for an unknown amount of
	/// time (e.g. screen is unredirected). Free some resources.
	void (*pause)(backend_t *backend_data, session_t *ps);

	/// Called before rendering is resumed
	void (*resume)(backend_t *backend_data, session_t *ps);

	/// Called when root property changed, returns the new
	/// backend_data. Even if the backend_data changed, all
	/// the existing win_data returned by prepare_win should
	/// remain valid.
	///
	/// Optional
	void *(*root_change)(backend_t *backend_data, session_t *ps);

	// ===========      Rendering      ============

	/// Called before any compose() calls.
	///
	/// Usually the backend should clear the buffer, or paint a background
	/// on the buffer (usually the wallpaper).
	///
	/// Optional
	void (*prepare)(backend_t *backend_data, const region_t *reg_damage);

	/**
	 * Paint the content of an image onto the (possibly buffered)
	 * target picture.
	 *
	 * @param backend_data the backend data
	 * @param image_data   the image to paint
	 * @param dst_x, dst_y the top left corner of the image in the target
	 * @param reg_paint    the clip region, in target coordinates
	 * @param reg_visibile the visible region, in target coordinates
	 */
	void (*compose)(backend_t *backend_data, void *image_data, int dst_x, int dst_y,
	                const region_t *reg_paint, const region_t *reg_visible);

	/// Fill rectangle of target, mostly for debug purposes, optional.
	void (*fill_rectangle)(backend_t *backend_data, double r, double g, double b, double a,
			       int x, int y, int width, int height, const region_t *clip);

	/// Blur a given region on of the target.
	bool (*blur)(backend_t *backend_data, double opacity, const region_t *reg_blur,
	             const region_t *reg_visible) attr_nonnull(1, 3, 4);

	/// Present the buffered target picture onto the screen. If target
	/// is not buffered, this should be NULL. Otherwise, it should always
	/// be non-NULL.
	///
	/// Optional
	void (*present)(backend_t *backend_data) attr_nonnull(1);

	/**
	 * Bind a X pixmap to the backend's internal image data structure.
	 *
	 * @param backend_data backend data
	 * @param pixmap X pixmap to bind
	 * @param fmt information of the pixmap's visual
	 * @param owned whether the ownership of the pixmap is transfered to the backend
	 * @return backend internal data structure bound with this pixmap
	 */
	void *(*bind_pixmap)(backend_t *backend_data, xcb_pixmap_t pixmap,
	                     struct xvisual_info fmt, bool owned);

	/// Create a shadow image based on the parameters
	void *(*render_shadow)(backend_t *backend_data, int width, int height,
	                       const conv *kernel, double r, double g, double b, double a);

	// ============ Resource management ===========

	// XXX Thoughts: calling release_image and render_* for every config notify
	//     is wasteful, since there can be multiple such notifies per drawing.
	//     But if we don't, it can mean there will be a state where is window is
	//     mapped and visible, but there is no win_data attached to it. We don't
	//     want to break that assumption as for now. We need to reconsider this.

	/// Free resources associated with an image data structure
	void (*release_image)(backend_t *backend_data, void *img_data)
	    attr_nonnull(1, 2);

	// ===========        Query         ===========

	/// Return if a window has transparent content. Guaranteed to only
	/// be called after render_win is called.
	///
	/// This function is needed because some backend might change the content of the
	/// window (e.g. when using a custom shader with the glx backend), so we only now
	/// the transparency after the window is rendered
	bool (*is_image_transparent)(backend_t *backend_data, void *image_data)
	    attr_nonnull(1, 2);

	/// Get the age of the buffer content we are currently rendering ontop
	/// of. The buffer that has just been `present`ed has a buffer age of 1.
	/// Everytime `present` is called, buffers get older. Return -1 if the
	/// buffer is empty.
	int (*buffer_age)(backend_t *backend_data);

	/// The maximum number buffer_age might return.
	int max_buffer_age;

	// ===========    Post-processing   ============
	/**
	 * Manipulate an image
	 *
	 * @param backend_data backend data
	 * @param op           the operation to perform
	 * @param image_data   an image data structure returned by the backend
	 * @param reg_op       the clip region, define the part of the image to be
	 *                     operated on.
	 * @param reg_visible  define the part of the image that will eventually
	 *                     be visible on screen. this is a hint to the backend
	 *                     for optimization purposes.
	 * @param args         extra arguments, operation specific
	 * @return a new image data structure containing the result
	 */
	bool (*image_op)(backend_t *backend_data, enum image_operations op, void *image_data,
	                 const region_t *reg_op, const region_t *reg_visible, void *args);

	/// Create another instance of the `image_data`. All `image_op` calls on the
	/// returned image should not affect the original image
	void *(*copy)(backend_t *base, const void *image_data, const region_t *reg_visible);

	// ===========         Hooks        ============
	/// Let the backend hook into the event handling queue
};

typedef backend_t *(*backend_init_fn)(session_t *ps) attr_nonnull(1);

extern struct backend_operations *backend_list[];

bool default_is_win_transparent(void *, win *, void *);
bool default_is_frame_transparent(void *, win *, void *);
void paint_all_new(session_t *ps, win *const t, bool ignore_damage) attr_nonnull(1);

// vim: set noet sw=8 ts=8 :
