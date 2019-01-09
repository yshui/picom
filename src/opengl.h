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
#include "log.h"

#include <ctype.h>
#include <locale.h>

/// @brief Wrapper of a GLX FBConfig.
typedef struct glx_fbconfig {
  GLXFBConfig cfg;
  GLint texture_fmt;
  GLint texture_tgts;
  bool y_inverted;
} glx_fbconfig_t;

#ifdef DEBUG_GLX_ERR

/**
 * Get a textual representation of an OpenGL error.
 */
static inline const char *
glx_dump_err_str(GLenum err) {
  switch (err) {
    CASESTRRET(GL_NO_ERROR);
    CASESTRRET(GL_INVALID_ENUM);
    CASESTRRET(GL_INVALID_VALUE);
    CASESTRRET(GL_INVALID_OPERATION);
    CASESTRRET(GL_INVALID_FRAMEBUFFER_OPERATION);
    CASESTRRET(GL_OUT_OF_MEMORY);
    CASESTRRET(GL_STACK_UNDERFLOW);
    CASESTRRET(GL_STACK_OVERFLOW);
  }

  return NULL;
}

/**
 * Check for GLX error.
 *
 * http://blog.nobel-joergensen.com/2013/01/29/debugging-opengl-using-glgeterror/
 */
static inline void
glx_check_err_(session_t *ps, const char *func, int line) {
  if (!ps->psglx->context) return;

  GLenum err = GL_NO_ERROR;

  while (GL_NO_ERROR != (err = glGetError())) {
    const char *errtext = glx_dump_err_str(err);
    if (errtext) {
      log_printf(tls_logger, LOG_LEVEL_ERROR, func, "GLX error at line %d: %s", line,
                 errtext);
    }
    else {
      log_printf(tls_logger, LOG_LEVEL_ERROR, func, "GLX error at line %d: %d", line,
                 err);
    }
  }
}

#define glx_check_err(ps) glx_check_err_(ps, __func__, __LINE__)
#else
#define glx_check_err(ps) ((void) 0)
#endif

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

/**
 * Check if a GLX extension exists.
 */
static inline bool
glx_hasglext(const char *ext) {
  const char *gl_exts = (const char *) glGetString(GL_EXTENSIONS);
  if (!gl_exts) {
    log_error("Failed get GL extension list.");
    return false;
  }

  bool found = wd_is_in_str(gl_exts, ext);
  if (!found)
    log_info("Missing GL extension %s.", ext);

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
    unsigned width, unsigned height, unsigned depth);

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
#endif
}
