// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <stdbool.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <xcb/render.h>

#include "log.h"
#include "compiler.h"
#include "utils.h"
#include "x.h"

struct glx_fbconfig_info {
	GLXFBConfig cfg;
	int texture_tgts;
	int texture_fmt;
	int y_inverted;
};

/// The search criteria for glx_find_fbconfig
struct glx_fbconfig_criteria {
	/// Bit width of the red component
	int red_size;
	/// Bit width of the green component
	int green_size;
	/// Bit width of the blue component
	int blue_size;
	/// Bit width of the alpha component
	int alpha_size;
	/// The depth of X visual
	int visual_depth;
};

struct glx_fbconfig_info *glx_find_fbconfig(Display *, int screen, struct glx_fbconfig_criteria);

/// Generate a search criteria for fbconfig from a X visual.
/// Returns {-1, -1, -1, -1, -1} on failure
static inline struct glx_fbconfig_criteria
x_visual_to_fbconfig_criteria(xcb_connection_t *c, xcb_visualid_t visual) {
	auto pictfmt = x_get_pictform_for_visual(c, visual);
	auto depth = x_get_visual_depth(c, visual);
	if (!pictfmt || depth == -1) {
		log_error("Invalid visual %#03x", visual);
		return (struct glx_fbconfig_criteria){-1, -1, -1, -1, -1};
	}
	if (pictfmt->type != XCB_RENDER_PICT_TYPE_DIRECT) {
		log_error("compton cannot handle non-DirectColor visuals. Report an "
		          "issue if you see this error message.");
		return (struct glx_fbconfig_criteria){-1, -1, -1, -1, -1};
	}

	int red_size = popcountl(pictfmt->direct.red_mask),
	    blue_size = popcountl(pictfmt->direct.blue_mask),
	    green_size = popcountl(pictfmt->direct.green_mask),
	    alpha_size = popcountl(pictfmt->direct.alpha_mask);

	return (struct glx_fbconfig_criteria){
	    .red_size = red_size,
	    .green_size = green_size,
	    .blue_size = blue_size,
	    .alpha_size = alpha_size,
	    .visual_depth = depth,
	};
}

struct glxext_info {
	bool initialized;
	bool has_GLX_SGI_video_sync;
	bool has_GLX_SGI_swap_control;
	bool has_GLX_OML_sync_control;
	bool has_GLX_MESA_swap_control;
	bool has_GLX_EXT_swap_control;
	bool has_GLX_EXT_texture_from_pixmap;
};

extern struct glxext_info glxext;

extern PFNGLXGETVIDEOSYNCSGIPROC glXGetVideoSyncSGI;
extern PFNGLXWAITVIDEOSYNCSGIPROC glXWaitVideoSyncSGI;
extern PFNGLXGETSYNCVALUESOMLPROC glXGetSyncValuesOML;
extern PFNGLXWAITFORMSCOMLPROC glXWaitForMscOML;
extern PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT;
extern PFNGLXSWAPINTERVALSGIPROC glXSwapIntervalSGI;
extern PFNGLXSWAPINTERVALMESAPROC glXSwapIntervalMESA;
extern PFNGLXBINDTEXIMAGEEXTPROC glXBindTexImageEXT;
extern PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT;

void glxext_init(Display *, int screen);
