// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>

struct command_builder;
struct backend_command;
struct layout;
struct x_monitors;
struct win_option;
struct shader_info;

struct command_builder *command_builder_new(void);
void command_builder_free(struct command_builder *);

void command_builder_command_list_free(struct backend_command *cmds);

/// Generate render commands that need to be executed to render the current layout.
/// This function updates `layout->commands` with the list of generated commands, and also
/// the `number_of_commands` field of each of the layers in `layout`. The list  of
/// commands must later be freed with `command_builder_command_list_free`
/// It is guaranteed that each of the command's region of operation (e.g. the mask.region
/// argument of blit), will be store in `struct backend_command::mask`. This might not
/// stay true after further passes.
void command_builder_build(struct command_builder *cb, struct layout *layout,
                           bool force_blend, bool blur_frame, bool inactive_dim_fixed,
                           double max_brightness, const struct x_monitors *monitors,
                           const struct shader_info *shaders);
