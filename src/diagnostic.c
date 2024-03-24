// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#include <stdio.h>
#include <xcb/composite.h>
#include <xcb/xcb.h>

#include "backend/driver.h"
#include "common.h"
#include "config.h"
#include "diagnostic.h"
#include "picom.h"

void print_diagnostics(session_t *ps, const char *config_file, bool compositor_running) {
	printf("**Version:** " PICOM_VERSION "\n");
	// printf("**CFLAGS:** %s\n", "??");
	printf("\n### Extensions:\n\n");
	printf("* Shape: %s\n", session_has_shape_extension(ps) ? "Yes" : "No");
	printf("* RandR: %s\n", session_has_randr_extension(ps) ? "Yes" : "No");
	printf("* Present: %s\n", session_has_present_extension(ps) ? "Present" : "Not Present");
	printf("\n### Misc:\n\n");
	printf("* Use Overlay: %s\n", session_get_overlay(ps) != XCB_NONE ? "Yes" : "No");
	if (session_get_overlay(ps) == XCB_NONE) {
		if (compositor_running) {
			printf("  (Another compositor is already running)\n");
		} else if (session_redirection_mode(ps) != XCB_COMPOSITE_REDIRECT_MANUAL) {
			printf("  (Not in manual redirection mode)\n");
		} else {
			printf("\n");
		}
	}
#ifdef __FAST_MATH__
	printf("* Fast Math: Yes\n");
#endif
	printf("* Config file used: %s\n", config_file ?: "None");
	printf("\n### Drivers (inaccurate):\n\n");
	print_drivers(session_get_driver(ps));

	for (int i = 0; i < NUM_BKEND; i++) {
		if (backend_list[i] && backend_list[i]->diagnostics) {
			printf("\n### Backend: %s\n\n", BACKEND_STRS[i]);
			auto data = backend_list[i]->init(ps, session_get_target_window(ps));
			if (!data) {
				printf(" Cannot initialize this backend\n");
			} else {
				backend_list[i]->diagnostics(data);
				backend_list[i]->deinit(data);
			}
		}
	}
}

// vim: set noet sw=8 ts=8 :
