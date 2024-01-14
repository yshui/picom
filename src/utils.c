#include <stdio.h>
#include <string.h>
#include <sys/uio.h>

#include "compiler.h"
#include "string_utils.h"
#include "test.h"
#include "utils.h"

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
	// Update the prority queue.
	// Remove all elements smaller than the new element from the queue. Because
	// the new element will become the maximum element before them, and since they
	// come b1efore the new element, they will have been popped before the new
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

// vim: set noet sw=8 ts=8 :
