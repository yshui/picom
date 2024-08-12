// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <inttypes.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>

#include <picom/types.h>

#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "renderer/layout.h"
#include "wm/win.h"
#include "x.h"

#include "backend.h"

static struct backend_info {
	UT_hash_handle hh;
	const char *name;
	struct backend_base *(*init)(session_t *ps, xcb_window_t target);
	bool can_present;
} *backend_registry = NULL;

PICOM_PUBLIC_API bool
backend_register(uint64_t major, uint64_t minor, const char *name,
                 struct backend_base *(*init)(session_t *ps, xcb_window_t target),
                 bool can_present) {
	if (major != PICOM_BACKEND_MAJOR) {
		log_error("Backend %s has incompatible major version %" PRIu64
		          ", expected %lu",
		          name, major, PICOM_BACKEND_MAJOR);
		return false;
	}
	if (minor > PICOM_BACKEND_MINOR) {
		log_error("Backend %s has incompatible minor version %" PRIu64
		          ", expected %lu",
		          name, minor, PICOM_BACKEND_MINOR);
		return false;
	}
	struct backend_info *info = NULL;
	HASH_FIND_STR(backend_registry, name, info);
	if (info) {
		log_error("Backend %s is already registered", name);
		return false;
	}

	info = cmalloc(struct backend_info);
	info->name = name;
	info->init = init;
	info->can_present = can_present;
	HASH_ADD_KEYPTR(hh, backend_registry, info->name, strlen(info->name), info);
	return true;
}

struct backend_info *backend_find(const char *name) {
	struct backend_info *info = NULL;
	HASH_FIND_STR(backend_registry, name, info);
	return info;
}

struct backend_base *
backend_init(struct backend_info *info, session_t *ps, xcb_window_t target) {
	return info->init(ps, target);
}

struct backend_info *backend_iter(void) {
	return backend_registry;
}

struct backend_info *backend_iter_next(struct backend_info *info) {
	return info->hh.next;
}

const char *backend_name(struct backend_info *info) {
	return info->name;
}

bool backend_can_present(struct backend_info *info) {
	return info->can_present;
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
			if (cmd->blit.opacity < 1. / MAX_ALPHA) {
				continue;
			}
			succeeded =
			    backend->ops.blit(backend, cmd->origin, target, &cmd->blit);
			break;
		case BACKEND_COMMAND_COPY_AREA:
			if (!pixman_region32_not_empty(cmd->copy_area.region)) {
				continue;
			}
			succeeded = backend->ops.copy_area(backend, cmd->origin, target,
			                                   cmd->copy_area.source_image,
			                                   cmd->copy_area.region);
			break;
		case BACKEND_COMMAND_BLUR:
			if (!pixman_region32_not_empty(cmd->blur.target_mask)) {
				continue;
			}
			succeeded =
			    backend->ops.blur(backend, cmd->origin, target, &cmd->blur);
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
	case BACKEND_COMMAND_SOURCE_WINDOW_SAVED: return "window_saved";
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
