// SPDX-License-Identifier: GPL-2.0-or-later
// Based on KWin's Wobbly Windows effect.
// SPDX-FileCopyrightText: 2008 Cédric Borgese <cedric.borgese@gmail.com>
// The 4x4 control surface and default spring parameters follow KWin's effect.
// The middle of the title edge is held during a move.
#include <math.h>
#include <test.h>

#include "wobbly.h"

static double limit(double value, double low, double high) {
	return fmax(low, fmin(value, high));
}

static double limit_motion(double value, double minimum, double maximum) {
	if (fabs(value) < minimum) {
		return 0.0;
	}
	return limit(value, -maximum, maximum);
}

static void smooth_grid(vec2 *values) {
	vec2 smoothed[WOBBLY_POINT_COUNT];
	for (int j = 0; j < 4; j++) {
		for (int i = 0; i < 4; i++) {
			int index = j * 4 + i;
			vec2 sum = {.x = values[index].x * 3.0,
			            .y = values[index].y * 3.0};
			int weight = 3;
			for (int y = j - 1; y <= j + 1; y++) {
				for (int x = i - 1; x <= i + 1; x++) {
					if (x < 0 || y < 0 || x >= 4 || y >= 4 ||
					    (x == i && y == j)) {
						continue;
					}
					sum.x += values[y * 4 + x].x;
					sum.y += values[y * 4 + x].y;
					weight++;
				}
			}
			smoothed[index] = (vec2){.x = sum.x / weight,
			                         .y = sum.y / weight};
		}
	}
	for (int i = 0; i < WOBBLY_POINT_COUNT; i++) {
		values[i] = smoothed[i];
	}
}

static vec2 rest_point(ivec2 origin, ivec2 size, int i, int j) {
	return (vec2){.x = origin.x + (double)size.width * i / 3.0,
	              .y = origin.y + (double)size.height * j / 3.0};
}

void wobbly_start(struct wobbly_window *state, ivec2 old_origin, ivec2 old_size,
                  struct wobbly_parameters parameters) {
	state->active = true;
	state->parameters = parameters;
	for (int j = 0; j < WOBBLY_GRID_SIZE; j++) {
		for (int i = 0; i < WOBBLY_GRID_SIZE; i++) {
			int index = j * WOBBLY_GRID_SIZE + i;
			state->position[index] = rest_point(old_origin, old_size, i, j);
			state->velocity[index] = (vec2){};
		}
	}
}

static bool wobbly_step(struct wobbly_window *state, ivec2 origin, ivec2 size,
                        double ms) {
	const struct wobbly_parameters *params = &state->parameters;
	vec2 acceleration[WOBBLY_POINT_COUNT];
	double acceleration_sum = 0.0;
	double velocity_sum = 0.0;
	for (int j = 0; j < WOBBLY_GRID_SIZE; j++) {
		for (int i = 0; i < WOBBLY_GRID_SIZE; i++) {
			int index = j * WOBBLY_GRID_SIZE + i;
			vec2 pos = state->position[index];
			vec2 target = rest_point(origin, size, i, j);
			// The two central points on the title edge follow the window.
			if (j == 0 && (i == 1 || i == 2)) {
				acceleration[index] = (vec2){(target.x - pos.x) * params->stiffness,
				                             (target.y - pos.y) * params->stiffness};
				continue;
			}
			vec2 force = {};
			int neighbors = 0;
			const int di[] = {-1, 1, 0, 0};
			const int dj[] = {0, 0, -1, 1};
			for (int n = 0; n < 4; n++) {
				int ni = i + di[n], nj = j + dj[n];
				if (ni < 0 || ni == WOBBLY_GRID_SIZE || nj < 0 ||
				    nj == WOBBLY_GRID_SIZE) {
					continue;
				}
				vec2 other = state->position[nj * WOBBLY_GRID_SIZE + ni];
				vec2 rest = rest_point(origin, size, ni, nj);
				force.x += other.x - pos.x - (rest.x - target.x);
				force.y += other.y - pos.y - (rest.y - target.y);
				neighbors++;
			}
			acceleration[index] =
			    (vec2){force.x * params->stiffness / neighbors,
			           force.y * params->stiffness / neighbors};
		}
	}
	smooth_grid(acceleration);
	for (int index = 0; index < WOBBLY_POINT_COUNT; index++) {
		vec2 *velocity = &state->velocity[index];
		double ax = limit_motion(acceleration[index].x,
		                         params->min_acceleration, params->max_acceleration);
		double ay = limit_motion(acceleration[index].y,
		                         params->min_acceleration, params->max_acceleration);
		acceleration_sum += fabs(ax) + fabs(ay);
		velocity->x = ax * ms + velocity->x * params->drag;
		velocity->y = ay * ms + velocity->y * params->drag;
	}
	smooth_grid(state->velocity);
	for (int index = 0; index < WOBBLY_POINT_COUNT; index++) {
		vec2 target = rest_point(origin, size, index % 4, index / 4);
		vec2 *velocity = &state->velocity[index];
		vec2 *position = &state->position[index];
		velocity->x = limit_motion(velocity->x, params->min_velocity,
		                           params->max_velocity);
		velocity->y = limit_motion(velocity->y, params->min_velocity,
		                           params->max_velocity);
		position->x += velocity->x * ms * params->move_factor;
		position->y += velocity->y * ms * params->move_factor;
		position->x = limit(position->x, target.x - WOBBLY_MARGIN,
		                    target.x + WOBBLY_MARGIN);
		position->y = limit(position->y, target.y - WOBBLY_MARGIN,
		                    target.y + WOBBLY_MARGIN);
		velocity_sum += fabs(velocity->x) + fabs(velocity->y);
	}
	// Match KWin's default stop thresholds. Position error can decay much more
	// slowly than visible motion and makes the window appear to align pixel by
	// pixel after the drag has ended.
	return acceleration_sum >= params->stop_acceleration ||
	       velocity_sum >= params->stop_velocity;
}

bool wobbly_advance(struct wobbly_window *state, ivec2 origin, ivec2 size,
                    double elapsed_ms) {
	if (!state->active) {
		return false;
	}
	// A very large move (workspace switch, placement, etc.) is not a drag.
	if (size.width <= 0 || size.height <= 0) {
		state->active = false;
		return false;
	}
	double remaining = limit(elapsed_ms, 1.0, 50.0);
	bool moving = true;
	while (remaining > 0) {
		double step = fmin(remaining, 10.0);
		moving = wobbly_step(state, origin, size, step);
		remaining -= step;
	}
	if (!moving) {
		state->active = false;
		// The next render uses the ordinary, pixel-aligned window path. Clear
		// the mesh too so a later move cannot inherit subpixel displacement.
		for (int index = 0; index < WOBBLY_POINT_COUNT; index++) {
			state->position[index] =
			    rest_point(origin, size, index % WOBBLY_GRID_SIZE,
			               index / WOBBLY_GRID_SIZE);
			state->velocity[index] = (vec2){};
		}
	}
	return state->active;
}

vec2 wobbly_point(const vec2 *points, double x, double y) {
	double u = 1.0 - x, v = 1.0 - y;
	double bx[4] = {u * u * u, 3.0 * x * u * u,
	                3.0 * x * x * u, x * x * x};
	double by[4] = {v * v * v, 3.0 * y * v * v,
	                3.0 * y * y * v, y * y * y};
	vec2 result = {};
	for (int j = 0; j < 4; j++) {
		for (int i = 0; i < 4; i++) {
			double weight = bx[i] * by[j];
			result.x += points[j * 4 + i].x * weight;
			result.y += points[j * 4 + i].y * weight;
		}
	}
	return result;
}

TEST_CASE(wobbly_surface_settles) {
	struct wobbly_window state = {};
	struct wobbly_parameters params = {
	    .stiffness = 0.15, .drag = 0.80, .move_factor = 0.10,
	    .max_acceleration = 1000.0, .max_velocity = 1000.0,
	    .stop_acceleration = 0.5, .stop_velocity = 0.5,
	    .x_tessellation = 20, .y_tessellation = 20,
	};
	wobbly_start(&state, (ivec2){40, 30}, (ivec2){600, 400}, params);
	vec2 center = wobbly_point(state.position, 0.5, 0.5);
	TEST_TRUE(fabs(center.x - 340.0) < 0.001);
	TEST_TRUE(fabs(center.y - 230.0) < 0.001);
	wobbly_advance(&state, (ivec2){100, 55}, (ivec2){600, 400}, 16.0);
	center = wobbly_point(state.position, 0.5, 0.5);
	TEST_TRUE(center.x > 340.0 && center.x < 400.0);
	int frames = 0;
	while (wobbly_advance(&state, (ivec2){100, 55}, (ivec2){600, 400},
	                      16.0) && frames++ < 300) {
	}
	TEST_TRUE(!state.active);
	for (int index = 0; index < WOBBLY_POINT_COUNT; index++) {
		vec2 target = rest_point((ivec2){100, 55}, (ivec2){600, 400},
		                         index % WOBBLY_GRID_SIZE,
		                         index / WOBBLY_GRID_SIZE);
		TEST_TRUE(state.position[index].x == target.x);
		TEST_TRUE(state.position[index].y == target.y);
		TEST_TRUE(state.velocity[index].x == 0.0);
		TEST_TRUE(state.velocity[index].y == 0.0);
	}
	center = wobbly_point(state.position, 0.5, 0.5);
	TEST_TRUE(fabs(center.x - 400.0) < 3.0);
	TEST_TRUE(fabs(center.y - 255.0) < 3.0);
}

TEST_CASE(wobbly_stiffness_changes_response) {
	struct wobbly_parameters params = {
	    .stiffness = 0.01, .drag = 0.80, .move_factor = 0.10,
	    .max_acceleration = 1000.0, .max_velocity = 1000.0,
	    .stop_acceleration = 0.5, .stop_velocity = 0.5,
	    .x_tessellation = 12, .y_tessellation = 24,
	};
	struct wobbly_window soft = {}, firm = {};
	wobbly_start(&soft, (ivec2){0, 0}, (ivec2){600, 400}, params);
	params.stiffness = 0.03;
	wobbly_start(&firm, (ivec2){0, 0}, (ivec2){600, 400}, params);
	wobbly_advance(&soft, (ivec2){30, 0}, (ivec2){600, 400}, 10.0);
	wobbly_advance(&firm, (ivec2){30, 0}, (ivec2){600, 400}, 10.0);
	TEST_TRUE(firm.position[1].x > soft.position[1].x);
	TEST_TRUE(soft.parameters.x_tessellation == 12);
	TEST_TRUE(soft.parameters.y_tessellation == 24);
}
