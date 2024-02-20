// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2024 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <xcb/xcb.h>
#include "compiler.h"

#ifdef CONFIG_LIBCONFIG
int inspect_main(int argc, char **argv, const char *config_file);
#else
static inline int inspect_main(int argc attr_unused, char **argv attr_unused,
                               const char *config_file attr_unused) {
	return 0;
}
#endif
