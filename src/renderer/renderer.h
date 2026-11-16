// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <xcb/sync.h>

#include <picom/types.h>

struct renderer;
struct layout_manager;
struct backend_base;
struct command_builder;
struct shader_info;
typedef struct image_handle *image_handle;
struct x_monitors;
struct wm;
struct win_option;
typedef struct pixman_region32 region_t;
typedef struct pixman_box32 rect_t;

/// A monitor whose picture is magnified.
struct renderer_zoom_monitor {
	/// The monitor's rectangle, in root window coordinates.
	int x, y, width, height;
	/// The zoom factor, greater than 1.
	double factor;
	/// Top-left corner of the part of the root window shown on the monitor. The size
	/// of that part is the monitor's size divided by `factor`.
	double view_x, view_y;
};

/// The cursor drawn on top of a magnified monitor.
struct renderer_zoom_cursor {
	/// Pixels of the cursor image, premultiplied ARGB, one 32-bit value per pixel.
	const uint32_t *pixels;
	int width, height;
	/// The hotspot, in the cursor image's coordinates.
	int xhot, yhot;
	/// Changes every time the cursor image changes, never 0.
	uint64_t serial;
	/// Index of the magnified monitor the cursor is drawn on.
	int monitor;
	/// Position of the pointer, in root window coordinates.
	int x, y;
};

/// Everything the renderer needs to know to magnify the screen.
struct renderer_zoom {
	const struct renderer_zoom_monitor *monitors;
	int nmonitors;
	/// The cursor to draw, NULL if no cursor needs to be drawn.
	const struct renderer_zoom_cursor *cursor;
};

void renderer_free(struct backend_base *backend, struct renderer *r);
struct renderer *
renderer_new(struct backend_base *backend, double shadow_radius, bool dithered_present);
bool renderer_render(struct renderer *r, struct backend_base *backend,
                     image_handle root_image, const rect_t *root_image_extent,
                     struct layout_manager *lm, struct command_builder *cb,
                     void *blur_context, uint64_t render_start_us,
                     xcb_sync_fence_t xsync_fence, bool use_damage, bool monitor_repaint,
                     bool force_blend, bool blur_frame, bool inactive_dim_fixed,
                     double max_brightness, const struct x_monitors *monitors,
                     const struct shader_info *root_pixmap_shader,
                     const struct shader_info *shaders, const struct renderer_zoom *zoom,
                     uint64_t *after_damage_us);
