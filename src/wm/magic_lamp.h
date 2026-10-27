// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <picom/backend.h>

struct magic_lamp_window {
	struct backend_magic_lamp mesh;
	bool active, pending, restoring, minimized;
	bool dirty, initialized, hidden, shaded, valid, supported, excluded;
	uint32_t wm_state;
	xcb_window_t client;
	double wait, opacity;
};

enum lamp_result {
	LAMP_IGNORE, LAMP_RUNNING, LAMP_FINISHED,
	LAMP_HIDE, LAMP_SHOW, LAMP_MINIMIZE_SCRIPT, LAMP_RESTORE_SCRIPT,
};

vec2 magic_lamp_point(const struct backend_magic_lamp *mesh, vec2 source);
double magic_lamp_advance(double progress, bool restoring, double dt, double duration);
bool win_magic_lamp_busy(const struct win *w);
void win_magic_lamp_cancel(struct win *w);
enum lamp_result win_magic_lamp_process(session_t *ps, struct win *w, double dt);
