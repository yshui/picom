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
  if (pvis)
    XFree(pvis);

  if (!success)
    glx_destroy(ps);

  return success;
}

/**
 * Destroy GLX related resources.
 */
void
glx_destroy(session_t *ps) {
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
  printf_errf("(): Blur on GLX backend isn't implemented yet, sorry.");
  return false;

// #ifdef CONFIG_VSYNC_OPENGL_GLSL
//           static const char *FRAG_SHADER_BLUR = "";
//           GLuint frag_shader = glx_create_shader(GL_FRAGMENT_SHADER, FRAG_SHADER_BLUR);
//           if (!frag_shader) {
//             printf_errf("(): Failed to create fragment shader for blurring.");
//             return false;
//           }
// #else
//           printf_errf("(): GLSL support not compiled in. Cannot do blur with GLX backend.");
//           return false;
// #endif
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

  if (pfbcfgs)
    XFree(pfbcfgs);

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
  // OpenGL doesn't support partial repaint without GLX_MESA_copy_sub_buffer,
  // we currently redraw the whole screen or copy unmodified pixels from
  // front buffer with --glx-copy-from-front.
  if (!ps->o.glx_copy_from_front || !*preg) {
    free_region(ps, preg);
  }
  else {
    {
      XserverRegion reg_copy = XFixesCreateRegion(ps->dpy, NULL, 0);
      XFixesSubtractRegion(ps->dpy, reg_copy, ps->screen_reg, *preg);
      glx_set_clip(ps, reg_copy);
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

  glx_set_clip(ps, *preg);
}

/**
 * Set clipping region on the target window.
 */
void
glx_set_clip(session_t *ps, XserverRegion reg) {
  // Quit if we aren't using stencils
  if (ps->o.glx_no_stencil)
    return;

  static XRectangle rect_blank = {
    .x = 0, .y = 0, .width = 0, .height = 0
  };

  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);

  if (!reg)
    return;

  int nrects = 0;
  XRectangle *rects = XFixesFetchRegion(ps->dpy, reg, &nrects);
  if (!nrects) {
    if (rects) XFree(rects);
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

  if (rects && &rect_blank != rects)
    XFree(rects);
}

bool
glx_blur_dst(session_t *ps, int dx, int dy, int width, int height, float z) {
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
  // TODO: Blur function. We are using color negation for testing now.
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  glTexEnvf(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
  glTexEnvf(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);

  glBegin(GL_QUADS);

  {
    const GLfloat rx = 0.0;
    const GLfloat ry = 1.0;
    const GLfloat rxe = 1.0;
    const GLfloat rye = 0.0;
    const GLint rdx = dx;
    const GLint rdy = ps->root_height - dy;
    const GLint rdxe = rdx + width;
    const GLint rdye = rdy - height;

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

  glEnd();

  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glBindTexture(tex_tgt, 0);
  glDeleteTextures(1, &tex_scr);
  glDisable(tex_tgt);

  return true;
}

/**
 * @brief Render a region with texture data.
 */
bool
glx_render(session_t *ps, const glx_texture_t *ptex,
    int x, int y, int dx, int dy, int width, int height, int z,
    double opacity, bool neg, XserverRegion reg_tgt) {
  bool blur_background = false;

  if (!ptex || !ptex->texture) {
    printf_errf("(): Missing texture.");
    return false;
  }

  // Enable blending if needed
  if (opacity < 1.0 || GLX_TEXTURE_FORMAT_RGBA_EXT ==
      ps->glx_fbconfigs[ptex->depth]->texture_fmt) {
    if (!ps->o.glx_no_stencil && blur_background)
      glx_blur_dst(ps, dx, dy, width, height, z - 0.5);

    glEnable(GL_BLEND);

    // Needed for handling opacity of ARGB texture
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    // This is all weird, but X Render is using premulitplied ARGB format, and
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
    // Blending color negation
    else {
      glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
      glTexEnvf(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
      glTexEnvf(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);
    }
  }

  {
    XserverRegion reg_new = None;

    XRectangle rec_all = {
      .x = dx,
      .y = dy,
      .width = width,
      .height = height
    };

    XRectangle *rects = &rec_all;
    int nrects = 1;

#ifdef DEBUG_GLX
    printf_dbgf("(): Draw: %d, %d, %d, %d -> %d, %d (%d, %d) z %d\n", x, y, width, height, dx, dy, ptex->width, ptex->height, z);
#endif

    // On no-stencil mode, calculate painting region here instead of relying
    // on stencil buffer
    if (ps->o.glx_no_stencil && reg_tgt) {
      reg_new = XFixesCreateRegion(ps->dpy, &rec_all, 1);
      XFixesIntersectRegion(ps->dpy, reg_new, reg_new, reg_tgt);

      nrects = 0;
      rects = XFixesFetchRegion(ps->dpy, reg_new, &nrects);
    }

    glEnable(ptex->target);
    glBindTexture(ptex->target, ptex->texture);

    glBegin(GL_QUADS);

    for (int i = 0; i < nrects; ++i) {
      GLfloat rx = (double) (rects[i].x - dx + x) / ptex->width;
      GLfloat ry = (double) (rects[i].y - dy + y) / ptex->height;
      GLfloat rxe = rx + (double) rects[i].width / ptex->width;
      GLfloat rye = ry + (double) rects[i].height / ptex->height;
      GLint rdx = rects[i].x;
      GLint rdy = ps->root_height - rects[i].y;
      GLint rdxe = rdx + rects[i].width;
      GLint rdye = rdy - rects[i].height;

      // Invert Y if needed, this may not work as expected, though. I don't
      // have such a FBConfig to test with.
      if (!ptex->y_inverted) {
        ry = 1.0 - ry;
        rye = 1.0 - rye;
      }

#ifdef DEBUG_GLX
      printf_dbgf("(): Rect %d: %f, %f, %f, %f -> %d, %d, %d, %d\n", i, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);
#endif

      glTexCoord2f(rx, ry);
      glVertex3i(rdx, rdy, z);

      glTexCoord2f(rxe, ry);
      glVertex3i(rdxe, rdy, z);

      glTexCoord2f(rxe, rye);
      glVertex3i(rdxe, rdye, z);

      glTexCoord2f(rx, rye);
      glVertex3i(rdx, rdye, z);
    }

    glEnd();

    if (rects && rects != &rec_all)
      XFree(rects);
    free_region(ps, &reg_new);
  }

  // Cleanup
  glBindTexture(ptex->target, 0);
  glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glDisable(GL_BLEND);
  glDisable(GL_COLOR_LOGIC_OP);
  glDisable(ptex->target);

  return true;
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
glx_create_program(GLenum shader_type, const GLuint * const shaders,
    int nshaders) {
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
  if (program && !success) {
    glDeleteProgram(program);
    program = 0;
  }

  return program;
}
#endif

