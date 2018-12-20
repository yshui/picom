// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <assert.h>
#include <stdio.h>

#include "compiler.h"

enum log_level {
	LOG_LEVEL_TRACE = 0,
	LOG_LEVEL_DEBUG,
	LOG_LEVEL_INFO,
	LOG_LEVEL_WARN,
	LOG_LEVEL_ERROR,
	LOG_LEVEL_INVALID
};

#define LOG(level, x, ...)                                                               \
	log_printf(tls_logger, LOG_LEVEL_##level, __func__, x, ##__VA_ARGS__)
#define log_trace(x, ...) LOG(TRACE, x, ##__VA_ARGS__)
#define log_debug(x, ...) LOG(DEBUG, x, ##__VA_ARGS__)
#define log_info(x, ...) LOG(INFO, x, ##__VA_ARGS__)
#define log_warn(x, ...) LOG(WARN, x, ##__VA_ARGS__)
#define log_error(x, ...) LOG(ERROR, x, ##__VA_ARGS__)

/// Print out an error message.
#define printf_err(format, ...) log_error(format, ##__VA_ARGS__)

/// Print out an error message with function name.
#define printf_errf(format, ...) log_error(format, ##__VA_ARGS__)

/// Print out an error message with function name, and quit with a
/// specific exit code.
#define printf_errfq(code, format, ...)                                                  \
	{                                                                                \
		log_error(format, ##__VA_ARGS__);                                        \
		exit(code);                                                              \
	}

/// Print out a debug message.
#define printf_dbg(format, ...) log_debug(format, ##__VA_ARGS__)

/// Print out a debug message with function name.
#define printf_dbgf(format, ...) log_debug(format, ##__VA_ARGS__)

struct log;
struct log_target;

attr_printf(4, 5) void log_printf(struct log *, int level, const char *func,
                                  const char *fmt, ...);

attr_malloc struct log *log_new(void);
attr_nonnull_all void log_destroy(struct log *);
attr_nonnull(1) void log_set_level(struct log *l, int level);
attr_nonnull_all void log_add_target(struct log *, struct log_target *);
attr_const enum log_level string_to_log_level(const char *);

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

static inline void log_deinit_tls(void) {
	assert(tls_logger);
	log_destroy(tls_logger);
	tls_logger = NULL;
}

attr_malloc struct log_target *stderr_logger_new(void);
attr_malloc struct log_target *file_logger_new(const char *file);
attr_malloc struct log_target *null_logger_new(void);
attr_malloc struct log_target *glx_string_marker_logger_new(void);

// vim: set noet sw=8 ts=8:
