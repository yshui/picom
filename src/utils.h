// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compiler.h"

#define ARR_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

#ifdef __FAST_MATH__
#warning Use of -ffast-math can cause rendering error or artifacts, \
  therefore it is not recommended.
#endif

#ifdef __clang__
__attribute__((optnone))
#else
__attribute__((optimize("-fno-fast-math")))
#endif
static inline bool
safe_isnan(double a) {
	return isnan(a);
}

typedef struct conv conv;

/**
 * Normalize an int value to a specific range.
 *
 * @param i int value to normalize
 * @param min minimal value
 * @param max maximum value
 * @return normalized value
 */
static inline int attr_const normalize_i_range(int i, int min, int max) {
	if (i > max)
		return max;
	if (i < min)
		return min;
	return i;
}

/**
 * Select the larger integer of two.
 */
static inline int attr_const max_i(int a, int b) {
	return (a > b ? a : b);
}

/**
 * Select the smaller integer of two.
 */
static inline int attr_const min_i(int a, int b) {
	return (a > b ? b : a);
}

/**
 * Select the larger long integer of two.
 */
static inline long attr_const max_l(long a, long b) {
	return (a > b ? a : b);
}

/**
 * Select the smaller long integer of two.
 */
static inline long attr_const min_l(long a, long b) {
	return (a > b ? b : a);
}

/**
 * Normalize a double value to a specific range.
 *
 * @param d double value to normalize
 * @param min minimal value
 * @param max maximum value
 * @return normalized value
 */
static inline double attr_const normalize_d_range(double d, double min, double max) {
	if (d > max)
		return max;
	if (d < min)
		return min;
	return d;
}

/**
 * Normalize a double value to 0.\ 0 - 1.\ 0.
 *
 * @param d double value to normalize
 * @return normalized value
 */
static inline double attr_const normalize_d(double d) {
	return normalize_d_range(d, 0.0, 1.0);
}

attr_noret void report_allocation_failure(const char *func, const char *file,
                                          unsigned int line);

/**
 * @brief Quit if the passed-in pointer is empty.
 */
static inline void *
allocchk_(const char *func_name, const char *file, unsigned int line, void *ptr) {
	if (unlikely(!ptr)) {
		report_allocation_failure(func_name, file, line);
	}
	return ptr;
}

/// @brief Wrapper of allocchk_().
#define allocchk(ptr) allocchk_(__func__, __FILE__, __LINE__, ptr)

/// @brief Wrapper of malloc().
#define cmalloc(type) ((type *)allocchk(malloc(sizeof(type))))

/// @brief Wrapper of malloc() that takes a size
#define cvalloc(size) allocchk(malloc(size))

/// @brief Wrapper of calloc().
#define ccalloc(nmemb, type) ((type *)allocchk(calloc((nmemb), sizeof(type))))

/// @brief Wrapper of ealloc().
#define crealloc(ptr, nmemb)                                                             \
	((__typeof__(ptr))allocchk(realloc((ptr), (nmemb) * sizeof(*(ptr)))))

/// RC_TYPE generates a reference counted type from `type`
///
/// parameters:
///   name = the generated type will be called `name`_t.
///   ctor = the constructor of `type`, will be called when
///          a value of `type` is created. should take one
///          argument of `type *`.
///   dtor = the destructor. will be called when all reference
///          is gone. has same signature as ctor
///   Q    = function qualifier. this is the qualifier that
///          will be put before generated functions
//
/// functions generated:
///   `name`_new:   create a new reference counted object of `type`
///   `name`_ref:   increment the reference counter, return a
///                 reference to the object
///   `name`_unref: decrement the reference counter. take a `type **`
///                 because it needs to nullify the reference.
#define RC_TYPE(type, name, ctor, dtor, Q)                                               \
	typedef struct {                                                                 \
		type inner;                                                              \
		int ref_count;                                                           \
	} name##_internal_t;                                                             \
	typedef type name##_t;                                                           \
	Q type *name##_new(void) {                                                       \
		name##_internal_t *ret = cmalloc(name##_internal_t);                     \
		ctor((type *)ret);                                                       \
		ret->ref_count = 1;                                                      \
		return (type *)ret;                                                      \
	}                                                                                \
	Q type *name##_ref(type *a) {                                                    \
		__auto_type b = (name##_internal_t *)a;                                  \
		b->ref_count++;                                                          \
		return a;                                                                \
	}                                                                                \
	Q void name##_unref(type **a) {                                                  \
		__auto_type b = (name##_internal_t *)*a;                                 \
		if (!b)                                                                  \
			return;                                                          \
		b->ref_count--;                                                          \
		if (!b->ref_count) {                                                     \
			dtor((type *)b);                                                 \
			free(b);                                                         \
		}                                                                        \
		*a = NULL;                                                               \
	}

/// Generate prototypes for functions generated by RC_TYPE
#define RC_TYPE_PROTO(type, name)                                                        \
	typedef type name##_t;                                                           \
	type *name##_new(void);                                                          \
	void name##_ref(type *a);                                                        \
	void name##_unref(type **a);

// vim: set noet sw=8 ts=8 :
