// SPDX-License-Identifier: MIT
#pragma once

#ifdef UNIT_TEST

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct test_file_metadata;

struct test_failure {
	bool present;
	const char *message;
	const char *file;
	int line;
};

struct test_case_metadata {
	void (*fn)(struct test_case_metadata *, struct test_file_metadata *);
	struct test_failure failure;
	const char *name;
	struct test_case_metadata *next;
};

struct test_file_metadata {
	bool registered;
	const char *name;
	struct test_file_metadata *next;
	struct test_case_metadata *tests;
};

struct test_file_metadata __attribute__((weak)) * test_file_head;

#define SET_FAILURE(_message)                                                       \
	metadata->failure = (struct test_failure) {                                 \
		.message = _message, .file = __FILE__, .line = __LINE__,            \
		.present = true,                                                    \
	}

#define TEST_EQUAL(a, b)                                                            \
	do {                                                                        \
		if ((a) != (b)) {                                                   \
			SET_FAILURE(#a " != " #b);                                  \
			return;                                                     \
		}                                                                   \
	} while (0)

#define TEST_TRUE(a)                                                                \
	do {                                                                        \
		if (!(a)) {                                                         \
			SET_FAILURE(#a " is not true");                             \
			return;                                                     \
		}                                                                   \
	} while (0)

#define TEST_STREQUAL(a, b)                                                         \
	do {                                                                        \
		if (strcmp(a, b) != 0) {                                            \
			SET_FAILURE(#a " != " #b);                                  \
			return;                                                     \
		}                                                                   \
	} while (0)

#define TEST_CASE(_name)                                                            \
	static void __test_h_##_name(struct test_case_metadata *,                   \
	                             struct test_file_metadata *);                  \
	static struct test_file_metadata __test_h_file;                             \
	static struct test_case_metadata __test_h_meta_##_name = {                  \
	    .name = #_name,                                                         \
	    .fn = __test_h_##_name,                                                 \
	};                                                                          \
	static void __attribute__((constructor(101)))                               \
	    __test_h_##_name##_register(void) {                                     \
		__test_h_meta_##_name.next = __test_h_file.tests;                   \
		__test_h_file.tests = &__test_h_meta_##_name;                       \
		if (!__test_h_file.registered) {                                    \
			__test_h_file.name = __FILE__;                              \
			__test_h_file.next = test_file_head;                        \
			test_file_head = &__test_h_file;                            \
			__test_h_file.registered = true;                            \
		}                                                                   \
	}                                                                           \
	static void __test_h_##_name(                                               \
	    struct test_case_metadata *metadata __attribute__((unused)),            \
	    struct test_file_metadata *file_metadata __attribute__((unused)))

extern void __attribute__((weak)) (*test_h_unittest_setup)(void);
/// Run defined tests, return true if all tests succeeds
/// @param[out] tests_run if not NULL, set to whether tests were run
static inline void __attribute__((constructor(102))) run_tests(void) {
	bool should_run = false;
	FILE *cmdlinef = fopen("/proc/self/cmdline", "r");
	char *arg = NULL;
	int arglen;
	fscanf(cmdlinef, "%ms%n", &arg, &arglen);
	fclose(cmdlinef);
	for (char *pos = arg; pos < arg + arglen; pos += strlen(pos) + 1) {
		if (strcmp(pos, "--unittest") == 0) {
			should_run = true;
			break;
		}
	}
	free(arg);

	if (!should_run) {
		return;
	}

	if (&test_h_unittest_setup) {
		test_h_unittest_setup();
	}

	struct test_file_metadata *i = test_file_head;
	int failed = 0, success = 0;
	while (i) {
		fprintf(stderr, "Running tests from %s:\n", i->name);
		struct test_case_metadata *j = i->tests;
		while (j) {
			fprintf(stderr, "\t%s ... ", j->name);
			j->failure.present = false;
			j->fn(j, i);
			if (j->failure.present) {
				fprintf(stderr, "failed (%s at %s:%d)\n",
				        j->failure.message, j->failure.file,
				        j->failure.line);
				failed++;
			} else {
				fprintf(stderr, "passed\n");
				success++;
			}
			j = j->next;
		}
		fprintf(stderr, "\n");
		i = i->next;
	}
	int total = failed + success;
	fprintf(stderr, "Test results: passed %d/%d, failed %d/%d\n", success, total,
	        failed, total);
	exit(failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

#else

#include <stdbool.h>

#define TEST_CASE(name) static void __attribute__((unused)) __test_h_##name(void)

#define TEST_EQUAL(a, b)                                                            \
	(void)(a);                                                                  \
	(void)(b)
#define TEST_TRUE(a) (void)(a)
#define TEST_STREQUAL(a, b)                                                         \
	(void)(a);                                                                  \
	(void)(b)

#endif
