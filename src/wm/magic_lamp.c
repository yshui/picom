// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2008 Martin Gräßlin <mgraesslin@kde.org>
// Vertex displacement adapted from KWin's Magic Lamp effect, based on
// Rivo Laks' minimize animation. Timeline and degenerate-geometry guards are
// specific to picom. Source: kwin/src/plugins/magiclamp/magiclamp.cpp.
#include <math.h>
#include <test.h>

#include "magic_lamp.h"

double magic_lamp_advance(double progress, bool restoring, double dt, double duration) {
	double step = fmax(0.0, dt) / fmax(duration, 0.001);
	return fmax(0.0, fmin(1.0, progress + (restoring ? -step : step)));
}

vec2 magic_lamp_point(const struct backend_magic_lamp *m, vec2 source) {
	double p = fmax(0.0, fmin(1.0, m->progress));
	vec2 point = {.x = m->origin.x + source.x, .y = m->origin.y + source.y};
	if (p == 0 || m->size.x <= 0 || m->size.y <= 0) {
		return point;
	}
	bool horizontal = m->direction == MAGIC_LAMP_LEFT || m->direction == MAGIC_LAMP_RIGHT;
	bool forward = m->direction == MAGIC_LAMP_BOTTOM || m->direction == MAGIC_LAMP_RIGHT;
	double length = horizontal ? m->size.x : m->size.y;
	double a = horizontal ? source.x : source.y;
	double origin = horizontal ? m->origin.x : m->origin.y;
	double target = horizontal ? m->target.x : m->target.y;
	double target_length = horizontal ? m->target_size.x : m->target_size.y;
	double edge = target + (forward ? 0 : target_length) - origin;
	double factor = (forward ? a + (length - a) * p : length - a + a * p) / length;
	double distance = forward ? edge - a : a - edge;
	double offset = (forward ? edge + a : length + a - edge) * p * factor * factor * factor;
	// A maximized window or pointer fallback can touch the destination exactly.
	// Avoid 0/0 and keep overlapping/stale icon rectangles from exploding the mesh.
	double q = distance > 1e-6 ? fmax(0.0, fmin(1.0, offset / distance)) : p;
	double moved = forward ? fmin(edge, a + offset) : fmax(edge, a - offset);
	if (p == 1) {
		q = 1;
		moved = edge;
	}
	if (horizontal) {
		point.x = origin + moved;
		point.y += (m->target.y + m->target_size.y * source.y / m->size.y - point.y) * q;
	} else {
		point.y = origin + moved;
		point.x += (m->target.x + m->target_size.x * source.x / m->size.x - point.x) * q;
	}
	return point;
}

TEST_CASE(magic_lamp_reference_and_endpoints) {
	struct backend_magic_lamp m = {
	    .origin = {100, 200}, .size = {400, 300},
	    .target = {700, 800}, .target_size = {40, 20},
	    .direction = MAGIC_LAMP_BOTTOM,
	};
	vec2 point = magic_lamp_point(&m, (vec2){200, 150});
	TEST_TRUE(point.x == 300 && point.y == 350);
	m.progress = 0.5;
	point = magic_lamp_point(&m, (vec2){200, 150});
	// KWin bottom-edge equation: offset=750*.5*.75^3=158.203125.
	TEST_TRUE(fabs(point.y - 508.203125) < 1e-6);
	TEST_TRUE(fabs(point.x - 447.65625) < 1e-6);
	for (int direction = 0; direction < 4; direction++) {
		m.direction = direction;
		m.progress = 1;
		point = magic_lamp_point(&m, (vec2){200, 150});
		TEST_TRUE(isfinite(point.x) && isfinite(point.y));
		if (direction == MAGIC_LAMP_BOTTOM || direction == MAGIC_LAMP_TOP) {
			TEST_TRUE(point.x == 720);
			TEST_TRUE(point.y == (direction == MAGIC_LAMP_BOTTOM ? 800 : 820));
		} else {
			TEST_TRUE(point.y == 810);
			TEST_TRUE(point.x == (direction == MAGIC_LAMP_LEFT ? 740 : 700));
		}
	}
}

TEST_CASE(magic_lamp_degenerate_and_reversal) {
	struct backend_magic_lamp m = {.size = {400, 300}, .target = {0, 300}};
	for (int direction = 0; direction < 4; direction++) {
		m.direction = direction;
		for (int i = 0; i <= 100; i++) {
			m.progress = (double)i / 100;
			vec2 point = magic_lamp_point(&m, (vec2){0, 300});
			TEST_TRUE(isfinite(point.x) && isfinite(point.y));
		}
	}
	double p = magic_lamp_advance(0, false, 0.1, 0.25);
	TEST_TRUE(fabs(p - 0.4) < 1e-9);
	p = magic_lamp_advance(p, true, 0.1, 0.25);
	TEST_TRUE(p == 0);
	double slow = 0, fast = 0;
	for (int i = 0; i < 10; i++) slow = magic_lamp_advance(slow, false, 0.01, 0.25);
	for (int i = 0; i < 20; i++) fast = magic_lamp_advance(fast, false, 0.005, 0.25);
	TEST_TRUE(fabs(slow - fast) < 1e-9);
}
