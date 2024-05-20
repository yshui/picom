// SPDX-License-Identifier: MIT
// Copyright (c) 2012-2014 Richard Grenville <pyxlcy@gmail.com>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libconfig.h>
#include <libgen.h>

#include "c2.h"
#include "common.h"
#include "config.h"
#include "err.h"
#include "log.h"
#include "script.h"
#include "string_utils.h"
#include "utils.h"
#include "win.h"

#pragma GCC diagnostic error "-Wunused-parameter"

/**
 * Wrapper of libconfig's <code>config_lookup_int</code>.
 *
 * So it takes a pointer to bool.
 */
static inline int lcfg_lookup_bool(const config_t *config, const char *path, bool *value) {
	int ival;

	int ret = config_lookup_bool(config, path, &ival);
	if (ret) {
		*value = ival;
	}

	return ret;
}

/// Search for config file under a base directory
FILE *open_config_file_at(const char *base, char **out_path) {
	static const char *config_paths[] = {"/picom.conf", "/picom/picom.conf",
	                                     "/compton.conf", "/compton/compton.conf"};
	for (size_t i = 0; i < ARR_SIZE(config_paths); i++) {
		char *path = mstrjoin(base, config_paths[i]);
		FILE *ret = fopen(path, "r");
		if (ret && out_path) {
			*out_path = path;
		} else {
			free(path);
		}
		if (ret) {
			if (strstr(config_paths[i], "compton")) {
				log_warn("This compositor has been renamed to \"picom\", "
				         "the old config file paths is deprecated. "
				         "Please replace the \"compton\"s in the path "
				         "with \"picom\"");
			}
			return ret;
		}
	}
	return NULL;
}

/**
 * Get a file stream of the configuration file to read.
 *
 * Follows the XDG specification to search for the configuration file.
 */
FILE *open_config_file(const char *cpath, char **ppath) {
	static const char config_filename_legacy[] = "/.compton.conf";

	if (cpath) {
		FILE *ret = fopen(cpath, "r");
		if (ret && ppath) {
			*ppath = strdup(cpath);
		}
		return ret;
	}

	// First search for config file in user config directory
	auto config_home = xdg_config_home();
	if (config_home) {
		auto ret = open_config_file_at(config_home, ppath);
		free((void *)config_home);
		if (ret) {
			return ret;
		}
	}

	// Fall back to legacy config file in user home directory
	const char *home = getenv("HOME");
	if (home && strlen(home)) {
		auto path = mstrjoin(home, config_filename_legacy);
		auto ret = fopen(path, "r");
		if (ret && ppath) {
			*ppath = path;
		} else {
			free(path);
		}
		if (ret) {
			return ret;
		}
	}

	// Fall back to config file in system config directory
	auto config_dirs = xdg_config_dirs();
	for (int i = 0; config_dirs[i]; i++) {
		auto ret = open_config_file_at(config_dirs[i], ppath);
		if (ret) {
			free(config_dirs);
			return ret;
		}
	}
	free(config_dirs);

	return NULL;
}

/**
 * Parse a condition list in configuration file.
 */
bool must_use parse_cfg_condlst(const config_t *pcfg, c2_lptr_t **pcondlst, const char *name) {
	config_setting_t *setting = config_lookup(pcfg, name);
	if (setting == NULL) {
		return true;
	}
	// Parse an array of options
	if (config_setting_is_array(setting)) {
		int i = config_setting_length(setting);
		while (i--) {
			if (!c2_parse(pcondlst,
			              config_setting_get_string_elem(setting, i), NULL)) {
				return false;
			}
		}
	}
	// Treat it as a single pattern if it's a string
	else if (CONFIG_TYPE_STRING == config_setting_type(setting)) {
		if (!c2_parse(pcondlst, config_setting_get_string(setting), NULL)) {
			return false;
		}
	}
	return true;
}

/**
 * Parse a window corner radius rule list in configuration file.
 */
static inline bool
parse_cfg_condlst_with_prefix(c2_lptr_t **condlst, const config_t *pcfg, const char *name,
                              void *(*parse_prefix)(const char *, const char **, void *),
                              void (*free_value)(void *), void *user_data) {
	config_setting_t *setting = config_lookup(pcfg, name);
	if (setting == NULL) {
		return true;
	}
	// Parse an array of options
	if (config_setting_is_array(setting)) {
		int i = config_setting_length(setting);
		while (i--) {
			if (!c2_parse_with_prefix(
			        condlst, config_setting_get_string_elem(setting, i),
			        parse_prefix, free_value, user_data)) {
				return false;
			}
		}
	}
	// Treat it as a single pattern if it's a string
	else if (config_setting_type(setting) == CONFIG_TYPE_STRING) {
		if (!c2_parse_with_prefix(condlst, config_setting_get_string(setting),
		                          parse_prefix, free_value, user_data)) {
			return false;
		}
	}
	return true;
}

static inline void parse_wintype_config(const config_t *cfg, const char *member_name,
                                        win_option_t *o, win_option_mask_t *mask) {
	char *str = mstrjoin("wintypes.", member_name);
	const config_setting_t *setting = config_lookup(cfg, str);
	free(str);

	int ival = 0;
	if (setting) {
		if (config_setting_lookup_bool(setting, "shadow", &ival)) {
			o->shadow = ival;
			mask->shadow = true;
		}
		if (config_setting_lookup_bool(setting, "fade", &ival)) {
			o->fade = ival;
			mask->fade = true;
		}
		if (config_setting_lookup_bool(setting, "focus", &ival)) {
			o->focus = ival;
			mask->focus = true;
		}
		if (config_setting_lookup_bool(setting, "blur-background", &ival)) {
			o->blur_background = ival;
			mask->blur_background = true;
		}
		if (config_setting_lookup_bool(setting, "full-shadow", &ival)) {
			o->full_shadow = ival;
			mask->full_shadow = true;
		}
		if (config_setting_lookup_bool(setting, "redir-ignore", &ival)) {
			o->redir_ignore = ival;
			mask->redir_ignore = true;
		}
		if (config_setting_lookup_bool(setting, "clip-shadow-above", &ival)) {
			o->clip_shadow_above = ival;
			mask->clip_shadow_above = true;
		}

		double fval;
		if (config_setting_lookup_float(setting, "opacity", &fval)) {
			o->opacity = normalize_d(fval);
			mask->opacity = true;
		}
	}
}

static enum animation_trigger parse_animation_trigger(const char *trigger) {
	for (int i = 0; i <= ANIMATION_TRIGGER_LAST; i++) {
		if (strcasecmp(trigger, animation_trigger_names[i]) == 0) {
			return i;
		}
	}
	return ANIMATION_TRIGGER_INVALID;
}

static struct script *
compile_win_script(config_setting_t *setting, int *output_indices, char **err) {
	struct script_output_info outputs[ARR_SIZE(win_script_outputs)];
	memcpy(outputs, win_script_outputs, sizeof(win_script_outputs));

	struct script_parse_config parse_config = {
	    .context_info = win_script_context_info,
	    .output_info = outputs,
	};
	auto script = script_compile(setting, parse_config, err);
	if (script == NULL) {
		return script;
	}
	for (int i = 0; i < NUM_OF_WIN_SCRIPT_OUTPUTS; i++) {
		output_indices[i] = outputs[i].slot;
	}
	return script;
}

static bool
set_animation(struct win_script *animations, const enum animation_trigger *triggers,
              int number_of_triggers, struct script *script, const int *output_indices,
              uint64_t suppressions, unsigned line) {
	bool needed = false;
	for (int i = 0; i < number_of_triggers; i++) {
		if (triggers[i] == ANIMATION_TRIGGER_INVALID) {
			log_error("Invalid trigger defined at line %d", line);
			continue;
		}
		if (animations[triggers[i]].script != NULL) {
			log_error("Duplicate animation defined for trigger %s at line "
			          "%d, it will be ignored.",
			          animation_trigger_names[triggers[i]], line);
			continue;
		}
		memcpy(animations[triggers[i]].output_indices, output_indices,
		       sizeof(int[NUM_OF_WIN_SCRIPT_OUTPUTS]));
		animations[triggers[i]].script = script;
		animations[triggers[i]].suppressions = suppressions;
		needed = true;
	}
	return needed;
}

static struct script *
parse_animation_one(struct win_script *animations, config_setting_t *setting) {
	auto triggers = config_setting_lookup(setting, "triggers");
	if (!triggers) {
		log_error("Missing triggers in animation script, at line %d",
		          config_setting_source_line(setting));
		return NULL;
	}
	if (!config_setting_is_list(triggers) && !config_setting_is_array(triggers) &&
	    config_setting_get_string(triggers) == NULL) {
		log_error("The \"triggers\" option must either be a string, a list, or "
		          "an array, but is none of those at line %d",
		          config_setting_source_line(triggers));
		return NULL;
	}
	auto number_of_triggers =
	    config_setting_get_string(triggers) == NULL ? config_setting_length(triggers) : 1;
	if (number_of_triggers > ANIMATION_TRIGGER_LAST) {
		log_error("Too many triggers in animation defined at line %d",
		          config_setting_source_line(triggers));
		return NULL;
	}
	if (number_of_triggers == 0) {
		log_error("Trigger list is empty in animation defined at line %d",
		          config_setting_source_line(triggers));
		return NULL;
	}
	enum animation_trigger *trigger_types =
	    alloca(sizeof(enum animation_trigger[number_of_triggers]));
	const char *trigger0 = config_setting_get_string(triggers);
	if (trigger0 == NULL) {
		for (int i = 0; i < number_of_triggers; i++) {
			auto trigger_i = config_setting_get_string_elem(triggers, i);
			trigger_types[i] = trigger_i == NULL
			                       ? ANIMATION_TRIGGER_INVALID
			                       : parse_animation_trigger(trigger_i);
		}
	} else {
		trigger_types[0] = parse_animation_trigger(trigger0);
	}

	// script parser shouldn't see this.
	config_setting_remove(setting, "triggers");

	uint64_t suppressions = 0;
	auto suppressions_setting = config_setting_lookup(setting, "suppressions");
	if (suppressions_setting != NULL) {
		auto single_suppression = config_setting_get_string(suppressions_setting);
		if (!config_setting_is_list(suppressions_setting) &&
		    !config_setting_is_array(suppressions_setting) &&
		    single_suppression == NULL) {
			log_error("The \"suppressions\" option must either be a string, "
			          "a list, or an array, but is none of those at line %d",
			          config_setting_source_line(suppressions_setting));
			return NULL;
		}
		if (single_suppression != NULL) {
			auto suppression = parse_animation_trigger(single_suppression);
			if (suppression == ANIMATION_TRIGGER_INVALID) {
				log_error("Invalid suppression defined at line %d",
				          config_setting_source_line(suppressions_setting));
				return NULL;
			}
			suppressions = 1 << suppression;
		} else {
			auto len = config_setting_length(suppressions_setting);
			for (int i = 0; i < len; i++) {
				auto suppression_str =
				    config_setting_get_string_elem(suppressions_setting, i);
				if (suppression_str == NULL) {
					log_error(
					    "The \"suppressions\" option must only "
					    "contain strings, but one of them is not at "
					    "line %d",
					    config_setting_source_line(suppressions_setting));
					return NULL;
				}
				auto suppression = parse_animation_trigger(suppression_str);
				if (suppression == ANIMATION_TRIGGER_INVALID) {
					log_error(
					    "Invalid suppression defined at line %d",
					    config_setting_source_line(suppressions_setting));
					return NULL;
				}
				suppressions |= 1 << suppression;
			}
		}
		config_setting_remove(setting, "suppressions");
	}

	int output_indices[NUM_OF_WIN_SCRIPT_OUTPUTS];
	char *err;
	auto script = compile_win_script(setting, output_indices, &err);
	if (!script) {
		log_error("Failed to parse animation script at line %d: %s",
		          config_setting_source_line(setting), err);
		free(err);
		return NULL;
	}

	bool needed = set_animation(animations, trigger_types, number_of_triggers, script,
	                            output_indices, suppressions,
	                            config_setting_source_line(setting));
	if (!needed) {
		script_free(script);
		script = NULL;
	}
	return script;
}

static struct script **parse_animations(struct win_script *animations,
                                        config_setting_t *setting, int *number_of_scripts) {
	auto number_of_animations = config_setting_length(setting);
	auto all_scripts = ccalloc(number_of_animations + 1, struct script *);
	auto len = 0;
	for (int i = 0; i < number_of_animations; i++) {
		auto sub = config_setting_get_elem(setting, (unsigned)i);
		auto script = parse_animation_one(animations, sub);
		if (script != NULL) {
			all_scripts[len++] = script;
		}
	}
	if (len == 0) {
		free(all_scripts);
		all_scripts = NULL;
	}
	*number_of_scripts = len;
	return all_scripts;
}

#define FADING_TEMPLATE_1                                                                \
	"opacity = { "                                                                   \
	"  timing = \"%fms linear\"; "                                                   \
	"  start = \"window-raw-opacity-before\"; "                                      \
	"  end = \"window-raw-opacity\"; "                                               \
	"};"                                                                             \
	"shadow-opacity = \"opacity\";"
#define FADING_TEMPLATE_2                                                                \
	"blur-opacity = { "                                                              \
	"  timing = \"%fms linear\"; "                                                   \
	"  start = %f; end = %f; "                                                       \
	"};"

static struct script *compile_win_script_from_string(const char *input, int *output_indices) {
	config_t tmp_config;
	config_setting_t *setting;
	config_init(&tmp_config);
	config_set_auto_convert(&tmp_config, true);
	config_read_string(&tmp_config, input);
	setting = config_root_setting(&tmp_config);

	// Since we are compiling scripts we generated, it can't fail.
	char *err = NULL;
	auto script = compile_win_script(setting, output_indices, &err);
	config_destroy(&tmp_config);
	BUG_ON(err != NULL);

	return script;
}

void generate_fading_config(struct options *opt) {
	// We create stand-in animations for fade-in/fade-out if they haven't be
	// overwritten
	char *str = NULL;
	size_t len = 0;
	enum animation_trigger trigger[2];
	struct script *scripts[4];
	int number_of_scripts = 0;
	int number_of_triggers = 0;

	int output_indices[NUM_OF_WIN_SCRIPT_OUTPUTS];
	double duration = 1.0 / opt->fade_in_step * opt->fade_delta;
	// Fading in from nothing, i.e. `open` and `show`
	asnprintf(&str, &len, FADING_TEMPLATE_1 FADING_TEMPLATE_2, duration, duration,
	          0.F, 1.F);

	auto fade_in1 = compile_win_script_from_string(str, output_indices);
	if (opt->animations[ANIMATION_TRIGGER_OPEN].script == NULL && !opt->no_fading_openclose) {
		trigger[number_of_triggers++] = ANIMATION_TRIGGER_OPEN;
	}
	if (opt->animations[ANIMATION_TRIGGER_SHOW].script == NULL) {
		trigger[number_of_triggers++] = ANIMATION_TRIGGER_SHOW;
	}
	if (set_animation(opt->animations, trigger, number_of_triggers, fade_in1,
	                  output_indices, 0, 0)) {
		scripts[number_of_scripts++] = fade_in1;
	} else {
		script_free(fade_in1);
	}

	// Fading for opacity change, for these, the blur opacity doesn't change.
	asnprintf(&str, &len, FADING_TEMPLATE_1, duration);
	auto fade_in2 = compile_win_script_from_string(str, output_indices);
	number_of_triggers = 0;
	if (opt->animations[ANIMATION_TRIGGER_INCREASE_OPACITY].script == NULL) {
		trigger[number_of_triggers++] = ANIMATION_TRIGGER_INCREASE_OPACITY;
	}
	if (set_animation(opt->animations, trigger, number_of_triggers, fade_in2,
	                  output_indices, 0, 0)) {
		scripts[number_of_scripts++] = fade_in2;
	} else {
		script_free(fade_in2);
	}

	duration = 1.0 / opt->fade_out_step * opt->fade_delta;
	// Fading out to nothing, i.e. `hide` and `close`
	asnprintf(&str, &len, FADING_TEMPLATE_1 FADING_TEMPLATE_2, duration, duration,
	          1.F, 0.F);
	auto fade_out1 = compile_win_script_from_string(str, output_indices);
	number_of_triggers = 0;
	if (opt->animations[ANIMATION_TRIGGER_CLOSE].script == NULL &&
	    !opt->no_fading_openclose) {
		trigger[number_of_triggers++] = ANIMATION_TRIGGER_CLOSE;
	}
	if (opt->animations[ANIMATION_TRIGGER_HIDE].script == NULL) {
		trigger[number_of_triggers++] = ANIMATION_TRIGGER_HIDE;
	}
	if (set_animation(opt->animations, trigger, number_of_triggers, fade_out1,
	                  output_indices, 0, 0)) {
		scripts[number_of_scripts++] = fade_out1;
	} else {
		script_free(fade_out1);
	}

	// Fading for opacity change
	asnprintf(&str, &len, FADING_TEMPLATE_1, duration);
	auto fade_out2 = compile_win_script_from_string(str, output_indices);
	number_of_triggers = 0;
	if (opt->animations[ANIMATION_TRIGGER_DECREASE_OPACITY].script == NULL) {
		trigger[number_of_triggers++] = ANIMATION_TRIGGER_DECREASE_OPACITY;
	}
	if (set_animation(opt->animations, trigger, number_of_triggers, fade_out2,
	                  output_indices, 0, 0)) {
		scripts[number_of_scripts++] = fade_out2;
	} else {
		script_free(fade_out2);
	}
	free(str);

	log_debug("Generated %d scripts for fading.", number_of_scripts);
	if (number_of_scripts) {
		auto ptr = realloc(
		    opt->all_scripts,
		    sizeof(struct scripts * [number_of_scripts + opt->number_of_scripts]));
		allocchk(ptr);
		opt->all_scripts = ptr;
		memcpy(&opt->all_scripts[opt->number_of_scripts], scripts,
		       sizeof(struct script *[number_of_scripts]));
		opt->number_of_scripts += number_of_scripts;
	}
}

static const char **resolve_include(config_t * /* cfg */, const char *include_dir,
                                    const char *path, const char **err) {
	char *result = locate_auxiliary_file("include", path, include_dir);
	if (result == NULL) {
		*err = "Failed to locate included file";
		return NULL;
	}

	log_debug("Resolved include file \"%s\" to \"%s\"", path, result);
	const char **ret = ccalloc(2, const char *);
	ret[0] = result;
	ret[1] = NULL;
	return ret;
}

/**
 * Parse a configuration file from default location.
 *
 * Returns the actually config_file name
 */
char *parse_config_libconfig(options_t *opt, const char *config_file) {

	const char *deprecation_message =
	    "option has been deprecated. Please remove it from your configuration file. "
	    "If you encounter any problems without this feature, please feel free to "
	    "open a bug report";
	char *path = NULL;
	FILE *f;
	config_t cfg;
	int ival = 0;
	bool bval;
	double dval = 0.0;
	// libconfig manages string memory itself, so no need to manually free
	// anything
	const char *sval = NULL;

	f = open_config_file(config_file, &path);
	if (!f) {
		free(path);
		if (config_file) {
			log_fatal("Failed to read configuration file \"%s\".", config_file);
			return ERR_PTR(-1);
		}
		return NULL;
	}

	config_init(&cfg);
#ifdef CONFIG_OPTION_ALLOW_OVERRIDES
	config_set_options(&cfg, CONFIG_OPTION_ALLOW_OVERRIDES);
#endif
	{
		char *abspath = realpath(path, NULL);
		char *parent = dirname(abspath);        // path2 may be modified

		if (parent) {
			config_set_include_dir(&cfg, parent);
		}
		config_set_include_func(&cfg, resolve_include);

		free(abspath);
	}

	{
		int read_result = config_read(&cfg, f);
		fclose(f);
		f = NULL;
		if (read_result == CONFIG_FALSE) {
			log_fatal("Error when reading configuration file \"%s\", line "
			          "%d: %s",
			          path, config_error_line(&cfg), config_error_text(&cfg));
			goto err;
		}
	}
	config_set_auto_convert(&cfg, 1);

	// Get options from the configuration file. We don't do range checking
	// right now. It will be done later

	// --dbus
	lcfg_lookup_bool(&cfg, "dbus", &opt->dbus);

	// -D (fade_delta)
	if (config_lookup_int(&cfg, "fade-delta", &ival)) {
		opt->fade_delta = ival;
	}
	// -I (fade_in_step)
	if (config_lookup_float(&cfg, "fade-in-step", &dval)) {
		opt->fade_in_step = normalize_d(dval);
	}
	// -O (fade_out_step)
	if (config_lookup_float(&cfg, "fade-out-step", &dval)) {
		opt->fade_out_step = normalize_d(dval);
	}
	// -r (shadow_radius)
	config_lookup_int(&cfg, "shadow-radius", &opt->shadow_radius);
	// -o (shadow_opacity)
	config_lookup_float(&cfg, "shadow-opacity", &opt->shadow_opacity);
	// -l (shadow_offset_x)
	config_lookup_int(&cfg, "shadow-offset-x", &opt->shadow_offset_x);
	// -t (shadow_offset_y)
	config_lookup_int(&cfg, "shadow-offset-y", &opt->shadow_offset_y);
	// -i (inactive_opacity)
	if (config_lookup_float(&cfg, "inactive-opacity", &dval)) {
		opt->inactive_opacity = normalize_d(dval);
	}
	// --active_opacity
	if (config_lookup_float(&cfg, "active-opacity", &dval)) {
		opt->active_opacity = normalize_d(dval);
	}
	// --corner-radius
	config_lookup_int(&cfg, "corner-radius", &opt->corner_radius);

	if (lcfg_lookup_bool(&cfg, "no-frame-pacing", &bval)) {
		opt->frame_pacing = !bval;
	}

	// -e (frame_opacity)
	config_lookup_float(&cfg, "frame-opacity", &opt->frame_opacity);
	// -c (shadow_enable)
	lcfg_lookup_bool(&cfg, "shadow", &opt->shadow_enable);
	// -m (menu_opacity)
	if (config_lookup_float(&cfg, "menu-opacity", &dval)) {
		log_warn("Option `menu-opacity` is deprecated, and will be removed."
		         "Please use the wintype option `opacity` of `popup_menu`"
		         "and `dropdown_menu` instead.");
		opt->wintype_option[WINTYPE_DROPDOWN_MENU].opacity = dval;
		opt->wintype_option[WINTYPE_POPUP_MENU].opacity = dval;
		opt->wintype_option_mask[WINTYPE_DROPDOWN_MENU].opacity = true;
		opt->wintype_option_mask[WINTYPE_POPUP_MENU].opacity = true;
	}
	// -f (fading_enable)
	if (config_lookup_bool(&cfg, "fading", &ival)) {
		opt->fading_enable = ival;
	}
	// --no-fading-open-close
	lcfg_lookup_bool(&cfg, "no-fading-openclose", &opt->no_fading_openclose);
	// --no-fading-destroyed-argb
	lcfg_lookup_bool(&cfg, "no-fading-destroyed-argb", &opt->no_fading_destroyed_argb);
	// --shadow-red
	config_lookup_float(&cfg, "shadow-red", &opt->shadow_red);
	// --shadow-green
	config_lookup_float(&cfg, "shadow-green", &opt->shadow_green);
	// --shadow-blue
	config_lookup_float(&cfg, "shadow-blue", &opt->shadow_blue);
	// --shadow-color
	if (config_lookup_string(&cfg, "shadow-color", &sval)) {
		struct color rgb;
		rgb = hex_to_rgb(sval);
		opt->shadow_red = rgb.red;
		opt->shadow_green = rgb.green;
		opt->shadow_blue = rgb.blue;
	}
	// --shadow-exclude-reg
	if (config_lookup_string(&cfg, "shadow-exclude-reg", &sval)) {
		log_error("shadow-exclude-reg is deprecated. Please use "
		          "clip-shadow-above for more flexible shadow exclusion.");
		goto err;
	}
	// --inactive-opacity-override
	lcfg_lookup_bool(&cfg, "inactive-opacity-override", &opt->inactive_opacity_override);
	// --inactive-dim
	config_lookup_float(&cfg, "inactive-dim", &opt->inactive_dim);
	// --mark-wmwin-focused
	lcfg_lookup_bool(&cfg, "mark-wmwin-focused", &opt->mark_wmwin_focused);
	// --mark-ovredir-focused
	lcfg_lookup_bool(&cfg, "mark-ovredir-focused", &opt->mark_ovredir_focused);
	// --shadow-ignore-shaped
	lcfg_lookup_bool(&cfg, "shadow-ignore-shaped", &opt->shadow_ignore_shaped);
	// --detect-rounded-corners
	lcfg_lookup_bool(&cfg, "detect-rounded-corners", &opt->detect_rounded_corners);
	// --crop-shadow-to-monitor
	if (lcfg_lookup_bool(&cfg, "xinerama-shadow-crop", &opt->crop_shadow_to_monitor)) {
		log_warn("xinerama-shadow-crop is deprecated. Use crop-shadow-to-monitor "
		         "instead.");
	}
	lcfg_lookup_bool(&cfg, "crop-shadow-to-monitor", &opt->crop_shadow_to_monitor);
	// --detect-client-opacity
	lcfg_lookup_bool(&cfg, "detect-client-opacity", &opt->detect_client_opacity);
	// --refresh-rate
	if (config_lookup_int(&cfg, "refresh-rate", &ival)) {
		log_warn("The refresh-rate %s", deprecation_message);
	}
	// --vsync
	if (config_lookup_string(&cfg, "vsync", &sval)) {
		bool parsed_vsync = parse_vsync(sval);
		log_error("vsync option will take a boolean from now on. \"%s\" in "
		          "your configuration should be changed to \"%s\"",
		          sval, parsed_vsync ? "true" : "false");
		goto err;
	}
	lcfg_lookup_bool(&cfg, "vsync", &opt->vsync);
	// --backend
	if (config_lookup_string(&cfg, "backend", &sval)) {
		opt->backend = parse_backend(sval);
		if (opt->backend >= NUM_BKEND) {
			log_fatal("Cannot parse backend");
			goto err;
		}
	}
	// --log-level
	if (config_lookup_string(&cfg, "log-level", &sval)) {
		opt->log_level = string_to_log_level(sval);
		if (opt->log_level == LOG_LEVEL_INVALID) {
			log_warn("Invalid log level, defaults to WARN");
		} else {
			log_set_level_tls(opt->log_level);
		}
	}
	// --log-file
	if (config_lookup_string(&cfg, "log-file", &sval)) {
		if (*sval != '/') {
			log_warn("The log-file in your configuration file is not an "
			         "absolute path");
		}
		opt->logpath = strdup(sval);
	}
	// --sw-opti
	if (lcfg_lookup_bool(&cfg, "sw-opti", &bval)) {
		log_error("The sw-opti %s", deprecation_message);
		goto err;
	}
	// --use-ewmh-active-win
	lcfg_lookup_bool(&cfg, "use-ewmh-active-win", &opt->use_ewmh_active_win);
	// --unredir-if-possible
	lcfg_lookup_bool(&cfg, "unredir-if-possible", &opt->unredir_if_possible);
	// --unredir-if-possible-delay
	if (config_lookup_int(&cfg, "unredir-if-possible-delay", &ival)) {
		if (ival < 0) {
			log_warn("Invalid unredir-if-possible-delay %d", ival);
		} else {
			opt->unredir_if_possible_delay = ival;
		}
	}
	// --inactive-dim-fixed
	lcfg_lookup_bool(&cfg, "inactive-dim-fixed", &opt->inactive_dim_fixed);
	// --detect-transient
	lcfg_lookup_bool(&cfg, "detect-transient", &opt->detect_transient);
	// --detect-client-leader
	lcfg_lookup_bool(&cfg, "detect-client-leader", &opt->detect_client_leader);
	// --no-ewmh-fullscreen
	lcfg_lookup_bool(&cfg, "no-ewmh-fullscreen", &opt->no_ewmh_fullscreen);
	// --transparent-clipping
	lcfg_lookup_bool(&cfg, "transparent-clipping", &opt->transparent_clipping);
	// --dithered_present
	lcfg_lookup_bool(&cfg, "dithered-present", &opt->dithered_present);

	if (!parse_cfg_condlst(&cfg, &opt->transparent_clipping_blacklist,
	                       "transparent-clipping-exclude") ||
	    !parse_cfg_condlst(&cfg, &opt->shadow_blacklist, "shadow-exclude") ||
	    !parse_cfg_condlst(&cfg, &opt->shadow_clip_list, "clip-shadow-above") ||
	    !parse_cfg_condlst(&cfg, &opt->fade_blacklist, "fade-exclude") ||
	    !parse_cfg_condlst(&cfg, &opt->focus_blacklist, "focus-exclude") ||
	    !parse_cfg_condlst(&cfg, &opt->invert_color_list, "invert-color-include") ||
	    !parse_cfg_condlst(&cfg, &opt->blur_background_blacklist, "blur-background-exclude") ||
	    !parse_cfg_condlst(&cfg, &opt->unredir_if_possible_blacklist,
	                       "unredir-if-possible-exclude") ||
	    !parse_cfg_condlst(&cfg, &opt->rounded_corners_blacklist, "rounded-corners-exclude") ||
	    !parse_cfg_condlst_with_prefix(&opt->corner_radius_rules, &cfg, "corner-radius-rules",
	                                   parse_numeric_prefix, NULL, (int[]){0, INT_MAX}) ||
	    !parse_cfg_condlst_with_prefix(&opt->opacity_rules, &cfg, "opacity-rule",
	                                   parse_numeric_prefix, NULL, (int[]){0, 100}) ||
	    !parse_cfg_condlst_with_prefix(
	        &opt->window_shader_fg_rules, &cfg, "window-shader-fg-rule",
	        parse_window_shader_prefix, free, (void *)config_get_include_dir(&cfg))) {
		goto err;
	}

	// --blur-method
	if (config_lookup_string(&cfg, "blur-method", &sval)) {
		int method = parse_blur_method(sval);
		if (method >= BLUR_METHOD_INVALID) {
			log_fatal("Invalid blur method %s", sval);
			goto err;
		}
		opt->blur_method = (enum blur_method)method;
	}
	// --blur-size
	config_lookup_int(&cfg, "blur-size", &opt->blur_radius);
	// --blur-deviation
	config_lookup_float(&cfg, "blur-deviation", &opt->blur_deviation);
	// --blur-strength
	config_lookup_int(&cfg, "blur-strength", &opt->blur_strength);
	// --blur-background
	if (config_lookup_bool(&cfg, "blur-background", &ival) && ival) {
		if (opt->blur_method == BLUR_METHOD_NONE) {
			opt->blur_method = BLUR_METHOD_KERNEL;
		}
	}
	// --blur-background-frame
	lcfg_lookup_bool(&cfg, "blur-background-frame", &opt->blur_background_frame);
	// --blur-background-fixed
	lcfg_lookup_bool(&cfg, "blur-background-fixed", &opt->blur_background_fixed);
	// --blur-kern
	if (config_lookup_string(&cfg, "blur-kern", &sval)) {
		opt->blur_kerns = parse_blur_kern_lst(sval, &opt->blur_kernel_count);
		if (!opt->blur_kerns) {
			log_fatal("Cannot parse \"blur-kern\"");
			goto err;
		}
	}
	// --resize-damage
	config_lookup_int(&cfg, "resize-damage", &opt->resize_damage);
	// --glx-no-stencil
	lcfg_lookup_bool(&cfg, "glx-no-stencil", &opt->glx_no_stencil);
	// --glx-no-rebind-pixmap
	lcfg_lookup_bool(&cfg, "glx-no-rebind-pixmap", &opt->glx_no_rebind_pixmap);
	lcfg_lookup_bool(&cfg, "force-win-blend", &opt->force_win_blend);
	// --glx-swap-method
	if (config_lookup_string(&cfg, "glx-swap-method", &sval)) {
		char *endptr;
		long val = strtol(sval, &endptr, 10);
		bool should_remove = true;
		if (*endptr || !(*sval)) {
			// sval is not a number, or an empty string
			val = -1;
		}
		if (strcmp(sval, "undefined") != 0 && val != 0) {
			// If not undefined, we will use damage and buffer-age to limit
			// the rendering area.
			should_remove = false;
		}
		log_error("glx-swap-method has been removed, your setting "
		          "\"%s\" should be %s.",
		          sval,
		          !should_remove ? "replaced by `use-damage = true`" : "removed");
		goto err;
	}
	// --use-damage
	lcfg_lookup_bool(&cfg, "use-damage", &opt->use_damage);

	// --max-brightness
	if (config_lookup_float(&cfg, "max-brightness", &opt->max_brightness) &&
	    opt->use_damage && opt->max_brightness < 1) {
		log_warn("max-brightness requires use-damage = false. Falling back to "
		         "1.0");
		opt->max_brightness = 1.0;
	}

	// --window-shader-fg
	if (config_lookup_string(&cfg, "window-shader-fg", &sval)) {
		opt->window_shader_fg =
		    locate_auxiliary_file("shaders", sval, config_get_include_dir(&cfg));
	}

	// --glx-use-gpushader4
	if (config_lookup_bool(&cfg, "glx-use-gpushader4", &ival)) {
		log_error("glx-use-gpushader4 has been removed, please remove it "
		          "from your config file");
		goto err;
	}
	// --xrender-sync-fence
	lcfg_lookup_bool(&cfg, "xrender-sync-fence", &opt->xrender_sync_fence);

	if (lcfg_lookup_bool(&cfg, "clear-shadow", &bval)) {
		log_warn("\"clear-shadow\" is removed as an option, and is always"
		         " enabled now. Consider removing it from your config file");
	}

	config_setting_t *blur_cfg = config_lookup(&cfg, "blur");
	if (blur_cfg) {
		if (config_setting_lookup_string(blur_cfg, "method", &sval)) {
			int method = parse_blur_method(sval);
			if (method >= BLUR_METHOD_INVALID) {
				log_warn("Invalid blur method %s, ignoring.", sval);
			} else {
				opt->blur_method = (enum blur_method)method;
			}
		}

		config_setting_lookup_int(blur_cfg, "size", &opt->blur_radius);

		if (config_setting_lookup_string(blur_cfg, "kernel", &sval)) {
			opt->blur_kerns = parse_blur_kern_lst(sval, &opt->blur_kernel_count);
			if (!opt->blur_kerns) {
				log_warn("Failed to parse blur kernel: %s", sval);
			}
		}

		config_setting_lookup_float(blur_cfg, "deviation", &opt->blur_deviation);
		config_setting_lookup_int(blur_cfg, "strength", &opt->blur_strength);
	}

	// --write-pid-path
	if (config_lookup_string(&cfg, "write-pid-path", &sval)) {
		if (*sval != '/') {
			log_warn("The write-pid-path in your configuration file is not"
			         " an absolute path");
		}
		opt->write_pid_path = strdup(sval);
	}

	// Wintype settings

	// XXX ! Refactor all the wintype_* arrays into a struct
	for (wintype_t i = 0; i < NUM_WINTYPES; ++i) {
		parse_wintype_config(&cfg, WINTYPES[i].name, &opt->wintype_option[i],
		                     &opt->wintype_option_mask[i]);
	}

	// Compatibility with the old name for notification windows.
	parse_wintype_config(&cfg, "notify", &opt->wintype_option[WINTYPE_NOTIFICATION],
	                     &opt->wintype_option_mask[WINTYPE_NOTIFICATION]);

	config_setting_t *animations = config_lookup(&cfg, "animations");
	if (animations) {
		opt->all_scripts =
		    parse_animations(opt->animations, animations, &opt->number_of_scripts);
	}

	config_destroy(&cfg);
	return path;

err:
	config_destroy(&cfg);
	free(path);
	return ERR_PTR(-1);
}
