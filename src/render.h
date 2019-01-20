// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

#include <xcb/xcb.h>
#include <xcb/render.h>
#include <stdbool.h>
#include "region.h"

typedef struct _glx_texture glx_texture_t;
typedef struct glx_prog_main glx_prog_main_t;
typedef struct win win;
typedef struct session session_t;

typedef struct paint {
  xcb_pixmap_t pixmap;
  xcb_render_picture_t pict;
  glx_texture_t *ptex;
} paint_t;

void
render(session_t *ps, int x, int y, int dx, int dy, int wid, int hei,
    double opacity, bool argb, bool neg,
    xcb_render_picture_t pict, glx_texture_t *ptex,
    const region_t *reg_paint, const glx_prog_main_t *pprogram);
void
paint_one(session_t *ps, win *w, const region_t *reg_paint);

void
paint_all(session_t *ps, region_t *region, const region_t *region_real, win * const t);

void free_picture(xcb_connection_t *c, xcb_render_picture_t *p);

void free_paint(session_t *ps, paint_t *ppaint);
void free_root_tile(session_t *ps);

bool init_render(session_t *ps);
void deinit_render(session_t *ps);
