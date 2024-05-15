// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <xcb/sync.h>
#include <xcb/xcb.h>

#include "backend/backend.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "types.h"
#include "win.h"
#include "x.h"

extern struct backend_operations xrender_ops, dummy_ops;
#ifdef CONFIG_OPENGL
extern struct backend_operations glx_ops;
extern struct backend_operations egl_ops;
#endif

struct backend_operations *backend_list[NUM_BKEND] = {
    [BKEND_XRENDER] = &xrender_ops,
    [BKEND_DUMMY] = &dummy_ops,
#ifdef CONFIG_OPENGL
    [BKEND_GLX] = &glx_ops,
    [BKEND_EGL] = &egl_ops,
#endif
};

void handle_device_reset(session_t *ps) {
	log_error("Device reset detected");
	// Wait for reset to complete
	// Although ideally the backend should return DEVICE_STATUS_NORMAL after a reset
	// is completed, it's not always possible.
	//
	// According to ARB_robustness (emphasis mine):
	//
	//     "If a reset status other than NO_ERROR is returned and subsequent
	//     calls return NO_ERROR, the context reset was encountered and
	//     completed. If a reset status is repeatedly returned, the context **may**
	//     be in the process of resetting."
	//
	//  Which means it may also not be in the process of resetting. For example on
	//  AMDGPU devices, Mesa OpenGL always return CONTEXT_RESET after a reset has
	//  started, completed or not.
	//
	//  So here we blindly wait 5 seconds and hope ourselves best of the luck.
	sleep(5);

	// Reset picom
	log_info("Resetting picom after device reset");
	ev_break(ps->loop, EVBREAK_ALL);
}

/// Execute a list of backend commands on the backend
/// @param target     the image to render into
/// @param root_image the image containing the desktop background
bool backend_execute(struct backend_base *backend, image_handle target, unsigned ncmds,
                     const struct backend_command cmds[ncmds]) {
	bool succeeded = true;
	for (auto cmd = &cmds[0]; succeeded && cmd != &cmds[ncmds]; cmd++) {
		switch (cmd->op) {
		case BACKEND_COMMAND_BLIT:
			if (!pixman_region32_not_empty(cmd->blit.target_mask)) {
				continue;
			}
			succeeded =
			    backend->ops->blit(backend, cmd->origin, target, &cmd->blit);
			break;
		case BACKEND_COMMAND_COPY_AREA:
			if (!pixman_region32_not_empty(cmd->copy_area.region)) {
				continue;
			}
			succeeded = backend->ops->copy_area(backend, cmd->origin, target,
			                                    cmd->copy_area.source_image,
			                                    cmd->copy_area.region);
			break;
		case BACKEND_COMMAND_BLUR:
			if (!pixman_region32_not_empty(cmd->blur.target_mask)) {
				continue;
			}
			succeeded =
			    backend->ops->blur(backend, cmd->origin, target, &cmd->blur);
			break;
		case BACKEND_COMMAND_INVALID:
		default: assert(false);
		}
	}
	return succeeded;
}

static inline const char *render_command_source_name(enum backend_command_source source) {
	switch (source) {
	case BACKEND_COMMAND_SOURCE_WINDOW: return "window";
	case BACKEND_COMMAND_SOURCE_SHADOW: return "shadow";
	case BACKEND_COMMAND_SOURCE_BACKGROUND: return "background";
	}
	unreachable();
}

void log_backend_command_(enum log_level level, const char *func,
                          const struct backend_command *cmd) {
	if (level < log_get_level_tls()) {
		return;
	}

	log_printf(tls_logger, level, func, "Render command: %p", cmd);
	switch (cmd->op) {
	case BACKEND_COMMAND_BLIT:
		log_printf(tls_logger, level, func, "blit %s%s",
		           render_command_source_name(cmd->source),
		           cmd->blit.source_mask != NULL ? ", with mask image" : "");
		log_printf(tls_logger, level, func, "origin: %d,%d", cmd->origin.x,
		           cmd->origin.y);
		log_printf(tls_logger, level, func, "mask region:");
		log_region_(level, func, cmd->blit.target_mask);
		log_printf(tls_logger, level, func, "opaque region:");
		log_region_(level, func, &cmd->opaque_region);
		break;
	case BACKEND_COMMAND_COPY_AREA:
		log_printf(tls_logger, level, func, "copy area from %s",
		           render_command_source_name(cmd->source));
		log_printf(tls_logger, level, func, "origin: %d,%d", cmd->origin.x,
		           cmd->origin.y);
		log_printf(tls_logger, level, func, "region:");
		log_region_(level, func, cmd->copy_area.region);
		break;
	case BACKEND_COMMAND_BLUR:
		log_printf(tls_logger, level, func, "blur%s",
		           cmd->blur.source_mask != NULL ? ", with mask image" : "");
		log_printf(tls_logger, level, func, "origin: %d,%d", cmd->origin.x,
		           cmd->origin.y);
		log_printf(tls_logger, level, func, "mask region:");
		log_region_(level, func, cmd->blur.target_mask);
		break;
	case BACKEND_COMMAND_INVALID:
		log_printf(tls_logger, level, func, "invalid");
		break;
	}
}

// vim: set noet sw=8 ts=8 :
