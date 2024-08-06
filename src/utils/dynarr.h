// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stddef.h>

#include "misc.h"

/// Dynamic array implementation, design is similar to sds [0].
///
/// Note this is not very type safe, be sure to annotate wherever you use this. And the
/// array won't have a fixed address in memory.
///
/// [0]: https://github.com/antirez/sds

struct dynarr_header {
	size_t len;
	size_t cap;
};

static inline void *dynarr_new_impl(size_t size, size_t nelem) {
	struct dynarr_header *ret = calloc(1, sizeof(struct dynarr_header) + size * nelem);
	allocchk(ret);
	ret->len = 0;
	ret->cap = nelem;
	return ret + 1;
}

static inline void *dynarr_reserve_impl(size_t size, void *arr, size_t nelem) {
	struct dynarr_header *hdr = (struct dynarr_header *)arr - 1;
	if (hdr->len + nelem > hdr->cap) {
		hdr->cap = max2(max2(1, hdr->cap * 2), hdr->len + nelem);
		auto new_hdr = realloc(hdr, sizeof(struct dynarr_header) + hdr->cap * size);
		allocchk(new_hdr);
		hdr = new_hdr;
	}
	return hdr + 1;
}

static inline void *dynarr_shrink_to_fit_impl(size_t size, void *arr) {
	struct dynarr_header *hdr = (struct dynarr_header *)arr - 1;
	if (hdr->len < hdr->cap) {
		hdr->cap = hdr->len;
		auto new_hdr = realloc(hdr, sizeof(struct dynarr_header) + hdr->cap * size);
		allocchk(new_hdr);
		hdr = new_hdr;
	}
	return hdr + 1;
}

static inline void dynarr_remove_impl(size_t size, void *arr, size_t idx) {
	struct dynarr_header *hdr = (struct dynarr_header *)arr - 1;
	BUG_ON(idx >= hdr->len);
	memmove((char *)arr + idx * size, (char *)arr + (idx + 1) * size,
	        (hdr->len - idx - 1) * size);
	hdr->len--;
}

static inline void dynarr_remove_swap_impl(size_t size, void *arr, size_t idx) {
	struct dynarr_header *hdr = (struct dynarr_header *)arr - 1;
	BUG_ON(idx >= hdr->len);
	hdr->len--;
	if (idx != hdr->len) {
		memcpy((char *)arr + idx * size, (char *)arr + hdr->len * size, size);
	}
}

/// Create a new dynamic array with capacity `cap` for type `type`.
#define dynarr_new(type, cap) ((type *)dynarr_new_impl(sizeof(type), cap))
/// Free a dynamic array, destructing each element with `dtor`.
#define dynarr_free(arr, dtor)                                                           \
	do {                                                                             \
		dynarr_clear(arr, dtor);                                                 \
		free((struct dynarr_header *)(arr) - 1);                                 \
		(arr) = NULL;                                                            \
	} while (0)
#define dynarr_free_pod(arr) dynarr_free(arr, (void (*)(typeof(arr)))NULL)

/// Expand the capacity of the array so it can hold at least `nelem` more elements
#define dynarr_reserve(arr, nelem)                                                       \
	((arr) = dynarr_reserve_impl(sizeof(typeof(*(arr))), (void *)(arr), (nelem)))
/// Resize the array to `newlen`. If `newlen` is greater than the current length, the
/// new elements will be initialized with `init`. If `newlen` is less than the current
/// length, the excess elements will be destructed with `dtor`.
#define dynarr_resize(arr, newlen, init, dtor)                                           \
	do {                                                                             \
		BUG_ON((arr) == NULL);                                                   \
		dynarr_reserve((arr), (newlen) - dynarr_len(arr));                       \
		if ((init) != NULL) {                                                    \
			for (size_t i = dynarr_len(arr); i < (newlen); i++) {            \
				(init)((arr) + i);                                       \
			}                                                                \
		}                                                                        \
		if ((dtor) != NULL) {                                                    \
			for (size_t i = (newlen); i < dynarr_len(arr); i++) {            \
				(dtor)((arr) + i);                                       \
			}                                                                \
		}                                                                        \
		dynarr_len(arr) = (newlen);                                              \
	} while (0)
/// Resize the array to `newlen`, for types that can be trivially constructed and
/// destructed.
#define dynarr_resize_pod(arr, newlen)                                                   \
	dynarr_resize(arr, newlen, (void (*)(typeof(arr)))NULL, (void (*)(typeof(arr)))NULL)
/// Shrink the capacity of the array to its length.
#define dynarr_shrink_to_fit(arr)                                                        \
	((arr) = dynarr_shrink_to_fit_impl(sizeof(typeof(*(arr))), (void *)(arr)))
/// Push an element to the end of the array
#define dynarr_push(arr, item)                                                           \
	do {                                                                             \
		dynarr_reserve(arr, 1);                                                  \
		(arr)[dynarr_len(arr)++] = (item);                                       \
	} while (0)
/// Pop an element from the end of the array
#define dynarr_pop(arr) ((arr)[--dynarr_len(arr)])
/// Remove an element from the array by shifting the rest of the array forward.
#define dynarr_remove(arr, idx)                                                          \
	dynarr_remove_impl(sizeof(typeof(*(arr))), (void *)(arr), idx)
/// Remove an element from the array by swapping it with the last element.
#define dynarr_remove_swap(arr, idx)                                                     \
	dynarr_remove_swap_impl(sizeof(typeof(*(arr))), (void *)(arr), idx)
/// Return the length of the array
#define dynarr_len(arr) (((struct dynarr_header *)(arr) - 1)->len)
/// Return the capacity of the array
#define dynarr_cap(arr) (((struct dynarr_header *)(arr) - 1)->cap)
/// Return the last element of the array
#define dynarr_last(arr) ((arr)[dynarr_len(arr) - 1])
/// Return the pointer just past the last element of the array
#define dynarr_end(arr) ((arr) + dynarr_len(arr))
/// Return whether the array is empty
#define dynarr_is_empty(arr) (dynarr_len(arr) == 0)

/// Reduce the length of the array to `n`, destructing each element with `dtor`. If `n`
/// is greater than the current length, this does nothing.
#define dynarr_truncate(arr, n, dtor)                                                    \
	do {                                                                             \
		if ((dtor) != NULL) {                                                    \
			for (size_t i = n; i < dynarr_len(arr); i++) {                   \
				(dtor)((arr) + i);                                       \
			}                                                                \
		}                                                                        \
		dynarr_len(arr) = n;                                                     \
	} while (0)
#define dynarr_truncate_pod(arr, n) dynarr_truncate(arr, n, (void (*)(typeof(arr)))NULL)

/// Clear the array, destructing each element with `dtor`.
#define dynarr_clear(arr, dtor) dynarr_truncate(arr, 0, dtor)
#define dynarr_clear_pod(arr) dynarr_truncate_pod(arr, 0)

/// Extend the array by copying `n` elements from `other`
#define dynarr_extend_from(arr, other, n)                                                \
	do {                                                                             \
		if ((n) > 0) {                                                           \
			dynarr_reserve(arr, n);                                          \
			memcpy(dynarr_end(arr), other, sizeof(typeof(*(arr))[(n)]));     \
			dynarr_len(arr) += (n);                                          \
		}                                                                        \
	} while (0)
/// Extend the array by `n` elements, initializing them with `init`
#define dynarr_extend_with(arr, init, n)                                                 \
	do {                                                                             \
		if ((n) > 0) {                                                           \
			dynarr_resize(arr, dynarr_len(arr) + (n), init,                  \
			              (void (*)(typeof(arr)))NULL);                      \
		}                                                                        \
	} while (0)

#define dynarr_foreach(arr, i) for (typeof(arr)(i) = (arr); (i) < dynarr_end(arr); (i)++)
#define dynarr_foreach_rev(arr, i)                                                       \
	for (typeof(arr)(i) = dynarr_end(arr) - 1; (i) >= (arr); (i)--)

/// Find the index of the first appearance of an element in the array by using trivial
/// comparison, returns -1 if not found.
#define dynarr_find_pod(arr, needle)                                                     \
	({                                                                               \
		ptrdiff_t dynarr_find_ret = -1;                                          \
		dynarr_foreach(arr, dynarr_find_i) {                                     \
			if (*dynarr_find_i == (needle)) {                                \
				dynarr_find_ret = dynarr_find_i - (arr);                 \
				break;                                                   \
			}                                                                \
		}                                                                        \
		dynarr_find_ret;                                                         \
	})

/// Concatenate a dynarr of strings into a single string, separated by `sep`. The array
/// will be freed by this function.
char *dynarr_join(char **arr, const char *sep);
