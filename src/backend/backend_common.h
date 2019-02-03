// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

#include <xcb/render.h>
#include <xcb/xcb_image.h>

#include <stdbool.h>

#include "region.h"

typedef struct session session_t;
typedef struct win win;
typedef struct conv conv;

bool build_shadow(session_t *ps, double opacity, const int width, const int height,
                  xcb_render_picture_t shadow_pixel, xcb_pixmap_t *pixmap,
                  xcb_render_picture_t *pict);

xcb_render_picture_t
solid_picture(session_t *ps, bool argb, double a, double r, double g, double b);

xcb_image_t *
make_shadow(xcb_connection_t *c, const conv *kernel, double opacity, int width, int height);

/// The default implementation of `is_win_transparent`, it simply looks at win::mode. So
/// this is not suitable for backends that alter the content of windows
bool default_is_win_transparent(void *, win *, void *);

/// The default implementation of `is_frame_transparent`, it uses win::frame_opacity. Same
/// caveat as `default_is_win_transparent` applies.
bool default_is_frame_transparent(void *, win *, void *);
