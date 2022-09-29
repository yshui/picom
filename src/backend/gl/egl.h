// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <stdbool.h>
// Older version of glx.h defines function prototypes for these extensions...
// Rename them to avoid conflicts
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

#include "compiler.h"
#include "log.h"
#include "utils.h"
#include "x.h"

struct eglext_info {
	bool initialized;
	bool has_EGL_MESA_query_driver;
	bool has_EGL_EXT_buffer_age;
	bool has_EGL_EXT_create_context_robustness;
	bool has_EGL_KHR_image_pixmap;
};

extern struct eglext_info eglext;

#ifdef EGL_MESA_query_driver
extern PFNEGLGETDISPLAYDRIVERNAMEPROC eglGetDisplayDriverName;
#endif

void eglext_init(EGLDisplay);
