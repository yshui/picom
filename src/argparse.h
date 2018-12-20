// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

#include <stdbool.h>

#include "compiler.h"

typedef struct session session_t;

/**
 * Process arguments and configuration files.
 */
void get_cfg(session_t *ps, int argc, char *const *argv, bool first_pass);
