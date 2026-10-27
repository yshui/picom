// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PICOM_API_MAJOR (0UL)
#define PICOM_API_MINOR (2UL)

struct backend_base;

/// The entry point of a backend plugin. Called after the backend is initialized.
typedef void (*picom_backend_plugin_entrypoint)(struct backend_base *backend, void *user_data);

/// An animation that is starting. Passed to animation variable providers, see
/// `add_animation_variable`. Only valid during the call. New fields are only added at
/// the end.
struct picom_animation_start {
	/// The animation trigger, by its name in picom(1): "open", "show", "hide" and
	/// so on, in lower case. The alias "geometry" arrives as "size" or "position".
	const char *trigger;
	/// The X window ids of the animated top-level window and of its client window.
	/// `client` is 0 if the window has no client window, or picom has not found it
	/// yet. For the "close" trigger the window is gone (destroyed, or reparented
	/// away), and `client` is 0.
	uint32_t window, client;
	/// Counts the passes of picom's update loop. All providers called in one pass
	/// see the same number, later passes get higher numbers, and a number is never
	/// reused, also not when picom reinitializes (SIGUSR1). Lets a plugin do shared
	/// work once per pass. Without vsync, passes can run more often than the display
	/// refreshes.
	uint64_t frame;
};

/// Returns the value of an animation variable for an animation that is starting. The
/// value must be a finite number. A NaN or infinite value fails the script, as any
/// expression with that result does: picom logs an error and skips the animation.
typedef double (*picom_animation_variable_provider)(const struct picom_animation_start *start,
                                                    void *user_data);

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
	/// Add a variable that animation scripts can read, like a context variable (see
	/// picom(1)). Scripts read it as `plugin`, '-' and `name`: plugin "plugin-foo"
	/// with name "bar" adds "plugin-foo-bar". When an animation whose script reads it
	/// starts, picom calls `provider` once, on the main thread, and the script sees
	/// that value until the animation ends. Scripts that don't read the variable
	/// never cause a call; a variable the script defines itself shadows it. Variables
	/// are looked up when scripts are compiled, after the plugins are loaded, so add
	/// them while the plugin loads; one added later is seen once picom reinitializes
	/// (SIGUSR1).
	///
	/// @param plugin    The plugin's name: "plugin-", then a letter, followed by
	///                  letters, digits, '-' or '_'. It keeps the plugin's variables
	///                  apart from picom's own, now and later, and from those of
	///                  other plugins.
	/// @param name      A letter followed by letters, digits, '-' or '_'.
	/// @param provider  Returns the value of the variable. Must be quick.
	/// @param user_data The user data to pass to `provider`.
	/// @return          Whether the variable was added. Fails for a NULL or invalid
	///                  plugin name or name, a NULL provider, or a variable that is
	///                  already added; the reason is logged.
	///
	/// Since API version 0.2.
	bool (*add_animation_variable)(const char *plugin, const char *name,
	                               picom_animation_variable_provider provider,
	                               void *user_data);
};

const struct picom_api *
picom_api_get_interfaces(uint64_t major, uint64_t minor, const char *context);
