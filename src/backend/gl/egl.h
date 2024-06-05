// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <stdbool.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

struct eglext_info {
	bool initialized;
	bool has_EGL_MESA_query_driver;
	bool has_EGL_EXT_buffer_age;
	bool has_EGL_EXT_create_context_robustness;
	bool has_EGL_KHR_image_pixmap;
};

extern struct eglext_info eglext;

void eglext_init(EGLDisplay);
