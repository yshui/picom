// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

#include <xcb/render.h>
#include <xcb/xcb_image.h>

#include <stdbool.h>

#include "config.h"

struct session;
struct win;
struct conv;
struct backend_base;
struct backend_operations;
struct x_connection;

struct dual_kawase_params {
	/// Number of downsample passes
	int iterations;
	/// Pixel offset for down- and upsample
	float offset;
	/// Save area around blur target (@ref resize_width, @ref resize_height)
	int expand;
};

uint8_t *make_shadow(struct x_connection *c, const conv *kernel, ivec2 window_size,
                     ivec2 *out_shadow_size, int *out_shadow_stride);

xcb_render_picture_t
solid_picture(struct x_connection *, bool argb, double a, double r, double g, double b);

void init_backend_base(struct backend_base *base, session_t *ps);

struct conv **generate_blur_kernel(enum blur_method method, void *args, int *kernel_count);
struct dual_kawase_params *generate_dual_kawase_params(void *args);

uint32_t backend_no_quirks(struct backend_base *base attr_unused);
