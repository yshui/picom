// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#include <stdio.h>
#include <xcb/composite.h>
#include <xcb/xcb.h>

#include "backend/backend.h"
#include "backend/driver.h"
#include "common.h"
#include "config.h"
#include "diagnostic.h"
#include "picom.h"

void print_diagnostics(session_t *ps, const char *config_file, bool compositor_running) {
	printf("**Version:** " PICOM_FULL_VERSION "\n");
	// printf("**CFLAGS:** %s\n", "??");
	printf("\n### Extensions:\n\n");
	printf("* Shape: %s\n", ps->shape_exists ? "Yes" : "No");
	printf("* RandR: %s\n", ps->randr_exists ? "Yes" : "No");
	printf("* Present: %s\n", ps->present_exists ? "Present" : "Not Present");
	printf("\n### Misc:\n\n");
	printf("* Use Overlay: %s\n", ps->overlay != XCB_NONE ? "Yes" : "No");
	if (ps->overlay == XCB_NONE) {
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
	printf("* Config file specified: %s\n", config_file ?: "None");
	printf("* Config file used: %s\n", ps->o.config_file_path ?: "None");
	if (!list_is_empty(&ps->o.included_config_files)) {
		printf("* Included config files:\n");
		list_foreach(struct included_config_file, i, &ps->o.included_config_files,
		             siblings) {
			printf("  - %s\n", i->path);
		}
	}
	printf("\n### Drivers (inaccurate):\n\n");
	print_drivers(ps->drivers);

	for (auto i = backend_iter(); i; i = backend_iter_next(i)) {
		auto backend_data = backend_init(i, ps, session_get_target_window(ps));
		if (!backend_data) {
			printf(" Cannot initialize backend %s\n", backend_name(i));
			continue;
		}
		if (backend_data->ops.diagnostics) {
			printf("\n### Backend: %s\n\n", backend_name(i));
			backend_data->ops.diagnostics(backend_data);
		}
		backend_data->ops.deinit(backend_data);
	}
}

// vim: set noet sw=8 ts=8 :
