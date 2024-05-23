// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <inttypes.h>

#include <picom/api.h>
#include <picom/backend.h>

#include "compiler.h"
#include "log.h"

static struct picom_api picom_api;

PICOM_PUBLIC_API const struct picom_api *
picom_api_get_interfaces(uint64_t major, uint64_t minor, const char *context) {
	if (major != PICOM_API_MAJOR || minor > PICOM_API_MINOR) {
		log_error("Cannot provide API interfaces to %s, because the requested"
		          "version %" PRIu64 ".%" PRIu64 " is incompatible with our "
		          "%lu.%lu",
		          context, major, minor, PICOM_API_MAJOR, PICOM_API_MINOR);
		return NULL;
	}
	return &picom_api;
}
