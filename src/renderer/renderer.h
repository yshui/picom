// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdbool.h>
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

void renderer_free(struct backend_base *backend, struct renderer *r);
struct renderer *renderer_new(struct backend_base *backend, double shadow_radius,
                              struct color shadow_color, bool dithered_present);
bool renderer_render(struct renderer *r, struct backend_base *backend,
                     image_handle root_image, struct layout_manager *lm,
                     struct command_builder *cb, void *blur_context, uint64_t render_start_us,
                     xcb_sync_fence_t xsync_fence, bool use_damage, bool monitor_repaint,
                     bool force_blend, bool blur_frame, bool inactive_dim_fixed,
                     double max_brightness, const struct x_monitors *monitors,
                     const struct shader_info *shaders, uint64_t *after_damage_us);
