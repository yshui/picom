// SPDX-License-Identifier: MPL-2.0
/*
 * Copyright (c) 2022 Yuxuan Shui <yshuiv7@gmail.com>
 */

#include <X11/Xlib-xcb.h>
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "backend/gl/egl.h"
#include "backend/gl/gl_common.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "picom.h"
#include "utils.h"
#include "x.h"

struct egl_pixmap {
	EGLImage image;
	xcb_pixmap_t pixmap;
	bool owned;
};

struct egl_data {
	struct gl_data gl;
	EGLDisplay display;
	EGLSurface target_win;
	EGLContext ctx;
};

const char *eglGetErrorString(EGLint error) {
#define CASE_STR(value)                                                                  \
	case value: return #value;
	switch (error) {
		CASE_STR(EGL_SUCCESS)
		CASE_STR(EGL_NOT_INITIALIZED)
		CASE_STR(EGL_BAD_ACCESS)
		CASE_STR(EGL_BAD_ALLOC)
		CASE_STR(EGL_BAD_ATTRIBUTE)
		CASE_STR(EGL_BAD_CONTEXT)
		CASE_STR(EGL_BAD_CONFIG)
		CASE_STR(EGL_BAD_CURRENT_SURFACE)
		CASE_STR(EGL_BAD_DISPLAY)
		CASE_STR(EGL_BAD_SURFACE)
		CASE_STR(EGL_BAD_MATCH)
		CASE_STR(EGL_BAD_PARAMETER)
		CASE_STR(EGL_BAD_NATIVE_PIXMAP)
		CASE_STR(EGL_BAD_NATIVE_WINDOW)
		CASE_STR(EGL_CONTEXT_LOST)
	default: return "Unknown";
	}
#undef CASE_STR
}

/**
 * Free a gl_texture_t.
 */
static void egl_release_image(backend_t *base, struct gl_texture *tex) {
	struct egl_data *gd = (void *)base;
	struct egl_pixmap *p = tex->user_data;
	// Release binding
	if (p->image != EGL_NO_IMAGE) {
		eglDestroyImage(gd->display, p->image);
		p->image = EGL_NO_IMAGE;
	}

	if (p->owned) {
		xcb_free_pixmap(base->c->c, p->pixmap);
		p->pixmap = XCB_NONE;
	}

	free(p);
	tex->user_data = NULL;
}

/**
 * Destroy EGL related resources.
 */
void egl_deinit(backend_t *base) {
	struct egl_data *gd = (void *)base;

	gl_deinit(&gd->gl);

	// Destroy EGL context
	if (gd->ctx != EGL_NO_CONTEXT) {
		eglMakeCurrent(gd->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(gd->display, gd->ctx);
		gd->ctx = EGL_NO_CONTEXT;
	}

	if (gd->target_win != EGL_NO_SURFACE) {
		eglDestroySurface(gd->display, gd->target_win);
		gd->target_win = EGL_NO_SURFACE;
	}

	if (gd->display != EGL_NO_DISPLAY) {
		eglTerminate(gd->display);
		gd->display = EGL_NO_DISPLAY;
	}

	free(gd);
}

static void *egl_decouple_user_data(backend_t *base attr_unused, void *ud attr_unused) {
	auto ret = cmalloc(struct egl_pixmap);
	ret->owned = false;
	ret->image = EGL_NO_IMAGE;
	ret->pixmap = 0;
	return ret;
}

static bool egl_set_swap_interval(int interval, EGLDisplay dpy) {
	return eglSwapInterval(dpy, interval);
}

/**
 * Initialize OpenGL.
 */
static backend_t *egl_init(session_t *ps, xcb_window_t target) {
	bool success = false;
	struct egl_data *gd = NULL;

	// Check if we have the X11 platform
	const char *exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (strstr(exts, "EGL_EXT_platform_x11") == NULL) {
		log_error("X11 platform not available.");
		return NULL;
	}

	gd = ccalloc(1, struct egl_data);
	auto c = session_get_x_connection(ps);
	gd->display = eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, c->dpy,
	                                       (EGLint[]){
	                                           EGL_PLATFORM_X11_SCREEN_EXT,
	                                           c->screen,
	                                           EGL_NONE,
	                                       });
	if (gd->display == EGL_NO_DISPLAY) {
		log_error("Failed to get EGL display.");
		goto end;
	}

	EGLint major, minor;
	if (!eglInitialize(gd->display, &major, &minor)) {
		log_error("Failed to initialize EGL.");
		goto end;
	}

	if (major < 1 || (major == 1 && minor < 5)) {
		log_error("EGL version too old, need at least 1.5.");
		goto end;
	}

	// Check if EGL supports OpenGL
	const char *apis = eglQueryString(gd->display, EGL_CLIENT_APIS);
	if (strstr(apis, "OpenGL") == NULL) {
		log_error("EGL does not support OpenGL.");
		goto end;
	}

	eglext_init(gd->display);
	init_backend_base(&gd->gl.base, ps);
	if (!eglext.has_EGL_KHR_image_pixmap) {
		log_error("EGL_KHR_image_pixmap not available.");
		goto end;
	}

	auto visual_info = x_get_visual_info(c, c->screen_info->root_visual);
	EGLConfig config = NULL;
	int nconfigs = 1;
	// clang-format off
	if (eglChooseConfig(gd->display,
	                    (EGLint[]){
	                             EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
	                             EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	                             EGL_RED_SIZE, visual_info.red_size,
	                             EGL_GREEN_SIZE, visual_info.green_size,
	                             EGL_BLUE_SIZE, visual_info.blue_size,
	                             EGL_ALPHA_SIZE, visual_info.alpha_size,
	                             EGL_STENCIL_SIZE, 1,
	                             EGL_CONFIG_CAVEAT, EGL_NONE,
	                             EGL_NONE,
	                     }, &config, nconfigs, &nconfigs) != EGL_TRUE) {
		log_error("Failed to choose EGL config for the root window.");
		goto end;
	}
	// clang-format on

	gd->target_win =
	    eglCreatePlatformWindowSurfaceEXT(gd->display, config, &target, NULL);
	if (gd->target_win == EGL_NO_SURFACE) {
		log_error("Failed to create EGL surface.");
		goto end;
	}

	if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
		log_error("Failed to bind OpenGL API.");
		goto end;
	}

	gd->ctx = eglCreateContext(gd->display, config, NULL, NULL);
	if (gd->ctx == EGL_NO_CONTEXT) {
		log_error("Failed to get EGL context.");
		goto end;
	}

	if (!eglMakeCurrent(gd->display, gd->target_win, gd->target_win, gd->ctx)) {
		log_error("Failed to attach EGL context.");
		goto end;
	}

	if (!gl_init(&gd->gl, ps)) {
		log_error("Failed to setup OpenGL");
		goto end;
	}
	if (!gd->gl.has_egl_image_storage) {
		log_error("GL_EXT_EGL_image_storage extension not available.");
		goto end;
	}

	gd->gl.decouple_texture_user_data = egl_decouple_user_data;
	gd->gl.release_user_data = egl_release_image;

	if (session_get_options(ps)->vsync) {
		if (!egl_set_swap_interval(1, gd->display)) {
			log_error("Failed to enable vsync. %#x", eglGetError());
		}
	} else {
		egl_set_swap_interval(0, gd->display);
	}

	success = true;

end:
	if (!success) {
		if (gd != NULL) {
			egl_deinit(&gd->gl.base);
		}
		return NULL;
	}

	return &gd->gl.base;
}

static image_handle
egl_bind_pixmap(backend_t *base, xcb_pixmap_t pixmap, struct xvisual_info fmt, bool owned) {
	struct egl_data *gd = (void *)base;
	struct egl_pixmap *eglpixmap = NULL;

	auto r =
	    xcb_get_geometry_reply(base->c->c, xcb_get_geometry(base->c->c, pixmap), NULL);
	if (!r) {
		log_error("Invalid pixmap %#010x", pixmap);
		return NULL;
	}

	log_trace("Binding pixmap %#010x", pixmap);
	auto wd = ccalloc(1, struct backend_image);
	wd->max_brightness = 1;
	auto inner = ccalloc(1, struct gl_texture);
	inner->width = wd->ewidth = r->width;
	inner->height = wd->eheight = r->height;
	wd->inner = (struct backend_image_inner_base *)inner;
	free(r);

	log_debug("depth %d", fmt.visual_depth);

	inner->y_inverted = true;

	eglpixmap = cmalloc(struct egl_pixmap);
	eglpixmap->pixmap = pixmap;
	eglpixmap->image = eglCreateImage(gd->display, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
	                                  (EGLClientBuffer)(uintptr_t)pixmap, NULL);
	eglpixmap->owned = owned;

	if (eglpixmap->image == EGL_NO_IMAGE) {
		log_error("Failed to create eglpixmap for pixmap %#010x: %s", pixmap,
		          eglGetErrorString(eglGetError()));
		goto err;
	}

	log_trace("EGLImage %p", eglpixmap->image);

	// Create texture
	inner->user_data = eglpixmap;
	inner->texture = gl_new_texture(GL_TEXTURE_2D);
	inner->has_alpha = fmt.alpha_size != 0;
	wd->opacity = 1;
	wd->color_inverted = false;
	wd->dim = 0;
	wd->inner->refcount = 1;
	glBindTexture(GL_TEXTURE_2D, inner->texture);
	glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, eglpixmap->image, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	gl_check_err();
	return (image_handle)wd;
err:
	if (eglpixmap && eglpixmap->image) {
		eglDestroyImage(gd->display, eglpixmap->image);
	}
	free(eglpixmap);

	if (owned) {
		xcb_free_pixmap(base->c->c, pixmap);
	}
	free(wd);
	return NULL;
}

static void egl_present(backend_t *base, const region_t *region attr_unused) {
	struct egl_data *gd = (void *)base;
	gl_present(base, region);
	eglSwapBuffers(gd->display, gd->target_win);
}

static int egl_buffer_age(backend_t *base) {
	if (!eglext.has_EGL_EXT_buffer_age) {
		return -1;
	}

	struct egl_data *gd = (void *)base;
	EGLint val;
	eglQuerySurface(gd->display, (EGLSurface)gd->target_win, EGL_BUFFER_AGE_EXT, &val);
	return (int)val ?: -1;
}

static void egl_diagnostics(backend_t *base) {
	struct egl_data *gd = (void *)base;
	bool warn_software_rendering = false;
	const char *software_renderer_names[] = {"llvmpipe", "SWR", "softpipe"};
	auto egl_vendor = eglQueryString(gd->display, EGL_VENDOR);
	printf("* Driver vendors:\n");
	printf(" * EGL: %s\n", egl_vendor);
	if (eglext.has_EGL_MESA_query_driver) {
		printf(" * EGL driver: %s\n", eglGetDisplayDriverName(gd->display));
	}
	printf(" * GL: %s\n", glGetString(GL_VENDOR));

	auto gl_renderer = (const char *)glGetString(GL_RENDERER);
	printf("* GL renderer: %s\n", gl_renderer);
	if (strstr(egl_vendor, "Mesa")) {
		for (size_t i = 0; i < ARR_SIZE(software_renderer_names); i++) {
			if (strstr(gl_renderer, software_renderer_names[i]) != NULL) {
				warn_software_rendering = true;
				break;
			}
		}
	}

	if (warn_software_rendering) {
		printf("\n(You are using a software renderer. Unless you are doing this\n"
		       "intentionally, this means you don't have a graphics driver\n"
		       "properly installed. Performance will suffer. Please fix this\n"
		       "before reporting your issue.)\n");
	}
}

struct backend_operations egl_ops = {
    .init = egl_init,
    .deinit = egl_deinit,
    .root_change = gl_root_change,
    .bind_pixmap = egl_bind_pixmap,
    .release_image = gl_release_image,
    .prepare = gl_prepare,
    .compose = gl_compose,
    .image_op = gl_image_op,
    .set_image_property = gl_set_image_property,
    .clone_image = default_clone_image,
    .blur = gl_blur,
    .is_image_transparent = default_is_image_transparent,
    .present = egl_present,
    .buffer_age = egl_buffer_age,
    .last_render_time = gl_last_render_time,
    .create_shadow_context = gl_create_shadow_context,
    .destroy_shadow_context = gl_destroy_shadow_context,
    .render_shadow = backend_render_shadow_from_mask,
    .shadow_from_mask = gl_shadow_from_mask,
    .make_mask = gl_make_mask,
    .fill = gl_fill,
    .create_blur_context = gl_create_blur_context,
    .destroy_blur_context = gl_destroy_blur_context,
    .get_blur_size = gl_get_blur_size,
    .diagnostics = egl_diagnostics,
    .device_status = gl_device_status,
    .create_shader = gl_create_window_shader,
    .destroy_shader = gl_destroy_window_shader,
    .get_shader_attributes = gl_get_shader_attributes,
    .max_buffer_age = 5,        // Why?
};

struct eglext_info eglext = {0};

void eglext_init(EGLDisplay dpy) {
	if (eglext.initialized) {
		return;
	}
	eglext.initialized = true;
#define check_ext(name)                                                                  \
	eglext.has_##name = epoxy_has_egl_extension(dpy, #name);                         \
	log_info("Extension " #name " - %s", eglext.has_##name ? "present" : "absent")

	check_ext(EGL_EXT_buffer_age);
	check_ext(EGL_EXT_create_context_robustness);
	check_ext(EGL_KHR_image_pixmap);
#ifdef EGL_MESA_query_driver
	check_ext(EGL_MESA_query_driver);
#endif
#undef check_ext
}
