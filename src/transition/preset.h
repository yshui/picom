// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>

typedef struct config_setting_t config_setting_t;
struct win_script;

/// Parse a animation preset definition into a win_script.
bool win_script_parse_preset(struct win_script *output, config_setting_t *setting);
