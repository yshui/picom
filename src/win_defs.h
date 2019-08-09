#pragma once

typedef enum {
	WINTYPE_UNKNOWN,
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

/// Transition table:
/// (DESTROYED is when the win struct is destroyed and freed)
/// ('o' means in all other cases)
/// (Window is created in the UNMAPPED state)
/// +-------------+---------+----------+-------+-------+--------+--------+---------+
/// |             |UNMAPPING|DESTROYING|MAPPING|FADING |UNMAPPED| MAPPED |DESTROYED|
/// +-------------+---------+----------+-------+-------+--------+--------+---------+
/// |  UNMAPPING  |    o    |  Window  |Window |  -    | Fading |  -     |    -    |
/// |             |         |destroyed |mapped |       |finished|        |         |
/// +-------------+---------+----------+-------+-------+--------+--------+---------+
/// |  DESTROYING |    -    |    o     |   -   |  -    |   -    |  -     | Fading  |
/// |             |         |          |       |       |        |        |finished |
/// +-------------+---------+----------+-------+-------+--------+--------+---------+
/// |   MAPPING   | Window  |  Window  |   o   |  -    |   -    | Fading |    -    |
/// |             |unmapped |destroyed |       |       |        |finished|         |
/// +-------------+---------+----------+-------+-------+--------+--------+---------+
/// |    FADING   | Window  |  Window  |   -   |  o    |   -    | Fading |    -    |
/// |             |unmapped |destroyed |       |       |        |finished|         |
/// +-------------+---------+----------+-------+-------+--------+--------+---------+
/// |   UNMAPPED  |    -    |    -     |Window |  -    |   o    |   -    | Window  |
/// |             |         |          |mapped |       |        |        |destroyed|
/// +-------------+---------+----------+-------+-------+--------+--------+---------+
/// |    MAPPED   | Window  |  Window  |   -   |Opacity|   -    |   o    |    -    |
/// |             |unmapped |destroyed |       |change |        |        |         |
/// +-------------+---------+----------+-------+-------+--------+--------+---------+
typedef enum {
	// The window is being faded out because it's unmapped.
	WSTATE_UNMAPPING,
	// The window is being faded out because it's destroyed,
	WSTATE_DESTROYING,
	// The window is being faded in
	WSTATE_MAPPING,
	// Window opacity is not at the target level
	WSTATE_FADING,
	// The window is mapped, no fading is in progress.
	WSTATE_MAPPED,
	// The window is unmapped, no fading is in progress.
	WSTATE_UNMAPPED,
} winstate_t;

enum win_flags {
	/// win_image/shadow_image is out of date
	WIN_FLAGS_IMAGE_STALE = 1,
	/// there was an error trying to bind the images
	WIN_FLAGS_IMAGE_ERROR = 2,
};
