// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

struct glx_fbconfig_info {
	GLXFBConfig cfg;
	int texture_tgts;
	int texture_fmt;
	int y_inverted;
};

struct glx_fbconfig_info *
glx_find_fbconfig(Display *, int screen, xcb_render_pictforminfo_t *, int depth);
