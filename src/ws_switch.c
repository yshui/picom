// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026, Nikolay Borodin <monsterovich@gmail.com>

#include "ws_switch.h"

#include <stdlib.h>
#include <time.h>
#include <xcb/xproto.h>

#include <picom/backend.h>

#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "picom.h"
#include "renderer/layout.h"
#include "renderer/renderer.h"
#include "wm/wm.h"
#include "x.h"

enum ws_switch_state {
	/// No workspace switch is in progress
	WS_SWITCH_IDLE = 0,
	/// A desktop change has been detected, the "before" snapshot needs to be taken
	/// before the new desktop is rendered.
	WS_SWITCH_CAPTURE_PRE,
	/// The "before" snapshot has been taken, waiting for the new desktop to settle.
	/// Normal frames are rendered, but the "before" snapshot is presented instead.
	WS_SWITCH_WAITING,
	/// Animating between the "before" and "after" snapshots.
	WS_SWITCH_ANIMATING,
};

struct ws_switch {
	enum ws_switch_state state;
	/// Cached value of the _NET_CURRENT_DESKTOP property of the root window
	long current_desktop;
	/// Direction of the slide animation, the side the new desktop comes in from
	enum ws_switch_direction direction;
	/// Workspace grid layout detected from the _NET_DESKTOP_LAYOUT property, or from
	/// the `workspace-layout` config option. `has_layout` is false when neither is
	/// available, in which case the direction falls back to the desktop numbers.
	bool has_layout;
	unsigned layout_orientation;
	unsigned layout_columns;
	unsigned layout_rows;
	unsigned layout_corner;
	/// Snapshot of the screen taken before the desktop switch
	image_handle pre_image;
	/// Snapshot of the screen taken after the desktop switch
	image_handle post_image;
	/// When we started waiting for the new desktop to settle, in milliseconds
	int64_t wait_start_ms;
	/// When the animation started, in milliseconds
	int64_t anim_start_ms;
	/// Number of consecutive frames without any changes
	unsigned quiet_frames;
	/// Number of upcoming normal frames that should be full repaints. The animation
	/// frames present the animation snapshots, not the result of the rendered layout,
	/// and the layout manager ring isn't advanced during them, so after the switch the
	/// buffer age based damage computation could reference pre-switch layouts. Forcing
	/// full repaints for a few frames repopulates the ring with post-switch layouts
	/// before the damage tracking is trusted again.
	unsigned full_repaint_frames;
};

static int64_t ws_switch_now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static long ws_switch_read_current_desktop(session_t *ps) {
	long desktop = 0;
	auto reply = XCB_AWAIT(xcb_get_property, &ps->c, 0, ps->c.screen_info->root,
	                       ps->atoms->a_NET_CURRENT_DESKTOP, XCB_ATOM_CARDINAL, 0, 1);
	if (reply != NULL) {
		if (reply->type == XCB_ATOM_CARDINAL &&
		    xcb_get_property_value_length(reply) >= 4) {
			desktop = *(const uint32_t *)xcb_get_property_value(reply);
		}
		free(reply);
	}
	return desktop;
}

/// Read the workspace grid layout. The user-configured `workspace-layout` option takes
/// precedence; otherwise the layout is detected from the `_NET_DESKTOP_LAYOUT` EWMH
/// property (a CARDINAL[4] array of orientation, columns, rows and starting corner).
static void ws_switch_read_layout(session_t *ps) {
	auto ws = ps->ws_switch;
	if (ps->o.workspace_layout_columns > 0 && ps->o.workspace_layout_rows > 0) {
		ws->has_layout = true;
		ws->layout_orientation = 0;
		ws->layout_columns = (unsigned)ps->o.workspace_layout_columns;
		ws->layout_rows = (unsigned)ps->o.workspace_layout_rows;
		ws->layout_corner = 0;
		return;
	}
	auto reply = XCB_AWAIT(xcb_get_property, &ps->c, 0, ps->c.screen_info->root,
	                       ps->atoms->a_NET_DESKTOP_LAYOUT, XCB_ATOM_CARDINAL, 0, 4);
	if (reply == NULL || reply->type != XCB_ATOM_CARDINAL ||
	    xcb_get_property_value_length(reply) < 4 * 4) {
		free(reply);
		return;
	}
	const auto values = (const uint32_t *)xcb_get_property_value(reply);
	unsigned columns = values[1], rows = values[2];
	if (columns > 0 && rows > 0) {
		ws->has_layout = true;
		ws->layout_orientation = values[0];
		ws->layout_columns = columns;
		ws->layout_rows = rows;
		ws->layout_corner = values[3];
	}
	free(reply);
}

/// Determine the grid position of the desktop with the given index. The position is
/// given in (column, row) coordinates, where the starting corner is at (0, 0) and the
/// second axis grows towards the opposite corner.
static void ws_switch_layout_position(unsigned index, unsigned orientation,
                                      unsigned columns, unsigned rows,
                                      unsigned starting_corner, int *col, int *row) {
	if (orientation == 0) {
		// Horizontal layout: desktops are laid out row by row
		*col = (int)(index % columns);
		*row = (int)(index / columns);
	} else {
		// Vertical layout: desktops are laid out column by column
		*col = (int)(index / rows);
		*row = (int)(index % rows);
	}
	switch (starting_corner) {
	case 1: // top-right
		*col = (int)columns - 1 - *col;
		break;
	case 2: // bottom-left
		*row = (int)rows - 1 - *row;
		break;
	case 3: // bottom-right
		*col = (int)columns - 1 - *col;
		*row = (int)rows - 1 - *row;
		break;
	}
}

/// Determine the direction the new desktop comes in from, based on the workspace grid
/// layout. If no layout is available, fall back to comparing desktop numbers.
static enum ws_switch_direction
ws_switch_direction_for(session_t *ps, long old_desktop, long new_desktop) {
	auto ws = ps->ws_switch;
	if (ws->has_layout) {
		int old_col = 0, old_row = 0, new_col = 0, new_row = 0;
		ws_switch_layout_position((unsigned)old_desktop, ws->layout_orientation,
		                          ws->layout_columns, ws->layout_rows, ws->layout_corner,
		                          &old_col, &old_row);
		ws_switch_layout_position((unsigned)new_desktop, ws->layout_orientation,
		                          ws->layout_columns, ws->layout_rows, ws->layout_corner,
		                          &new_col, &new_row);
		if (new_col > old_col) {
			return WS_SWITCH_DIRECTION_RIGHT;
		}
		if (new_col < old_col) {
			return WS_SWITCH_DIRECTION_LEFT;
		}
		if (new_row > old_row) {
			return WS_SWITCH_DIRECTION_DOWN;
		}
		if (new_row < old_row) {
			return WS_SWITCH_DIRECTION_UP;
		}
	}
	return new_desktop > old_desktop ? WS_SWITCH_DIRECTION_RIGHT
	                                 : WS_SWITCH_DIRECTION_LEFT;
}

struct ws_switch *ws_switch_new(session_t *ps) {
	auto ws = ccalloc(1, struct ws_switch);
	ws->state = WS_SWITCH_IDLE;
	ws->current_desktop = ws_switch_read_current_desktop(ps);
	ws_switch_read_layout(ps);
	return ws;
}

/// Release the snapshots, `ps->backend_data` must be valid.
static void ws_switch_release_images(session_t *ps) {
	auto ws = ps->ws_switch;
	assert(ps->backend_data != NULL);
	if (ws->pre_image != NULL) {
		ps->backend_data->ops.release_image(ps->backend_data, ws->pre_image);
		ws->pre_image = NULL;
	}
	if (ws->post_image != NULL) {
		ps->backend_data->ops.release_image(ps->backend_data, ws->post_image);
		ws->post_image = NULL;
	}
}

void ws_switch_cancel(session_t *ps) {
	auto ws = ps->ws_switch;
	if (ws == NULL) {
		return;
	}
	if (ps->backend_data != NULL) {
		ws_switch_release_images(ps);
	}
	assert(ws->pre_image == NULL && ws->post_image == NULL);
	ws->state = WS_SWITCH_IDLE;
}

void ws_switch_free(session_t *ps, struct ws_switch *ws) {
	ws_switch_cancel(ps);
	free(ws);
}

void ws_switch_desktop_changed(session_t *ps) {
	auto ws = ps->ws_switch;
	auto new_desktop = ws_switch_read_current_desktop(ps);
	if (new_desktop == ws->current_desktop) {
		return;
	}

	auto old_desktop = ws->current_desktop;
	ws->current_desktop = new_desktop;
	if (!ps->o.workspace_animation) {
		return;
	}

	log_debug("Desktop switched from %ld to %ld", old_desktop, new_desktop);
	ws->direction = ws_switch_direction_for(ps, old_desktop, new_desktop);
	// If another switch is already in progress, restart the animation targeting the
	// new desktop.
	ws_switch_cancel(ps);
	ws->state = WS_SWITCH_CAPTURE_PRE;
	queue_redraw(ps);
}

bool ws_switch_is_active(session_t *ps) {
	return ps->ws_switch != NULL && ps->ws_switch->state != WS_SWITCH_IDLE;
}

bool ws_switch_consume_full_repaint(session_t *ps) {
	auto ws = ps->ws_switch;
	if (ws == NULL || ws->full_repaint_frames == 0) {
		return false;
	}
	ws->full_repaint_frames--;
	return true;
}

/// Render a normal frame, but present `present_override` instead of the rendered result
/// if it's not NULL. If `present_override` is not NULL and `capture_target` is not NULL,
/// the rendered frame is copied into `*capture_target` before the override is presented.
static bool ws_switch_render_normal(session_t *ps, uint64_t render_start_us,
                                    image_handle present_override, bool full_repaint,
                                    image_handle *capture_target, bool *frame_changed) {
	layout_manager_append_layout(
	    ps->layout_manager, ps->wm, ps->root_image_generation,
	    (ivec2){.width = ps->root_width, .height = ps->root_height});
	uint64_t after_damage_us = 0;
	return renderer_render(
	    ps->renderer, ps->backend_data, ps->root_image, &ps->root_image_extent,
	    ps->layout_manager, ps->command_builder, ps->backend_blur_context,
	    render_start_us, ps->sync_fence, full_repaint ? false : ps->o.use_damage,
	    ps->o.monitor_repaint, ps->o.force_win_blend, ps->o.blur_background_frame,
	    ps->o.inactive_dim_fixed, ps->o.max_brightness,
	    ps->o.crop_shadow_to_monitor ? &ps->monitors : NULL, ps->root_pixmap_shader,
	    ps->shaders, present_override, capture_target, frame_changed, &after_damage_us);
}

bool ws_switch_render(session_t *ps, uint64_t render_start_us, bool window_animation) {
	auto ws = ps->ws_switch;
	if (ws == NULL || ws->state == WS_SWITCH_IDLE) {
		return false;
	}
	if (!ps->redirected || ps->backend_data == NULL || ps->renderer == NULL ||
	    ps->layout_manager == NULL) {
		ws_switch_cancel(ps);
		return false;
	}

	switch (ws->state) {
	case WS_SWITCH_CAPTURE_PRE: {
		// force a full repaint before snapshotting
		bool ok = ws_switch_render_normal(ps, render_start_us, NULL, true, NULL,
		                                   NULL);
		if (!ok) {
			log_warn("forced full repaint before snapshot failed");
			ws_switch_cancel(ps);
			return false;
		}

		// The renderer's back image still holds the last frame of the previous
		// desktop, take a snapshot of it before the new desktop is rendered.
		if (!renderer_copy_back_image(ps->renderer, ps->backend_data, &ws->pre_image)) {
			log_warn("Failed to take a snapshot of the previous desktop, the "
					"workspace switch animation is cancelled.");
			ws_switch_cancel(ps);
			return false;
		}
		ws->state = WS_SWITCH_WAITING;
		ws->wait_start_ms = ws_switch_now_ms();
		ws->quiet_frames = 0;
	} fallthrough();
	case WS_SWITCH_WAITING: {
		// Render the new desktop, while presenting the snapshot of the previous
		// desktop, until the new desktop settles. The rendered frame is captured
		// into `post_image` as the "after" snapshot, before the pre snapshot is
		// presented in its place.
		bool frame_changed = false;
		// Force a full repaint while waiting for the new desktop to settle, so
		// the "after" snapshot captures a clean, fully rendered frame of the
		// new desktop, instead of a mix of the old and the new desktop.
		if (!ws_switch_render_normal(ps, render_start_us, ws->pre_image, true,
		                             &ws->post_image, &frame_changed)) {
			return false;
		}
		bool quiet = !ps->pending_updates && !window_animation;
		ws->quiet_frames = quiet ? ws->quiet_frames + 1 : 0;
		// When a full repaint is forced, `frame_changed` is always true, so we
		// fall back to counting consecutive quiet frames.
		bool settled =
		    quiet && (frame_changed ? ws->quiet_frames >= 2 : !frame_changed);
		bool timed_out = ws_switch_now_ms() - ws->wait_start_ms >=
		                 ps->o.workspace_animation_wait;
		if (settled || timed_out) {
			if (timed_out && !settled) {
				log_warn("The new desktop didn't settle in time, starting "
				         "the workspace switch animation anyway.");
			}
			ws->state = WS_SWITCH_ANIMATING;
			ws->anim_start_ms = ws_switch_now_ms();
		}
		return true;
	}
	case WS_SWITCH_ANIMATING: {
		double progress =
		    (double)(ws_switch_now_ms() - ws->anim_start_ms) /
		    (double)max2(ps->o.workspace_animation_duration, 1);
		progress = min2(progress, 1.0);
		if (progress >= 1.0) {
			bool frame_changed = false;
			bool succeeded = ws_switch_render_normal(ps, render_start_us, NULL, true,
			                                         NULL, &frame_changed);
			ws->full_repaint_frames =
			    layout_manager_max_buffer_age(ps->layout_manager) + 1;
			ws_switch_cancel(ps);
			queue_redraw(ps);
			return succeeded;
		}
		// Render an intermediate frame of the animation, blending between the
		// "before" and the "after" snapshots.
		return renderer_render_workspace_switch(
		    ps->renderer, ps->backend_data, ws->pre_image, ws->post_image,
		    ps->o.workspace_animation_effect, progress, ws->direction);
	}
	default: unreachable();
	}
}
