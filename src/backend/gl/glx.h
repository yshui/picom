// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE-mit for more information.
 *
 */

#pragma once

#include <ctype.h>
#include <locale.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <assert.h>
#include <stddef.h>

#include "common.h"

void
glx_destroy(session_t *ps);

bool
glx_reinit(session_t *ps, bool need_render);

void
glx_on_root_change(session_t *ps);

bool
glx_bind_pixmap(session_t *ps, glx_texture_t **pptex, xcb_pixmap_t pixmap,
    unsigned width, unsigned height, unsigned depth);

void
glx_release_pixmap(session_t *ps, glx_texture_t *ptex);

void glx_paint_pre(session_t *ps, region_t *preg)
__attribute__((nonnull(1, 2)));

/**
 * Check if a texture is binded, or is binded to the given pixmap.
 */
static inline bool
glx_tex_binded(const glx_texture_t *ptex, xcb_pixmap_t pixmap) {
  return ptex && ptex->glpixmap && ptex->texture
    && (!pixmap || pixmap == ptex->pixmap);
}

