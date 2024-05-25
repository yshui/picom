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

xcb_image_t *make_shadow(struct x_connection *c, const conv *kernel, double opacity,
                         int width, int height);
bool build_shadow(struct x_connection *, double opacity, int width, int height,
                  const conv *kernel, xcb_render_picture_t shadow_pixel,
                  xcb_pixmap_t *pixmap, xcb_render_picture_t *pict);

xcb_render_picture_t
solid_picture(struct x_connection *, bool argb, double a, double r, double g, double b);

void init_backend_base(struct backend_base *base, session_t *ps);

struct conv **generate_blur_kernel(enum blur_method method, void *args, int *kernel_count);
struct dual_kawase_params *generate_dual_kawase_params(void *args);

uint32_t backend_no_quirks(struct backend_base *base attr_unused);
