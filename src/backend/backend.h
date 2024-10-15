// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018, Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>

#include <picom/backend.h>

#include "log.h"

enum backend_command_op {
	BACKEND_COMMAND_INVALID = -1,
	BACKEND_COMMAND_BLIT,
	BACKEND_COMMAND_BLUR,
	BACKEND_COMMAND_COPY_AREA,
};

/// Symbolic references used as render command source images. The actual `image_handle`
/// will later be filled in by the renderer using this symbolic reference.
enum backend_command_source {
	BACKEND_COMMAND_SOURCE_WINDOW,
	BACKEND_COMMAND_SOURCE_WINDOW_SAVED,
	BACKEND_COMMAND_SOURCE_SHADOW,
	BACKEND_COMMAND_SOURCE_BACKGROUND,
};

// TODO(yshui) might need better names

struct backend_command {
	enum backend_command_op op;
	ivec2 origin;
	enum backend_command_source source;
	union {
		struct {
			struct backend_blit_args blit;
			/// Region of the screen that will be covered by this blit
			/// operations, in screen coordinates.
			region_t opaque_region;
		};
		struct {
			image_handle source_image;
			const region_t *region;
		} copy_area;
		struct backend_blur_args blur;
	};
	/// Source mask for the operation.
	/// If the `source_mask` of the operation's argument points to this, a mask image
	/// will be created for the operation for the renderer.
	struct backend_mask_image source_mask;
	/// Target mask for the operation.
	region_t target_mask;
};

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
