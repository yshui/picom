// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018, Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>

#include <picom/backend.h>

#include "log.h"

bool backend_execute(struct backend_base *backend, image_handle target, unsigned ncmds,
                     const struct backend_command cmds[ncmds]);

struct backend_info *backend_find(const char *name);
struct backend_base *
backend_init(struct backend_info *info, session_t *ps, xcb_window_t target);
struct backend_info *backend_iter(void);
struct backend_info *backend_iter_next(struct backend_info *info);
const char *backend_name(struct backend_info *info);
bool backend_can_present(struct backend_info *info);
void log_backend_command_(enum log_level level, const char *func,
                          const struct backend_command *cmd);
#define log_backend_command(level, cmd)                                                  \
	log_backend_command_(LOG_LEVEL_##level, __func__, &(cmd));

/// Define a backend entry point. (Note constructor priority 202 is used here because 1xx
/// is reversed by test.h, and 201 is used for logging initialization.)
#define BACKEND_ENTRYPOINT(func) static void __attribute__((constructor(202))) func(void)
