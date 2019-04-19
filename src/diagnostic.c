// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#include <stdio.h>
#include <xcb/xcb.h>

#include "backend/driver.h"
#include "diagnostic.h"
#include "config.h"
#include "common.h"

void print_diagnostics(session_t *ps, const char *config_file) {
	printf("**Version:** " COMPTON_VERSION "\n");
	//printf("**CFLAGS:** %s\n", "??");
	printf("\n### Extensions:\n\n");
	printf("* Shape: %s\n", ps->shape_exists ? "Yes" : "No");
	printf("* XRandR: %s\n", ps->randr_exists ? "Yes" : "No");
	printf("* Present: %s\n", ps->present_exists ? "Present" : "Not Present");
	printf("\n### Misc:\n\n");
	printf("* Use Overlay: %s\n", ps->overlay != XCB_NONE ? "Yes" : "No");
#ifdef __FAST_MATH__
	printf("* Fast Math: Yes\n");
#endif
	printf("* Config file used: %s\n", config_file ?: "None");
	printf("\n### Drivers (inaccurate):\n\n");
	print_drivers(ps->drivers);
}

// vim: set noet sw=8 ts=8 :
