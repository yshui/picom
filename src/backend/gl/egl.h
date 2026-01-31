// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <stdbool.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

#define EGL_EXTS                                                                         \
	X(EXT_buffer_age)                                                                \
	X(EXT_create_context_robustness)                                                 \
	X(KHR_image_pixmap)

struct eglext_info {
	bool initialized;

#ifdef EGL_MESA_query_driver
	bool has_MESA_query_driver;
#endif
#define X(x) bool has_##x;
	EGL_EXTS
#undef X
};

extern struct eglext_info eglext;

void eglext_init(EGLDisplay);
