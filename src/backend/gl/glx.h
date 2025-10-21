// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <stdbool.h>

#include <X11/Xlib.h>
#include <epoxy/glx.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

#include "x.h"

struct glx_fbconfig_info {
	GLXFBConfig cfg;
	int texture_tgts;
	int texture_fmt;
	int y_inverted;
};

bool glx_find_fbconfig(struct x_connection *c, struct xvisual_info m,
                       struct glx_fbconfig_info *info);

#define GLX_EXTS                                                                         \
	X(SGI_video_sync)                                                                \
	X(SGI_swap_control)                                                              \
	X(OML_sync_control)                                                              \
	X(MESA_swap_control)                                                             \
	X(EXT_swap_control)                                                              \
	X(EXT_texture_from_pixmap)                                                       \
	X(ARB_create_context)                                                            \
	X(EXT_buffer_age)                                                                \
	X(ARB_create_context_robustness)

struct glxext_info {
	bool initialized;
#define X(name) bool has_##name;
	GLX_EXTS
#undef X
#ifdef GLX_MESA_query_renderer
	bool has_MESA_query_renderer;
#endif
};

extern struct glxext_info glxext;

void glxext_init(Display *, int screen);
