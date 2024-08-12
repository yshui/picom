// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

typedef enum {
	WINTYPE_UNKNOWN = 0,
	WINTYPE_DESKTOP,
	WINTYPE_DOCK,
	WINTYPE_TOOLBAR,
	WINTYPE_MENU,
	WINTYPE_UTILITY,
	WINTYPE_SPLASH,
	WINTYPE_DIALOG,
	WINTYPE_NORMAL,
	WINTYPE_DROPDOWN_MENU,
	WINTYPE_POPUP_MENU,
	WINTYPE_TOOLTIP,
	WINTYPE_NOTIFICATION,
	WINTYPE_COMBO,
	WINTYPE_DND,
	NUM_WINTYPES
} wintype_t;

/// Enumeration type of window painting mode.
typedef enum {
	WMODE_TRANS,              // The window body is (potentially) transparent
	WMODE_FRAME_TRANS,        // The window body is opaque, but the frame is not
	WMODE_SOLID,              // The window is opaque including the frame
} winmode_t;

/// The state of a window from Xserver's perspective
typedef enum {
	/// The window is unmapped. Equivalent to map-state == XCB_MAP_STATE_UNMAPPED
	WSTATE_UNMAPPED,
	/// The window no longer exists on the X server.
	WSTATE_DESTROYED,
	/// The window is mapped and viewable. Equivalent to map-state ==
	/// XCB_MAP_STATE_VIEWABLE
	WSTATE_MAPPED,
	// XCB_MAP_STATE_UNVIEWABLE is not represented here because it should not be
	// possible for top-level windows.
} winstate_t;

#define NUM_OF_WSTATES (WSTATE_MAPPED + 1)

enum win_flags {
	// Note: *_NONE flags are mostly redundant and meant for detecting logical errors
	// in the code

	/// pixmap is out of date, will be update in win_process_flags
	WIN_FLAGS_PIXMAP_STALE = 1,
	/// there was an error binding the window pixmap
	WIN_FLAGS_PIXMAP_ERROR = 4,
	/// Window is damaged, and should be added to the damage region
	/// (only used by the legacy backends, remove)
	WIN_FLAGS_DAMAGED = 8,
	/// the client window needs to be updated
	WIN_FLAGS_CLIENT_STALE = 32,
	/// the window is mapped by X, we need to call map_win_start for it
	WIN_FLAGS_MAPPED = 64,
	/// this window has properties which needs to be updated
	WIN_FLAGS_PROPERTY_STALE = 128,
	// TODO(yshui) _maybe_ split SIZE_STALE into SIZE_STALE and SHAPE_STALE
	/// this window has an unhandled size/shape change
	WIN_FLAGS_SIZE_STALE = 256,
	/// this window has an unhandled position (i.e. x and y) change
	WIN_FLAGS_POSITION_STALE = 512,
	/// need better name for this, is set when some aspects of the window changed
	WIN_FLAGS_FACTOR_CHANGED = 1024,
};

enum win_script_output {
	/// Additional X offset of the window.
	WIN_SCRIPT_OFFSET_X = 0,
	/// Additional Y offset of the window.
	WIN_SCRIPT_OFFSET_Y,
	/// Additional X offset of the shadow.
	WIN_SCRIPT_SHADOW_OFFSET_X,
	/// Additional Y offset of the shadow.
	WIN_SCRIPT_SHADOW_OFFSET_Y,
	/// Opacity of the window.
	WIN_SCRIPT_OPACITY,
	/// Opacity of the blurred background of the window.
	WIN_SCRIPT_BLUR_OPACITY,
	/// Opacity of the shadow.
	WIN_SCRIPT_SHADOW_OPACITY,
	/// Horizontal scale
	WIN_SCRIPT_SCALE_X,
	/// Vertical scale
	WIN_SCRIPT_SCALE_Y,
	/// Horizontal scale of the shadow
	WIN_SCRIPT_SHADOW_SCALE_X,
	/// Vertical scale of the shadow
	WIN_SCRIPT_SHADOW_SCALE_Y,
	/// X coordinate of the origin of the crop box
	WIN_SCRIPT_CROP_X,
	/// Y coordinate of the origin of the crop box
	WIN_SCRIPT_CROP_Y,
	/// Width of the crop box
	WIN_SCRIPT_CROP_WIDTH,
	/// Height of the crop box
	WIN_SCRIPT_CROP_HEIGHT,
	/// How much to blend in the saved window image
	WIN_SCRIPT_SAVED_IMAGE_BLEND,

	NUM_OF_WIN_SCRIPT_OUTPUTS,
};
