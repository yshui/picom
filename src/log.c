#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_OPENGL
#include <GL/glx.h>
#endif

#include "compiler.h"
#include "log.h"
#include "utils.h"

thread_local struct log *tls_logger;

struct log_target;

struct log {
	struct log_target *head;

	int log_level;
};

struct log_target {
	const struct log_ops *ops;
	struct log_target *next;
};

struct log_ops {
	void (*write)(struct log_target *, const char *, size_t);
	void (*destroy)(struct log_target *);

	/// Additional strings to print around the log_level string
	const char *(*colorize_begin)(enum log_level);
	const char *(*colorize_end)(enum log_level);
};

/// Helper function for writing null terminated strings
static void log_strwrite(struct log_target *tgt, const char *str) {
	return tgt->ops->write(tgt, str, strlen(str));
}

static attr_const const char *log_level_to_string(enum log_level level) {
	switch (level) {
	case LOG_LEVEL_TRACE: return "TRACE";
	case LOG_LEVEL_DEBUG: return "DEBUG";
	case LOG_LEVEL_INFO: return "INFO";
	case LOG_LEVEL_WARN: return "WARN";
	case LOG_LEVEL_ERROR: return "ERROR";
	case LOG_LEVEL_FATAL: return "FATAL ERROR";
	default: assert(false);
	}
}

enum log_level string_to_log_level(const char *str) {
	if (strcasecmp(str, "TRACE") == 0)
		return LOG_LEVEL_TRACE;
	else if (strcasecmp(str, "DEBUG") == 0)
		return LOG_LEVEL_DEBUG;
	else if (strcasecmp(str, "INFO") == 0)
		return LOG_LEVEL_INFO;
	else if (strcasecmp(str, "WARN") == 0)
		return LOG_LEVEL_WARN;
	else if (strcasecmp(str, "ERROR") == 0)
		return LOG_LEVEL_ERROR;
	return LOG_LEVEL_INVALID;
}

struct log *log_new(void) {
	auto ret = cmalloc(struct log);
	ret->log_level = LOG_LEVEL_WARN;
	ret->head = NULL;
	return ret;
}

void log_add_target(struct log *l, struct log_target *tgt) {
	tgt->next = l->head;
	l->head = tgt;
}

/// Destroy a log struct
void log_destroy(struct log *l) {
	// free all tgt
	struct log_target *head = l->head;
	while (head) {
		auto next = head->next;
		head->ops->destroy(head);
		head = next;
	}
	free(l);
}

void log_set_level(struct log *l, int level) {
	assert(level <= LOG_LEVEL_FATAL && level >= 0);
	l->log_level = level;
}

attr_printf(4, 5) void log_printf(struct log *l, int level, const char *func,
                                  const char *fmt, ...) {
	assert(level <= LOG_LEVEL_FATAL && level >= 0);
	if (level < l->log_level)
		return;

	char *buf = NULL;
	va_list args;

	va_start(args, fmt);
	size_t len = vasprintf(&buf, fmt, args);
	va_end(args);

	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	auto tm = localtime(&ts.tv_sec);
	char time_buf[100];
	strftime(time_buf, sizeof time_buf, "%x %T", tm);

	if (!buf)
		return;

	const char *log_level_str = log_level_to_string(level);
	char *common = NULL;
	size_t plen = asprintf(&common, "[ %s.%03ld %s %s ] ", time_buf,
	                       ts.tv_nsec / 1000000, func, log_level_str);
	if (!common)
		return;

	common = crealloc(common, plen + len + 2);
	strcpy(common + plen, buf);
	strcpy(common + plen + len, "\n");

	struct log_target *head = l->head;
	while (head) {
		if (head->ops->colorize_begin) {
			// construct target specific prefix
			const char *p = head->ops->colorize_begin(level);
			const char *s = "";
			if (head->ops->colorize_end)
				s = head->ops->colorize_end(level);
			char *str = NULL;
			size_t plen2 =
			    asprintf(&str, "[ %s.%03ld %s %s%s%s ] ", time_buf,
			             ts.tv_nsec / 1000000, func, p, log_level_str, s);
			if (!str) {
				log_strwrite(head, common);
				continue;
			}
			str = crealloc(str, plen2 + len + 2);
			strcpy(str + plen2, buf);
			strcpy(str + plen2 + len, "\n");
			log_strwrite(head, str);
			free(str);
		} else {
			log_strwrite(head, common);
		}
		head = head->next;
	}
	free(common);
}

/// A trivial deinitializer that simply frees the memory
static attr_unused void logger_trivial_destroy(struct log_target *tgt) {
	free(tgt);
}

/// A null log target that does nothing
static const struct log_ops null_logger_ops;
static struct log_target null_logger_target = {
    .ops = &null_logger_ops,
};

struct log_target *null_logger_new(void) {
	return &null_logger_target;
}

static void null_logger_write(struct log_target *tgt, const char *str, size_t len) {
	return;
}

static const struct log_ops null_logger_ops = {
    .write = null_logger_write,
};

/// A file based logger that writes to file (or stdout/stderr)
struct file_logger {
	struct log_target tgt;
	FILE *f;
	struct log_ops ops;
};

void file_logger_write(struct log_target *tgt, const char *str, size_t len) {
	auto f = (struct file_logger *)tgt;
	fwrite(str, 1, len, f->f);
}

void file_logger_destroy(struct log_target *tgt) {
	auto f = (struct file_logger *)tgt;
	fclose(f->f);
	free(tgt);
}

#define ANSI(x) "\033[" x "m"
const char *terminal_colorize_begin(enum log_level level) {
	switch (level) {
	case LOG_LEVEL_TRACE: return ANSI("30;2");
	case LOG_LEVEL_DEBUG: return ANSI("37;2");
	case LOG_LEVEL_INFO: return ANSI("92");
	case LOG_LEVEL_WARN: return ANSI("33");
	case LOG_LEVEL_ERROR: return ANSI("31;1");
	case LOG_LEVEL_FATAL: return ANSI("30;103;1");
	default: assert(false);
	}
}

const char *terminal_colorize_end(enum log_level level) {
	return ANSI("0");
}
#undef PREFIX

static const struct log_ops file_logger_ops = {
    .write = file_logger_write,
    .destroy = file_logger_destroy,
};

struct log_target *file_logger_new(const char *filename) {
	FILE *f = fopen(filename, "w+");
	if (!f) {
		return NULL;
	}

	auto ret = cmalloc(struct file_logger);
	ret->tgt.ops = &ret->ops;
	ret->f = f;

	// Always assume a file is not a terminal
	ret->ops = file_logger_ops;

	return &ret->tgt;
}

struct log_target *stderr_logger_new(void) {
	int fd = dup(STDERR_FILENO);
	if (fd < 0) {
		return NULL;
	}

	FILE *f = fdopen(fd, "w");
	if (!f) {
		return NULL;
	}

	auto ret = cmalloc(struct file_logger);
	ret->tgt.ops = &ret->ops;
	ret->f = f;
	ret->ops = file_logger_ops;

	if (isatty(fd)) {
		ret->ops.colorize_begin = terminal_colorize_begin;
		ret->ops.colorize_end = terminal_colorize_end;
	}
	return &ret->tgt;
}

#ifdef CONFIG_OPENGL
/// An opengl logger that can be used for logging into opengl debugging tools,
/// such as apitrace
struct glx_string_marker_logger {
	struct log_target tgt;
	void (*glx_string_marker)(GLsizei len, const char *);
};

void glx_string_marker_logger_write(struct log_target *tgt, const char *str, size_t len) {
	auto g = (struct glx_string_marker_logger *)tgt;
	g->glx_string_marker(len, str);
}

static const struct log_ops glx_string_marker_logger_ops = {
    .write = glx_string_marker_logger_write,
    .destroy = logger_trivial_destroy,
};

struct log_target *glx_string_marker_logger_new(void) {
	void *fnptr = glXGetProcAddress((GLubyte *)"glStringMarkerGREMEDY");
	if (!fnptr)
		return NULL;

	auto ret = cmalloc(struct glx_string_marker_logger);
	ret->tgt.ops = &glx_string_marker_logger_ops;
	ret->glx_string_marker = fnptr;
	return &ret->tgt;
}

#else
struct log_target *glx_string_marker_logger_new(void) {
	return null_logger_new();
}
#endif

// vim: set noet sw=8 ts=8:
