//! Rendering statistics
//!
//! Tracks how long it takes to render a frame, for measuring performance, and for pacing
//! the frames.

#include "statistics.h"
#include "log.h"
#include "utils.h"

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
///
/// A `divisor` is also returned, indicating the target framerate. The divisor is
/// the number of vblanks we should wait between each frame. A divisor of 1 means
/// full framerate, 2 means half framerate, etc.
unsigned int
render_statistics_get_budget(struct render_statistics *rs, unsigned int *divisor) {
	if (rs->render_times.nelem < rs->render_times.window_size) {
		// No valid render time estimates yet. Assume maximum budget.
		*divisor = 1;
		return UINT_MAX;
	}

	// N-th percentile of render times, see render_statistics_init for N.
	auto render_time_percentile =
	    rolling_quantile_estimate(&rs->render_time_quantile, &rs->render_times);
	auto vblank_time_us = render_statistics_get_vblank_time(rs);
	if (vblank_time_us == 0) {
		// We don't have a good estimate of the vblank time yet, so we
		// assume we can finish in one vblank.
		*divisor = 1;
	} else {
		*divisor =
		    (unsigned int)(render_time_percentile / rs->vblank_time_us.mean + 1);
	}
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
