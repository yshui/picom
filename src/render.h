// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

#include <stdbool.h>
#include <xcb/render.h>
#include <xcb/xcb.h>
#ifdef CONFIG_OPENGL
#include "backend/gl/glx.h"
#endif
#include "region.h"

typedef struct _glx_texture glx_texture_t;
typedef struct glx_prog_main glx_prog_main_t;
typedef struct session session_t;

struct managed_win;

typedef struct paint {
	xcb_pixmap_t pixmap;
	xcb_render_picture_t pict;
	glx_texture_t *ptex;
#ifdef CONFIG_OPENGL
	struct glx_fbconfig_info *fbcfg;
#endif
} paint_t;

typedef struct clip {
	xcb_render_picture_t pict;
	int x;
	int y;
} clip_t;

void render(session_t *ps, int x, int y, int dx, int dy, int w, int h, int fullw,
            int fullh, double opacity, bool argb, bool neg, int cr,
            xcb_render_picture_t pict, glx_texture_t *ptex, const region_t *reg_paint,
            const glx_prog_main_t *pprogram, clip_t *clip);
void paint_one(session_t *ps, struct managed_win *w, const region_t *reg_paint);

void paint_all(session_t *ps, struct managed_win *const t, bool ignore_damage);

void free_picture(xcb_connection_t *c, xcb_render_picture_t *p);

void free_paint(session_t *ps, paint_t *ppaint);
void free_root_tile(session_t *ps);

bool init_render(session_t *ps);
void deinit_render(session_t *ps);

int maximum_buffer_age(session_t *);
