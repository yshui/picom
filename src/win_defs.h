#pragma once
#include <stdint.h>

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

enum win_flags {
	// Note: *_NONE flags are mostly redundant and meant for detecting logical errors
	// in the code

	/// pixmap is out of date, will be update in win_process_flags
	WIN_FLAGS_PIXMAP_STALE = 1,
	/// window does not have pixmap bound
	WIN_FLAGS_PIXMAP_NONE = 2,
	/// there was an error trying to bind the images
	WIN_FLAGS_IMAGE_ERROR = 4,
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
