// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <test.h>

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
	return __builtin_isnan(a);
}

#define CASESTRRET(s)                                                                    \
	case s: return #s

/// Same as assert(false), but make sure we abort _even in release builds_.
/// Silence compiler warning caused by release builds making some code paths reachable.
#define BUG()                                                                            \
	do {                                                                             \
		assert(false);                                                           \
		abort();                                                                 \
	} while (0)
#define CHECK_EXPR(...) ((void)0)
/// Same as assert, but evaluates the expression even in release builds
#define CHECK(expr)                                                                      \
	do {                                                                             \
		auto _ = (expr);                                                         \
		/* make sure the original expression appears in the assertion message */ \
		assert((CHECK_EXPR(expr), _));                                           \
		(void)_;                                                                 \
	} while (0)

/// Asserts that var is within [lower, upper]. Silence compiler warning about expressions
/// being always true or false.
#define ASSERT_IN_RANGE(var, lower, upper)                                               \
	do {                                                                             \
		auto __tmp = (var);                                                      \
		_Pragma("GCC diagnostic push");                                          \
		_Pragma("GCC diagnostic ignored \"-Wtype-limits\"");                     \
		assert(__tmp >= lower);                                                  \
		assert(__tmp <= upper);                                                  \
		_Pragma("GCC diagnostic pop");                                           \
	} while (0)

/// Asserts that var >= lower. Silence compiler warning about expressions
/// being always true or false.
#define ASSERT_GEQ(var, lower)                                                           \
	do {                                                                             \
		auto __tmp = (var);                                                      \
		_Pragma("GCC diagnostic push");                                          \
		_Pragma("GCC diagnostic ignored \"-Wtype-limits\"");                     \
		assert(__tmp >= lower);                                                  \
		_Pragma("GCC diagnostic pop");                                           \
	} while (0)

// Some macros for checked cast
// Note these macros are not complete, as in, they won't work for every integer types. But
// they are good enough for compton.

#define to_int_checked(val)                                                              \
	({                                                                               \
		int64_t tmp = (val);                                                     \
		ASSERT_IN_RANGE(tmp, INT_MIN, INT_MAX);                                  \
		(int)tmp;                                                                \
	})

#define to_char_checked(val)                                                             \
	({                                                                               \
		int64_t tmp = (val);                                                     \
		ASSERT_IN_RANGE(tmp, CHAR_MIN, CHAR_MAX);                                \
		(char)tmp;                                                               \
	})

#define to_u16_checked(val)                                                              \
	({                                                                               \
		auto tmp = (val);                                                        \
		ASSERT_IN_RANGE(tmp, 0, UINT16_MAX);                                     \
		(uint16_t) tmp;                                                          \
	})

#define to_i16_checked(val)                                                              \
	({                                                                               \
		int64_t tmp = (val);                                                     \
		ASSERT_IN_RANGE(tmp, INT16_MIN, INT16_MAX);                              \
		(int16_t) tmp;                                                           \
	})

#define to_u32_checked(val)                                                              \
	({                                                                               \
		auto tmp = (val);                                                        \
		int64_t max = UINT32_MAX; /* silence clang tautological                  \
		                                         comparison warning*/            \
		ASSERT_IN_RANGE(tmp, 0, max);                                            \
		(uint32_t) tmp;                                                          \
	})
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

#define min2(a, b) ((a) > (b) ? (b) : (a))
#define max2(a, b) ((a) > (b) ? (a) : (b))

/// clamp `val` into interval [min, max]
#define clamp(val, min, max) max2(min2(val, max), min)

static inline int attr_const popcountl(unsigned long a) {
	return __builtin_popcountl(a);
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

attr_noret void
report_allocation_failure(const char *func, const char *file, unsigned int line);

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
#define ccalloc(nmemb, type)                                                             \
	({                                                                               \
		auto tmp = (nmemb);                                                      \
		ASSERT_GEQ(tmp, 0);                                                      \
		((type *)allocchk(calloc((size_t)tmp, sizeof(type))));                   \
	})

/// @brief Wrapper of ealloc().
#define crealloc(ptr, nmemb)                                                               \
	({                                                                                 \
		auto tmp = (nmemb);                                                        \
		ASSERT_GEQ(tmp, 0);                                                        \
		((__typeof__(ptr))allocchk(realloc((ptr), (size_t)tmp * sizeof(*(ptr))))); \
	})

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
