/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE for more information.
 *
 */

#include "opengl.h"

/**
 * Initialize OpenGL.
 */
bool
glx_init(session_t *ps, bool need_render) {
  bool success = false;
  XVisualInfo *pvis = NULL;

  // Check for GLX extension
  if (!ps->glx_exists) {
    if (glXQueryExtension(ps->dpy, &ps->glx_event, &ps->glx_error))
      ps->glx_exists = true;
    else {
      printf_errf("(): No GLX extension.");
      goto glx_init_end;
    }
  }

  // Get XVisualInfo
  pvis = get_visualinfo_from_visual(ps, ps->vis);
  if (!pvis) {
    printf_errf("(): Failed to acquire XVisualInfo for current visual.");
    goto glx_init_end;
  }

  // Ensure the visual is double-buffered
  if (need_render) {
    int value = 0;
    if (Success != glXGetConfig(ps->dpy, pvis, GLX_USE_GL, &value) || !value) {
      printf_errf("(): Root visual is not a GL visual.");
      goto glx_init_end;
    }

    if (Success != glXGetConfig(ps->dpy, pvis, GLX_DOUBLEBUFFER, &value)
        || !value) {
      printf_errf("(): Root visual is not a double buffered GL visual.");
      goto glx_init_end;
    }
  }

  // Ensure GLX_EXT_texture_from_pixmap exists
  if (need_render && !glx_hasglxext(ps, "GLX_EXT_texture_from_pixmap"))
    goto glx_init_end;

  // Get GLX context
  ps->glx_context = glXCreateContext(ps->dpy, pvis, None, GL_TRUE);

  if (!ps->glx_context) {
    printf_errf("(): Failed to get GLX context.");
    goto glx_init_end;
  }

  // Attach GLX context
  if (!glXMakeCurrent(ps->dpy, get_tgt_window(ps), ps->glx_context)) {
    printf_errf("(): Failed to attach GLX context.");
    goto glx_init_end;
  }

  // Ensure we have a stencil buffer. X Fixes does not guarantee rectangles
  // in regions don't overlap, so we must use stencil buffer to make sure
  // we don't paint a region for more than one time, I think?
  if (need_render && !ps->o.glx_no_stencil) {
    GLint val = 0;
    glGetIntegerv(GL_STENCIL_BITS, &val);
    if (!val) {
      printf_errf("(): Target window doesn't have stencil buffer.");
      goto glx_init_end;
    }
  }

  // Check GL_ARB_texture_non_power_of_two, requires a GLX context and
  // must precede FBConfig fetching
  if (need_render)
    ps->glx_has_texture_non_power_of_two = glx_hasglext(ps,
        "GL_ARB_texture_non_power_of_two");

  // Acquire function addresses
  if (need_render) {
    ps->glXBindTexImageProc = (f_BindTexImageEXT)
      glXGetProcAddress((const GLubyte *) "glXBindTexImageEXT");
    ps->glXReleaseTexImageProc = (f_ReleaseTexImageEXT)
      glXGetProcAddress((const GLubyte *) "glXReleaseTexImageEXT");
    if (!ps->glXBindTexImageProc || !ps->glXReleaseTexImageProc) {
      printf_errf("(): Failed to acquire glXBindTexImageEXT() / glXReleaseTexImageEXT().");
      goto glx_init_end;
    }

    if (ps->o.glx_use_copysubbuffermesa) {
      ps->glXCopySubBufferProc = (f_CopySubBuffer)
        glXGetProcAddress((const GLubyte *) "glXCopySubBufferMESA");
      if (!ps->glXCopySubBufferProc) {
        printf_errf("(): Failed to acquire glXCopySubBufferMESA().");
        goto glx_init_end;
      }
    }
  }

  // Acquire FBConfigs
  if (need_render && !glx_update_fbconfig(ps))
    goto glx_init_end;

  if (need_render) {
    glx_on_root_change(ps);

    // glEnable(GL_DEPTH_TEST);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glDisable(GL_BLEND);

    if (!ps->o.glx_no_stencil) {
      // Initialize stencil buffer
      glClear(GL_STENCIL_BUFFER_BIT);
      glDisable(GL_STENCIL_TEST);
      glStencilMask(0x1);
      glStencilFunc(GL_EQUAL, 0x1, 0x1);
    }

    // Clear screen
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    // glXSwapBuffers(ps->dpy, get_tgt_window(ps));
  }

  success = true;

glx_init_end:
  cxfree(pvis);

  if (!success)
    glx_destroy(ps);

  return success;
}

/**
 * Destroy GLX related resources.
 */
void
glx_destroy(session_t *ps) {
#ifdef CONFIG_VSYNC_OPENGL_GLSL
  // Free GLSL shaders/programs
  if (ps->glx_frag_shader_blur)
    glDeleteShader(ps->glx_frag_shader_blur);
  if (ps->glx_prog_blur)
    glDeleteProgram(ps->glx_prog_blur);
#endif

  // Free FBConfigs
  for (int i = 0; i <= OPENGL_MAX_DEPTH; ++i) {
    free(ps->glx_fbconfigs[i]);
    ps->glx_fbconfigs[i] = NULL;
  }

  // Destroy GLX context
  if (ps->glx_context) {
    glXDestroyContext(ps->dpy, ps->glx_context);
    ps->glx_context = NULL;
  }
}

/**
 * Callback to run on root window size change.
 */
void
glx_on_root_change(session_t *ps) {
  glViewport(0, 0, ps->root_width, ps->root_height);

  // Initialize matrix, copied from dcompmgr
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, ps->root_width, 0, ps->root_height, -1000.0, 1000.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
}

/**
 * Initialize GLX blur filter.
 */
bool
glx_init_blur(session_t *ps) {
#ifdef CONFIG_VSYNC_OPENGL_GLSL
  // Build shader
  static const char *FRAG_SHADER_BLUR =
    "#version 110\n"
    "uniform float offset_x;\n"
    "uniform float offset_y;\n"
    "uniform float factor_center;\n"
    "uniform sampler2D tex_scr;\n"
    "\n"
    "void main() {\n"
    "  vec4 sum = vec4(0.0, 0.0, 0.0, 0.0);\n"
    "  sum += texture2D(tex_scr, vec2(gl_TexCoord[0].x - offset_x, gl_TexCoord[0].y - offset_y));\n"
    "  sum += texture2D(tex_scr, vec2(gl_TexCoord[0].x - offset_x, gl_TexCoord[0].y));\n"
    "  sum += texture2D(tex_scr, vec2(gl_TexCoord[0].x - offset_x, gl_TexCoord[0].y + offset_y));\n"
    "  sum += texture2D(tex_scr, vec2(gl_TexCoord[0].x, gl_TexCoord[0].y - offset_y));\n"
    "  sum += texture2D(tex_scr, vec2(gl_TexCoord[0].x, gl_TexCoord[0].y)) * factor_center;\n"
    "  sum += texture2D(tex_scr, vec2(gl_TexCoord[0].x, gl_TexCoord[0].y + offset_y));\n"
    "  sum += texture2D(tex_scr, vec2(gl_TexCoord[0].x + offset_x, gl_TexCoord[0].y - offset_y));\n"
    "  sum += texture2D(tex_scr, vec2(gl_TexCoord[0].x + offset_x, gl_TexCoord[0].y));\n"
    "  sum += texture2D(tex_scr, vec2(gl_TexCoord[0].x + offset_x, gl_TexCoord[0].y + offset_y));\n"
    "  gl_FragColor = sum / (factor_center + 8.0);\n"
    "}\n"
    ;
  ps->glx_frag_shader_blur = glx_create_shader(GL_FRAGMENT_SHADER, FRAG_SHADER_BLUR);
  if (!ps->glx_frag_shader_blur) {
    printf_errf("(): Failed to create fragment shader.");
    return false;
  }

  ps->glx_prog_blur = glx_create_program(&ps->glx_frag_shader_blur, 1);
  if (!ps->glx_prog_blur) {
    printf_errf("(): Failed to create GLSL program.");
    return false;
  }

#define P_GET_UNIFM_LOC(name, target) { \
  ps->target = glGetUniformLocation(ps->glx_prog_blur, name); \
  if (ps->target < 0) { \
    printf_errf("(): Failed to get location of uniform '" name "'."); \
    return false; \
  } \
}

  P_GET_UNIFM_LOC("factor_center", glx_prog_blur_unifm_factor_center);
  P_GET_UNIFM_LOC("offset_x", glx_prog_blur_unifm_offset_x);
  P_GET_UNIFM_LOC("offset_y", glx_prog_blur_unifm_offset_y);

#undef P_GET_UNIFM_LOC

  return true;
#else
  printf_errf("(): GLSL support not compiled in. Cannot do blur with GLX backend.");
  return false;
#endif
}

/**
 * @brief Update the FBConfig of given depth.
 */
static inline void
glx_update_fbconfig_bydepth(session_t *ps, int depth, glx_fbconfig_t *pfbcfg) {
  // Make sure the depth is sane
  if (depth < 0 || depth > OPENGL_MAX_DEPTH)
    return;

  // Compare new FBConfig with current one
  if (glx_cmp_fbconfig(ps, ps->glx_fbconfigs[depth], pfbcfg) < 0) {
#ifdef DEBUG_GLX
    printf_dbgf("(%d): %#x overrides %#x, target %#x.\n", depth, (unsigned) pfbcfg->cfg, (ps->glx_fbconfigs[depth] ? (unsigned) ps->glx_fbconfigs[depth]->cfg: 0), pfbcfg->texture_tgts);
#endif
    if (!ps->glx_fbconfigs[depth]) {
      ps->glx_fbconfigs[depth] = malloc(sizeof(glx_fbconfig_t));
      allocchk(ps->glx_fbconfigs[depth]);
    }
    (*ps->glx_fbconfigs[depth]) = *pfbcfg;
  }
}

/**
 * Get GLX FBConfigs for all depths.
 */
static bool
glx_update_fbconfig(session_t *ps) {
  // Acquire all FBConfigs and loop through them
  int nele = 0;
  GLXFBConfig* pfbcfgs = glXGetFBConfigs(ps->dpy, ps->scr, &nele);

  for (GLXFBConfig *pcur = pfbcfgs; pcur < pfbcfgs + nele; pcur++) {
    glx_fbconfig_t fbinfo = {
      .cfg = *pcur,
      .texture_fmt = 0,
      .texture_tgts = 0,
      .y_inverted = false,
    };
    int depth = 0, depth_alpha = 0, val = 0;

    if (Success != glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BUFFER_SIZE, &depth)
        || Success != glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_ALPHA_SIZE, &depth_alpha)) {
      printf_errf("(): Failed to retrieve buffer size and alpha size of FBConfig %d.", (int) (pcur - pfbcfgs));
      continue;
    }
    if (Success != glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BIND_TO_TEXTURE_TARGETS_EXT, &fbinfo.texture_tgts)) {
      printf_errf("(): Failed to retrieve BIND_TO_TEXTURE_TARGETS_EXT of FBConfig %d.", (int) (pcur - pfbcfgs));
      continue;
    }

    bool rgb = false;
    bool rgba = false;

    if (depth >= 32 && depth_alpha && Success == glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BIND_TO_TEXTURE_RGBA_EXT, &val) && val)
      rgba = true;

    if (Success == glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BIND_TO_TEXTURE_RGB_EXT, &val) && val)
      rgb = true;

    if (Success == glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_Y_INVERTED_EXT, &val))
      fbinfo.y_inverted = val;

    if ((depth - depth_alpha) < 32 && rgb) {
      fbinfo.texture_fmt = GLX_TEXTURE_FORMAT_RGB_EXT;
      glx_update_fbconfig_bydepth(ps, depth - depth_alpha, &fbinfo);
    }

    if (rgba) {
      fbinfo.texture_fmt = GLX_TEXTURE_FORMAT_RGBA_EXT;
      glx_update_fbconfig_bydepth(ps, depth, &fbinfo);
    }
  }

  cxfree(pfbcfgs);

  // Sanity checks
  if (!ps->glx_fbconfigs[ps->depth]) {
    printf_errf("(): No FBConfig found for default depth %d.", ps->depth);
    return false;
  }

  if (!ps->glx_fbconfigs[32]) {
    printf_errf("(): No FBConfig found for depth 32. Expect crazy things.");
  }

  return true;
}

static inline int
glx_cmp_fbconfig_cmpattr(session_t *ps,
    const glx_fbconfig_t *pfbc_a, const glx_fbconfig_t *pfbc_b,
    int attr) {
  int attr_a = 0, attr_b = 0;

  // TODO: Error checking
  glXGetFBConfigAttrib(ps->dpy, pfbc_a->cfg, attr, &attr_a);
  glXGetFBConfigAttrib(ps->dpy, pfbc_b->cfg, attr, &attr_b);

  return attr_a - attr_b;
}

/**
 * Compare two GLX FBConfig's to find the preferred one.
 */
static int
glx_cmp_fbconfig(session_t *ps,
    const glx_fbconfig_t *pfbc_a, const glx_fbconfig_t *pfbc_b) {
  int result = 0;

  if (!pfbc_a)
    return -1;
  if (!pfbc_b)
    return 1;

#define P_CMPATTR_LT(attr) { if ((result = glx_cmp_fbconfig_cmpattr(ps, pfbc_a, pfbc_b, (attr)))) return -result; }
#define P_CMPATTR_GT(attr) { if ((result = glx_cmp_fbconfig_cmpattr(ps, pfbc_a, pfbc_b, (attr)))) return result; }

  P_CMPATTR_LT(GLX_BIND_TO_TEXTURE_RGBA_EXT);
  P_CMPATTR_LT(GLX_DOUBLEBUFFER);
  P_CMPATTR_LT(GLX_STENCIL_SIZE);
  P_CMPATTR_LT(GLX_DEPTH_SIZE);
  P_CMPATTR_GT(GLX_BIND_TO_MIPMAP_TEXTURE_EXT);

  return 0;
}

/**
 * Bind a X pixmap to an OpenGL texture.
 */
bool
glx_bind_pixmap(session_t *ps, glx_texture_t **pptex, Pixmap pixmap,
    unsigned width, unsigned height, unsigned depth) {
  if (!pixmap) {
    printf_errf("(%#010lx): Binding to an empty pixmap. This can't work.",
        pixmap);
    return false;
  }

  glx_texture_t *ptex = *pptex;
  bool need_release = true;

  // Allocate structure
  if (!ptex) {
    static const glx_texture_t GLX_TEX_DEF = {
      .texture = 0,
      .glpixmap = 0,
      .pixmap = 0,
      .target = 0,
      .width = 0,
      .height = 0,
      .depth = 0,
      .y_inverted = false,
    };

    ptex = malloc(sizeof(glx_texture_t));
    allocchk(ptex);
    memcpy(ptex, &GLX_TEX_DEF, sizeof(glx_texture_t));
    *pptex = ptex;
  }

  // Release pixmap if parameters are inconsistent
  if (ptex->texture && ptex->pixmap != pixmap) {
    glx_release_pixmap(ps, ptex);
  }

  // Create GLX pixmap
  if (!ptex->glpixmap) {
    need_release = false;

    // Retrieve pixmap parameters, if they aren't provided
    if (!(width && height && depth)) {
      Window rroot = None;
      int rx = 0, ry = 0;
      unsigned rbdwid = 0;
      if (!XGetGeometry(ps->dpy, pixmap, &rroot, &rx, &ry,
            &width, &height, &rbdwid, &depth)) {
        printf_errf("(%#010lx): Failed to query Pixmap info.", pixmap);
        return false;
      }
      if (depth > OPENGL_MAX_DEPTH) {
        printf_errf("(%d): Requested depth higher than %d.", depth,
            OPENGL_MAX_DEPTH);
        return false;
      }
    }

    const glx_fbconfig_t *pcfg = ps->glx_fbconfigs[depth];
    if (!pcfg) {
      printf_errf("(%d): Couldn't find FBConfig with requested depth.", depth);
      return false;
    }

    // Determine texture target, copied from compiz
    // The assumption we made here is the target never changes based on any
    // pixmap-specific parameters, and this may change in the future
    GLenum tex_tgt = 0;
    if (GLX_TEXTURE_2D_BIT_EXT & pcfg->texture_tgts
        && ps->glx_has_texture_non_power_of_two)
      tex_tgt = GLX_TEXTURE_2D_EXT;
    else if (GLX_TEXTURE_RECTANGLE_BIT_EXT & pcfg->texture_tgts)
      tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
    else if (!(GLX_TEXTURE_2D_BIT_EXT & pcfg->texture_tgts))
      tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
    else
      tex_tgt = GLX_TEXTURE_2D_EXT;

#ifdef DEBUG_GLX
    printf_dbgf("(): depth %d, tgt %#x, rgba %d\n", depth, tex_tgt,
        (GLX_TEXTURE_FORMAT_RGBA_EXT == pcfg->texture_fmt));
#endif

    GLint attrs[] = {
        GLX_TEXTURE_FORMAT_EXT,
        pcfg->texture_fmt,
        GLX_TEXTURE_TARGET_EXT,
        tex_tgt,
        0,
    };

    ptex->glpixmap = glXCreatePixmap(ps->dpy, pcfg->cfg, pixmap, attrs);
    ptex->pixmap = pixmap;
    ptex->target = (GLX_TEXTURE_2D_EXT == tex_tgt ? GL_TEXTURE_2D:
        GL_TEXTURE_RECTANGLE);
    ptex->width = width;
    ptex->height = height;
    ptex->depth = depth;
    ptex->y_inverted = pcfg->y_inverted;
  }
  if (!ptex->glpixmap) {
    printf_errf("(): Failed to allocate GLX pixmap.");
    return false;
  }

  glEnable(ptex->target);

  // Create texture
  if (!ptex->texture) {
    need_release = false;

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(ptex->target, texture);

    glTexParameteri(ptex->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(ptex->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(ptex->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(ptex->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(ptex->target, 0);

    ptex->texture = texture;
  }
  if (!ptex->texture) {
    printf_errf("(): Failed to allocate texture.");
    return false;
  }

  glBindTexture(ptex->target, ptex->texture);

  // The specification requires rebinding whenever the content changes...
  // We can't follow this, too slow.
  if (need_release)
    ps->glXReleaseTexImageProc(ps->dpy, ptex->glpixmap, GLX_FRONT_LEFT_EXT);

  ps->glXBindTexImageProc(ps->dpy, ptex->glpixmap, GLX_FRONT_LEFT_EXT, NULL);

  // Cleanup
  glBindTexture(ptex->target, 0);
  glDisable(ptex->target);

  return true;
}

/**
 * @brief Release binding of a texture.
 */
void
glx_release_pixmap(session_t *ps, glx_texture_t *ptex) {
  // Release binding
  if (ptex->glpixmap && ptex->texture) {
    glBindTexture(ptex->target, ptex->texture);
    ps->glXReleaseTexImageProc(ps->dpy, ptex->glpixmap, GLX_FRONT_LEFT_EXT);
    glBindTexture(ptex->target, 0);
  }

  // Free GLX Pixmap
  if (ptex->glpixmap) {
    glXDestroyPixmap(ps->dpy, ptex->glpixmap);
    ptex->glpixmap = 0;
  }
}

/**
 * Preprocess function before start painting.
 */
void
glx_paint_pre(session_t *ps, XserverRegion *preg) {
  ps->glx_z = 0.0;
  // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Exchange swap is interested in the raw damaged region only
  XserverRegion all_damage_last = ps->all_damage_last;
  ps->all_damage_last = None;
  if (SWAPM_EXCHANGE == ps->o.glx_swap_method && *preg)
    ps->all_damage_last = copy_region(ps, *preg);

  // OpenGL doesn't support partial repaint without GLX_MESA_copy_sub_buffer,
  // we could redraw the whole screen or copy unmodified pixels from
  // front buffer with --glx-copy-from-front.
  if (ps->o.glx_use_copysubbuffermesa || SWAPM_COPY == ps->o.glx_swap_method
      || !*preg) {
  }
  else if (SWAPM_EXCHANGE == ps->o.glx_swap_method && all_damage_last) {
    XFixesUnionRegion(ps->dpy, *preg, *preg, all_damage_last);
  }
  else if (!ps->o.glx_copy_from_front) {
    free_region(ps, preg);
  }
  else {
    {
      XserverRegion reg_copy = XFixesCreateRegion(ps->dpy, NULL, 0);
      XFixesSubtractRegion(ps->dpy, reg_copy, ps->screen_reg, *preg);
      glx_set_clip(ps, reg_copy, NULL);
      free_region(ps, &reg_copy);
    }

    {
      GLfloat raster_pos[4];
      glGetFloatv(GL_CURRENT_RASTER_POSITION, raster_pos);
      glReadBuffer(GL_FRONT);
      glRasterPos2f(0.0, 0.0);
      glCopyPixels(0, 0, ps->root_width, ps->root_height, GL_COLOR);
      glReadBuffer(GL_BACK);
      glRasterPos4fv(raster_pos);
    }
  }

  free_region(ps, &all_damage_last);

  glx_set_clip(ps, *preg, NULL);
}

/**
 * Set clipping region on the target window.
 */
void
glx_set_clip(session_t *ps, XserverRegion reg, const reg_data_t *pcache_reg) {
  // Quit if we aren't using stencils
  if (ps->o.glx_no_stencil)
    return;

  static XRectangle rect_blank = { .x = 0, .y = 0, .width = 0, .height = 0 };

  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);

  if (!reg)
    return;

  int nrects = 0;
  XRectangle *rects_free = NULL;
  const XRectangle *rects = NULL;
  if (pcache_reg) {
    rects = pcache_reg->rects;
    nrects = pcache_reg->nrects;
  }
  if (!rects) {
    nrects = 0;
    rects = rects_free = XFixesFetchRegion(ps->dpy, reg, &nrects);
  }
  // Use one empty rectangle if the region is empty
  if (!nrects) {
    cxfree(rects_free);
    rects_free = NULL;
    nrects = 1;
    rects = &rect_blank;
  }

  assert(nrects);
  if (1 == nrects) {
    glEnable(GL_SCISSOR_TEST);
    glScissor(rects[0].x, ps->root_height - rects[0].y - rects[0].height,
        rects[0].width, rects[0].height);
  }
  else {
    glEnable(GL_STENCIL_TEST);
    glClear(GL_STENCIL_BUFFER_BIT);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);

    glBegin(GL_QUADS);

    for (int i = 0; i < nrects; ++i) {
      GLint rx = rects[i].x;
      GLint ry = ps->root_height - rects[i].y;
      GLint rxe = rx + rects[i].width;
      GLint rye = ry - rects[i].height;
      GLint z = 0;

#ifdef DEBUG_GLX
      printf_dbgf("(): Rect %d: %d, %d, %d, %d\n", i, rx, ry, rxe, rye);
#endif

      glVertex3i(rx, ry, z);
      glVertex3i(rxe, ry, z);
      glVertex3i(rxe, rye, z);
      glVertex3i(rx, rye, z);
    }

    glEnd();

    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
  }

  cxfree(rects_free);
}

#define P_PAINTREG_START() \
  XserverRegion reg_new = None; \
  XRectangle rec_all = { .x = dx, .y = dy, .width = width, .height = height }; \
  XRectangle *rects = &rec_all; \
  int nrects = 1; \
 \
  if (ps->o.glx_no_stencil && reg_tgt) { \
    if (pcache_reg) { \
      rects = pcache_reg->rects; \
      nrects = pcache_reg->nrects; \
    } \
    else { \
      reg_new = XFixesCreateRegion(ps->dpy, &rec_all, 1); \
      XFixesIntersectRegion(ps->dpy, reg_new, reg_new, reg_tgt); \
 \
      nrects = 0; \
      rects = XFixesFetchRegion(ps->dpy, reg_new, &nrects); \
    } \
  } \
  glBegin(GL_QUADS); \
 \
  for (int i = 0; i < nrects; ++i) { \
    XRectangle crect; \
    rect_crop(&crect, &rects[i], &rec_all); \
 \
    if (!crect.width || !crect.height) \
      continue; \

#define P_PAINTREG_END() \
  } \
  glEnd(); \
 \
  if (rects && rects != &rec_all && !(pcache_reg && pcache_reg->rects == rects)) \
    cxfree(rects); \
  free_region(ps, &reg_new); \

bool
glx_blur_dst(session_t *ps, int dx, int dy, int width, int height, float z,
    GLfloat factor_center, XserverRegion reg_tgt, const reg_data_t *pcache_reg) {
  // Read destination pixels into a texture
  GLuint tex_scr = 0;
  glGenTextures(1, &tex_scr);
  if (!tex_scr) {
    printf_errf("(): Failed to allocate texture.");
    return false;
  }

  GLenum tex_tgt = GL_TEXTURE_RECTANGLE;
  if (ps->glx_has_texture_non_power_of_two)
    tex_tgt = GL_TEXTURE_2D;

  glEnable(tex_tgt);
  glBindTexture(tex_tgt, tex_scr);
  glTexParameteri(tex_tgt, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(tex_tgt, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(tex_tgt, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(tex_tgt, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(tex_tgt, 0, GL_RGB, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
  glCopyTexSubImage2D(tex_tgt, 0, 0, 0, dx, ps->root_height - dy - height, width, height);

#ifdef DEBUG_GLX
  printf_dbgf("(): %d, %d, %d, %d\n", dx, ps->root_height - dy - height, width, height);
#endif

  // Paint it back
  // Color negation for testing...
  // glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  // glTexEnvf(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
  // glTexEnvf(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);

  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
#ifdef CONFIG_VSYNC_OPENGL_GLSL
  glUseProgram(ps->glx_prog_blur);
  glUniform1f(ps->glx_prog_blur_unifm_offset_x, 1.0f / width);
  glUniform1f(ps->glx_prog_blur_unifm_offset_y, 1.0f / height);
  glUniform1f(ps->glx_prog_blur_unifm_factor_center, factor_center);
#endif

  {
    P_PAINTREG_START();
    {
      const GLfloat rx = (double) (crect.x - dx) / width;
      const GLfloat ry = 1.0 - (double) (crect.y - dy) / height;
      const GLfloat rxe = rx + (double) crect.width / width;
      const GLfloat rye = ry - (double) crect.height / height;
      const GLfloat rdx = crect.x;
      const GLfloat rdy = ps->root_height - crect.y;
      const GLfloat rdxe = rdx + crect.width;
      const GLfloat rdye = rdy - crect.height;

#ifdef DEBUG_GLX
      printf_dbgf("(): %f, %f, %f, %f -> %d, %d, %d, %d\n", rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);
#endif

      glTexCoord2f(rx, ry);
      glVertex3f(rdx, rdy, z);

      glTexCoord2f(rxe, ry);
      glVertex3f(rdxe, rdy, z);

      glTexCoord2f(rxe, rye);
      glVertex3f(rdxe, rdye, z);

      glTexCoord2f(rx, rye);
      glVertex3f(rdx, rdye, z);
    }
    P_PAINTREG_END();
  }

#ifdef CONFIG_VSYNC_OPENGL_GLSL
  glUseProgram(0);
#endif
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glBindTexture(tex_tgt, 0);
  glDeleteTextures(1, &tex_scr);
  glDisable(tex_tgt);

  return true;
}

bool
glx_dim_dst(session_t *ps, int dx, int dy, int width, int height, float z,
    GLfloat factor, XserverRegion reg_tgt, const reg_data_t *pcache_reg) {
  // It's possible to dim in glx_render(), but it would be over-complicated
  // considering all those mess in color negation and modulation
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(0.0f, 0.0f, 0.0f, factor);

  {
    P_PAINTREG_START();
    {
      GLint rdx = crect.x;
      GLint rdy = ps->root_height - crect.y;
      GLint rdxe = rdx + crect.width;
      GLint rdye = rdy - crect.height;

      glVertex3i(rdx, rdy, z);
      glVertex3i(rdxe, rdy, z);
      glVertex3i(rdxe, rdye, z);
      glVertex3i(rdx, rdye, z);
    }
    P_PAINTREG_END();
  }

  glEnd();

  glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
  glDisable(GL_BLEND);

  return true;
}

/**
 * @brief Render a region with texture data.
 */
bool
glx_render(session_t *ps, const glx_texture_t *ptex,
    int x, int y, int dx, int dy, int width, int height, int z,
    double opacity, bool neg,
    XserverRegion reg_tgt, const reg_data_t *pcache_reg) {
  if (!ptex || !ptex->texture) {
    printf_errf("(): Missing texture.");
    return false;
  }

  const bool argb = (GLX_TEXTURE_FORMAT_RGBA_EXT ==
      ps->glx_fbconfigs[ptex->depth]->texture_fmt);
  bool dual_texture = false;

  // It's required by legacy versions of OpenGL to enable texture target
  // before specifying environment. Thanks to madsy for telling me.
  glEnable(ptex->target);

  // Enable blending if needed
  if (opacity < 1.0 || argb) {

    glEnable(GL_BLEND);

    // Needed for handling opacity of ARGB texture
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    // This is all weird, but X Render is using premultiplied ARGB format, and
    // we need to use those things to correct it. Thanks to derhass for help.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(opacity, opacity, opacity, opacity);
  }

  // Color negation
  if (neg) {
    // Simple color negation
    if (!glIsEnabled(GL_BLEND)) {
      glEnable(GL_COLOR_LOGIC_OP);
      glLogicOp(GL_COPY_INVERTED);
    }
    // ARGB texture color negation
    else if (argb) {
      dual_texture = true;

      // Use two texture stages because the calculation is too complicated,
      // thanks to madsy for providing code
      // Texture stage 0
      glActiveTexture(GL_TEXTURE0);

      // Negation for premultiplied color: color = A - C
      glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
      glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_SUBTRACT);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_ALPHA);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

      // Pass texture alpha through
      glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
   
      // Texture stage 1
      glActiveTexture(GL_TEXTURE1);
      glEnable(ptex->target);
      glBindTexture(ptex->target, ptex->texture);

      glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);

      // Modulation with constant factor
      glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_ALPHA);

      // Modulation with constant factor
      glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);

      glActiveTexture(GL_TEXTURE0);
    }
    // RGB blend color negation
    else {
      glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);

      // Modulation with constant factor
      glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

      // Modulation with constant factor
      glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
    }
  }

#ifdef DEBUG_GLX
  printf_dbgf("(): Draw: %d, %d, %d, %d -> %d, %d (%d, %d) z %d\n", x, y, width, height, dx, dy, ptex->width, ptex->height, z);
#endif

  // Bind texture
  glBindTexture(ptex->target, ptex->texture);
  if (dual_texture) {
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(ptex->target, ptex->texture);
    glActiveTexture(GL_TEXTURE0);
  }

  // Painting
  {
    P_PAINTREG_START();
    {
      GLfloat rx = (double) (crect.x - dx + x) / ptex->width;
      GLfloat ry = (double) (crect.y - dy + y) / ptex->height;
      GLfloat rxe = rx + (double) crect.width / ptex->width;
      GLfloat rye = ry + (double) crect.height / ptex->height;
      GLint rdx = crect.x;
      GLint rdy = ps->root_height - crect.y;
      GLint rdxe = rdx + crect.width;
      GLint rdye = rdy - crect.height;

      // Invert Y if needed, this may not work as expected, though. I don't
      // have such a FBConfig to test with.
      if (!ptex->y_inverted) {
        ry = 1.0 - ry;
        rye = 1.0 - rye;
      }

#ifdef DEBUG_GLX
      printf_dbgf("(): Rect %d: %f, %f, %f, %f -> %d, %d, %d, %d\n", i, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);
#endif

#define P_TEXCOORD(cx, cy) { \
  if (dual_texture) { \
    glMultiTexCoord2f(GL_TEXTURE0, cx, cy); \
    glMultiTexCoord2f(GL_TEXTURE1, cx, cy); \
  } \
  else glTexCoord2f(cx, cy); \
}
      P_TEXCOORD(rx, ry);
      glVertex3i(rdx, rdy, z);

      P_TEXCOORD(rxe, ry);
      glVertex3i(rdxe, rdy, z);

      P_TEXCOORD(rxe, rye);
      glVertex3i(rdxe, rdye, z);

      P_TEXCOORD(rx, rye);
      glVertex3i(rdx, rdye, z);
    }
    P_PAINTREG_END();
  }

  // Cleanup
  glBindTexture(ptex->target, 0);
  glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glDisable(GL_BLEND);
  glDisable(GL_COLOR_LOGIC_OP);
  glDisable(ptex->target);

  if (dual_texture) {
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(ptex->target, 0);
    glDisable(ptex->target);
    glActiveTexture(GL_TEXTURE0);
  }

  return true;
}

/**
 * Swap buffer with glXCopySubBufferMESA().
 */
void
glx_swap_copysubbuffermesa(session_t *ps, XserverRegion reg) {
  int nrects = 0;
  XRectangle *rects = XFixesFetchRegion(ps->dpy, reg, &nrects);

  if (1 == nrects && rect_is_fullscreen(ps, rects[0].x, rects[0].y,
        rects[0].width, rects[0].height)) {
    glXSwapBuffers(ps->dpy, get_tgt_window(ps));
  }
  else {
    glx_set_clip(ps, None, NULL);
    for (int i = 0; i < nrects; ++i) {
      const int x = rects[i].x;
      const int y = ps->root_height - rects[i].y - rects[i].height;
      const int wid = rects[i].width;
      const int hei = rects[i].height;

#ifdef DEBUG_GLX
      printf_dbgf("(): %d, %d, %d, %d\n", x, y, wid, hei);
#endif
      ps->glXCopySubBufferProc(ps->dpy, get_tgt_window(ps), x, y, wid, hei);
    }
  }

  cxfree(rects);
}

#ifdef CONFIG_VSYNC_OPENGL_GLSL
GLuint
glx_create_shader(GLenum shader_type, const char *shader_str) {
  bool success = false;
  GLuint shader = glCreateShader(shader_type);
  if (!shader) {
    printf_errf("(): Failed to create shader with type %d.", shader_type);
    goto glx_create_shader_end;
  }
  glShaderSource(shader, 1, &shader_str, NULL);
  glCompileShader(shader);

  // Get shader status
  {
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (GL_FALSE == status) {
      GLint log_len = 0;
      glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
      if (log_len) {
        char log[log_len + 1];
        glGetShaderInfoLog(shader, log_len, NULL, log);
        printf_errf("(): Failed to compile shader with type %d: %s",
            shader_type, log);
      }
      goto glx_create_shader_end;
    }
  }

  success = true;

glx_create_shader_end:
  if (shader && !success) {
    glDeleteShader(shader);
    shader = 0;
  }

  return shader;
}

GLuint
glx_create_program(const GLuint * const shaders, int nshaders) {
  bool success = false;
  GLuint program = glCreateProgram();
  if (!program) {
    printf_errf("(): Failed to create program.");
    goto glx_create_program_end;
  }

  for (int i = 0; i < nshaders; ++i)
    glAttachShader(program, shaders[i]);
  glLinkProgram(program);

  // Get program status
  {
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (GL_FALSE == status) {
      GLint log_len = 0;
      glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
      if (log_len) {
        char log[log_len + 1];
        glGetProgramInfoLog(program, log_len, NULL, log);
        printf_errf("(): Failed to link program: %s", log);
      }
      goto glx_create_program_end;
    }
  }
  success = true;

glx_create_program_end:
  if (program) {
    for (int i = 0; i < nshaders; ++i)
      glDetachShader(program, shaders[i]);
  }
  if (program && !success) {
    glDeleteProgram(program);
    program = 0;
  }

  return program;
}
#endif

