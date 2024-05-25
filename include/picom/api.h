// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PICOM_API_MAJOR (0UL)
#define PICOM_API_MINOR (1UL)

struct backend_base;

/// The entry point of a backend plugin. Called after the backend is initialized.
typedef void (*picom_backend_plugin_entrypoint)(struct backend_base *backend, void *user_data);
struct picom_api {
	/// Add a plugin for a specific backend. The plugin's entry point will be called
	/// when the specified backend is initialized.
	///
	/// @param backend_name The name of the backend to add the plugin to.
	/// @param major        The major version of the backend API interface this plugin
	///                     is compatible with.
	/// @param minor        The minor version of the backend API interface this plugin
	///                     is compatible with.
	/// @param entrypoint   The entry point of the plugin.
	/// @param user_data    The user data to pass to the plugin's entry point.
	bool (*add_backend_plugin)(const char *backend_name, uint64_t major, uint64_t minor,
	                           picom_backend_plugin_entrypoint entrypoint,
	                           void *user_data);
};

const struct picom_api *
picom_api_get_interfaces(uint64_t major, uint64_t minor, const char *context);
