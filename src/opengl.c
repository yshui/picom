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

  if (!ps->glx_context) {
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
#ifdef DEBUG_GLX_MARK
    ps->glStringMarkerGREMEDY = (f_StringMarkerGREMEDY)
      glXGetProcAddress((const GLubyte *) "glStringMarkerGREMEDY");
    ps->glFrameTerminatorGREMEDY = (f_FrameTerminatorGREMEDY)
      glXGetProcAddress((const GLubyte *) "glFrameTerminatorGREMEDY");
#endif

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

  // Render preparations
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
  for (int i = 0; i < MAX_BLUR_PASS; ++i) {
    glx_blur_pass_t *ppass = &ps->glx_blur_passes[i];
    if (ppass->frag_shader)
      glDeleteShader(ppass->frag_shader);
    if (ppass->prog)
      glDeleteProgram(ppass->prog);
  }
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
  assert(ps->o.blur_kerns[0]);

  // Allocate PBO if more than one blur kernel is present
  if (ps->o.blur_kerns[1]) {
#ifdef CONFIG_VSYNC_OPENGL_FBO
    // Try to generate a framebuffer
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    if (!fbo) {
      printf_errf("(): Failed to generate Framebuffer. Cannot do "
          "multi-pass blur with GLX backend.");
      return false;
    }
    glDeleteFramebuffers(1, &fbo);
#else
    printf_errf("(): FBO support not compiled in. Cannot do multi-pass blur "
        "with GLX backend.");
    return false;
#endif
  }

#ifdef CONFIG_VSYNC_OPENGL_GLSL
  {
    char *lc_numeric_old = mstrcpy(setlocale(LC_NUMERIC, NULL));
    // Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
    // Thanks to hiciu for reporting.
    setlocale(LC_NUMERIC, "C");

    static const char *FRAG_SHADER_BLUR_PREFIX =
      "#version 110\n"
      "%s"
      "uniform float offset_x;\n"
      "uniform float offset_y;\n"
      "uniform float factor_center;\n"
      "uniform %s tex_scr;\n"
      "\n"
      "void main() {\n"
      "  vec4 sum = vec4(0.0, 0.0, 0.0, 0.0);\n";
    static const char *FRAG_SHADER_BLUR_ADD =
      "  sum += float(%.7g) * %s(tex_scr, vec2(gl_TexCoord[0].x + offset_x * float(%d), gl_TexCoord[0].y + offset_y * float(%d)));\n";
    static const char *FRAG_SHADER_BLUR_ADD_GPUSHADER4 =
      "  sum += float(%.7g) * %sOffset(tex_scr, vec2(gl_TexCoord[0].x, gl_TexCoord[0].y), ivec2(%d, %d));\n";
    static const char *FRAG_SHADER_BLUR_SUFFIX =
      "  sum += %s(tex_scr, vec2(gl_TexCoord[0].x, gl_TexCoord[0].y)) * factor_center;\n"
      "  gl_FragColor = sum / (factor_center + float(%.7g));\n"
      "}\n";

    const bool use_texture_rect = !ps->glx_has_texture_non_power_of_two;
    const char *sampler_type = (use_texture_rect ?
        "sampler2DRect": "sampler2D");
    const char *texture_func = (use_texture_rect ?
        "texture2DRect": "texture2D");
    const char *shader_add = FRAG_SHADER_BLUR_ADD;
    char *extension = mstrcpy("");
    if (use_texture_rect)
      mstrextend(&extension, "#extension GL_ARB_texture_rectangle : require\n");
    if (ps->o.glx_use_gpushader4) {
      mstrextend(&extension, "#extension GL_EXT_gpu_shader4 : require\n");
      shader_add = FRAG_SHADER_BLUR_ADD_GPUSHADER4;
    }

    for (int i = 0; i < MAX_BLUR_PASS && ps->o.blur_kerns[i]; ++i) {
      XFixed *kern = ps->o.blur_kerns[i];
      if (!kern)
        break;

      glx_blur_pass_t *ppass = &ps->glx_blur_passes[i];

      // Build shader
      {
        int wid = XFixedToDouble(kern[0]), hei = XFixedToDouble(kern[1]);
        int nele = wid * hei - 1;
        int len = strlen(FRAG_SHADER_BLUR_PREFIX) + strlen(sampler_type) + strlen(extension) + (strlen(shader_add) + strlen(texture_func) + 42) * nele + strlen(FRAG_SHADER_BLUR_SUFFIX) + strlen(texture_func) + 12 + 1;
        char *shader_str = calloc(len, sizeof(char));
        if (!shader_str) {
          printf_errf("(): Failed to allocate %d bytes for shader string.", len);
          return false;
        }
        {
          char *pc = shader_str;
          sprintf(pc, FRAG_SHADER_BLUR_PREFIX, extension, sampler_type);
          pc += strlen(pc);
          assert(strlen(shader_str) < len);

          double sum = 0.0;
          for (int j = 0; j < hei; ++j) {
            for (int k = 0; k < wid; ++k) {
              if (hei / 2 == j && wid / 2 == k)
                continue;
              double val = XFixedToDouble(kern[2 + j * wid + k]);
              if (0.0 == val)
                continue;
              sum += val;
              sprintf(pc, shader_add, val, texture_func, k - wid / 2, j - hei / 2);
              pc += strlen(pc);
              assert(strlen(shader_str) < len);
            }
          }

          sprintf(pc, FRAG_SHADER_BLUR_SUFFIX, texture_func, sum);
          assert(strlen(shader_str) < len);
        }
        ppass->frag_shader = glx_create_shader(GL_FRAGMENT_SHADER, shader_str);
        free(shader_str);
      }

      if (!ppass->frag_shader) {
        printf_errf("(): Failed to create fragment shader %d.", i);
        return false;
      }

      // Build program
      ppass->prog = glx_create_program(&ppass->frag_shader, 1);
      if (!ppass->prog) {
        printf_errf("(): Failed to create GLSL program.");
        return false;
      }

      // Get uniform addresses
#define P_GET_UNIFM_LOC(name, target) { \
      ppass->target = glGetUniformLocation(ppass->prog, name); \
      if (ppass->target < 0) { \
        printf_errf("(): Failed to get location of %d-th uniform '" name "'. Might be troublesome.", i); \
      } \
    }

      P_GET_UNIFM_LOC("factor_center", unifm_factor_center);
      if (!ps->o.glx_use_gpushader4) {
        P_GET_UNIFM_LOC("offset_x", unifm_offset_x);
        P_GET_UNIFM_LOC("offset_y", unifm_offset_y);
      }
#undef P_GET_UNIFM_LOC
    }
    free(extension);

    // Restore LC_NUMERIC
    setlocale(LC_NUMERIC, lc_numeric_old);
    free(lc_numeric_old);
  }


#ifdef DEBUG_GLX_ERR
  glx_check_err(ps);
#endif

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
    int id = (int) (pcur - pfbcfgs);
    int depth = 0, depth_alpha = 0, val = 0;

    // Skip over multi-sampled visuals
    // http://people.freedesktop.org/~glisse/0001-glx-do-not-use-multisample-visual-config-for-front-o.patch
#ifdef GLX_SAMPLES
    if (Success == glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_SAMPLES, &val)
        && val > 1)
      continue;
#endif

    if (Success != glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BUFFER_SIZE, &depth)
        || Success != glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_ALPHA_SIZE, &depth_alpha)) {
      printf_errf("(): Failed to retrieve buffer size and alpha size of FBConfig %d.", id);
      continue;
    }
    if (Success != glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BIND_TO_TEXTURE_TARGETS_EXT, &fbinfo.texture_tgts)) {
      printf_errf("(): Failed to retrieve BIND_TO_TEXTURE_TARGETS_EXT of FBConfig %d.", id);
      continue;
    }

    int visualdepth = 0;
    {
      XVisualInfo *pvi = glXGetVisualFromFBConfig(ps->dpy, *pcur);
      if (!pvi) {
        // On nvidia-drivers-325.08 this happens slightly too often...
        // printf_errf("(): Failed to retrieve X Visual of FBConfig %d.", id);
        continue;
      }
      visualdepth = pvi->depth;
      cxfree(pvi);
    }

    bool rgb = false;
    bool rgba = false;

    if (depth >= 32 && depth_alpha && Success == glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BIND_TO_TEXTURE_RGBA_EXT, &val) && val)
      rgba = true;

    if (Success == glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BIND_TO_TEXTURE_RGB_EXT, &val) && val)
      rgb = true;

    if (Success == glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_Y_INVERTED_EXT, &val))
      fbinfo.y_inverted = val;

    {
      int tgtdpt = depth - depth_alpha;
      if (tgtdpt == visualdepth && tgtdpt < 32 && rgb) {
        fbinfo.texture_fmt = GLX_TEXTURE_FORMAT_RGB_EXT;
        glx_update_fbconfig_bydepth(ps, tgtdpt, &fbinfo);
      }
    }

    if (depth == visualdepth && rgba) {
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

#ifdef DEBUG_GLX
  printf_dbgf("(): %d-bit: %#3x, 32-bit: %#3x\n",
      ps->depth, (int) ps->glx_fbconfigs[ps->depth]->cfg,
      (int) ps->glx_fbconfigs[32]->cfg);
#endif

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

#ifdef DEBUG_GLX_ERR
  glx_check_err(ps);
#endif

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

#ifdef DEBUG_GLX_ERR
  glx_check_err(ps);
#endif
}

/**
 * Preprocess function before start painting.
 */
void
glx_paint_pre(session_t *ps, XserverRegion *preg) {
  ps->glx_z = 0.0;
  // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Get buffer age
  bool trace_damage = (ps->o.glx_swap_method < 0 || ps->o.glx_swap_method > 1);

  // Trace raw damage regions
  XserverRegion newdamage = None;
  if (trace_damage && *preg)
    newdamage = copy_region(ps, *preg);

  // OpenGL doesn't support partial repaint without GLX_MESA_copy_sub_buffer,
  // we could redraw the whole screen or copy unmodified pixels from
  // front buffer with --glx-copy-from-front.
  if (ps->o.glx_use_copysubbuffermesa || !*preg) {
  }
  else {
    int buffer_age = ps->o.glx_swap_method;

    // Getting buffer age
    {
      // Query GLX_EXT_buffer_age for buffer age
      if (SWAPM_BUFFER_AGE == buffer_age) {
        unsigned val = 0;
        glXQueryDrawable(ps->dpy, get_tgt_window(ps),
            GLX_BACK_BUFFER_AGE_EXT, &val);
        buffer_age = val;
      }

      // Buffer age too high
      if (buffer_age > CGLX_MAX_BUFFER_AGE + 1)
        buffer_age = 0;

      // Make sure buffer age >= 0
      buffer_age = max_i(buffer_age, 0);

      // Check if we have we have empty regions
      if (buffer_age > 1) {
        for (int i = 0; i < buffer_age - 1; ++i)
          if (!ps->all_damage_last[i]) { buffer_age = 0; break; }
      }
    }

    // Do nothing for buffer_age 1 (copy)
    if (1 != buffer_age) {
      // Copy pixels
      if (ps->o.glx_copy_from_front) {
        // Determine copy area
        XserverRegion reg_copy = XFixesCreateRegion(ps->dpy, NULL, 0);
        if (!buffer_age) {
          XFixesSubtractRegion(ps->dpy, reg_copy, ps->screen_reg, *preg);
        }
        else {
          for (int i = 0; i < buffer_age - 1; ++i)
            XFixesUnionRegion(ps->dpy, reg_copy, reg_copy,
                ps->all_damage_last[i]);
          XFixesSubtractRegion(ps->dpy, reg_copy, reg_copy, *preg);
        }

        // Actually copy pixels
        {
          GLfloat raster_pos[4];
          GLfloat curx = 0.0f, cury = 0.0f;
          glGetFloatv(GL_CURRENT_RASTER_POSITION, raster_pos);
          glReadBuffer(GL_FRONT);
          glRasterPos2f(0.0, 0.0);
          {
            int nrects = 0;
            XRectangle *rects = XFixesFetchRegion(ps->dpy, reg_copy, &nrects);
            for (int i = 0; i < nrects; ++i) {
              const int x = rects[i].x;
              const int y = ps->root_height - rects[i].y - rects[i].height;
              // Kwin patch says glRasterPos2f() causes artifacts on bottom
              // screen edge with some drivers
              glBitmap(0, 0, 0, 0, x - curx, y - cury, NULL);
              curx = x;
              cury = y;
              glCopyPixels(x, y, rects[i].width, rects[i].height, GL_COLOR);
            }
            cxfree(rects);
          }
          glReadBuffer(GL_BACK);
          glRasterPos4fv(raster_pos);
        }

        free_region(ps, &reg_copy);
      }

      // Determine paint area
      if (ps->o.glx_copy_from_front) { }
      else if (buffer_age) {
        for (int i = 0; i < buffer_age - 1; ++i)
          XFixesUnionRegion(ps->dpy, *preg, *preg, ps->all_damage_last[i]);
      }
      else {
        free_region(ps, preg);
      }
    }
  }

  if (trace_damage) {
    free_region(ps, &ps->all_damage_last[CGLX_MAX_BUFFER_AGE - 1]);
    memmove(ps->all_damage_last + 1, ps->all_damage_last,
        (CGLX_MAX_BUFFER_AGE - 1) * sizeof(XserverRegion));
    ps->all_damage_last[0] = newdamage;
  }

  glx_set_clip(ps, *preg, NULL);

#ifdef DEBUG_GLX_PAINTREG
  glx_render_color(ps, 0, 0, ps->root_width, ps->root_height, 0, *preg, NULL);
#endif

#ifdef DEBUG_GLX_ERR
  glx_check_err(ps);
#endif
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

#ifdef DEBUG_GLX_ERR
  glx_check_err(ps);
#endif
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
  for (int ri = 0; ri < nrects; ++ri) { \
    XRectangle crect; \
    rect_crop(&crect, &rects[ri], &rec_all); \
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

static inline GLuint
glx_gen_texture(session_t *ps, GLenum tex_tgt, int width, int height) {
  GLuint tex = 0;
  glGenTextures(1, &tex);
  if (!tex) return 0;
  glEnable(tex_tgt);
  glBindTexture(tex_tgt, tex);
  glTexParameteri(tex_tgt, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(tex_tgt, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(tex_tgt, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(tex_tgt, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(tex_tgt, 0, GL_RGB, width, height, 0, GL_RGB,
      GL_UNSIGNED_BYTE, NULL);
  glBindTexture(tex_tgt, 0);

  return tex;
}

static inline void
glx_copy_region_to_tex(session_t *ps, GLenum tex_tgt, int basex, int basey,
    int dx, int dy, int width, int height) {
  if (width > 0 && height > 0)
    glCopyTexSubImage2D(tex_tgt, 0, dx - basex, dy - basey,
        dx, ps->root_height - dy - height, width, height);
}

#ifdef CONFIG_VSYNC_OPENGL_GLSL
/**
 * Blur contents in a particular region.
 */
bool
glx_blur_dst(session_t *ps, int dx, int dy, int width, int height, float z,
    GLfloat factor_center,
    XserverRegion reg_tgt, const reg_data_t *pcache_reg,
    glx_blur_cache_t *pbc) {
  assert(ps->glx_blur_passes[0].prog);
  const bool more_passes = ps->glx_blur_passes[1].prog;
  const bool have_scissors = glIsEnabled(GL_SCISSOR_TEST);
  const bool have_stencil = glIsEnabled(GL_STENCIL_TEST);
  bool ret = false;

  // Calculate copy region size
  glx_blur_cache_t ibc = { .width = 0, .height = 0 };
  if (!pbc)
    pbc = &ibc;

  int mdx = dx, mdy = dy, mwidth = width, mheight = height;
#ifdef DEBUG_GLX
  printf_dbgf("(): %d, %d, %d, %d\n", mdx, mdy, mwidth, mheight);
#endif

  /*
  if (ps->o.resize_damage > 0) {
    int inc_x = 0, inc_y = 0;
    for (int i = 0; i < MAX_BLUR_PASS; ++i) {
      XFixed *kern = ps->o.blur_kerns[i];
      if (!kern) break;
      inc_x += XFixedToDouble(kern[0]) / 2;
      inc_y += XFixedToDouble(kern[1]) / 2;
    }
    inc_x = min_i(ps->o.resize_damage, inc_x);
    inc_y = min_i(ps->o.resize_damage, inc_y);

    mdx = max_i(dx - inc_x, 0);
    mdy = max_i(dy - inc_y, 0);
    int mdx2 = min_i(dx + width + inc_x, ps->root_width),
        mdy2 = min_i(dy + height + inc_y, ps->root_height);
    mwidth = mdx2 - mdx;
    mheight = mdy2 - mdy;
  }
  */

  GLenum tex_tgt = GL_TEXTURE_RECTANGLE;
  if (ps->glx_has_texture_non_power_of_two)
    tex_tgt = GL_TEXTURE_2D;

  // Free textures if size inconsistency discovered
  if (mwidth != pbc->width || mheight != pbc->height)
    free_glx_bc_resize(ps, pbc);

  // Generate FBO and textures if needed
  if (!pbc->textures[0])
    pbc->textures[0] = glx_gen_texture(ps, tex_tgt, mwidth, mheight);
  GLuint tex_scr = pbc->textures[0];
  if (more_passes && !pbc->textures[1])
    pbc->textures[1] = glx_gen_texture(ps, tex_tgt, mwidth, mheight);
  pbc->width = mwidth;
  pbc->height = mheight;
  GLuint tex_scr2 = pbc->textures[1];
#ifdef CONFIG_VSYNC_OPENGL_FBO
  if (more_passes && !pbc->fbo)
    glGenFramebuffers(1, &pbc->fbo);
  const GLuint fbo = pbc->fbo;
#endif

  if (!tex_scr || (more_passes && !tex_scr2)) {
    printf_errf("(): Failed to allocate texture.");
    goto glx_blur_dst_end;
  }
#ifdef CONFIG_VSYNC_OPENGL_FBO
  if (more_passes && !fbo) {
    printf_errf("(): Failed to allocate framebuffer.");
    goto glx_blur_dst_end;
  }
#endif

  // Read destination pixels into a texture
  glEnable(tex_tgt);
  glBindTexture(tex_tgt, tex_scr);
  glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, mdy, mwidth, mheight);
  /*
  if (tex_scr2) {
    glBindTexture(tex_tgt, tex_scr2);
    glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, mdy, mwidth, dx - mdx);
    glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, dy + height,
        mwidth, mdy + mheight - dy - height);
    glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, dy, dx - mdx, height);
    glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, dx + width, dy,
        mdx + mwidth - dx - width, height);
  } */

  // Texture scaling factor
  GLfloat texfac_x = 1.0f, texfac_y = 1.0f;
  if (GL_TEXTURE_2D == tex_tgt) {
    texfac_x /= mwidth;
    texfac_y /= mheight;
  }

  // Paint it back
  if (more_passes) {
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
  }

  bool last_pass = false;
  for (int i = 0; !last_pass; ++i) {
    last_pass = !ps->glx_blur_passes[i + 1].prog;
    assert(i < MAX_BLUR_PASS - 1);
    const glx_blur_pass_t *ppass = &ps->glx_blur_passes[i];
    assert(ppass->prog);

    assert(tex_scr);
    glBindTexture(tex_tgt, tex_scr);

#ifdef CONFIG_VSYNC_OPENGL_FBO
    if (!last_pass) {
      static const GLenum DRAWBUFS[2] = { GL_COLOR_ATTACHMENT0 };
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
          GL_TEXTURE_2D, tex_scr2, 0);
      glDrawBuffers(1, DRAWBUFS);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER)
          != GL_FRAMEBUFFER_COMPLETE) {
        printf_errf("(): Framebuffer attachment failed.");
        goto glx_blur_dst_end;
      }
    }
    else {
      static const GLenum DRAWBUFS[2] = { GL_BACK };
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDrawBuffers(1, DRAWBUFS);
      if (have_scissors)
        glEnable(GL_SCISSOR_TEST);
      if (have_stencil)
        glEnable(GL_STENCIL_TEST);
    }
#endif

    // Color negation for testing...
    // glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    // glTexEnvf(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
    // glTexEnvf(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glUseProgram(ppass->prog);
    if (ppass->unifm_offset_x >= 0)
      glUniform1f(ppass->unifm_offset_x, texfac_x);
    if (ppass->unifm_offset_y >= 0)
      glUniform1f(ppass->unifm_offset_y, texfac_y);
    if (ppass->unifm_factor_center >= 0)
      glUniform1f(ppass->unifm_factor_center, factor_center);

    {
      P_PAINTREG_START();
      {
        const GLfloat rx = (crect.x - mdx) * texfac_x;
        const GLfloat ry = (mheight - (crect.y - mdy)) * texfac_y;
        const GLfloat rxe = rx + crect.width * texfac_x;
        const GLfloat rye = ry - crect.height * texfac_y;
        GLfloat rdx = crect.x - mdx;
        GLfloat rdy = mheight - crect.y + mdy;
        GLfloat rdxe = rdx + crect.width;
        GLfloat rdye = rdy - crect.height;

        if (last_pass) {
          rdx = crect.x;
          rdy = ps->root_height - crect.y;
          rdxe = rdx + crect.width;
          rdye = rdy - crect.height;
        }

#ifdef DEBUG_GLX
        printf_dbgf("(): %f, %f, %f, %f -> %f, %f, %f, %f\n", rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);
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

    glUseProgram(0);

    // Swap tex_scr and tex_scr2
    {
      GLuint tmp = tex_scr2;
      tex_scr2 = tex_scr;
      tex_scr = tmp;
    }
  }

  ret = true;

glx_blur_dst_end:
#ifdef CONFIG_VSYNC_OPENGL_FBO
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
  glBindTexture(tex_tgt, 0);
  glDisable(tex_tgt);
  if (have_scissors)
    glEnable(GL_SCISSOR_TEST);
  if (have_stencil)
    glEnable(GL_STENCIL_TEST);

  if (&ibc == pbc) {
    free_glx_bc(ps, pbc);
  }

#ifdef DEBUG_GLX_ERR
  glx_check_err(ps);
#endif

  return ret;
}
#endif

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

#ifdef DEBUG_GLX_ERR
  glx_check_err(ps);
#endif

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

#ifdef DEBUG_GLX_PAINTREG
  glx_render_dots(ps, dx, dy, width, height, z, reg_tgt, pcache_reg);
  return true;
#endif

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
      GLfloat rx = (double) (crect.x - dx + x);
      GLfloat ry = (double) (crect.y - dy + y);
      GLfloat rxe = rx + (double) crect.width;
      GLfloat rye = ry + (double) crect.height;
      // Rectangle textures have [0-w] [0-h] while 2D texture has [0-1] [0-1]
      // Thanks to amonakov for pointing out!
      if (GL_TEXTURE_2D == ptex->target) {
        rx = rx / ptex->width;
        ry = ry / ptex->height;
        rxe = rxe / ptex->width;
        rye = rye / ptex->height;
      }
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
      printf_dbgf("(): Rect %d: %f, %f, %f, %f -> %d, %d, %d, %d\n", ri, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);
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

#ifdef DEBUG_GLX_ERR
  glx_check_err(ps);
#endif

  return true;
}

/**
 * Render a region with color.
 */
static void
glx_render_color(session_t *ps, int dx, int dy, int width, int height, int z,
    XserverRegion reg_tgt, const reg_data_t *pcache_reg) {
  static int color = 0;

  color = color % (3 * 3 * 3 - 1) + 1;
  glColor4f(1.0 / 3.0 * (color / (3 * 3)),
      1.0 / 3.0 * (color % (3 * 3) / 3),
      1.0 / 3.0 * (color % 3),
      1.0f
      );
  z -= 0.2;

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
  glColor4f(0.0f, 0.0f, 0.0f, 0.0f);

#ifdef DEBUG_GLX_ERR
  glx_check_err(ps);
#endif
}

/**
 * Render a region with dots.
 */
static void
glx_render_dots(session_t *ps, int dx, int dy, int width, int height, int z,
    XserverRegion reg_tgt, const reg_data_t *pcache_reg) {
  glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
  z -= 0.1;

  {
    P_PAINTREG_START();
    {
      static const GLint BLK_WID = 5, BLK_HEI = 5;

      glEnd();
      glPointSize(1.0);
      glBegin(GL_POINTS);

      GLint rdx = crect.x;
      GLint rdy = ps->root_height - crect.y;
      GLint rdxe = rdx + crect.width;
      GLint rdye = rdy - crect.height;
      rdx = (rdx) / BLK_WID * BLK_WID;
      rdy = (rdy) / BLK_HEI * BLK_HEI;
      rdxe = (rdxe) / BLK_WID * BLK_WID;
      rdye = (rdye) / BLK_HEI * BLK_HEI;

      for (GLint cdx = rdx; cdx < rdxe; cdx += BLK_WID)
        for (GLint cdy = rdy; cdy > rdye; cdy -= BLK_HEI)
          glVertex3i(cdx + BLK_WID / 2, cdy - BLK_HEI / 2, z);
    }
    P_PAINTREG_END();
  }
  glColor4f(0.0f, 0.0f, 0.0f, 0.0f);

#ifdef DEBUG_GLX_ERR
  glx_check_err(ps);
#endif
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

#ifdef DEBUG_GLX_ERR
  glx_check_err(ps);
#endif

  cxfree(rects);
}

#ifdef CONFIG_VSYNC_OPENGL_GLSL
GLuint
glx_create_shader(GLenum shader_type, const char *shader_str) {
#ifdef DEBUG_GLX_GLSL
  printf("glx_create_shader(): ===\n%s\n===\n", shader_str);
  fflush(stdout);
#endif

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

