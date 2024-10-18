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
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib-xcb.h>
#include <pixman.h>
#include <uthash.h>
#include <xcb/composite.h>
#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "picom.h"
#include "utils/misc.h"
#include "x.h"

#include "gl_common.h"
#include "glx.h"

struct _glx_data {
	struct gl_data gl;
	xcb_window_t target_win;
	GLXContext ctx;
	struct glx_fbconfig_cache *cached_fbconfigs;
};

struct glx_fbconfig_cache {
	UT_hash_handle hh;
	struct xvisual_info visual_info;
	struct glx_fbconfig_info info;
};

#define glXGetFBConfigAttribChecked(a, b, attr, c)                                       \
	do {                                                                             \
		if (glXGetFBConfigAttrib(a, b, attr, c)) {                               \
			log_info("Cannot get FBConfig attribute " #attr);                \
			break;                                                           \
		}                                                                        \
	} while (0)

bool glx_find_fbconfig(struct x_connection *c, struct xvisual_info m,
                       struct glx_fbconfig_info *info) {
	log_debug("Looking for FBConfig for RGBA%d%d%d%d, depth: %d, visual id: %#x", m.red_size,
	          m.blue_size, m.green_size, m.alpha_size, m.visual_depth, m.visual);

	info->cfg = NULL;

	int ncfg;
	// clang-format off
	GLXFBConfig *cfg =
	    glXChooseFBConfig(c->dpy, c->screen, (int[]){
	                          GLX_RED_SIZE, m.red_size,
	                          GLX_GREEN_SIZE, m.green_size,
	                          GLX_BLUE_SIZE, m.blue_size,
	                          GLX_ALPHA_SIZE, m.alpha_size,
	                          GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
	                          GLX_X_RENDERABLE, True,
	                          GLX_CONFIG_CAVEAT, GLX_NONE,
	                          None,
	                      }, &ncfg);
	// clang-format on

	int texture_tgts, y_inverted, texture_fmt;
	bool found = false;
	int min_cost = INT_MAX;
	GLXFBConfig ret;
	for (int i = 0; i < ncfg; i++) {
		int depthbuf, stencil, doublebuf, bufsize;
		glXGetFBConfigAttribChecked(c->dpy, cfg[i], GLX_BUFFER_SIZE, &bufsize);
		glXGetFBConfigAttribChecked(c->dpy, cfg[i], GLX_DEPTH_SIZE, &depthbuf);
		glXGetFBConfigAttribChecked(c->dpy, cfg[i], GLX_STENCIL_SIZE, &stencil);
		glXGetFBConfigAttribChecked(c->dpy, cfg[i], GLX_DOUBLEBUFFER, &doublebuf);
		if (depthbuf + stencil + bufsize * (doublebuf + 1) >= min_cost) {
			continue;
		}
		int red, green, blue;
		glXGetFBConfigAttribChecked(c->dpy, cfg[i], GLX_RED_SIZE, &red);
		glXGetFBConfigAttribChecked(c->dpy, cfg[i], GLX_BLUE_SIZE, &blue);
		glXGetFBConfigAttribChecked(c->dpy, cfg[i], GLX_GREEN_SIZE, &green);
		if (red != m.red_size || green != m.green_size || blue != m.blue_size) {
			// Color size doesn't match, this cannot work
			continue;
		}

		int rgb, rgba;
		glXGetFBConfigAttribChecked(c->dpy, cfg[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &rgb);
		glXGetFBConfigAttribChecked(c->dpy, cfg[i], GLX_BIND_TO_TEXTURE_RGBA_EXT,
		                            &rgba);
		if (!rgb && !rgba) {
			log_info("FBConfig is neither RGBA nor RGB, we cannot "
			         "handle this setup.");
			continue;
		}

		int visual;
		glXGetFBConfigAttribChecked(c->dpy, cfg[i], GLX_VISUAL_ID, &visual);
		if (m.visual_depth != -1 &&
		    xcb_aux_get_depth_of_visual(c->screen_info, (xcb_visualid_t)visual) !=
		        m.visual_depth) {
			// FBConfig and the correspondent X Visual might not have the same
			// depth. (e.g. 32 bit FBConfig with a 24 bit Visual). This is
			// quite common, seen in both open source and proprietary drivers.
			//
			// If the FBConfig has a matching depth but its visual doesn't, we
			// still cannot use it.
			continue;
		}

		// All check passed, we are using this one.
		found = true;
		ret = cfg[i];
		glXGetFBConfigAttribChecked(
		    c->dpy, cfg[i], GLX_BIND_TO_TEXTURE_TARGETS_EXT, &texture_tgts);
		glXGetFBConfigAttribChecked(c->dpy, cfg[i], GLX_Y_INVERTED_EXT, &y_inverted);

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
	free(cfg);
	if (found) {
		info->cfg = ret;
		info->texture_tgts = texture_tgts;
		info->texture_fmt = texture_fmt;
		info->y_inverted = y_inverted;
	}
	return found;
}

/**
 * Free a glx_texture_t.
 */
static void glx_release_image(backend_t *base, struct gl_texture *tex) {
	GLXPixmap *p = tex->user_data;
	// Release binding
	if (p && tex->texture) {
		glBindTexture(GL_TEXTURE_2D, tex->texture);
		glXReleaseTexImageEXT(base->c->dpy, *p, GLX_FRONT_LEFT_EXT);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	// Free GLX Pixmap
	if (p) {
		glXDestroyPixmap(base->c->dpy, *p);
		*p = 0;
	}

	free(p);
	tex->user_data = NULL;
}

/**
 * Destroy GLX related resources.
 */
void glx_deinit(backend_t *base) {
	struct _glx_data *gd = (void *)base;

	gl_deinit(&gd->gl);

	// Destroy GLX context
	if (gd->ctx) {
		glXMakeCurrent(base->c->dpy, None, NULL);
		glXDestroyContext(base->c->dpy, gd->ctx);
		gd->ctx = 0;
	}

	struct glx_fbconfig_cache *cached_fbconfig = NULL, *tmp = NULL;
	HASH_ITER(hh, gd->cached_fbconfigs, cached_fbconfig, tmp) {
		HASH_DEL(gd->cached_fbconfigs, cached_fbconfig);
		free(cached_fbconfig);
	}

	free(gd);
}

static void *glx_decouple_user_data(backend_t *base attr_unused, void *ud attr_unused) {
	return NULL;
}

static bool glx_set_swap_interval(int interval, Display *dpy, GLXDrawable drawable) {
	bool vsync_enabled = false;
	if (glxext.has_GLX_MESA_swap_control) {
		vsync_enabled = (glXSwapIntervalMESA((uint)interval) == 0);
	}
	if (!vsync_enabled && glxext.has_GLX_SGI_swap_control) {
		vsync_enabled = (glXSwapIntervalSGI(interval) == 0);
	}
	if (!vsync_enabled && glxext.has_GLX_EXT_swap_control) {
		// glXSwapIntervalEXT doesn't return if it's successful
		glXSwapIntervalEXT(dpy, drawable, interval);
		vsync_enabled = true;
	}
	return vsync_enabled;
}

const struct backend_operations glx_ops;
/**
 * Initialize OpenGL.
 */
static backend_t *glx_init(session_t *ps, xcb_window_t target) {
	bool success = false;
	glxext_init(ps->c.dpy, ps->c.screen);
	auto gd = ccalloc(1, struct _glx_data);
	init_backend_base(&gd->gl.base, ps);
	gd->gl.base.ops = glx_ops;

	gd->target_win = target;

	XVisualInfo *pvis = NULL;

	// Check for GLX extension
	if (!ps->c.e.has_glx) {
		log_error("No GLX extension.");
		goto end;
	}

	// Get XVisualInfo
	int nitems = 0;
	XVisualInfo vreq = {.visualid = ps->c.screen_info->root_visual};
	pvis = XGetVisualInfo(ps->c.dpy, VisualIDMask, &vreq, &nitems);
	if (!pvis) {
		log_error("Failed to acquire XVisualInfo for current visual.");
		goto end;
	}

	// Ensure the visual is double-buffered
	int value = 0;
	if (glXGetConfig(ps->c.dpy, pvis, GLX_USE_GL, &value) || !value) {
		log_error("Root visual is not a GL visual.");
		goto end;
	}

	if (glXGetConfig(ps->c.dpy, pvis, GLX_DOUBLEBUFFER, &value) || !value) {
		log_error("Root visual is not a double buffered GL visual.");
		goto end;
	}

	if (glXGetConfig(ps->c.dpy, pvis, GLX_RGBA, &value) || !value) {
		log_error("Root visual is a color index visual, not supported");
		goto end;
	}

	if (!glxext.has_GLX_EXT_texture_from_pixmap) {
		log_error("GLX_EXT_texture_from_pixmap is not supported by your driver");
		goto end;
	}

	if (!glxext.has_GLX_ARB_create_context) {
		log_error("GLX_ARB_create_context is not supported by your driver");
		goto end;
	}

	// Find a fbconfig with visualid matching the one from the target win, so we can
	// be sure that the fbconfig is compatible with our target window.
	int ncfgs;
	GLXFBConfig *cfg = glXGetFBConfigs(ps->c.dpy, ps->c.screen, &ncfgs);
	bool found = false;
	for (int i = 0; i < ncfgs; i++) {
		int visualid;
		glXGetFBConfigAttribChecked(ps->c.dpy, cfg[i], GLX_VISUAL_ID, &visualid);
		if ((VisualID)visualid != pvis->visualid) {
			continue;
		}

		int *attributes = (int[]){GLX_CONTEXT_MAJOR_VERSION_ARB,
		                          3,
		                          GLX_CONTEXT_MINOR_VERSION_ARB,
		                          3,
		                          GLX_CONTEXT_PROFILE_MASK_ARB,
		                          GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
		                          0,
		                          0,
		                          0};
		if (glxext.has_GLX_ARB_create_context_robustness) {
			attributes[6] = GLX_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB;
			attributes[7] = GLX_LOSE_CONTEXT_ON_RESET_ARB;
		}

		gd->ctx = glXCreateContextAttribsARB(ps->c.dpy, cfg[i], 0, true, attributes);
		free(cfg);

		if (!gd->ctx) {
			log_error("Failed to get GLX context.");
			goto end;
		}
		found = true;
		break;
	}

	if (!found) {
		log_error("Couldn't find a suitable fbconfig for the target window");
		goto end;
	}

	// Attach GLX context
	GLXDrawable tgt = gd->target_win;
	if (!glXMakeCurrent(ps->c.dpy, tgt, gd->ctx)) {
		log_error("Failed to attach GLX context.");
		goto end;
	}

	if (!gl_init(&gd->gl, ps)) {
		log_error("Failed to setup OpenGL");
		goto end;
	}

	gd->gl.decouple_texture_user_data = glx_decouple_user_data;
	gd->gl.release_user_data = glx_release_image;

	if (ps->o.vsync) {
		if (!glx_set_swap_interval(1, ps->c.dpy, tgt)) {
			log_error("Failed to enable vsync.");
		}
	} else {
		glx_set_swap_interval(0, ps->c.dpy, tgt);
	}

	success = true;

end:
	if (pvis) {
		XFree(pvis);
	}

	if (!success) {
		glx_deinit(&gd->gl.base);
		return NULL;
	}

	return &gd->gl.base;
}

static image_handle
glx_bind_pixmap(backend_t *base, xcb_pixmap_t pixmap, struct xvisual_info fmt) {
	GLXPixmap *glxpixmap = NULL;
	auto gd = (struct _glx_data *)base;

	if (fmt.visual_depth < 0) {
		log_error("Pixmap %#010x with invalid depth %d", pixmap, fmt.visual_depth);
		return NULL;
	}

	auto r =
	    xcb_get_geometry_reply(base->c->c, xcb_get_geometry(base->c->c, pixmap), NULL);
	if (!r) {
		log_error("Invalid pixmap %#010x", pixmap);
		return NULL;
	}

	log_trace("Binding pixmap %#010x", pixmap);
	auto inner = ccalloc(1, struct gl_texture);
	inner->width = r->width;
	inner->height = r->height;
	inner->format = BACKEND_IMAGE_FORMAT_PIXMAP;
	free(r);

	struct glx_fbconfig_cache *cached_fbconfig = NULL;
	HASH_FIND(hh, gd->cached_fbconfigs, &fmt, sizeof(fmt), cached_fbconfig);
	if (!cached_fbconfig) {
		struct glx_fbconfig_info fbconfig;
		if (!glx_find_fbconfig(base->c, fmt, &fbconfig)) {
			log_error("Couldn't find FBConfig with requested visual %#x",
			          fmt.visual);
			goto err;
		}
		cached_fbconfig = cmalloc(struct glx_fbconfig_cache);
		cached_fbconfig->visual_info = fmt;
		cached_fbconfig->info = fbconfig;
		HASH_ADD(hh, gd->cached_fbconfigs, visual_info, sizeof(fmt), cached_fbconfig);
	} else {
		log_debug("Found cached FBConfig for RGBA%d%d%d%d, depth: %d, visual id: "
		          "%#x",
		          fmt.red_size, fmt.blue_size, fmt.green_size, fmt.alpha_size,
		          fmt.visual_depth, fmt.visual);
	}
	struct glx_fbconfig_info *fbconfig = &cached_fbconfig->info;

	// Choose a suitable texture target for our pixmap.
	// Refer to GLX_EXT_texture_om_pixmap spec to see what are the mean
	// of the bits in texture_tgts
	if (!(fbconfig->texture_tgts & GLX_TEXTURE_2D_BIT_EXT)) {
		log_error("Cannot bind pixmap to GL_TEXTURE_2D, giving up");
		goto err;
	}

	log_debug("depth %d, rgba %d", fmt.visual_depth,
	          (fbconfig->texture_fmt == GLX_TEXTURE_FORMAT_RGBA_EXT));

	GLint attrs[] = {
	    GLX_TEXTURE_FORMAT_EXT,
	    fbconfig->texture_fmt,
	    GLX_TEXTURE_TARGET_EXT,
	    GLX_TEXTURE_2D_EXT,
	    0,
	};

	inner->y_inverted = fbconfig->y_inverted;

	glxpixmap = cmalloc(GLXPixmap);
	inner->pixmap = pixmap;
	*glxpixmap = glXCreatePixmap(base->c->dpy, fbconfig->cfg, pixmap, attrs);

	if (!*glxpixmap) {
		log_error("Failed to create glpixmap for pixmap %#010x", pixmap);
		goto err;
	}

	log_trace("GLXPixmap %#010lx", *glxpixmap);

	// Create texture
	inner->user_data = glxpixmap;
	inner->texture = gl_new_texture();
	glBindTexture(GL_TEXTURE_2D, inner->texture);
	glXBindTexImageEXT(base->c->dpy, *glxpixmap, GLX_FRONT_LEFT_EXT, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	gl_check_err();
	return (image_handle)inner;
err:
	if (glxpixmap && *glxpixmap) {
		glXDestroyPixmap(base->c->dpy, *glxpixmap);
	}
	free(glxpixmap);
	return NULL;
}

static bool glx_present(backend_t *base) {
	struct _glx_data *gd = (void *)base;
	gl_finish_render(&gd->gl);
	glXSwapBuffers(base->c->dpy, gd->target_win);
	return true;
}

static int glx_buffer_age(backend_t *base) {
	if (!glxext.has_GLX_EXT_buffer_age) {
		return -1;
	}

	struct _glx_data *gd = (void *)base;
	unsigned int val;
	glXQueryDrawable(base->c->dpy, gd->target_win, GLX_BACK_BUFFER_AGE_EXT, &val);
	return (int)val ?: -1;
}

static void glx_diagnostics(backend_t *base) {
	bool warn_software_rendering = false;
	const char *software_renderer_names[] = {"llvmpipe", "SWR", "softpipe"};
	auto glx_vendor = glXGetClientString(base->c->dpy, GLX_VENDOR);
	printf("* Driver vendors:\n");
	printf(" * GLX: %s\n", glx_vendor);
	printf(" * GL: %s\n", glGetString(GL_VENDOR));

	auto gl_renderer = (const char *)glGetString(GL_RENDERER);
	printf("* GL renderer: %s\n", gl_renderer);
	if (strcmp(glx_vendor, "Mesa Project and SGI") == 0) {
		for (size_t i = 0; i < ARR_SIZE(software_renderer_names); i++) {
			if (strstr(gl_renderer, software_renderer_names[i]) != NULL) {
				warn_software_rendering = true;
				break;
			}
		}
	}

#ifdef GLX_MESA_query_renderer
	if (glxext.has_GLX_MESA_query_renderer) {
		unsigned int accelerated = 0;
		glXQueryCurrentRendererIntegerMESA(GLX_RENDERER_ACCELERATED_MESA, &accelerated);
		printf("* Accelerated: %d\n", accelerated);

		// Trust GLX_MESA_query_renderer when it's available
		warn_software_rendering = (accelerated == 0);
	}
#endif

	if (warn_software_rendering) {
		printf("\n(You are using a software renderer. Unless you are doing this\n"
		       "intentionally, this means you don't have a graphics driver\n"
		       "properly installed. Performance will suffer. Please fix this\n"
		       "before reporting your issue.)\n");
	}
}

static int glx_max_buffer_age(struct backend_base *base attr_unused) {
	return 5;        // Why?
}

#define PICOM_BACKEND_GLX_MAJOR (0UL)
#define PICOM_BACKEND_GLX_MINOR (1UL)

static void glx_version(struct backend_base * /*base*/, uint64_t *major, uint64_t *minor) {
	*major = PICOM_BACKEND_GLX_MAJOR;
	*minor = PICOM_BACKEND_GLX_MINOR;
}

const struct backend_operations glx_ops = {
    .apply_alpha = gl_apply_alpha,
    .back_buffer = gl_back_buffer,
    .bind_pixmap = glx_bind_pixmap,
    .blit = gl_blit,
    .blur = gl_blur,
    .clear = gl_clear,
    .copy_area = gl_copy_area,
    .copy_area_quantize = gl_copy_area_quantize,
    .image_capabilities = gl_image_capabilities,
    .is_format_supported = gl_is_format_supported,
    .new_image = gl_new_image,
    .present = glx_present,
    .quirks = backend_no_quirks,
    .version = glx_version,
    .release_image = gl_release_image,

    .init = glx_init,
    .deinit = glx_deinit,
    .root_change = gl_root_change,
    .prepare = gl_prepare,
    .buffer_age = glx_buffer_age,
    .last_render_time = gl_last_render_time,
    .create_blur_context = gl_create_blur_context,
    .destroy_blur_context = gl_destroy_blur_context,
    .get_blur_size = gl_get_blur_size,
    .diagnostics = glx_diagnostics,
    .device_status = gl_device_status,
    .create_shader = gl_create_window_shader,
    .destroy_shader = gl_destroy_window_shader,
    .get_shader_attributes = gl_get_shader_attributes,
    .max_buffer_age = glx_max_buffer_age,
};

struct glxext_info glxext = {0};

void glxext_init(Display *dpy, int screen) {
	if (glxext.initialized) {
		return;
	}
	glxext.initialized = true;
#define check_ext(name)                                                                  \
	glxext.has_##name = epoxy_has_glx_extension(dpy, screen, #name);                 \
	log_info("Extension " #name " - %s", glxext.has_##name ? "present" : "absent")

	check_ext(GLX_SGI_video_sync);
	check_ext(GLX_SGI_swap_control);
	check_ext(GLX_OML_sync_control);
	check_ext(GLX_MESA_swap_control);
	check_ext(GLX_EXT_swap_control);
	check_ext(GLX_EXT_texture_from_pixmap);
	check_ext(GLX_ARB_create_context);
	check_ext(GLX_EXT_buffer_age);
	check_ext(GLX_ARB_create_context_robustness);
#ifdef GLX_MESA_query_renderer
	check_ext(GLX_MESA_query_renderer);
#endif
#undef check_ext
}

BACKEND_ENTRYPOINT(glx_register) {
	if (!backend_register(PICOM_BACKEND_MAJOR, PICOM_BACKEND_MINOR, "glx",
	                      glx_ops.init, true)) {
		log_error("Failed to register glx backend");
	}
}
