// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdbool.h>
#include <xcb/sync.h>

#include <picom/types.h>

#include "config.h"

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
                     const struct shader_info *shaders, image_handle present_override,
                     image_handle *capture_target, bool *frame_changed,
                     uint64_t *after_damage_us);

/// Copy the content of the renderer's back image into `*target`. If `*target` is NULL,
/// or has a different size than the back image, a new image will be created to replace
/// it. Returns false if the back image doesn't exist yet (i.e. no frame has been
/// rendered).
bool renderer_copy_back_image(struct renderer *r, struct backend_base *backend,
                              image_handle *target);

/// Direction of a workspace switch animation
enum ws_switch_direction {
	/// The new desktop comes in from the right
	WS_SWITCH_DIRECTION_RIGHT = 0,
	/// The new desktop comes in from the left
	WS_SWITCH_DIRECTION_LEFT,
	/// The new desktop comes in from the bottom
	WS_SWITCH_DIRECTION_DOWN,
	/// The new desktop comes in from the top
	WS_SWITCH_DIRECTION_UP,
};

/// Render a frame of the workspace switch animation, blending the `from` and `to`
/// screen snapshots according to `progress` (from 0 to 1), and present the result.
bool renderer_render_workspace_switch(struct renderer *r, struct backend_base *backend,
                                      image_handle from, image_handle to,
                                      enum ws_switch_effect effect, double progress,
                                      enum ws_switch_direction direction);

/// Present the given screen snapshot as-is. The image must have the size of the screen.
bool renderer_present_image(struct renderer *r, struct backend_base *backend,
                            image_handle image);
