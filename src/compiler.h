// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#ifdef HAS_STDC_PREDEF_H
#include <stdc-predef.h>
#endif

// clang-format off
#if __STDC_VERSION__ <= 201710L
// Polyfill for C23's `auto` and `typeof`
# define auto           __auto_type
# define typeof         __typeof__
#endif
#define likely(x)      __builtin_expect(!!(x), 1)
#define unlikely(x)    __builtin_expect(!!(x), 0)
#define likely_if(x)   if (likely(x))
#define unlikely_if(x) if (unlikely(x))

#ifndef __has_attribute
# if __GNUC__ >= 4
#  define __has_attribute(x) 1
# else
#  define __has_attribute(x) 0
# endif
#endif

#if __has_attribute(const)
# define attr_const __attribute__((const))
#else
# define attr_const
#endif

#if __has_attribute(format)
# define attr_printf(a, b) __attribute__((format(printf, a, b)))
#else
# define attr_printf(a, b)
#endif

#if __has_attribute(pure)
# define attr_pure __attribute__((pure))
#else
# define attr_pure
#endif

#if __has_attribute(unused)
# define attr_unused __attribute__((unused))
#else
# define attr_unused
#endif

#if __has_attribute(warn_unused_result)
# define attr_warn_unused_result __attribute__((warn_unused_result))
#else
# define attr_warn_unused_result
#endif
// An alias for convenience
#define must_use attr_warn_unused_result

#if __has_attribute(nonnull)
# define attr_nonnull(...) __attribute__((nonnull(__VA_ARGS__)))
# define attr_nonnull_all __attribute__((nonnull))
#else
# define attr_nonnull(...)
# define attr_nonnull_all
#endif

#if __has_attribute(returns_nonnull)
# define attr_ret_nonnull __attribute__((returns_nonnull))
#else
# define attr_ret_nonnull
#endif

#if __has_attribute(deprecated)
# define attr_deprecated __attribute__((deprecated))
#else
# define attr_deprecated
#endif

#if __has_attribute(malloc)
# define attr_malloc __attribute__((malloc))
#else
# define attr_malloc
#endif

#if __has_attribute(fallthrough)
# define fallthrough() __attribute__((fallthrough))
#else
# define fallthrough()
#endif

#if __has_attribute(cleanup)
# define cleanup(func) __attribute__((cleanup(func)))
#else
# error "Compiler is missing cleanup attribute"
#endif

#if __STDC_VERSION__ >= 201112L
# define attr_noret _Noreturn
#else
# if __has_attribute(noreturn)
#  define attr_noret __attribute__((noreturn))
# else
#  define attr_noret
# endif
#endif

#ifndef unreachable
# if defined(__GNUC__) || defined(__clang__)
#  define unreachable() assert(false); __builtin_unreachable()
# else
#  define unreachable() assert(false); do {} while(0)
# endif
#endif

#ifndef __has_include
# define __has_include(x) 0
#endif

#ifndef __has_builtin
# define __has_builtin(x) 0
#endif

#if !defined(__STDC_NO_THREADS__) && __has_include(<threads.h>)
# include <threads.h>
#elif __STDC_VERSION__ >= 201112L
# define thread_local _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
# define thread_local __thread
#else
# define thread_local _Pragma("GCC error \"No thread local storage support\"") __error__
#endif

#define PICOM_PUBLIC_API __attribute__((visibility("default")))
// clang-format on

typedef unsigned long ulong;
typedef unsigned int uint;

static inline int attr_const popcntul(unsigned long a) {
	return __builtin_popcountl(a);
}

/// Get the index of the lowest bit set in a number. The result is undefined if
/// `a` is 0.
static inline int attr_const index_of_lowest_one(unsigned a) {
#if __has_builtin(__builtin_ctz)
	return __builtin_ctz(a);
#else
	auto lowbit = (a & -a);
	int r = (lowbit & 0xAAAAAAAA) != 0;
	r |= ((lowbit & 0xCCCCCCCC) != 0) << 1;
	r |= ((lowbit & 0xF0F0F0F0) != 0) << 2;
	r |= ((lowbit & 0xFF00FF00) != 0) << 3;
	r |= ((lowbit & 0xFFFF0000) != 0) << 4;
	return r;
#endif
}
