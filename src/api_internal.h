// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdint.h>

struct backend_base;
struct script_instance;

/// Invoke all backend plugins for the specified backend.
void api_backend_plugins_invoke(const char *backend_name, struct backend_base *backend);

/// Called at the start of each pass of picom's update loop. Advances the number plugins
/// see as `picom_animation_start::frame`.
void api_frame_begin(void);
/// Names of the animation variables plugins added, NULL terminated. NULL if there are
/// none. For `script_parse_config::inputs`.
const char *const *api_animation_variable_names(void);
/// Ask the plugins for the values of the animation variables `instance` reads, and store
/// them in it. Must be called before the instance is evaluated for the first time.
/// `trigger` is the name of the animation trigger, `window` and `client` are the animated
/// window and its client window, if any.
void api_animation_variables_fill(struct script_instance *instance, const char *trigger,
                                  uint32_t window, uint32_t client);
