// SPDX-License-Identifier: GPL-2.0-or-later
// Wobbly window spring surface, adapted from KWin's Wobbly Windows effect.
// SPDX-FileCopyrightText: 2008 Cédric Borgese <cedric.borgese@gmail.com>
#pragma once

#include <stdbool.h>
#include <picom/types.h>

#define WOBBLY_GRID_SIZE 4
#define WOBBLY_POINT_COUNT (WOBBLY_GRID_SIZE * WOBBLY_GRID_SIZE)
#define WOBBLY_MARGIN 100

struct wobbly_parameters {
	double stiffness;
	double drag;
	double move_factor;
	double min_velocity;
	double max_velocity;
	double stop_velocity;
	double min_acceleration;
	double max_acceleration;
	double stop_acceleration;
	int x_tessellation;
	int y_tessellation;
};

struct wobbly_window {
	vec2 position[WOBBLY_POINT_COUNT];
	vec2 velocity[WOBBLY_POINT_COUNT];
	struct wobbly_parameters parameters;
	bool active;
};

void wobbly_start(struct wobbly_window *state, ivec2 old_origin, ivec2 old_size,
                  struct wobbly_parameters parameters);
bool wobbly_advance(struct wobbly_window *state, ivec2 origin, ivec2 size,
                    double elapsed_ms);
vec2 wobbly_point(const vec2 *points, double x, double y);
