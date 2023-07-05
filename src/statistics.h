#pragma once

#include "utils.h"

#define NTIERS (3)

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
///
/// A `divisor` is also returned, indicating the target framerate. The divisor is
/// the number of vblanks we should wait between each frame. A divisor of 1 means
/// full framerate, 2 means half framerate, etc.
unsigned int
render_statistics_get_budget(struct render_statistics *rs, unsigned int *divisor);

/// Return the measured vblank interval in microseconds. Returns 0 if not enough
/// samples have been collected yet.
unsigned int render_statistics_get_vblank_time(struct render_statistics *rs);
