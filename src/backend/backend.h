// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018, Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include "region.h"
#include "compiler.h"

typedef struct session session_t;
typedef struct win win;
typedef struct backend_info {

	// ===========    Initialization    ===========

	/// Initialize the backend, prepare for rendering to the target window.
	/// Here is how you should choose target window:
	///    1) if ps->overlay is not XCB_NONE, use that
	///    2) use ps->root otherwise
	/// XXX make the target window a parameter
	void *(*init)(session_t *ps) __attribute__((nonnull(1)));
	void (*deinit)(void *backend_data, session_t *ps) __attribute__((nonnull(1, 2)));

	/// Called when rendering will be stopped for an unknown amount of
	/// time (e.g. screen is unredirected). Free some resources.
	void (*pause)(void *backend_data, session_t *ps);

	/// Called before rendering is resumed
	void (*resume)(void *backend_data, session_t *ps);

	/// Called when root property changed, returns the new
	/// backend_data. Even if the backend_data changed, all
	/// the existing win_data returned by prepare_win should
	/// remain valid.
	///
	/// Optional
	void *(*root_change)(void *backend_data, session_t *ps);

	/// Called when vsync is toggled after initialization. If vsync is enabled when init()
	/// is called, these function won't be called
	void (*vsync_start)(void *backend_data, session_t *ps);
	void (*vsync_stop)(void *backend_data, session_t *ps);

	// ===========      Rendering      ============

	/// Called before any compose() calls.
	///
	/// Usually the backend should clear the buffer, or paint a background
	/// on the buffer (usually the wallpaper).
	///
	/// Optional?
	void (*prepare)(void *backend_data, session_t *ps, const region_t *reg_paint);

	/// Paint the content of the window onto the (possibly buffered)
	/// target picture. Always called after render_win(). Maybe called
	/// multiple times between render_win() and finish_render_win().
	/// The origin is the top left of the window, exclude the shadow,
	/// (dst_x, dst_y) refers to where the origin should be in the target
	/// buffer.
	void (*compose)(void *backend_data, session_t *ps, win *w, void *win_data,
	                int dst_x, int dst_y, const region_t *reg_paint);

	/// Blur a given region on of the target.
	bool (*blur)(void *backend_data, session_t *ps, double opacity, const region_t *)
	    __attribute__((nonnull(1, 2, 4)));

	/// Present the buffered target picture onto the screen. If target
	/// is not buffered, this should be NULL. Otherwise, it should always
	/// be non-NULL.
	///
	/// Optional
	void (*present)(void *backend_data, session_t *ps) __attribute__((nonnull(1, 2)));

	/**
	 * Render the content of a window into an opaque
	 * data structure. Dimming, shadow and color inversion is handled
	 * here.
	 *
	 * This function is allowed to allocate additional resource needed
	 * for rendering.
	 *
	 * Params:
	 *    reg_paint = the paint region, meaning painting should only
	 *                be happening within that region. It's in global
	 *                coordinates. If NULL, the region of paint is the
	 *                whole screen.
	 */
	void (*render_win)(void *backend_data, session_t *ps, win *w, void *win_data,
	                   const region_t *reg_paint);

	/// Free resource allocated for rendering. After this function is
	/// called, compose() won't be called before render_win is called
	/// another time.
	///
	/// Optional
	void (*finish_render_win)(void *backend_data, session_t *ps, win *w, void *win_data);

	// ============ Resource management ===========

	// XXX Thoughts: calling release_win and prepare_win for every config notify
	//     is wasteful, since there can be multiple such notifies per drawing.
	//     But if we don't, it can mean there will be a state where is window is
	//     mapped and visible, but there is no win_data attached to it. We don't
	//     want to break that assumption as for now. We need to reconsider this.

	/// Create a structure to stored additional data needed for rendering a
	/// window, later used for render() and compose().
	///
	/// Backend can assume this function will only be called with visible
	/// InputOutput windows, and only be called when screen is redirected.
	///
	/// Backend can assume size, shape and visual of the window won't change between
	/// prepare_win() and release_win().
	void *(*prepare_win)(void *backend_data, session_t *ps, win *w)
	    __attribute__((nonnull(1, 2, 3)));

	/// Free resources allocated by prepare()
	void (*release_win)(void *backend_data, session_t *ps, win *w, void *win_data)
	    __attribute__((nonnull(1, 2, 3)));

	// ===========        Query         ===========

	/// Return if a window has transparent content. Guaranteed to only
	/// be called after render_win is called.
	///
	/// This function is needed because some backend might change the content of the
	/// window (e.g. when using a custom shader with the glx backend), so we only now
	/// the transparency after the window is rendered
	bool (*is_win_transparent)(void *backend_data, win *w, void *win_data)
	    __attribute__((nonnull(1, 2)));

	/// Return if the frame window has transparent content. Guaranteed to
	/// only be called after render_win is called.
	///
	/// Same logic as is_win_transparent applies here.
	bool (*is_frame_transparent)(void *backend_data, win *w, void *win_data)
	    __attribute__((nonnull(1, 2)));

	/// Get the age of the buffer content we are currently rendering ontop
	/// of. The buffer that has just been `present`ed has a buffer age of 1.
	/// Everytime `present` is called, buffers get older. Return -1 if the
	/// buffer is empty.
	int (*buffer_age)(void *backend_data, session_t *);

	// ===========         Hooks        ============
	/// Let the backend hook into the event handling queue
} backend_info_t;

extern backend_info_t xrender_backend;
extern backend_info_t glx_backend;
extern backend_info_t *backend_list[];

bool default_is_win_transparent(void *, win *, void *);
bool default_is_frame_transparent(void *, win *, void *);
void paint_all_new(session_t *ps, region_t *region, win *const t) attr_nonnull(1);

// vim: set noet sw=8 ts=8 :
