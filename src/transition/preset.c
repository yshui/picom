// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <libconfig.h>
#include <stdbool.h>

#include "config.h"
#include "preset.h"
#include "script.h"

extern struct {
	const char *name;
	bool (*func)(struct win_script *output, config_setting_t *setting);
} win_script_presets[];

bool win_script_parse_preset(struct win_script *output, config_setting_t *setting) {
	const char *preset = NULL;
	if (!config_setting_lookup_string(setting, "preset", &preset)) {
		log_error("Missing preset name in script");
		return false;
	}
	for (unsigned i = 0; win_script_presets[i].name; i++) {
		if (strcmp(preset, win_script_presets[i].name) == 0) {
			log_debug("Using animation preset: %s", preset);
			return win_script_presets[i].func(output, setting);
		}
	}
	log_error("Unknown preset: %s", preset);
	return false;
}
