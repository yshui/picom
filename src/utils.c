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

	unreachable;
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

/// Track the rolling maximum of a stream of integers.
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

	/// The elemets
	int *elem;
	int elem_head, nelem;

	int window_size;
};

void rolling_max_destroy(struct rolling_max *rm) {
	free(rm->elem);
	free(rm->p);
	free(rm);
}

struct rolling_max *rolling_max_new(int size) {
	auto rm = ccalloc(1, struct rolling_max);
	if (!rm) {
		return NULL;
	}

	rm->p = ccalloc(size, int);
	rm->elem = ccalloc(size, int);
	rm->window_size = size;
	if (!rm->p || !rm->elem) {
		goto err;
	}

	return rm;

err:
	rolling_max_destroy(rm);
	return NULL;
}

void rolling_max_reset(struct rolling_max *rm) {
	rm->p_head = 0;
	rm->np = 0;
	rm->nelem = 0;
	rm->elem_head = 0;
}

void rolling_max_push(struct rolling_max *rm, int val) {
#define IDX(n) ((n) % rm->window_size)
	if (rm->nelem == rm->window_size) {
		auto old_head = rm->elem_head;
		// Discard the oldest element.
		// rm->elem.pop_front();
		rm->nelem--;
		rm->elem_head = IDX(rm->elem_head + 1);

		// Remove discarded element from the priority queue too.
		assert(rm->np);
		if (rm->p[rm->p_head] == old_head) {
			// rm->p.pop_front()
			rm->p_head = IDX(rm->p_head + 1);
			rm->np--;
		}
	}

	// Add the new element to the queue.
	// rm->elem.push_back(val)
	rm->elem[IDX(rm->elem_head + rm->nelem)] = val;
	rm->nelem++;

	// Update the prority queue.
	// Remove all elements smaller than the new element from the queue. Because
	// the new element will become the maximum element before them, and since they
	// come b1efore the new element, they will have been popped before the new
	// element, so they will never become the maximum element.
	while (rm->np) {
		int p_tail = IDX(rm->p_head + rm->np - 1);
		if (rm->elem[rm->p[p_tail]] > val) {
			break;
		}
		// rm->p.pop_back()
		rm->np--;
	}
	// Add the new element to the end of the queue.
	// rm->p.push_back(rm->start_index + rm->nelem - 1)
	rm->p[IDX(rm->p_head + rm->np)] = IDX(rm->elem_head + rm->nelem - 1);
	rm->np++;
#undef IDX
}

int rolling_max_get_max(struct rolling_max *rm) {
	if (rm->np == 0) {
		return INT_MIN;
	}
	return rm->elem[rm->p[rm->p_head]];
}

TEST_CASE(rolling_max_test) {
#define NELEM 15
	auto rm = rolling_max_new(3);
	const int data[NELEM] = {1, 2, 3, 1, 4, 5, 2, 3, 6, 5, 4, 3, 2, 0, 0};
	const int expected_max[NELEM] = {1, 2, 3, 3, 4, 5, 5, 5, 6, 6, 6, 5, 4, 3, 2};
	int max[NELEM] = {0};
	for (int i = 0; i < NELEM; i++) {
		rolling_max_push(rm, data[i]);
		max[i] = rolling_max_get_max(rm);
	}
	TEST_TRUE(memcmp(max, expected_max, sizeof(max)) == 0);
#undef NELEM
}

/// A rolling average of a stream of integers.
struct rolling_avg {
	/// The sum of the elements in the window.
	int64_t sum;

	/// The elements in the window.
	int *elem;
	int head, nelem;

	int window_size;
};

struct rolling_avg *rolling_avg_new(int size) {
	auto rm = ccalloc(1, struct rolling_avg);
	if (!rm) {
		return NULL;
	}

	rm->elem = ccalloc(size, int);
	rm->window_size = size;
	if (!rm->elem) {
		free(rm);
		return NULL;
	}

	return rm;
}

void rolling_avg_destroy(struct rolling_avg *rm) {
	free(rm->elem);
	free(rm);
}

void rolling_avg_reset(struct rolling_avg *ra) {
	ra->sum = 0;
	ra->nelem = 0;
	ra->head = 0;
}

void rolling_avg_push(struct rolling_avg *ra, int val) {
	if (ra->nelem == ra->window_size) {
		// Discard the oldest element.
		// rm->elem.pop_front();
		ra->sum -= ra->elem[ra->head % ra->window_size];
		ra->nelem--;
		ra->head = (ra->head + 1) % ra->window_size;
	}

	// Add the new element to the queue.
	// rm->elem.push_back(val)
	ra->elem[(ra->head + ra->nelem) % ra->window_size] = val;
	ra->sum += val;
	ra->nelem++;
}

double rolling_avg_get_avg(struct rolling_avg *ra) {
	if (ra->nelem == 0) {
		return 0;
	}
	return (double)ra->sum / (double)ra->nelem;
}

TEST_CASE(rolling_avg_test) {
#define NELEM 15
	auto rm = rolling_avg_new(3);
	const int data[NELEM] = {1, 2, 3, 1, 4, 5, 2, 3, 6, 5, 4, 3, 2, 0, 0};
	const double expected_avg[NELEM] = {
	    1,          1.5,        2, 2, 8.0 / 3.0, 10.0 / 3.0, 11.0 / 3.0, 10.0 / 3.0,
	    11.0 / 3.0, 14.0 / 3.0, 5, 4, 3,         5.0 / 3.0,  2.0 / 3.0};
	double avg[NELEM] = {0};
	for (int i = 0; i < NELEM; i++) {
		rolling_avg_push(rm, data[i]);
		avg[i] = rolling_avg_get_avg(rm);
	}
	for (int i = 0; i < NELEM; i++) {
		TEST_EQUAL(avg[i], expected_avg[i]);
	}
}

// vim: set noet sw=8 ts=8 :
