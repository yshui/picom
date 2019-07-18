// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

#include <xcb/render.h>
#include <xcb/xcb_image.h>

#include <stdbool.h>

#include "config.h"
#include "region.h"

typedef struct session session_t;
typedef struct win win;
typedef struct conv conv;
typedef struct backend_base backend_t;
struct backend_operations;

bool build_shadow(xcb_connection_t *, xcb_drawable_t, double opacity, int width,
                  int height, const conv *kernel, xcb_render_picture_t shadow_pixel,
                  xcb_pixmap_t *pixmap, xcb_render_picture_t *pict);

xcb_render_picture_t solid_picture(xcb_connection_t *, xcb_drawable_t, bool argb,
                                   double a, double r, double g, double b);

xcb_image_t *
make_shadow(xcb_connection_t *c, const conv *kernel, double opacity, int width, int height);

/// The default implementation of `is_win_transparent`, it simply looks at win::mode. So
/// this is not suitable for backends that alter the content of windows
bool default_is_win_transparent(void *, win *, void *);

/// The default implementation of `is_frame_transparent`, it uses win::frame_opacity. Same
/// caveat as `default_is_win_transparent` applies.
bool default_is_frame_transparent(void *, win *, void *);

void *
default_backend_render_shadow(backend_t *backend_data, int width, int height,
                              const conv *kernel, double r, double g, double b, double a);

void init_backend_base(struct backend_base *base, session_t *ps);

struct conv **generate_blur_kernel(enum blur_method method, void *args, int *kernel_count);
