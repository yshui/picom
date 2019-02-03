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

#include "common.h"
#include "region.h"
#include "render.h"
#include "compiler.h"
#include "win.h"
#include "log.h"

#include <xcb/xcb.h>
#include <xcb/render.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>
#include <GL/glx.h>

/**
 * Check if a word is in string.
 */
static inline bool
wd_is_in_str(const char *haystick, const char *needle) {
  if (!haystick)
    return false;

  assert(*needle);

  const char *pos = haystick - 1;
  while ((pos = strstr(pos + 1, needle))) {
    // Continue if it isn't a word boundary
    if (((pos - haystick) && !isspace(*(pos - 1)))
        || (strlen(pos) > strlen(needle) && !isspace(pos[strlen(needle)])))
      continue;
    return true;
  }

  return false;
}

/**
 * Check if a GLX extension exists.
 */
static inline bool
glx_hasglxext(session_t *ps, const char *ext) {
  const char *glx_exts = glXQueryExtensionsString(ps->dpy, ps->scr);
  if (!glx_exts) {
    log_error("Failed get GLX extension list.");
    return false;
  }

  bool found = wd_is_in_str(glx_exts, ext);
  if (!found)
    log_info("Missing GLX extension %s.", ext);

  return found;
}

bool
glx_dim_dst(session_t *ps, int dx, int dy, int width, int height, float z,
    GLfloat factor, const region_t *reg_tgt);

bool
glx_render(session_t *ps, const glx_texture_t *ptex,
    int x, int y, int dx, int dy, int width, int height, int z,
    double opacity, bool argb, bool neg,
    const region_t *reg_tgt,
    const glx_prog_main_t *pprogram);

bool
glx_init(session_t *ps, bool need_render);

void
glx_destroy(session_t *ps);

bool
glx_reinit(session_t *ps, bool need_render);

void
glx_on_root_change(session_t *ps);

bool
glx_init_blur(session_t *ps);

#ifdef CONFIG_OPENGL
bool
glx_load_prog_main(session_t *ps,
    const char *vshader_str, const char *fshader_str,
    glx_prog_main_t *pprogram);
#endif

bool
glx_bind_pixmap(session_t *ps, glx_texture_t **pptex, xcb_pixmap_t pixmap,
    unsigned width, unsigned height, const struct glx_fbconfig_info *);

void
glx_release_pixmap(session_t *ps, glx_texture_t *ptex);

void glx_paint_pre(session_t *ps, region_t *preg)
attr_nonnull(1, 2);

/**
 * Check if a texture is binded, or is binded to the given pixmap.
 */
static inline bool
glx_tex_binded(const glx_texture_t *ptex, xcb_pixmap_t pixmap) {
  return ptex && ptex->glpixmap && ptex->texture
    && (!pixmap || pixmap == ptex->pixmap);
}

void
glx_set_clip(session_t *ps, const region_t *reg);

bool
glx_blur_dst(session_t *ps, int dx, int dy, int width, int height, float z,
    GLfloat factor_center,
    const region_t *reg_tgt,
    glx_blur_cache_t *pbc);

GLuint
glx_create_shader(GLenum shader_type, const char *shader_str);

GLuint
glx_create_program(const GLuint * const shaders, int nshaders);

GLuint
glx_create_program_from_str(const char *vert_shader_str,
    const char *frag_shader_str);

unsigned char *
glx_take_screenshot(session_t *ps, int *out_length);

/**
 * Check if there's a GLX context.
 */
static inline bool
glx_has_context(session_t *ps) {
  return ps->psglx && ps->psglx->context;
}

/**
 * Ensure we have a GLX context.
 */
static inline bool
ensure_glx_context(session_t *ps) {
  // Create GLX context
  if (!glx_has_context(ps))
    glx_init(ps, false);

  return ps->psglx->context;
}

/**
 * Free a GLX texture.
 */
static inline void
free_texture_r(session_t *ps, GLuint *ptexture) {
  if (*ptexture) {
    assert(glx_has_context(ps));
    glDeleteTextures(1, ptexture);
    *ptexture = 0;
  }
}

/**
 * Free a GLX Framebuffer object.
 */
static inline void
free_glx_fbo(session_t *ps, GLuint *pfbo) {
  if (*pfbo) {
    glDeleteFramebuffers(1, pfbo);
    *pfbo = 0;
  }
  assert(!*pfbo);
}

/**
 * Free data in glx_blur_cache_t on resize.
 */
static inline void
free_glx_bc_resize(session_t *ps, glx_blur_cache_t *pbc) {
  free_texture_r(ps, &pbc->textures[0]);
  free_texture_r(ps, &pbc->textures[1]);
  pbc->width = 0;
  pbc->height = 0;
}

/**
 * Free a glx_blur_cache_t
 */
static inline void
free_glx_bc(session_t *ps, glx_blur_cache_t *pbc) {
  free_glx_fbo(ps, &pbc->fbo);
  free_glx_bc_resize(ps, pbc);
}

/**
 * Free a glx_texture_t.
 */
static inline void
free_texture(session_t *ps, glx_texture_t **pptex) {
  glx_texture_t *ptex = *pptex;

  // Quit if there's nothing
  if (!ptex)
    return;

  glx_release_pixmap(ps, ptex);

  free_texture_r(ps, &ptex->texture);

  // Free structure itself
  free(ptex);
  *pptex = NULL;
  assert(!*pptex);
}

/**
 * Free GLX part of paint_t.
 */
static inline void
free_paint_glx(session_t *ps, paint_t *ppaint) {
  free_texture(ps, &ppaint->ptex);
}

/**
 * Free GLX part of win.
 */
static inline void
free_win_res_glx(session_t *ps, win *w) {
  free_paint_glx(ps, &w->paint);
  free_paint_glx(ps, &w->shadow_paint);
#ifdef CONFIG_OPENGL
  free_glx_bc(ps, &w->glx_blur_cache);
  free(w->paint.fbcfg);
#endif
}
