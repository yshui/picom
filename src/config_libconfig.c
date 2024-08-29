// SPDX-License-Identifier: MIT
// Copyright (c) 2012-2014 Richard Grenville <pyxlcy@gmail.com>
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libconfig.h>
#include <libgen.h>

#include "backend/backend.h"
#include "c2.h"
#include "common.h"
#include "config.h"
#include "log.h"
#include "transition/preset.h"
#include "transition/script.h"
#include "utils/dynarr.h"
#include "utils/misc.h"
#include "utils/str.h"
#include "wm/win.h"

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
bool must_use parse_cfg_condlst(struct list_node *list, const config_t *pcfg, const char *name) {
	config_setting_t *setting = config_lookup(pcfg, name);
	if (setting == NULL) {
		return true;
	}
	// Parse an array of options
	if (config_setting_is_array(setting)) {
		int i = config_setting_length(setting);
		while (i--) {
			if (!c2_parse(list, config_setting_get_string_elem(setting, i), NULL)) {
				return false;
			}
		}
	}
	// Treat it as a single pattern if it's a string
	else if (CONFIG_TYPE_STRING == config_setting_type(setting)) {
		if (!c2_parse(list, config_setting_get_string(setting), NULL)) {
			return false;
		}
	}
	return true;
}

/**
 * Parse a window corner radius rule list in configuration file.
 */
static inline bool
parse_cfg_condlst_with_prefix(struct list_node *list, const config_t *pcfg, const char *name,
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
			        list, config_setting_get_string_elem(setting, i),
			        parse_prefix, free_value, user_data)) {
				return false;
			}
		}
	}
	// Treat it as a single pattern if it's a string
	else if (config_setting_type(setting) == CONFIG_TYPE_STRING) {
		if (!c2_parse_with_prefix(list, config_setting_get_string(setting),
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

enum animation_trigger parse_animation_trigger(const char *trigger) {
	for (unsigned i = 0; i < ANIMATION_TRIGGER_COUNT; i++) {
		if (strcasecmp(trigger, animation_trigger_names[i]) == 0) {
			return i;
		}
	}
	return ANIMATION_TRIGGER_INVALID;
}

/// Compile a script from `setting` into `result`, return false on failure.
/// Only the `script` and `output_indices` fields of `result` will be modified.
static bool
compile_win_script(struct win_script *result, config_setting_t *setting, char **err) {
	if (config_setting_lookup(setting, "preset")) {
		return win_script_parse_preset(result, setting);
	}

	struct script_output_info outputs[ARR_SIZE(win_script_outputs)];
	memcpy(outputs, win_script_outputs, sizeof(win_script_outputs));

	struct script_parse_config parse_config = {
	    .context_info = win_script_context_info,
	    .output_info = outputs,
	};
	result->script = script_compile(setting, parse_config, err);
	if (result->script == NULL) {
		return false;
	}
	for (int i = 0; i < NUM_OF_WIN_SCRIPT_OUTPUTS; i++) {
		result->output_indices[i] = outputs[i].slot;
	}
	return true;
}

static bool
set_animation(struct win_script *animations, const enum animation_trigger *triggers,
              int number_of_triggers, struct win_script animation, unsigned line) {
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
		animations[triggers[i]] = animation;
		needed = true;
	}
	return needed;
}

static bool parse_animation_one(struct win_script *animations,
                                struct script ***all_scripts, config_setting_t *setting) {
	struct win_script result = {};
	auto triggers = config_setting_lookup(setting, "triggers");
	if (!triggers) {
		log_error("Missing triggers in animation script, at line %d",
		          config_setting_source_line(setting));
		return false;
	}
	if (!config_setting_is_list(triggers) && !config_setting_is_array(triggers) &&
	    config_setting_get_string(triggers) == NULL) {
		log_error("The \"triggers\" option must either be a string, a list, or "
		          "an array, but is none of those at line %d",
		          config_setting_source_line(triggers));
		return false;
	}
	auto number_of_triggers =
	    config_setting_get_string(triggers) == NULL ? config_setting_length(triggers) : 1;
	if (number_of_triggers > ANIMATION_TRIGGER_COUNT) {
		log_error("Too many triggers in animation defined at line %d",
		          config_setting_source_line(triggers));
		return false;
	}
	if (number_of_triggers == 0) {
		log_error("Trigger list is empty in animation defined at line %d",
		          config_setting_source_line(triggers));
		return false;
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

	auto suppressions_setting = config_setting_lookup(setting, "suppressions");
	if (suppressions_setting != NULL) {
		auto single_suppression = config_setting_get_string(suppressions_setting);
		if (!config_setting_is_list(suppressions_setting) &&
		    !config_setting_is_array(suppressions_setting) &&
		    single_suppression == NULL) {
			log_error("The \"suppressions\" option must either be a string, "
			          "a list, or an array, but is none of those at line %d",
			          config_setting_source_line(suppressions_setting));
			return false;
		}
		if (single_suppression != NULL) {
			auto suppression = parse_animation_trigger(single_suppression);
			if (suppression == ANIMATION_TRIGGER_INVALID) {
				log_error("Invalid suppression defined at line %d",
				          config_setting_source_line(suppressions_setting));
				return false;
			}
			result.suppressions = 1 << suppression;
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
					return false;
				}
				auto suppression = parse_animation_trigger(suppression_str);
				if (suppression == ANIMATION_TRIGGER_INVALID) {
					log_error(
					    "Invalid suppression defined at line %d",
					    config_setting_source_line(suppressions_setting));
					return false;
				}
				result.suppressions |= 1U << suppression;
			}
		}
		config_setting_remove(setting, "suppressions");
	}

	char *err;
	if (!compile_win_script(&result, setting, &err)) {
		log_error("Failed to parse animation script at line %d: %s",
		          config_setting_source_line(setting), err);
		free(err);
		return false;
	}

	bool needed = set_animation(animations, trigger_types, number_of_triggers, result,
	                            config_setting_source_line(setting));
	if (!needed) {
		script_free(result.script);
	} else {
		dynarr_push(*all_scripts, result.script);
	}
	return true;
}

/// `out_scripts`: all the script objects created, this is a dynarr.
static void parse_animations(struct win_script *animations, config_setting_t *setting,
                             struct script ***out_scripts) {
	auto number_of_animations = (unsigned)config_setting_length(setting);
	for (unsigned i = 0; i < number_of_animations; i++) {
		auto sub = config_setting_get_elem(setting, (unsigned)i);
		parse_animation_one(animations, out_scripts, sub);
	}
}

#define FADING_TEMPLATE_1                                                                \
	"opacity = { "                                                                   \
	"  duration = %s; "                                                              \
	"  start = \"window-raw-opacity-before\"; "                                      \
	"  end = \"window-raw-opacity\"; "                                               \
	"};"                                                                             \
	"shadow-opacity = \"opacity\";"
#define FADING_TEMPLATE_2                                                                \
	"blur-opacity = { "                                                              \
	"  duration = %s; "                                                              \
	"  start = %d; end = %d; "                                                       \
	"};"

static bool compile_win_script_from_string(struct win_script *result, const char *input) {
	config_t tmp_config;
	config_setting_t *setting;
	config_init(&tmp_config);
	config_set_auto_convert(&tmp_config, true);
	config_read_string(&tmp_config, input);
	setting = config_root_setting(&tmp_config);

	// Since we are compiling scripts we generated, it can't fail.
	char *err = NULL;
	bool succeeded = compile_win_script(result, setting, &err);
	config_destroy(&tmp_config);
	BUG_ON(err != NULL);

	return succeeded;
}

void generate_fading_config(struct options *opt) {
	// We create stand-in animations for fade-in/fade-out if they haven't be
	// overwritten
	scoped_charp str = NULL;
	size_t len = 0;
	enum animation_trigger trigger[2];
	struct script *scripts[4];
	unsigned number_of_scripts = 0;
	int number_of_triggers = 0;

	double duration = 1.0 / opt->fade_in_step * opt->fade_delta / 1000.0;
	if (!safe_isinf(duration) && !safe_isnan(duration) && duration > 0) {
		scoped_charp duration_str = NULL;
		dtostr(duration, &duration_str);

		// Fading in from nothing, i.e. `open` and `show`
		asnprintf(&str, &len, FADING_TEMPLATE_1 FADING_TEMPLATE_2, duration_str,
		          duration_str, 0, 1);

		struct win_script fade_in1 = {.is_generated = true};
		BUG_ON(!compile_win_script_from_string(&fade_in1, str));
		if (opt->animations[ANIMATION_TRIGGER_OPEN].script == NULL &&
		    !opt->no_fading_openclose) {
			trigger[number_of_triggers++] = ANIMATION_TRIGGER_OPEN;
		}
		if (opt->animations[ANIMATION_TRIGGER_SHOW].script == NULL) {
			trigger[number_of_triggers++] = ANIMATION_TRIGGER_SHOW;
		}
		if (set_animation(opt->animations, trigger, number_of_triggers, fade_in1, 0)) {
			scripts[number_of_scripts++] = fade_in1.script;
		} else {
			script_free(fade_in1.script);
		}

		// Fading for opacity change, for these, the blur opacity doesn't change.
		asnprintf(&str, &len, FADING_TEMPLATE_1, duration_str);
		struct win_script fade_in2 = {.is_generated = true};
		BUG_ON(!compile_win_script_from_string(&fade_in2, str));
		number_of_triggers = 0;
		if (opt->animations[ANIMATION_TRIGGER_INCREASE_OPACITY].script == NULL) {
			trigger[number_of_triggers++] = ANIMATION_TRIGGER_INCREASE_OPACITY;
		}
		if (set_animation(opt->animations, trigger, number_of_triggers, fade_in2, 0)) {
			scripts[number_of_scripts++] = fade_in2.script;
		} else {
			script_free(fade_in2.script);
		}
	} else {
		log_error("Invalid fade-in setting (step: %f, delta: %d), ignoring.",
		          opt->fade_in_step, opt->fade_delta);
	}

	duration = 1.0 / opt->fade_out_step * opt->fade_delta / 1000.0;
	if (!safe_isinf(duration) && !safe_isnan(duration) && duration > 0) {
		scoped_charp duration_str = NULL;
		dtostr(duration, &duration_str);

		// Fading out to nothing, i.e. `hide` and `close`
		asnprintf(&str, &len, FADING_TEMPLATE_1 FADING_TEMPLATE_2, duration_str,
		          duration_str, 1, 0);
		struct win_script fade_out1 = {.is_generated = true};
		BUG_ON(!compile_win_script_from_string(&fade_out1, str));
		number_of_triggers = 0;
		if (opt->animations[ANIMATION_TRIGGER_CLOSE].script == NULL &&
		    !opt->no_fading_openclose) {
			trigger[number_of_triggers++] = ANIMATION_TRIGGER_CLOSE;
		}
		if (opt->animations[ANIMATION_TRIGGER_HIDE].script == NULL) {
			trigger[number_of_triggers++] = ANIMATION_TRIGGER_HIDE;
		}
		if (set_animation(opt->animations, trigger, number_of_triggers, fade_out1, 0)) {
			scripts[number_of_scripts++] = fade_out1.script;
		} else {
			script_free(fade_out1.script);
		}

		// Fading for opacity change
		asnprintf(&str, &len, FADING_TEMPLATE_1, duration_str);
		struct win_script fade_out2 = {.is_generated = true};
		BUG_ON(!compile_win_script_from_string(&fade_out2, str));
		number_of_triggers = 0;
		if (opt->animations[ANIMATION_TRIGGER_DECREASE_OPACITY].script == NULL) {
			trigger[number_of_triggers++] = ANIMATION_TRIGGER_DECREASE_OPACITY;
		}
		if (set_animation(opt->animations, trigger, number_of_triggers, fade_out2, 0)) {
			scripts[number_of_scripts++] = fade_out2.script;
		} else {
			script_free(fade_out2.script);
		}
	} else {
		log_error("Invalid fade-out setting (step: %f, delta: %d), ignoring.",
		          opt->fade_out_step, opt->fade_delta);
	}

	log_debug("Generated %d scripts for fading.", number_of_scripts);
	dynarr_extend_from(opt->all_scripts, scripts, number_of_scripts);
}

static enum window_unredir_option parse_unredir_option(config_setting_t *setting) {
	if (config_setting_type(setting) == CONFIG_TYPE_BOOL) {
		auto bval = config_setting_get_bool(setting);
		return bval ? WINDOW_UNREDIR_WHEN_POSSIBLE_ELSE_TERMINATE
		            : WINDOW_UNREDIR_TERMINATE;
	}
	const char *sval = config_setting_get_string(setting);
	if (!sval) {
		log_error("Invalid value for \"unredir\" at line %d. It must be a "
		          "boolean or a string.",
		          config_setting_source_line(setting));
		return WINDOW_UNREDIR_INVALID;
	}
	if (strcmp(sval, "yes") == 0 || strcmp(sval, "true") == 0 ||
	    strcmp(sval, "default") == 0 || strcmp(sval, "when-possible-else-terminate") == 0) {
		return WINDOW_UNREDIR_WHEN_POSSIBLE_ELSE_TERMINATE;
	}
	if (strcmp(sval, "preferred") == 0 || strcmp(sval, "when-possible") == 0) {
		return WINDOW_UNREDIR_WHEN_POSSIBLE;
	}
	if (strcmp(sval, "no") == 0 || strcmp(sval, "false") == 0 ||
	    strcmp(sval, "terminate") == 0) {
		return WINDOW_UNREDIR_TERMINATE;
	}
	if (strcmp(sval, "passive") == 0) {
		return WINDOW_UNREDIR_PASSIVE;
	}
	if (strcmp(sval, "forced") == 0) {
		return WINDOW_UNREDIR_FORCED;
	}
	log_error("Invalid string value for \"unredir\" at line %d. It must be one of "
	          "\"preferred\", \"passive\", or \"force\". Got \"%s\".",
	          config_setting_source_line(setting), sval);
	return WINDOW_UNREDIR_INVALID;
}

static const struct {
	const char *name;
	ptrdiff_t offset;
} all_window_options[] = {
    {"fade", offsetof(struct window_maybe_options, fade)},
    {"paint", offsetof(struct window_maybe_options, paint)},
    {"shadow", offsetof(struct window_maybe_options, shadow)},
    {"full-shadow", offsetof(struct window_maybe_options, full_shadow)},
    {"invert-color", offsetof(struct window_maybe_options, invert_color)},
    {"blur-background", offsetof(struct window_maybe_options, blur_background)},
    {"clip-shadow-above", offsetof(struct window_maybe_options, clip_shadow_above)},
    {"transparent-clipping", offsetof(struct window_maybe_options, transparent_clipping)},
};

static c2_condition *
parse_rule(struct list_node *rules, config_setting_t *setting, struct script ***out_scripts) {
	if (!config_setting_is_group(setting)) {
		log_error("Invalid rule at line %d. It must be a group.",
		          config_setting_source_line(setting));
		return NULL;
	}
	int ival;
	double fval;
	const char *sval;
	c2_condition *rule = NULL;
	if (config_setting_lookup_string(setting, "match", &sval)) {
		rule = c2_parse(rules, sval, NULL);
		if (!rule) {
			log_error("Failed to parse rule at line %d.",
			          config_setting_source_line(setting));
			return NULL;
		}
	} else {
		// If no match condition is specified, it matches all windows
		rule = c2_new_true(rules);
	}

	auto wopts = cmalloc(struct window_maybe_options);
	*wopts = WIN_MAYBE_OPTIONS_DEFAULT;
	c2_condition_set_data(rule, wopts);

	for (size_t i = 0; i < ARR_SIZE(all_window_options); i++) {
		if (config_setting_lookup_bool(setting, all_window_options[i].name, &ival)) {
			void *ptr = (char *)wopts + all_window_options[i].offset;
			*(enum tristate *)ptr = tri_from_bool(ival);
		}
	}
	if (config_setting_lookup_float(setting, "opacity", &fval)) {
		wopts->opacity = normalize_d(fval);
	}
	if (config_setting_lookup_float(setting, "dim", &fval)) {
		wopts->dim = normalize_d(fval);
	}
	if (config_setting_lookup_int(setting, "corner-radius", &ival)) {
		wopts->corner_radius = ival;
	}

	auto unredir_setting = config_setting_lookup(setting, "unredir");
	if (unredir_setting) {
		wopts->unredir = parse_unredir_option(unredir_setting);
	}

	auto animations = config_setting_lookup(setting, "animations");
	if (animations) {
		parse_animations(wopts->animations, animations, out_scripts);
	}

	config_setting_lookup_string(setting, "shader", &wopts->shader);
	return rule;
}

static void parse_rules(struct list_node *rules, config_setting_t *setting,
                        struct script ***out_scripts) {
	if (!config_setting_is_list(setting)) {
		log_error("Invalid value for \"rules\" at line %d. It must be a list.",
		          config_setting_source_line(setting));
		return;
	}
	const auto length = (unsigned int)config_setting_length(setting);
	for (unsigned int i = 0; i < length; i++) {
		auto sub = config_setting_get_elem(setting, i);
		parse_rule(rules, sub, out_scripts);
	}
}

static const char **
resolve_include(config_t *cfg, const char *include_dir, const char *path, const char **err) {
	char *result = locate_auxiliary_file("include", path, include_dir);
	if (result == NULL) {
		*err = "Failed to locate included file";
		return NULL;
	}

	struct options *opt = config_get_hook(cfg);
	auto included = ccalloc(1, struct included_config_file);
	included->path = strdup(result);
	list_insert_after(&opt->included_config_files, &included->siblings);

	log_debug("Resolved include file \"%s\" to \"%s\"", path, result);
	const char **ret = ccalloc(2, const char *);
	ret[0] = result;
	ret[1] = NULL;
	return ret;
}

/**
 * Parse a configuration file from default location.
 *
 * Returns if config is successfully parsed.
 */
bool parse_config_libconfig(options_t *opt, const char *config_file) {

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
	bool succeeded = false;

	f = open_config_file(config_file, &path);
	if (!f) {
		free(path);
		if (config_file) {
			log_fatal("Failed to read configuration file \"%s\".", config_file);
			return false;
		}
		// No config file found, but that's OK.
		return true;
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
		config_set_hook(&cfg, opt);

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
			goto out;
		}
	}
	config_set_auto_convert(&cfg, 1);

	// --log-level
	if (config_lookup_string(&cfg, "log-level", &sval)) {
		opt->log_level = string_to_log_level(sval);
		if (opt->log_level == LOG_LEVEL_INVALID) {
			log_warn("Invalid log level, defaults to WARN");
		} else {
			log_set_level_tls(opt->log_level);
		}
	}

	// Get options from the configuration file. We don't do range checking
	// right now. It will be done later

	// Load user specified plugins at the very beginning, because list of backends
	// depends on the plugins loaded.
	auto plugins = config_lookup(&cfg, "plugins");
	if (plugins != NULL) {
		sval = config_setting_get_string(plugins);
		if (sval) {
			if (!load_plugin(sval, NULL)) {
				log_fatal("Failed to load plugin \"%s\".", sval);
				goto out;
			}
		} else if (config_setting_is_array(plugins) || config_setting_is_list(plugins)) {
			for (int i = 0; i < config_setting_length(plugins); i++) {
				sval = config_setting_get_string_elem(plugins, i);
				if (!sval) {
					log_fatal("Invalid value for \"plugins\" at line "
					          "%d.",
					          config_setting_source_line(plugins));
					goto out;
				}
				if (!load_plugin(sval, NULL)) {
					log_fatal("Failed to load plugin \"%s\".", sval);
					goto out;
				}
			}
		} else {
			log_fatal("Invalid value for \"plugins\" at line %d.",
			          config_setting_source_line(plugins));
			goto out;
		}
	}

	config_setting_t *rules = config_lookup(&cfg, "rules");
	if (rules) {
		parse_rules(&opt->rules, rules, &opt->all_scripts);
		c2_condition_list_foreach(&opt->rules, i) {
			auto data = (struct window_maybe_options *)c2_condition_get_data(i);
			if (data->shader == NULL) {
				continue;
			}
			data->shader = locate_auxiliary_file(
			    "shaders", data->shader, config_get_include_dir(&cfg));
		}
	}

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
		if (!list_is_empty(&opt->rules)) {
			log_warn_both_style_of_rules("inactive-opacity");
			opt->has_both_style_of_rules = true;
		}
	}
	// --active_opacity
	if (config_lookup_float(&cfg, "active-opacity", &dval)) {
		opt->active_opacity = normalize_d(dval);
		if (!list_is_empty(&opt->rules)) {
			log_warn_both_style_of_rules("active-opacity");
			opt->has_both_style_of_rules = true;
		}
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
		goto out;
	}
	// --inactive-opacity-override
	if (lcfg_lookup_bool(&cfg, "inactive-opacity-override", &opt->inactive_opacity_override) &&
	    !list_is_empty(&opt->rules)) {
		log_warn_both_style_of_rules("inactive-opacity-override");
		opt->has_both_style_of_rules = true;
	}
	// --inactive-dim
	if (config_lookup_float(&cfg, "inactive-dim", &opt->inactive_dim) &&
	    !list_is_empty(&opt->rules)) {
		log_warn_both_style_of_rules("inactive-dim");
		opt->has_both_style_of_rules = true;
	}
	// --mark-wmwin-focused
	if (lcfg_lookup_bool(&cfg, "mark-wmwin-focused", &opt->mark_wmwin_focused) &&
	    !list_is_empty(&opt->rules)) {
		log_warn_both_style_of_rules("mark-wmwin-focused");
		opt->has_both_style_of_rules = true;
	}
	// --mark-ovredir-focused
	if (lcfg_lookup_bool(&cfg, "mark-ovredir-focused", &opt->mark_ovredir_focused) &&
	    !list_is_empty(&opt->rules)) {
		log_warn_both_style_of_rules("mark-ovredir-focused");
		opt->has_both_style_of_rules = true;
	}
	// --shadow-ignore-shaped
	if (lcfg_lookup_bool(&cfg, "shadow-ignore-shaped", &opt->shadow_ignore_shaped) &&
	    !list_is_empty(&opt->rules)) {
		log_warn_both_style_of_rules("shadow-ignore-shaped");
		opt->has_both_style_of_rules = true;
	}
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
		goto out;
	}
	lcfg_lookup_bool(&cfg, "vsync", &opt->vsync);
	// --backend
	if (config_lookup_string(&cfg, "backend", &sval)) {
		opt->legacy_backend = parse_backend(sval);
		opt->backend = backend_find(sval);
		if (opt->legacy_backend >= NUM_BKEND && opt->backend == NULL) {
			log_fatal("Invalid backend: %s", sval);
			goto out;
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
		goto out;
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

	if (!list_is_empty(&opt->rules)) {
		static const char *rule_list[] = {
		    "transparent-clipping-exclude",
		    "shadow-exclude",
		    "clip-shadow-above",
		    "fade-exclude",
		    "focus-exclude",
		    "invert-color-include",
		    "blur-background-exclude",
		    "unredir-if-possible-exclude",
		    "rounded-corners-exclude",
		    "corner-radius-rules",
		    "opacity-rule",
		    "window-shader-fg-rule",
		    "wintypes",
		};
		for (size_t i = 0; i < sizeof(rule_list) / sizeof(rule_list[0]); i++) {
			if (config_lookup(&cfg, rule_list[i])) {
				log_warn_both_style_of_rules(rule_list[i]);
				opt->has_both_style_of_rules = true;
			}
		}
	} else if (!parse_cfg_condlst(&opt->transparent_clipping_blacklist, &cfg,
	                              "transparent-clipping-exclude") ||
	           !parse_cfg_condlst(&opt->shadow_blacklist, &cfg, "shadow-exclude") ||
	           !parse_cfg_condlst(&opt->shadow_clip_list, &cfg, "clip-shadow-above") ||
	           !parse_cfg_condlst(&opt->fade_blacklist, &cfg, "fade-exclude") ||
	           !parse_cfg_condlst(&opt->focus_blacklist, &cfg, "focus-exclude") ||
	           !parse_cfg_condlst(&opt->invert_color_list, &cfg, "invert-color-include") ||
	           !parse_cfg_condlst(&opt->blur_background_blacklist, &cfg,
	                              "blur-background-exclude") ||
	           !parse_cfg_condlst(&opt->unredir_if_possible_blacklist, &cfg,
	                              "unredir-if-possible-exclude") ||
	           !parse_cfg_condlst(&opt->rounded_corners_blacklist, &cfg,
	                              "rounded-corners-exclude") ||
	           !parse_cfg_condlst_with_prefix(
	               &opt->corner_radius_rules, &cfg, "corner-radius-rules",
	               parse_numeric_prefix, NULL, (int[]){0, INT_MAX}) ||
	           !parse_cfg_condlst_with_prefix(&opt->opacity_rules, &cfg, "opacity-rule",
	                                          parse_numeric_prefix, NULL, (int[]){0, 100}) ||
	           !parse_cfg_condlst_with_prefix(&opt->window_shader_fg_rules, &cfg,
	                                          "window-shader-fg-rule",
	                                          parse_window_shader_prefix, free,
	                                          (void *)config_get_include_dir(&cfg))) {
		goto out;
	}

	// --blur-method
	if (config_lookup_string(&cfg, "blur-method", &sval)) {
		int method = parse_blur_method(sval);
		if (method >= BLUR_METHOD_INVALID) {
			log_fatal("Invalid blur method %s", sval);
			goto out;
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
			goto out;
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
		goto out;
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
		goto out;
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
	if (list_is_empty(&opt->rules)) {
		for (wintype_t i = 0; i < NUM_WINTYPES; ++i) {
			parse_wintype_config(&cfg, WINTYPES[i].name, &opt->wintype_option[i],
			                     &opt->wintype_option_mask[i]);
		}
		// Compatibility with the old name for notification windows.
		parse_wintype_config(&cfg, "notify",
		                     &opt->wintype_option[WINTYPE_NOTIFICATION],
		                     &opt->wintype_option_mask[WINTYPE_NOTIFICATION]);
	}

	config_setting_t *animations = config_lookup(&cfg, "animations");
	if (animations) {
		parse_animations(opt->animations, animations, &opt->all_scripts);
	}

	opt->config_file_path = path;
	path = NULL;
	succeeded = true;

out:
	config_destroy(&cfg);
	free(path);
	return succeeded;
}
