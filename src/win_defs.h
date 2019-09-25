#pragma once
#include <stdint.h>

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

/// Pending window updates
enum win_update {
	WIN_UPDATE_MAP = 1,
};

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
	// Note: *_NONE flags are mostly redudant and meant for detecting logical errors
	// in the code

	/// pixmap is out of date, will be update in win_process_flags
	WIN_FLAGS_PIXMAP_STALE = 1,
	/// window does not have pixmap bound
	WIN_FLAGS_PIXMAP_NONE = 2,
	/// there was an error trying to bind the images
	WIN_FLAGS_IMAGE_ERROR = 4,
	/// shadow is out of date, will be updated in win_process_flags
	WIN_FLAGS_SHADOW_STALE = 8,
	/// shadow has not been generated
	WIN_FLAGS_SHADOW_NONE = 16,
};

static const int_fast16_t WIN_FLAGS_IMAGES_STALE =
    WIN_FLAGS_PIXMAP_STALE | WIN_FLAGS_SHADOW_STALE;

#define WIN_FLAGS_IMAGES_NONE (WIN_FLAGS_PIXMAP_NONE | WIN_FLAGS_SHADOW_NONE)
