// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <assert.h>
#include <stdio.h>

#include "compiler.h"

enum log_level {
	LOG_LEVEL_INVALID = -1,
	LOG_LEVEL_TRACE = 0,
	LOG_LEVEL_DEBUG,
	LOG_LEVEL_INFO,
	LOG_LEVEL_WARN,
	LOG_LEVEL_ERROR,
	LOG_LEVEL_FATAL,
};

#define LOG_UNLIKELY(level, x, ...)                                                            \
	do {                                                                                   \
		if (unlikely(LOG_LEVEL_##level >= log_get_level_tls())) {                      \
			log_printf(tls_logger, LOG_LEVEL_##level, __func__, x, ##__VA_ARGS__); \
		}                                                                              \
	} while (0)

#define LOG(level, x, ...)                                                                     \
	do {                                                                                   \
		if (LOG_LEVEL_##level >= log_get_level_tls()) {                                \
			log_printf(tls_logger, LOG_LEVEL_##level, __func__, x, ##__VA_ARGS__); \
		}                                                                              \
	} while (0)
#define log_trace(x, ...) LOG_UNLIKELY(TRACE, x, ##__VA_ARGS__)
#define log_debug(x, ...) LOG_UNLIKELY(DEBUG, x, ##__VA_ARGS__)
#define log_info(x, ...) LOG(INFO, x, ##__VA_ARGS__)
#define log_warn(x, ...) LOG(WARN, x, ##__VA_ARGS__)
#define log_error(x, ...) LOG(ERROR, x, ##__VA_ARGS__)
#define log_fatal(x, ...) LOG(FATAL, x, ##__VA_ARGS__)

#define log_error_errno(x, ...) LOG(ERROR, x ": %s", ##__VA_ARGS__, strerror(errno))

struct log;
struct log_target;

attr_printf(4, 5) void log_printf(struct log *, int level, const char *func,
                                  const char *fmt, ...);

attr_malloc struct log *log_new(void);
/// Destroy a log struct and every log target added to it
attr_nonnull_all void log_destroy(struct log *);
attr_nonnull(1) void log_set_level(struct log *l, int level);
attr_pure enum log_level log_get_level(const struct log *l);
attr_nonnull_all void log_add_target(struct log *, struct log_target *);
attr_pure enum log_level string_to_log_level(const char *);
/// Remove a previously added log target for a log struct, and destroy it. If the log
/// target was never added, nothing happens.
void log_remove_target(struct log *l, struct log_target *tgt);

extern thread_local struct log *tls_logger;

/// Create a thread local logger
static inline void log_init_tls(void) {
	tls_logger = log_new();
}
/// Set thread local logger log level
static inline void log_set_level_tls(int level) {
	assert(tls_logger);
	log_set_level(tls_logger, level);
}
static inline attr_nonnull_all void log_add_target_tls(struct log_target *tgt) {
	assert(tls_logger);
	log_add_target(tls_logger, tgt);
}

static inline attr_nonnull_all void log_remove_target_tls(struct log_target *tgt) {
	assert(tls_logger);
	log_remove_target(tls_logger, tgt);
}

static inline attr_pure enum log_level log_get_level_tls(void) {
	assert(tls_logger);
	return log_get_level(tls_logger);
}

static inline void log_deinit_tls(void) {
	assert(tls_logger);
	log_destroy(tls_logger);
	tls_logger = NULL;
}

attr_malloc struct log_target *stderr_logger_new(void);
attr_malloc struct log_target *file_logger_new(const char *file);
attr_malloc struct log_target *null_logger_new(void);
attr_malloc struct log_target *gl_string_marker_logger_new(void);

// vim: set noet sw=8 ts=8:
