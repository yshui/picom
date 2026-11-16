// SPDX-License-Identifier: MPL-2.0
// Copyright (c) picom contributors

/// Full-screen magnifier.
///
/// Every monitor has a zoom factor. On a zoomed monitor (factor > 1) the compositor
/// shows a part of the root window, the "view", enlarged to fill the monitor. The view
/// follows the pointer and the keyboard focus according to the configured tracking
/// modes. The zoom is only a matter of presentation: input, window geometry and the
/// monitor layout are not changed.
///
/// The per-monitor state lives in a `struct zoom_state`, which outlives sessions, so
/// it survives picom resetting itself. `struct zoom` holds the per-session parts.

#pragma once

#include <stdbool.h>
#include <stddef.h>

struct session;
struct win;
struct renderer_zoom;
struct x_monitors;

#define ZOOM_FACTOR_MIN 1.0
#define ZOOM_FACTOR_MAX 32.0
/// Factor used by toggle on a monitor that has never been zoomed.
#define ZOOM_FACTOR_TOGGLE_DEFAULT 2.0

enum zoom_result {
	ZOOM_OK = 0,
	/// Factor is not a finite number between ZOOM_FACTOR_MIN and ZOOM_FACTOR_MAX.
	ZOOM_ERROR_INVALID_FACTOR,
	/// Step is not a finite number greater than zero.
	ZOOM_ERROR_INVALID_STEP,
	/// No current monitor has the requested name.
	ZOOM_ERROR_UNKNOWN_MONITOR,
	/// The pointer is not on any monitor.
	ZOOM_ERROR_NO_MONITOR_UNDER_POINTER,
};

/// Operations on the monitor under the pointer.
enum zoom_verb {
	ZOOM_VERB_IN,
	ZOOM_VERB_OUT,
	ZOOM_VERB_SET,
	ZOOM_VERB_OFF,
	ZOOM_VERB_TOGGLE,
};

/// Public view of a monitor's zoom state.
struct zoom_monitor_info {
	const char *name;
	double factor;
	/// The monitor's rectangle, in root window coordinates.
	int x, y, width, height;
	/// The part of the root window shown on the monitor.
	double view_x, view_y, view_width, view_height;
};

struct zoom_state;
struct zoom;

struct zoom_state *zoom_state_new(void);
void zoom_state_free(struct zoom_state *state);

/// Create the per-session part of the magnifier. Queries the current monitors.
struct zoom *zoom_new(struct session *ps, struct zoom_state *state);
void zoom_free(struct zoom *z);

/// Whether any monitor is zoomed.
bool zoom_is_active(const struct zoom *z);

/// The monitor list was updated.
void zoom_handle_monitors(struct zoom *z, const struct x_monitors *monitors);
/// The cursor image changed.
void zoom_handle_cursor_change(struct zoom *z);
/// Toplevel window `w` gained focus.
void zoom_handle_focus(struct zoom *z, const struct win *w);

/// Set the zoom factor of the monitor named `name`, or of every monitor if `name`
/// is "*". Fails without changing anything if the arguments are invalid.
enum zoom_result zoom_set_monitor_factor(struct zoom *z, const char *name, double factor);
/// Apply `verb` to the monitor under the pointer. `arg` is the step for ZOOM_VERB_IN
/// and ZOOM_VERB_OUT, the factor for ZOOM_VERB_SET, and unused otherwise.
enum zoom_result zoom_apply_verb(struct zoom *z, enum zoom_verb verb, double arg);

/// Get information about the `index`-th current monitor. Returns false if there are
/// no more than `index` monitors.
bool zoom_monitor_get(const struct zoom *z, size_t index, struct zoom_monitor_info *info);

/// A human readable description of a result.
const char *zoom_result_to_string(enum zoom_result result);

/// Parameters for the renderer, NULL if nothing is zoomed. The returned pointer is
/// valid until the next call.
const struct renderer_zoom *zoom_render_parameters(struct zoom *z);
