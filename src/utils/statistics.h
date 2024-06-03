// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "compiler.h"

#define NTIERS (3)

struct rolling_window {
	int *elem;
	int elem_head, nelem;
	int window_size;
};

void rolling_window_destroy(struct rolling_window *rw);
void rolling_window_reset(struct rolling_window *rw);
void rolling_window_init(struct rolling_window *rw, int size);
int rolling_window_pop_front(struct rolling_window *rw);
bool rolling_window_push_back(struct rolling_window *rw, int val, int *front);

/// Copy the contents of the rolling window to an array. The array is assumed to
/// have enough space to hold the contents of the rolling window.
static inline void attr_unused rolling_window_copy_to_array(struct rolling_window *rw,
                                                            int *arr) {
	// The length from head to the end of the array
	auto head_len = (size_t)(rw->window_size - rw->elem_head);
	if (head_len >= (size_t)rw->nelem) {
		memcpy(arr, rw->elem + rw->elem_head, sizeof(int) * (size_t)rw->nelem);
	} else {
		auto tail_len = (size_t)((size_t)rw->nelem - head_len);
		memcpy(arr, rw->elem + rw->elem_head, sizeof(int) * head_len);
		memcpy(arr + head_len, rw->elem, sizeof(int) * tail_len);
	}
}

struct rolling_max;

struct rolling_max *rolling_max_new(int capacity);
void rolling_max_destroy(struct rolling_max *rm);
void rolling_max_reset(struct rolling_max *rm);
void rolling_max_pop_front(struct rolling_max *rm, int front);
void rolling_max_push_back(struct rolling_max *rm, int val);
int rolling_max_get_max(struct rolling_max *rm);

/// Estimate the mean and variance of random variable X using Welford's online
/// algorithm.
struct cumulative_mean_and_var {
	double mean;
	double m2;
	unsigned int n;
};

static inline attr_unused void
cumulative_mean_and_var_init(struct cumulative_mean_and_var *cmv) {
	*cmv = (struct cumulative_mean_and_var){0};
}

static inline attr_unused void
cumulative_mean_and_var_update(struct cumulative_mean_and_var *cmv, double x) {
	if (cmv->n == UINT_MAX) {
		// We have too many elements, let's keep the mean and variance.
		return;
	}
	cmv->n++;
	double delta = x - cmv->mean;
	cmv->mean += delta / (double)cmv->n;
	cmv->m2 += delta * (x - cmv->mean);
}

static inline attr_unused double
cumulative_mean_and_var_get_var(struct cumulative_mean_and_var *cmv) {
	if (cmv->n < 2) {
		return 0;
	}
	return cmv->m2 / (double)(cmv->n - 1);
}

/// A naive quantile estimator.
///
/// Estimates the N-th percentile of a random variable X in a sliding window.
struct rolling_quantile {
	int current_rank;
	int min_target_rank, max_target_rank;
	int estimate;
	int capacity;
	int *tmp_buffer;
};

void rolling_quantile_init(struct rolling_quantile *rq, int capacity, int mink, int maxk);
void rolling_quantile_init_with_tolerance(struct rolling_quantile *rq, int window_size,
                                          double target, double tolerance);
void rolling_quantile_reset(struct rolling_quantile *rq);
void rolling_quantile_destroy(struct rolling_quantile *rq);
int rolling_quantile_estimate(struct rolling_quantile *rq, struct rolling_window *elements);
void rolling_quantile_push_back(struct rolling_quantile *rq, int x);
void rolling_quantile_pop_front(struct rolling_quantile *rq, int x);

struct render_statistics {
	/// Rolling window of rendering times (in us) and the tiers they belong to.
	/// We keep track of the tiers because the vblank time estimate can change over
	/// time.
	struct rolling_window render_times;
	/// Estimate the 95-th percentile of rendering times
	struct rolling_quantile render_time_quantile;
	/// Time between each vblanks
	struct cumulative_mean_and_var vblank_time_us;
};

void render_statistics_init(struct render_statistics *rs, int window_size);
void render_statistics_reset(struct render_statistics *rs);
void render_statistics_destroy(struct render_statistics *rs);

void render_statistics_add_vblank_time_sample(struct render_statistics *rs, int time_us);
void render_statistics_add_render_time_sample(struct render_statistics *rs, int time_us);

/// How much time budget we should give to the backend for rendering, in microseconds.
unsigned int render_statistics_get_budget(struct render_statistics *rs);

/// Return the measured vblank interval in microseconds. Returns 0 if not enough
/// samples have been collected yet.
unsigned int render_statistics_get_vblank_time(struct render_statistics *rs);
