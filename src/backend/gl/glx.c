// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * Copyright (c) 2019 Yuxuan Shui <yshuiv7@gmail.com>
 * See LICENSE-mit for more information.
 *
 */

#include <X11/Xlib-xcb.h>
#include <assert.h>
#include <limits.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/composite.h>
#include <xcb/xcb.h>

#include "backend/backend.h"
#include "backend/gl/gl_common.h"
#include "backend/gl/glx.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "utils.h"
#include "win.h"
#include "x.h"

struct _glx_win_data {
	gl_texture_t texture;
	GLXPixmap glpixmap;
	xcb_pixmap_t pixmap;
};

struct _glx_data {
	backend_t base;
	int glx_event;
	int glx_error;
	GLXContext ctx;
	gl_cap_t cap;
	gl_win_shader_t win_shader;
	gl_blur_shader_t blur_shader[MAX_BLUR_PASS];

	void (*glXBindTexImage)(Display *display, GLXDrawable drawable, int buffer,
	                        const int *attrib_list);
	void (*glXReleaseTexImage)(Display *display, GLXDrawable drawable, int buffer);
};

struct glx_fbconfig_info *
glx_find_fbconfig(Display *dpy, int screen, struct xvisual_info m) {
	log_debug("Looking for FBConfig for RGBA%d%d%d%d, depth %d", m.red_size,
	          m.blue_size, m.green_size, m.alpha_size, m.visual_depth);

	int ncfg;
	// clang-format off
	GLXFBConfig *cfg =
	    glXChooseFBConfig(dpy, screen, (int[]){
	                          GLX_RENDER_TYPE, GLX_RGBA_BIT,
	                          GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
	                          GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
	                          GLX_X_RENDERABLE, true,
	                          GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT, GLX_DONT_CARE,
	                          GLX_BUFFER_SIZE, m.red_size + m.green_size +
	                                           m.blue_size + m.alpha_size,
	                          GLX_RED_SIZE, m.red_size,
	                          GLX_BLUE_SIZE, m.blue_size,
	                          GLX_GREEN_SIZE, m.green_size,
	                          GLX_ALPHA_SIZE, m.alpha_size,
	                          GLX_STENCIL_SIZE, 0,
	                          GLX_DEPTH_SIZE, 0,
	                          0
	                      }, &ncfg);
	// clang-format on

#define glXGetFBConfigAttribChecked(a, b, attr, c)                                       \
	do {                                                                             \
		if (glXGetFBConfigAttrib(a, b, attr, c)) {                               \
			log_info("Cannot get FBConfig attribute " #attr);                \
			continue;                                                        \
		}                                                                        \
	} while (0)
	int texture_tgts, y_inverted, texture_fmt;
	bool found = false;
	int min_cost = INT_MAX;
	GLXFBConfig ret;
	for (int i = 0; i < ncfg; i++) {
		int depthbuf, stencil, doublebuf, bufsize;
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BUFFER_SIZE, &bufsize);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_DEPTH_SIZE, &depthbuf);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_STENCIL_SIZE, &stencil);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_DOUBLEBUFFER, &doublebuf);
		if (depthbuf + stencil + bufsize * (doublebuf + 1) >= min_cost) {
			continue;
		}
		int red, green, blue;
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_RED_SIZE, &red);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BLUE_SIZE, &blue);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_GREEN_SIZE, &green);
		if (red != m.red_size || green != m.green_size || blue != m.blue_size) {
			// Color size doesn't match, this cannot work
			continue;
		}

		int rgb, rgba;
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &rgb);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &rgba);
		if (!rgb && !rgba) {
			log_info("FBConfig is neither RGBA nor RGB, compton cannot "
			         "handle this setup.");
			continue;
		}

		int visual;
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_VISUAL_ID, &visual);
		if (m.visual_depth != -1 &&
		    x_get_visual_depth(XGetXCBConnection(dpy), visual) != m.visual_depth) {
			// Some driver might attach fbconfig to a GLX visual with a
			// different depth.
			//
			// (That makes total sense. - NVIDIA developers)
			continue;
		}

		// All check passed, we are using this one.
		found = true;
		ret = cfg[i];
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BIND_TO_TEXTURE_TARGETS_EXT,
		                            &texture_tgts);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_Y_INVERTED_EXT, &y_inverted);

		// Prefer the texture format with matching alpha, with the other one as
		// fallback
		if (m.alpha_size) {
			texture_fmt = rgba ? GLX_TEXTURE_FORMAT_RGBA_EXT
			                   : GLX_TEXTURE_FORMAT_RGB_EXT;
		} else {
			texture_fmt =
			    rgb ? GLX_TEXTURE_FORMAT_RGB_EXT : GLX_TEXTURE_FORMAT_RGBA_EXT;
		}
		min_cost = depthbuf + stencil + bufsize * (doublebuf + 1);
	}
#undef glXGetFBConfigAttribChecked
	free(cfg);
	if (!found) {
		return NULL;
	}

	auto info = cmalloc(struct glx_fbconfig_info);
	info->cfg = ret;
	info->texture_tgts = texture_tgts;
	info->texture_fmt = texture_fmt;
	info->y_inverted = y_inverted;
	return info;
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
void glx_release_win(void *backend_data, session_t *ps, win *w, void *win_data) {
	struct _glx_win_data *wd = win_data;
	struct _glx_data *gd = backend_data;
	glx_release_pixmap(gd, ps->dpy, wd);
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
backend_t *backend_glx_init(session_t *ps) {
	bool success = false;
	glxext_init(ps->dpy, ps->scr);
	auto gd = ccalloc(1, struct _glx_data);
	gd->base.c = ps->c;
	gd->base.root = ps->root;
	gd->base.ops = NULL; // TODO

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
	if (!glxext.has_GLX_EXT_texture_from_pixmap) {
		log_error("GLX_EXT_texture_from_pixmap is not supported by your driver");
		goto end;
	}

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
	gd->cap.non_power_of_two_texture = gl_has_extension("GL_ARB_texture_non_"
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

	// Initialize blur filters
	// gl_create_blur_filters(ps, gd->blur_shader, &gd->cap);

	success = true;

end:
	if (pvis)
		XFree(pvis);

	if (!success) {
		glx_deinit(gd, ps);
		return NULL;
	}

	return &gd->base;
}

void *glx_prepare_win(void *backend_data, session_t *ps, win *w) {
	struct _glx_data *gd = backend_data;
	// Retrieve pixmap parameters, if they aren't provided
	if (w->g.depth > OPENGL_MAX_DEPTH) {
		log_error("Requested depth %d higher than max possible depth %d.",
		          w->g.depth, OPENGL_MAX_DEPTH);
		return false;
	}

	auto wd = ccalloc(1, struct _glx_win_data);
	wd->pixmap = xcb_generate_id(ps->c);
	xcb_composite_name_window_pixmap(ps->c, w->id, wd->pixmap);
	if (!wd->pixmap) {
		log_error("Failed to get pixmap for window %#010x", w->id);
		goto err;
	}

	auto visual_info = x_get_visual_info(ps->c, w->a.visual);
	auto fbcfg = glx_find_fbconfig(ps->dpy, ps->scr, visual_info);
	if (!fbcfg) {
		log_error("Couldn't find FBConfig with requested visual %x", w->a.visual);
		goto err;
	}

	// Choose a suitable texture target for our pixmap.
	// Refer to GLX_EXT_texture_om_pixmap spec to see what are the mean
	// of the bits in texture_tgts
	GLenum tex_tgt = 0;
	if (GLX_TEXTURE_2D_BIT_EXT & fbcfg->texture_tgts && gd->cap.non_power_of_two_texture)
		tex_tgt = GLX_TEXTURE_2D_EXT;
	else if (GLX_TEXTURE_RECTANGLE_BIT_EXT & fbcfg->texture_tgts)
		tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
	else if (!(GLX_TEXTURE_2D_BIT_EXT & fbcfg->texture_tgts))
		tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
	else
		tex_tgt = GLX_TEXTURE_2D_EXT;

	log_debug("depth %d, tgt %#x, rgba %d\n", w->g.depth, tex_tgt,
	          (GLX_TEXTURE_FORMAT_RGBA_EXT == fbcfg->texture_fmt));

	GLint attrs[] = {
	    GLX_TEXTURE_FORMAT_EXT,
	    fbcfg->texture_fmt,
	    GLX_TEXTURE_TARGET_EXT,
	    tex_tgt,
	    0,
	};

	wd->texture.target =
	    (GLX_TEXTURE_2D_EXT == tex_tgt ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE);
	wd->texture.y_inverted = fbcfg->y_inverted;

	wd->glpixmap = glXCreatePixmap(ps->dpy, fbcfg->cfg, wd->pixmap, attrs);
	free(fbcfg);

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
	wd->texture.width = w->widthb;
	wd->texture.height = w->heightb;
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
void glx_render_win(void *backend_data, session_t *ps, win *w, void *win_data,
                           const region_t *reg_paint) {
	struct _glx_data *gd = backend_data;
	struct _glx_win_data *wd = win_data;

	assert(wd->pixmap);
	assert(wd->glpixmap);
	assert(wd->texture.texture);

	glBindTexture(wd->texture.target, wd->texture.texture);
	gd->glXBindTexImage(ps->dpy, wd->glpixmap, GLX_FRONT_LEFT_EXT, NULL);
	glBindTexture(wd->texture.target, 0);

	gl_check_err();
}

void glx_present(void *backend_data, session_t *ps) {
	glXSwapBuffers(ps->dpy, ps->overlay != XCB_NONE ? ps->overlay : ps->root);
}

int glx_buffer_age(void *backend_data, session_t *ps) {
	if (ps->o.glx_swap_method == SWAPM_BUFFER_AGE) {
		unsigned int val;
		glXQueryDrawable(ps->dpy, get_tgt_window(ps), GLX_BACK_BUFFER_AGE_EXT, &val);
		return (int)val ?: -1;
	} else {
		return -1;
	}
}

void glx_compose(void *backend_data, session_t *ps, win *w, void *win_data,
                        int dst_x, int dst_y, const region_t *region) {
	struct _glx_data *gd = backend_data;
	struct _glx_win_data *wd = win_data;

	// OpenGL and Xorg uses different coordinate systems.
	// First, We need to flip the y axis of the paint region
	region_t region_yflipped;
	pixman_region32_init(&region_yflipped);
	pixman_region32_copy(&region_yflipped, (region_t *)region);

	int nrects;
	auto rect = pixman_region32_rectangles(&region_yflipped, &nrects);
	for (int i = 0; i < nrects; i++) {
		auto tmp = rect[i].y1;
		rect[i].y1 = ps->root_height - rect[i].y2;
		rect[i].y2 = ps->root_height - tmp;
	}
	dump_region(&region_yflipped);

	// Then, we still need to convert the origin of painting.
	// Note, in GL coordinates, we need to specified the bottom left corner of the
	// rectangle, while what we get from the arguments are the top left corner.
	gl_compose(&wd->texture, 0, 0, dst_x, ps->root_height - dst_y - w->heightb, w->widthb,
	           w->heightb, 0, 1, true, false, &region_yflipped, &gd->win_shader);
	pixman_region32_fini(&region_yflipped);
}

/* backend_info_t glx_backend = { */
/*     .init = glx_init, */
/*     .deinit = glx_deinit, */
/*     .prepare_win = glx_prepare_win, */
/*     .render_win = glx_render_win, */
/*     .release_win = glx_release_win, */
/*     .present = glx_present, */
/*     .compose = glx_compose, */
/*     .is_win_transparent = default_is_win_transparent, */
/*     .is_frame_transparent = default_is_frame_transparent, */
/*     .buffer_age = glx_buffer_age, */
/*     .max_buffer_age = 5,        // XXX why? */
/* }; */

/**
 * Check if a GLX extension exists.
 */
static inline bool glx_has_extension(Display *dpy, int screen, const char *ext) {
	const char *glx_exts = glXQueryExtensionsString(dpy, screen);
	if (!glx_exts) {
		log_error("Failed get GLX extension list.");
		return false;
	}

	long inlen = strlen(ext);
	const char *curr = glx_exts;
	bool match = false;
	while (curr && !match) {
		const char *end = strchr(curr, ' ');
		if (!end) {
			// Last extension string
			match = strcmp(ext, curr) == 0;
		} else if (end - curr == inlen) {
			// Length match, do match string
			match = strncmp(ext, curr, end - curr) == 0;
		}
		curr = end ? end + 1 : NULL;
	}

	if (!match) {
		log_info("Missing GLX extension %s.", ext);
	} else {
		log_info("Found GLX extension %s.", ext);
	}

	return match;
}

struct glxext_info glxext = {0};
PFNGLXGETVIDEOSYNCSGIPROC glXGetVideoSyncSGI;
PFNGLXWAITVIDEOSYNCSGIPROC glXWaitVideoSyncSGI;
PFNGLXGETSYNCVALUESOMLPROC glXGetSyncValuesOML;
PFNGLXWAITFORMSCOMLPROC glXWaitForMscOML;
PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT;
PFNGLXSWAPINTERVALSGIPROC glXSwapIntervalSGI;
PFNGLXSWAPINTERVALMESAPROC glXSwapIntervalMESA;
PFNGLXBINDTEXIMAGEEXTPROC glXBindTexImageEXT;
PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT;

void glxext_init(Display *dpy, int screen) {
	if (glxext.initialized) {
		return;
	}
	glxext.initialized = true;
#define check_ext(name) glxext.has_##name = glx_has_extension(dpy, screen, #name)
	check_ext(GLX_SGI_video_sync);
	check_ext(GLX_SGI_swap_control);
	check_ext(GLX_OML_sync_control);
	check_ext(GLX_MESA_swap_control);
	check_ext(GLX_EXT_swap_control);
	check_ext(GLX_EXT_texture_from_pixmap);
#undef check_ext

#define lookup(name) (name = (__typeof__(name))glXGetProcAddress((GLubyte *)#name))
	// Checking if the returned function pointer is NULL is not really necessary,
	// or maybe not even useful, since glXGetProcAddress might always return something.
	// We are doing it just for completeness' sake.
	if (!lookup(glXGetVideoSyncSGI) || !lookup(glXWaitVideoSyncSGI)) {
		glxext.has_GLX_SGI_video_sync = false;
	}
	if (!lookup(glXSwapIntervalEXT)) {
		glxext.has_GLX_EXT_swap_control = false;
	}
	if (!lookup(glXSwapIntervalMESA)) {
		glxext.has_GLX_MESA_swap_control = false;
	}
	if (!lookup(glXSwapIntervalSGI)) {
		glxext.has_GLX_SGI_swap_control = false;
	}
	if (!lookup(glXWaitForMscOML) || !lookup(glXGetSyncValuesOML)) {
		glxext.has_GLX_OML_sync_control = false;
	}
	if (!lookup(glXBindTexImageEXT) || !lookup(glXReleaseTexImageEXT)) {
		glxext.has_GLX_EXT_texture_from_pixmap = false;
	}
#undef lookup
}
