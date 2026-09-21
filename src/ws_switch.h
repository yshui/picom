// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026, Nikolay Borodin <monsterovich@gmail.com>

#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct session session_t;

struct ws_switch;

/// Create a new workspace switch animation state machine. Reads the current value of
/// _NET_CURRENT_DESKTOP, so must be called after atoms are initialized.
struct ws_switch *ws_switch_new(session_t *ps);
void ws_switch_free(session_t *ps, struct ws_switch *ws);

/// Called when the _NET_CURRENT_DESKTOP property of the root window changed.
void ws_switch_desktop_changed(session_t *ps);

/// Cancel any ongoing workspace switch, and release the snapshots. Must be called before
/// the backend is deinitialized, or when the root window size changes.
void ws_switch_cancel(session_t *ps);

/// Whether a workspace switch is in progress.
bool ws_switch_is_active(session_t *ps);

/// Return whether the next normal frame should be a full repaint, and clear the request.
/// This returns true for the first few frames after a workspace switch animation
/// finishes, so the layout manager ring is fully repopulated with post-switch layouts
/// before the buffer age based damage computation is trusted again.
bool ws_switch_consume_full_repaint(session_t *ps);

/// Render the current frame as part of a workspace switch.
///
/// @param ps              the session
/// @param render_start_us time when this frame started rendering, in microseconds
/// @param window_animation whether any window animation is running this frame
/// @return true if this frame has been rendered as part of a workspace switch, and the
///         caller should not render a normal frame; false otherwise.
bool ws_switch_render(session_t *ps, uint64_t render_start_us, bool window_animation);
