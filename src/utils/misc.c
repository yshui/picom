// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>

#include "compiler.h"
#include "misc.h"
#include "rtkit.h"
#include "str.h"

/// Report allocation failure without allocating memory
void report_allocation_failure(const char *func, const char *file, unsigned int line) {
	// Since memory allocation failed, we try to print this error message without any
	// memory allocation. Since logging framework allocates memory (and might even
	// have not been initialized yet), so we can't use it.
	char buf[11];
	int llen = uitostr(line, buf);
	const char msg1[] = " has failed to allocate memory, ";
	const char msg2[] = ". Aborting...\n";
	const struct iovec v[] = {
	    {.iov_base = (void *)func, .iov_len = strlen(func)},
	    {.iov_base = "()", .iov_len = 2},
	    {.iov_base = (void *)msg1, .iov_len = sizeof(msg1) - 1},
	    {.iov_base = "at ", .iov_len = 3},
	    {.iov_base = (void *)file, .iov_len = strlen(file)},
	    {.iov_base = ":", .iov_len = 1},
	    {.iov_base = buf, .iov_len = (size_t)llen},
	    {.iov_base = (void *)msg2, .iov_len = sizeof(msg2) - 1},
	};

	ssize_t _ attr_unused = writev(STDERR_FILENO, v, ARR_SIZE(v));
	abort();

	unreachable();
}

///
/// Calculates next closest power of two of 32bit integer n
/// ref: https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
///
int next_power_of_two(int n) {
	n--;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	n |= n >> 16;
	n++;
	return n;
}
// Find the k-th smallest element in an array.
int quickselect(int *elems, int nelem, int k) {
	int l = 0, r = nelem;        // [l, r) is the range of candidates
	while (l != r) {
		int pivot = elems[l];
		int i = l, j = r;
		while (i < j) {
			while (i < j && elems[--j] >= pivot) {
			}
			elems[i] = elems[j];
			while (i < j && elems[++i] <= pivot) {
			}
			elems[j] = elems[i];
		}
		elems[i] = pivot;

		if (i == k) {
			break;
		}

		if (i < k) {
			l = i + 1;
		} else {
			r = i;
		}
	}
	return elems[k];
}

/// Switch to real-time scheduling policy (SCHED_RR) if possible
///
/// Make picom realtime to reduce latency, and make rendering times more predictable to
/// help pacing.
///
/// This requires the user to set up permissions for the real-time scheduling. e.g. by
/// setting `ulimit -r`, or giving us the CAP_SYS_NICE capability.
void set_rr_scheduling(void) {
	static thread_local bool already_tried = false;
	if (already_tried) {
		return;
	}
	already_tried = true;

	int priority = sched_get_priority_min(SCHED_RR);

	if (rtkit_make_realtime(0, priority)) {
		log_info("Set realtime priority to %d with rtkit.", priority);
		return;
	}

	// Fallback to use pthread_setschedparam
	struct sched_param param;
	int old_policy;
	int ret = pthread_getschedparam(pthread_self(), &old_policy, &param);
	if (ret != 0) {
		log_info("Couldn't get old scheduling priority.");
		return;
	}

	param.sched_priority = priority;

	ret = pthread_setschedparam(pthread_self(), SCHED_RR, &param);
	if (ret != 0) {
		log_info("Couldn't set real-time scheduling priority to %d.", priority);
		return;
	}

	log_info("Set real-time scheduling priority to %d.", priority);
}

// vim: set noet sw=8 ts=8 :
