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

#include <GL/glx.h>
#include <locale.h>
#include "backend/backend.h"
#include "backend/gl/gl_common.h"
#include "string_utils.h"

/// @brief Wrapper of a GLX FBConfig.
typedef struct glx_fbconfig {
	GLXFBConfig cfg;
	GLint texture_fmt;
	GLint texture_tgts;
	bool y_inverted;
} glx_fbconfig_t;

struct _glx_win_data {
	gl_texture_t texture;
	GLXPixmap glpixmap;
	xcb_pixmap_t pixmap;
};

struct _glx_data {
	int glx_event;
	int glx_error;
	GLXContext ctx;
	gl_cap_t cap;
	gl_win_shader_t win_shader;
	gl_blur_shader_t blur_shader[MAX_BLUR_PASS];
	glx_fbconfig_t *fbconfigs[OPENGL_MAX_DEPTH + 1];

	void (*glXBindTexImage)(Display *display, GLXDrawable drawable, int buffer,
	                        const int *attrib_list);
	void (*glXReleaseTexImage)(Display *display, GLXDrawable drawable, int buffer);
};

/**
 * Check if a GLX extension exists.
 */
static inline bool glx_has_extension(session_t *ps, const char *ext) {
	const char *glx_exts = glXQueryExtensionsString(ps->dpy, ps->scr);
	if (!glx_exts) {
		log_error("Failed get GLX extension list.");
		return false;
	}

	int len = strlen(ext);
	char *found = strstr(glx_exts, ext);
	if (!found) {
		log_info("Missing GLX extension %s.", ext);
		return false;
	}

	// Make sure extension names are not crazy...
	assert(found[len] == ' ' || found[len] == 0);
	return true;
}

/**
 * @brief Release binding of a texture.
 */
static void glx_release_pixmap(struct _glx_data *gd, Display *dpy, struct _glx_win_data *wd) {
	// Release binding
	if (wd->glpixmap && wd->texture.texture) {
		glBindTexture(wd->texture.target, wd->texture.texture);
		gd->glXReleaseTexImage(dpy, wd->glpixmap, GLX_FRONT_LEFT_EXT);
		glBindTexture(wd->texture.target, 0);
	}

	// Free GLX Pixmap
	if (wd->glpixmap) {
		glXDestroyPixmap(dpy, wd->glpixmap);
		wd->glpixmap = 0;
	}

	gl_check_err();
}

/**
 * Free a glx_texture_t.
 */
static void attr_unused glx_release_win(struct _glx_data *gd, Display *dpy,
                                        struct _glx_win_data *wd) {
	glx_release_pixmap(gd, dpy, wd);
	glDeleteTextures(1, &wd->texture.texture);

	// Free structure itself
	free(wd);
}

/**
 * Free GLX part of win.
 */
static inline void free_win_res_glx(session_t *ps, win *w) {
	/*free_paint_glx(ps, &w->paint);*/
	/*free_paint_glx(ps, &w->shadow_paint);*/
	/*free_glx_bc(ps, &w->glx_blur_cache);*/
}

static inline int glx_cmp_fbconfig_cmpattr(session_t *ps, const glx_fbconfig_t *pfbc_a,
                                           const glx_fbconfig_t *pfbc_b, int attr) {
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
glx_cmp_fbconfig(session_t *ps, const glx_fbconfig_t *pfbc_a, const glx_fbconfig_t *pfbc_b) {
	int result = 0;

	if (!pfbc_a)
		return -1;
	if (!pfbc_b)
		return 1;
	int tmpattr;

	// Avoid 10-bit colors
	glXGetFBConfigAttrib(ps->dpy, pfbc_a->cfg, GLX_RED_SIZE, &tmpattr);
	if (tmpattr != 8)
		return -1;

	glXGetFBConfigAttrib(ps->dpy, pfbc_b->cfg, GLX_RED_SIZE, &tmpattr);
	if (tmpattr != 8)
		return 1;

#define P_CMPATTR_LT(attr)                                                               \
	{                                                                                \
		if ((result = glx_cmp_fbconfig_cmpattr(ps, pfbc_a, pfbc_b, (attr))))     \
			return -result;                                                  \
	}
#define P_CMPATTR_GT(attr)                                                               \
	{                                                                                \
		if ((result = glx_cmp_fbconfig_cmpattr(ps, pfbc_a, pfbc_b, (attr))))     \
			return result;                                                   \
	}

	P_CMPATTR_LT(GLX_BIND_TO_TEXTURE_RGBA_EXT);
	P_CMPATTR_LT(GLX_DOUBLEBUFFER);
	P_CMPATTR_LT(GLX_STENCIL_SIZE);
	P_CMPATTR_LT(GLX_DEPTH_SIZE);
	P_CMPATTR_GT(GLX_BIND_TO_MIPMAP_TEXTURE_EXT);

	return 0;
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
	if (glx_cmp_fbconfig(ps, ps->psglx->fbconfigs[depth], pfbcfg) < 0) {
		log_debug("(depth %d): %p overrides %p, target %#x.\n", depth, pfbcfg->cfg,
		          ps->psglx->fbconfigs[depth] ? ps->psglx->fbconfigs[depth]->cfg : 0,
		          pfbcfg->texture_tgts);
		if (!ps->psglx->fbconfigs[depth]) {
			ps->psglx->fbconfigs[depth] = cmalloc(glx_fbconfig_t);
		}
		(*ps->psglx->fbconfigs[depth]) = *pfbcfg;
	}
}

/**
 * Get GLX FBConfigs for all depths.
 */
static bool glx_update_fbconfig(struct _glx_data *gd, session_t *ps) {
	// Acquire all FBConfigs and loop through them
	int nele = 0;
	GLXFBConfig *pfbcfgs = glXGetFBConfigs(ps->dpy, ps->scr, &nele);

	for (GLXFBConfig *pcur = pfbcfgs; pcur < pfbcfgs + nele; pcur++) {
		glx_fbconfig_t fbinfo = {
		    .cfg = *pcur,
		    .texture_fmt = 0,
		    .texture_tgts = 0,
		    .y_inverted = false,
		};
		int id = (int)(pcur - pfbcfgs);
		int depth = 0, depth_alpha = 0, val = 0;

		// Skip over multi-sampled visuals
		// http://people.freedesktop.org/~glisse/0001-glx-do-not-use-multisample-visual-config-for-front-o.patch
#ifdef GLX_SAMPLES
		if (Success == glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_SAMPLES, &val) &&
		    val > 1)
			continue;
#endif

		if (Success != glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BUFFER_SIZE, &depth) ||
		    Success != glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_ALPHA_SIZE,
		                                    &depth_alpha)) {
			log_error("Failed to retrieve buffer size and alpha size of "
			          "FBConfig %d.",
			          id);
			continue;
		}
		if (Success != glXGetFBConfigAttrib(ps->dpy, *pcur,
		                                    GLX_BIND_TO_TEXTURE_TARGETS_EXT,
		                                    &fbinfo.texture_tgts)) {
			log_error("Failed to retrieve BIND_TO_TEXTURE_TARGETS_EXT of "
			          "FBConfig %d.",
			          id);
			continue;
		}

		int visualdepth = 0;
		{
			XVisualInfo *pvi = glXGetVisualFromFBConfig(ps->dpy, *pcur);
			if (!pvi) {
				// On nvidia-drivers-325.08 this happens slightly too often...
				// log_error("Failed to retrieve X Visual of FBConfig %d.", id);
				continue;
			}
			visualdepth = pvi->depth;
			cxfree(pvi);
		}

		bool rgb = false;
		bool rgba = false;

		if (depth >= 32 && depth_alpha &&
		    Success == glXGetFBConfigAttrib(ps->dpy, *pcur,
		                                    GLX_BIND_TO_TEXTURE_RGBA_EXT, &val) &&
		    val)
			rgba = true;

		if (Success == glXGetFBConfigAttrib(ps->dpy, *pcur,
		                                    GLX_BIND_TO_TEXTURE_RGB_EXT, &val) &&
		    val)
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
	if (!gd->fbconfigs[ps->depth]) {
		log_error("No FBConfig found for default depth %d.", ps->depth);
		return false;
	}

	if (!gd->fbconfigs[32]) {
		log_error("No FBConfig found for depth 32. compton may not work "
		          "correctly");
	}

	return true;
}

#ifdef DEBUG_GLX_DEBUG_CONTEXT
static inline GLXFBConfig
get_fbconfig_from_visualinfo(session_t *ps, const XVisualInfo *visualinfo) {
	int nelements = 0;
	GLXFBConfig *fbconfigs = glXGetFBConfigs(ps->dpy, visualinfo->screen, &nelements);
	for (int i = 0; i < nelements; ++i) {
		int visual_id = 0;
		if (Success == glXGetFBConfigAttrib(ps->dpy, fbconfigs[i], GLX_VISUAL_ID,
		                                    &visual_id) &&
		    visual_id == visualinfo->visualid)
			return fbconfigs[i];
	}

	return NULL;
}

static void glx_debug_msg_callback(GLenum source, GLenum type, GLuint id, GLenum severity,
                                   GLsizei length, const GLchar *message, GLvoid *userParam) {
	log_trace("(): source 0x%04X, type 0x%04X, id %u, severity 0x%0X, \"%s\"", source,
	          type, id, severity, message);
}
#endif

/**
 * Destroy GLX related resources.
 */
void glx_deinit(void *backend_data, session_t *ps) {
	struct _glx_data *gd = backend_data;

	// Free all GLX resources of windows
	for (win *w = ps->list; w; w = w->next)
		free_win_res_glx(ps, w);

	// Free GLSL shaders/programs
	for (int i = 0; i < MAX_BLUR_PASS; ++i) {
		gl_free_blur_shader(&gd->blur_shader[i]);
	}

	gl_free_prog_main(ps, &gd->win_shader);

	gl_check_err();

	// Free FBConfigs
	for (int i = 0; i <= OPENGL_MAX_DEPTH; ++i) {
		free(ps->psglx->fbconfigs[i]);
		ps->psglx->fbconfigs[i] = NULL;
	}

	// Destroy GLX context
	if (gd->ctx) {
		glXDestroyContext(ps->dpy, gd->ctx);
		gd->ctx = 0;
	}

	free(gd);
}

/**
 * Initialize OpenGL.
 */
static void *glx_init(session_t *ps) {
	bool success = false;
	auto gd = ccalloc(1, struct _glx_data);
	XVisualInfo *pvis = NULL;

	// Check for GLX extension
	if (!glXQueryExtension(ps->dpy, &gd->glx_event, &gd->glx_error)) {
		log_error("No GLX extension.");
		goto end;
	}

	// Get XVisualInfo
	int nitems = 0;
	XVisualInfo vreq = {.visualid = ps->vis};
	pvis = XGetVisualInfo(ps->dpy, VisualIDMask, &vreq, &nitems);
	if (!pvis) {
		log_error("Failed to acquire XVisualInfo for current visual.");
		goto end;
	}

	// Ensure the visual is double-buffered
	int value = 0;
	if (glXGetConfig(ps->dpy, pvis, GLX_USE_GL, &value) || !value) {
		log_error("Root visual is not a GL visual.");
		goto end;
	}

	if (glXGetConfig(ps->dpy, pvis, GLX_DOUBLEBUFFER, &value) || !value) {
		log_error("Root visual is not a double buffered GL visual.");
		goto end;
	}

	// Ensure GLX_EXT_texture_from_pixmap exists
	if (!glx_has_extension(ps, "GLX_EXT_texture_from_pixmap"))
		goto end;

	// Initialize GLX data structure
	for (int i = 0; i < MAX_BLUR_PASS; ++i) {
		gd->blur_shader[i] = (gl_blur_shader_t){.frag_shader = -1,
		                                        .prog = -1,
		                                        .unifm_offset_x = -1,
		                                        .unifm_offset_y = -1,
		                                        .unifm_factor_center = -1};
	}

	// Get GLX context
	gd->ctx = glXCreateContext(ps->dpy, pvis, NULL, GL_TRUE);

	if (!gd->ctx) {
		log_error("Failed to get GLX context.");
		goto end;
	}

	// Attach GLX context
	GLXDrawable tgt = ps->overlay;
	if (!tgt) {
		tgt = ps->root;
	}
	if (!glXMakeCurrent(ps->dpy, tgt, gd->ctx)) {
		log_error("Failed to attach GLX context.");
		goto end;
	}

#ifdef DEBUG_GLX_DEBUG_CONTEXT
	f_DebugMessageCallback p_DebugMessageCallback =
	    (f_DebugMessageCallback)glXGetProcAddress((const GLubyte *)"glDebugMessageCal"
	                                                               "lback");
	if (!p_DebugMessageCallback) {
		log_error("Failed to get glDebugMessageCallback(0.");
		goto glx_init_end;
	}
	p_DebugMessageCallback(glx_debug_msg_callback, ps);
#endif

	// Ensure we have a stencil buffer. X Fixes does not guarantee rectangles
	// in regions don't overlap, so we must use stencil buffer to make sure
	// we don't paint a region for more than one time, I think?
	if (!ps->o.glx_no_stencil) {
		GLint val = 0;
		glGetIntegerv(GL_STENCIL_BITS, &val);
		if (!val) {
			log_error("Target window doesn't have stencil buffer.");
			goto end;
		}
	}

	// Check GL_ARB_texture_non_power_of_two, requires a GLX context and
	// must precede FBConfig fetching
	gd->cap.non_power_of_two_texture = gl_has_extension(ps, "GL_ARB_texture_non_"
	                                                        "power_of_two");

	gd->glXBindTexImage = (void *)glXGetProcAddress((const GLubyte *)"glXBindTexImage"
	                                                                 "EXT");
	gd->glXReleaseTexImage = (void *)glXGetProcAddress((const GLubyte *)"glXReleaseTe"
	                                                                    "xImageEXT");
	if (!gd->glXBindTexImage || !gd->glXReleaseTexImage) {
		log_error("Failed to acquire glXBindTexImageEXT() and/or "
		          "glXReleaseTexImageEXT(), make sure your OpenGL supports"
		          "GLX_EXT_texture_from_pixmap");
		goto end;
	}

	// Acquire FBConfigs
	if (!glx_update_fbconfig(gd, ps))
		goto end;

	// Render preparations
	gl_resize(ps->root_width, ps->root_height);

	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
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

	success = true;

end:
	cxfree(pvis);

	if (!success) {
		glx_deinit(gd, ps);
		return NULL;
	}

	return gd;
}

/**
 * Initialize GLX blur filter.
 */
static bool attr_unused glx_init_blur(session_t *ps) {
	struct _glx_data *gd = ps->backend_data;
	assert(ps->o.blur_kerns[0]);

	// Allocate PBO if more than one blur kernel is present
	if (ps->o.blur_kerns[1]) {
		// Try to generate a framebuffer
		GLuint fbo = 0;
		glGenFramebuffers(1, &fbo);
		if (!fbo) {
			log_error("Failed to generate Framebuffer. Cannot do "
			          "multi-pass blur with GLX backend.");
			return false;
		}
		glDeleteFramebuffers(1, &fbo);
	}

	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
	// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
	// Thanks to hiciu for reporting.
	setlocale(LC_NUMERIC, "C");

	static const char *FRAG_SHADER_BLUR_PREFIX = "#version 110\n"
	                                             "%s"
	                                             "uniform float offset_x;\n"
	                                             "uniform float offset_y;\n"
	                                             "uniform float factor_center;\n"
	                                             "uniform %s tex_scr;\n\n"
	                                             "void main() {\n"
	                                             "  vec4 sum = vec4(0.0, 0.0, 0.0, "
	                                             "0.0);\n";
	static const char *FRAG_SHADER_BLUR_ADD =
	    "  sum += float(%.7g) * %s(tex_scr, vec2(gl_TexCoord[0].x + offset_x "
	    "* float(%d), gl_TexCoord[0].y + offset_y * float(%d)));\n";
	static const char *FRAG_SHADER_BLUR_ADD_GPUSHADER4 =
	    "  sum += float(%.7g) * %sOffset(tex_scr, vec2(gl_TexCoord[0].x, "
	    "gl_TexCoord[0].y), ivec2(%d, %d));\n";
	static const char *FRAG_SHADER_BLUR_SUFFIX =
	    "  sum += %s(tex_scr, vec2(gl_TexCoord[0].x, gl_TexCoord[0].y)) * "
	    "factor_center;\n"
	    "  gl_FragColor = sum / (factor_center + float(%.7g));\n"
	    "}\n";

	const bool use_texture_rect = !gd->cap.non_power_of_two_texture;
	const char *sampler_type = (use_texture_rect ? "sampler2DRect" : "sampler2D");
	const char *texture_func = (use_texture_rect ? "texture2DRect" : "texture2D");
	const char *shader_add = FRAG_SHADER_BLUR_ADD;
	char *extension = strdup("");
	if (use_texture_rect)
		mstrextend(&extension, "#extension GL_ARB_texture_rectangle : "
		                       "require\n");
	if (ps->o.glx_use_gpushader4) {
		mstrextend(&extension, "#extension GL_EXT_gpu_shader4 : "
		                       "require\n");
		shader_add = FRAG_SHADER_BLUR_ADD_GPUSHADER4;
	}

	for (int i = 0; i < MAX_BLUR_PASS && ps->o.blur_kerns[i]; ++i) {
		xcb_render_fixed_t *kern = ps->o.blur_kerns[i];
		if (!kern)
			break;

		glx_blur_pass_t *ppass = &ps->psglx->blur_passes[i];

		// Build shader
		int wid = XFIXED_TO_DOUBLE(kern[0]), hei = XFIXED_TO_DOUBLE(kern[1]);
		int nele = wid * hei - 1;
		unsigned int len =
		    strlen(FRAG_SHADER_BLUR_PREFIX) + strlen(sampler_type) +
		    strlen(extension) +
		    (strlen(shader_add) + strlen(texture_func) + 42) * nele +
		    strlen(FRAG_SHADER_BLUR_SUFFIX) + strlen(texture_func) + 12 + 1;
		char *shader_str = ccalloc(len, char);
		char *pc = shader_str;
		sprintf(pc, FRAG_SHADER_BLUR_PREFIX, extension, sampler_type);
		pc += strlen(pc);
		assert(strlen(shader_str) < len);

		double sum = 0.0;
		for (int j = 0; j < hei; ++j) {
			for (int k = 0; k < wid; ++k) {
				if (hei / 2 == j && wid / 2 == k)
					continue;
				double val = XFIXED_TO_DOUBLE(kern[2 + j * wid + k]);
				if (0.0 == val)
					continue;
				sum += val;
				sprintf(pc, shader_add, val, texture_func, k - wid / 2,
				        j - hei / 2);
				pc += strlen(pc);
				assert(strlen(shader_str) < len);
			}
		}

		sprintf(pc, FRAG_SHADER_BLUR_SUFFIX, texture_func, sum);
		assert(strlen(shader_str) < len);
		ppass->frag_shader = gl_create_shader(GL_FRAGMENT_SHADER, shader_str);
		free(shader_str);

		if (!ppass->frag_shader) {
			log_error("Failed to create fragment shader %d.", i);
			goto err;
		}

		// Build program
		ppass->prog = gl_create_program(&ppass->frag_shader, 1);
		if (!ppass->prog) {
			log_error("Failed to create GLSL program.");
			goto err;
		}

		// Get uniform addresses
		ppass->unifm_factor_center =
		    glGetUniformLocationChecked(ppass->prog, "factor_center");
		if (!ps->o.glx_use_gpushader4) {
			ppass->unifm_offset_x =
			    glGetUniformLocationChecked(ppass->prog, "offset_x");
			ppass->unifm_offset_y =
			    glGetUniformLocationChecked(ppass->prog, "offset_y");
		}
	}
	free(extension);

	// Restore LC_NUMERIC
	setlocale(LC_NUMERIC, lc_numeric_old);
	free(lc_numeric_old);

	gl_check_err();

	return true;
err:
	free(extension);
	setlocale(LC_NUMERIC, lc_numeric_old);
	free(lc_numeric_old);
	return false;
}

void *glx_prepare_win(void *backend_data, session_t *ps, win *w) {
	struct _glx_data *gd = backend_data;
	// Retrieve pixmap parameters, if they aren't provided
	if (w->g.depth > OPENGL_MAX_DEPTH) {
		log_error("Requested depth %d higher than max possible depth %d.",
		          w->g.depth, OPENGL_MAX_DEPTH);
		return false;
	}

	const glx_fbconfig_t *pcfg = gd->fbconfigs[w->g.depth];
	if (!pcfg) {
		log_error("Couldn't find FBConfig with requested depth %d", w->g.depth);
		return false;
	}

	// Choose a suitable texture target for our pixmap.
	// Refer to GLX_EXT_texture_om_pixmap spec to see what are the mean
	// of the bits in texture_tgts
	GLenum tex_tgt = 0;
	if (GLX_TEXTURE_2D_BIT_EXT & pcfg->texture_tgts &&
	    ps->psglx->has_texture_non_power_of_two)
		tex_tgt = GLX_TEXTURE_2D_EXT;
	else if (GLX_TEXTURE_RECTANGLE_BIT_EXT & pcfg->texture_tgts)
		tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
	else if (!(GLX_TEXTURE_2D_BIT_EXT & pcfg->texture_tgts))
		tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
	else
		tex_tgt = GLX_TEXTURE_2D_EXT;

	log_debug("depth %d, tgt %#x, rgba %d\n", w->g.depth, tex_tgt,
	          (GLX_TEXTURE_FORMAT_RGBA_EXT == pcfg->texture_fmt));

	GLint attrs[] = {
	    GLX_TEXTURE_FORMAT_EXT, pcfg->texture_fmt, GLX_TEXTURE_TARGET_EXT, tex_tgt, 0,
	};

	auto wd = ccalloc(1, struct _glx_win_data);
	wd->texture.target =
	    (GLX_TEXTURE_2D_EXT == tex_tgt ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE);
	wd->texture.y_inverted = pcfg->y_inverted;

	if (ps->has_name_pixmap) {
		wd->pixmap = xcb_generate_id(ps->c);
		xcb_composite_name_window_pixmap(ps->c, w->id, wd->pixmap);
	} else {
		wd->pixmap = w->id;
	}
	if (!wd->pixmap) {
		log_error("Failed to get pixmap for window %#010x", w->id);
		goto err;
	}

	wd->glpixmap = glXCreatePixmap(ps->dpy, pcfg->cfg, wd->pixmap, attrs);
	if (!wd->glpixmap) {
		log_error("Failed to create glpixmap for window %#010x", w->id);
		goto err;
	}

	// Create texture

	GLuint texture = 0;
	GLuint target = wd->texture.target;
	glGenTextures(1, &texture);
	if (!texture) {
		log_error("Failed to generate texture for window %#010x", w->id);
		goto err;
	}

	glBindTexture(target, texture);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(target, 0);

	wd->texture.texture = texture;
	return wd;
err:
	if (wd->pixmap && wd->pixmap != w->id) {
		xcb_free_pixmap(ps->c, wd->pixmap);
	}
	if (wd->glpixmap) {
		glXDestroyPixmap(ps->dpy, wd->glpixmap);
	}
	free(wd);
	return NULL;
}

/**
 * Bind a X pixmap to an OpenGL texture.
 */
bool glx_render_win(void *backend_data, session_t *ps, win *w, void *win_data,
                    const region_t *reg_paint) {
	struct _glx_data *gd = backend_data;
	struct _glx_win_data *wd = win_data;
	if (ps->o.backend != BKEND_GLX && ps->o.backend != BKEND_XR_GLX_HYBRID)
		return true;

	assert(wd->pixmap);
	assert(wd->glpixmap);
	assert(wd->texture.texture);

	glBindTexture(wd->texture.target, wd->texture.texture);
	gd->glXBindTexImage(ps->dpy, wd->glpixmap, GLX_FRONT_LEFT_EXT, NULL);
	glBindTexture(wd->texture.target, 0);

	gl_check_err();

	return true;
}

#if 0
/**
 * Preprocess function before start painting.
 */
void
glx_paint_pre(session_t *ps, region_t *preg) {
  ps->psglx->z = 0.0;
  // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Get buffer age
  bool trace_damage = (ps->o.glx_swap_method < 0 || ps->o.glx_swap_method > 1);

  // Trace raw damage regions
  region_t newdamage;
  pixman_region32_init(&newdamage);
  if (trace_damage)
    copy_region(&newdamage, preg);

  // We use GLX buffer_age extension to decide which pixels in
  // the back buffer is reusable, and limit our redrawing
  int buffer_age = 0;

  // Query GLX_EXT_buffer_age for buffer age
  if (ps->o.glx_swap_method == SWAPM_BUFFER_AGE) {
    unsigned val = 0;
    glXQueryDrawable(ps->dpy, get_tgt_window(ps),
        GLX_BACK_BUFFER_AGE_EXT, &val);
    buffer_age = val;
  }

  // Buffer age too high
  if (buffer_age > CGLX_MAX_BUFFER_AGE + 1)
    buffer_age = 0;

  assert(buffer_age >= 0);

  if (buffer_age) {
    // Determine paint area
      for (int i = 0; i < buffer_age - 1; ++i)
        pixman_region32_union(preg, preg, &ps->all_damage_last[i]);
  } else
    // buffer_age == 0 means buffer age is not available, paint everything
    copy_region(preg, &ps->screen_reg);

  if (trace_damage) {
    // XXX use a circular queue instead of memmove
    pixman_region32_fini(&ps->all_damage_last[CGLX_MAX_BUFFER_AGE - 1]);
    memmove(ps->all_damage_last + 1, ps->all_damage_last,
        (CGLX_MAX_BUFFER_AGE - 1) * sizeof(region_t *));
    ps->all_damage_last[0] = newdamage;
  }

  glx_set_clip(ps, preg);

#ifdef DEBUG_GLX_PAINTREG
  glx_render_color(ps, 0, 0, ps->root_width, ps->root_height, 0, *preg, NULL);
#endif

  gl_check_err();
}
#endif

backend_info_t glx_backend = {
    .init = glx_init,

};
