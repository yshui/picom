// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#include "diagnostic.h"
#include "common.h"

void print_diagnostics(session_t *ps) {
	printf("**Version:** " COMPTON_VERSION "\n");
	//printf("**CFLAGS:** %s\n", "??");
	printf("\n### Extensions:\n\n");
	printf("* Name Pixmap: %s\n", ps->has_name_pixmap ? "Yes" : "No");
	printf("* Shape: %s\n", ps->shape_exists ? "Yes" : "No");
	printf("* XRandR: %s\n", ps->randr_exists ? "Yes" : "No");
	printf("* Present: %s\n", ps->present_exists ? "Present" : "Not Present");
	printf("\n### Misc:\n\n");
	printf("* Use Overlay: %s\n", ps->overlay != XCB_NONE ? "Yes" : "No");
#ifdef __FAST_MATH__
	printf("* Fast Math: Yes\n");
#endif
}

// vim: set noet sw=8 ts=8 :
