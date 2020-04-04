// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdbool.h>

typedef struct session session_t;

void print_diagnostics(session_t *, const char *config_file, bool compositor_running);
