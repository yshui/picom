// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

#include <stdbool.h>

#include "compiler.h"

typedef struct session session_t;

/// Get config options that are needed to parse the rest of the options
/// Return true if we should quit
bool get_early_config(int argc, char *const *argv, char **config_file, bool *all_xerrors,
                      int *exit_code);

/**
 * Process arguments and configuration files.
 */
void get_cfg(session_t *ps, int argc, char *const *argv);
