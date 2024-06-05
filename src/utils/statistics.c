// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

/// Rendering statistics
///
/// Tracks how long it takes to render a frame, for measuring performance, and for pacing
/// the frames.

#include <stdlib.h>

#include "log.h"
#include "misc.h"
#include "statistics.h"

void rolling_window_destroy(struct rolling_window *rw) {
	free(rw->elem);
	rw->elem = NULL;
}

void rolling_window_reset(struct rolling_window *rw) {
	rw->nelem = 0;
	rw->elem_head = 0;
}

void rolling_window_init(struct rolling_window *rw, int size) {
	rw->elem = ccalloc(size, int);
	rw->window_size = size;
	rolling_window_reset(rw);
}

int rolling_window_pop_front(struct rolling_window *rw) {
	assert(rw->nelem > 0);
	auto ret = rw->elem[rw->elem_head];
	rw->elem_head = (rw->elem_head + 1) % rw->window_size;
	rw->nelem--;
	return ret;
}

bool rolling_window_push_back(struct rolling_window *rw, int val, int *front) {
	bool full = rw->nelem == rw->window_size;
	if (full) {
		*front = rolling_window_pop_front(rw);
	}
	rw->elem[(rw->elem_head + rw->nelem) % rw->window_size] = val;
	rw->nelem++;
	return full;
}

/// Track the maximum member of a FIFO queue of integers. Integers are pushed to the back
/// and popped from the front, the maximum of the current members in the queue is
/// tracked.
struct rolling_max {
	/// A priority queue holding the indices of the maximum element candidates.
	/// The head of the queue is the index of the maximum element.
	/// The indices in the queue are the "original" indices.
	///
	/// There are only `capacity` elements in `elem`, all previous elements are
	/// discarded. But the discarded elements' indices are not forgotten, that's why
	/// it's called the "original" indices.
	int *p;
	int p_head, np;
	/// The maximum number of in flight elements.
	int capacity;
};

void rolling_max_destroy(struct rolling_max *rm) {
	free(rm->p);
	free(rm);
}

struct rolling_max *rolling_max_new(int capacity) {
	auto rm = ccalloc(1, struct rolling_max);
	if (!rm) {
		return NULL;
	}

	rm->p = ccalloc(capacity, int);
	if (!rm->p) {
		goto err;
	}
	rm->capacity = capacity;

	return rm;

err:
	rolling_max_destroy(rm);
	return NULL;
}

void rolling_max_reset(struct rolling_max *rm) {
	rm->p_head = 0;
	rm->np = 0;
}

#define IDX(n) ((n) % rm->capacity)
/// Remove the oldest element in the window. The caller must maintain the list of elements
/// themselves, i.e. the behavior is undefined if `front` does not 1match the oldest
/// element.
void rolling_max_pop_front(struct rolling_max *rm, int front) {
	if (rm->p[rm->p_head] == front) {
		// rm->p.pop_front()
		rm->p_head = IDX(rm->p_head + 1);
		rm->np--;
	}
}

void rolling_max_push_back(struct rolling_max *rm, int val) {
	// Update the priority queue.
	// Remove all elements smaller than the new element from the queue. Because
	// the new element will become the maximum element before them, and since they
	// come before the new element, they will have been popped before the new
	// element, so they will never become the maximum element.
	while (rm->np) {
		int p_tail = IDX(rm->p_head + rm->np - 1);
		if (rm->p[p_tail] > val) {
			break;
		}
		// rm->p.pop_back()
		rm->np--;
	}
	// Add the new element to the end of the queue.
	// rm->p.push_back(rm->start_index + rm->nelem - 1)
	assert(rm->np < rm->capacity);
	rm->p[IDX(rm->p_head + rm->np)] = val;
	rm->np++;
}
#undef IDX

int rolling_max_get_max(struct rolling_max *rm) {
	if (rm->np == 0) {
		return INT_MIN;
	}
	return rm->p[rm->p_head];
}

TEST_CASE(rolling_max_test) {
#define NELEM 15
	struct rolling_window queue;
	rolling_window_init(&queue, 3);
	auto rm = rolling_max_new(3);
	const int data[NELEM] = {1, 2, 3, 1, 4, 5, 2, 3, 6, 5, 4, 3, 2, 0, 0};
	const int expected_max[NELEM] = {1, 2, 3, 3, 4, 5, 5, 5, 6, 6, 6, 5, 4, 3, 2};
	int max[NELEM] = {0};
	for (int i = 0; i < NELEM; i++) {
		int front;
		bool full = rolling_window_push_back(&queue, data[i], &front);
		if (full) {
			rolling_max_pop_front(rm, front);
		}
		rolling_max_push_back(rm, data[i]);
		max[i] = rolling_max_get_max(rm);
	}
	rolling_window_destroy(&queue);
	rolling_max_destroy(rm);
	TEST_TRUE(memcmp(max, expected_max, sizeof(max)) == 0);
#undef NELEM
}

void rolling_quantile_init(struct rolling_quantile *rq, int capacity, int mink, int maxk) {
	*rq = (struct rolling_quantile){0};
	rq->tmp_buffer = malloc(sizeof(int) * (size_t)capacity);
	rq->capacity = capacity;
	rq->min_target_rank = mink;
	rq->max_target_rank = maxk;
}

void rolling_quantile_init_with_tolerance(struct rolling_quantile *rq, int window_size,
                                          double target, double tolerance) {
	rolling_quantile_init(rq, window_size, (int)((target - tolerance) * window_size),
	                      (int)((target + tolerance) * window_size));
}

void rolling_quantile_reset(struct rolling_quantile *rq) {
	rq->current_rank = 0;
	rq->estimate = 0;
}

void rolling_quantile_destroy(struct rolling_quantile *rq) {
	free(rq->tmp_buffer);
}

int rolling_quantile_estimate(struct rolling_quantile *rq, struct rolling_window *elements) {
	if (rq->current_rank < rq->min_target_rank || rq->current_rank > rq->max_target_rank) {
		if (elements->nelem != elements->window_size) {
			return INT_MIN;
		}
		// Re-estimate the quantile.
		assert(elements->nelem <= rq->capacity);
		rolling_window_copy_to_array(elements, rq->tmp_buffer);
		const int target_rank =
		    rq->min_target_rank + (rq->max_target_rank - rq->min_target_rank) / 2;
		rq->estimate = quickselect(rq->tmp_buffer, elements->nelem, target_rank);
		rq->current_rank = target_rank;
	}
	return rq->estimate;
}

void rolling_quantile_push_back(struct rolling_quantile *rq, int x) {
	if (x <= rq->estimate) {
		rq->current_rank++;
	}
}

void rolling_quantile_pop_front(struct rolling_quantile *rq, int x) {
	if (x <= rq->estimate) {
		rq->current_rank--;
	}
}

void render_statistics_init(struct render_statistics *rs, int window_size) {
	*rs = (struct render_statistics){0};

	rolling_window_init(&rs->render_times, window_size);
	rolling_quantile_init_with_tolerance(&rs->render_time_quantile, window_size,
	                                     /* q */ 0.98, /* tolerance */ 0.01);
}

void render_statistics_add_vblank_time_sample(struct render_statistics *rs, int time_us) {
	auto sample_sd = sqrt(cumulative_mean_and_var_get_var(&rs->vblank_time_us));
	auto current_estimate = render_statistics_get_vblank_time(rs);
	if (current_estimate != 0 && fabs((double)time_us - current_estimate) > sample_sd * 3) {
		// Deviated from the mean by more than 3 sigma (p < 0.003)
		log_debug("vblank time outlier: %d %f %f", time_us, rs->vblank_time_us.mean,
		          cumulative_mean_and_var_get_var(&rs->vblank_time_us));
		// An outlier sample, this could mean things like refresh rate changes, so
		// we reset the statistics. This could also be benign, but we like to be
		// cautious.
		cumulative_mean_and_var_init(&rs->vblank_time_us);
	}

	if (rs->vblank_time_us.mean != 0) {
		auto nframes_in_10_seconds =
		    (unsigned int)(10. * 1000000. / rs->vblank_time_us.mean);
		if (rs->vblank_time_us.n > 20 && rs->vblank_time_us.n > nframes_in_10_seconds) {
			// We collected 10 seconds worth of samples, we assume the
			// estimated refresh rate is stable. We will still reset the
			// statistics if we get an outlier sample though, see above.
			return;
		}
	}
	cumulative_mean_and_var_update(&rs->vblank_time_us, time_us);
}

void render_statistics_add_render_time_sample(struct render_statistics *rs, int time_us) {
	int oldest;
	if (rolling_window_push_back(&rs->render_times, time_us, &oldest)) {
		rolling_quantile_pop_front(&rs->render_time_quantile, oldest);
	}

	rolling_quantile_push_back(&rs->render_time_quantile, time_us);
}

/// How much time budget we should give to the backend for rendering, in microseconds.
unsigned int render_statistics_get_budget(struct render_statistics *rs) {
	if (rs->render_times.nelem < rs->render_times.window_size) {
		// No valid render time estimates yet. Assume maximum budget.
		return UINT_MAX;
	}

	// N-th percentile of render times, see render_statistics_init for N.
	auto render_time_percentile =
	    rolling_quantile_estimate(&rs->render_time_quantile, &rs->render_times);
	return (unsigned int)render_time_percentile;
}

unsigned int render_statistics_get_vblank_time(struct render_statistics *rs) {
	if (rs->vblank_time_us.n <= 20 || rs->vblank_time_us.mean < 100) {
		// Not enough samples yet, or the vblank time is too short to be
		// meaningful. Assume maximum budget.
		return 0;
	}
	return (unsigned int)rs->vblank_time_us.mean;
}

void render_statistics_reset(struct render_statistics *rs) {
	rolling_window_reset(&rs->render_times);
	rolling_quantile_reset(&rs->render_time_quantile);
	rs->vblank_time_us = (struct cumulative_mean_and_var){0};
}

void render_statistics_destroy(struct render_statistics *rs) {
	render_statistics_reset(rs);
	rolling_window_destroy(&rs->render_times);
	rolling_quantile_destroy(&rs->render_time_quantile);
}
