// SPDX-License-Identifier: MIT
#pragma once

#ifdef UNIT_TEST

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

struct test_case_metadata {
	void (*fn)(struct test_case_metadata *);
	bool success;
	struct test_case_metadata *next;
};

struct test_case_metadata __attribute__((weak)) *test_case_head;

static inline void report_failure(const char *message, const char *file, int line) {
	fprintf(stderr, "Test failed, %s at %s:%d\n", message, file, line);
}

#define SHOULD_EQUAL(a, b) do { \
	if ((a) != (b)) { \
		report_failure(#a " != " #b, __FILE__, __LINE__); \
		metadata->success = false; \
		return; \
	} \
} while(0)

#define TEST_CASE(name) \
static void __test_h_##name(struct test_case_metadata *); \
static void __attribute__((constructor)) __test_h_##name##_register(void) { \
	struct test_case_metadata *t = malloc(sizeof(*t)); \
	t->fn = __test_h_##name; \
	t->next = test_case_head; \
	test_case_head = t; \
} \
static void __test_h_##name(struct test_case_metadata *metadata)

static inline void run_tests(void) {
	struct test_case_metadata *i = test_case_head;
	int failed = 0;
	while(i) {
		i->success = true;
		i->fn(i);
		if (!i->success) {
			failed++;
		}
		i = i->next;
	}
}

#else

#define TEST_CASE(name) \
static void __attribute__((unused)) __test_h_##name(void)

#define SHOULD_EQUAL(a, b)

#endif
