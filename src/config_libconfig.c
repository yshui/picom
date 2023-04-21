// SPDX-License-Identifier: MIT
// Copyright (c) 2012-2014 Richard Grenville <pyxlcy@gmail.com>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libconfig.h>
#include <libgen.h>

#include "common.h"
#include "compiler.h"
#include "config.h"
#include "err.h"
#include "log.h"
#include "options.h"
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
		if (ret && ppath)
			*ppath = strdup(cpath);
		return ret;
	}

	// First search for config file in user config directory
	auto config_home = xdg_config_home();
	auto ret = open_config_file_at(config_home, ppath);
	free((void *)config_home);
	if (ret) {
		return ret;
	}

	// Fall back to legacy config file in user home directory
	const char *home = getenv("HOME");
	if (home && strlen(home)) {
		auto path = mstrjoin(home, config_filename_legacy);
		ret = fopen(path, "r");
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
		ret = open_config_file_at(config_dirs[i], ppath);
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
void parse_cfg_condlst(const config_t *pcfg, c2_lptr_t **pcondlst, const char *name) {
	config_setting_t *setting = config_lookup(pcfg, name);
	if (setting) {
		// Parse an array of options
		if (config_setting_is_array(setting)) {
			int i = config_setting_length(setting);
			while (i--)
				condlst_add(pcondlst,
				            config_setting_get_string_elem(setting, i));
		}
		// Treat it as a single pattern if it's a string
		else if (CONFIG_TYPE_STRING == config_setting_type(setting)) {
			condlst_add(pcondlst, config_setting_get_string(setting));
		}
	}
}

/**
 * Parse a window corner radius rule list in configuration file.
 */
static inline void
parse_cfg_condlst_corner(options_t *opt, const config_t *pcfg, const char *name) {
	config_setting_t *setting = config_lookup(pcfg, name);
	if (setting) {
		// Parse an array of options
		if (config_setting_is_array(setting)) {
			int i = config_setting_length(setting);
			while (i--)
				if (!parse_numeric_window_rule(
				        &opt->corner_radius_rules,
				        config_setting_get_string_elem(setting, i), 0, INT_MAX))
					exit(1);
		}
		// Treat it as a single pattern if it's a string
		else if (config_setting_type(setting) == CONFIG_TYPE_STRING) {
			if (!parse_numeric_window_rule(&opt->corner_radius_rules,
			                               config_setting_get_string(setting),
			                               0, INT_MAX))
				exit(1);
		}
	}
}

/**
 * Parse an opacity rule list in configuration file.
 */
static inline void
parse_cfg_condlst_opct(options_t *opt, const config_t *pcfg, const char *name) {
	config_setting_t *setting = config_lookup(pcfg, name);
	if (setting) {
		// Parse an array of options
		if (config_setting_is_array(setting)) {
			int i = config_setting_length(setting);
			while (i--)
				if (!parse_numeric_window_rule(
				        &opt->opacity_rules,
				        config_setting_get_string_elem(setting, i), 0, 100))
					exit(1);
		}
		// Treat it as a single pattern if it's a string
		else if (config_setting_type(setting) == CONFIG_TYPE_STRING) {
			if (!parse_numeric_window_rule(
			        &opt->opacity_rules, config_setting_get_string(setting), 0, 100))
				exit(1);
		}
	}
}

/**
 * Parse a window shader rule list in configuration file.
 */
static inline void parse_cfg_condlst_shader(options_t *opt, const config_t *pcfg,
                                            const char *name, const char *include_dir) {
	config_setting_t *setting = config_lookup(pcfg, name);
	if (setting) {
		// Parse an array of options
		if (config_setting_is_array(setting)) {
			int i = config_setting_length(setting);
			while (i--) {
				if (!parse_rule_window_shader(
				        &opt->window_shader_fg_rules,
				        config_setting_get_string_elem(setting, i),
				        include_dir)) {
					exit(1);
				}
			}
		}
		// Treat it as a single pattern if it's a string
		else if (config_setting_type(setting) == CONFIG_TYPE_STRING) {
			if (!parse_rule_window_shader(&opt->window_shader_fg_rules,
			                              config_setting_get_string(setting),
			                              include_dir)) {
				exit(1);
			}
		}
	}
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

		if (config_setting_lookup_int(setting, "corner-radius", &ival)) {
			o->corner_radius = ival;
			mask->corner_radius = true;
			// log_warn("%s: corner-radius: %d", member_name, ival);
		}
		if (config_setting_lookup_int(setting, "round-borders", &ival)) {
			o->round_borders = ival;
			mask->round_borders = true;
			// log_warn("%s: round_borders: %d", member_name, ival);
		}
	}
}

/**
 * Parse a configuration file from default location.
 *
 * Returns the actually config_file name
 */
char *parse_config_libconfig(options_t *opt, const char *config_file, bool *shadow_enable,
                             bool *fading_enable, bool *conv_kern_hasneg,
                             win_option_mask_t *winopt_mask) {

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
<<<<<<< HEAD
	}
=======

	// --transition-length
	if (config_lookup_int(&cfg, "transition-length", &ival))
		opt->transition_length = ival;
	// --transition-pow-x
	if (config_lookup_float(&cfg, "transition-pow-x", &dval))
		opt->transition_pow_x = dval;
	// --transition-pow-y
	if (config_lookup_float(&cfg, "transition-pow-y", &dval))
		opt->transition_pow_y = dval;
	// --transition-pow-w
	if (config_lookup_float(&cfg, "transition-pow-w", &dval))
		opt->transition_pow_w = dval;
	// --transition-pow-h
	if (config_lookup_float(&cfg, "transition-pow-h", &dval))
		opt->transition_pow_h = dval;
	// --size-transition
	lcfg_lookup_bool(&cfg, "size-transition", &opt->size_transition);
	// --spawn-center-screen
	lcfg_lookup_bool(&cfg, "spawn-center-screen", &opt->spawn_center_screen);
	// --spawn-center
	lcfg_lookup_bool(&cfg, "spawn-center", &opt->spawn_center);
	// --no-scale-down
	lcfg_lookup_bool(&cfg, "no-scale-down", &opt->no_scale_down);

>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
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
<<<<<<< HEAD
	}
=======
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
	// --corner-radius
	config_lookup_int(&cfg, "corner-radius", &opt->corner_radius);
	// --rounded-corners-exclude
	parse_cfg_condlst(&cfg, &opt->rounded_corners_blacklist, "rounded-corners-exclude");
<<<<<<< HEAD
	// --corner-radius-rules
	parse_cfg_condlst_corner(opt, &cfg, "corner-radius-rules");

=======
	// --round-borders
	config_lookup_int(&cfg, "round-borders", &opt->round_borders);
	// --round-borders-exclude
	parse_cfg_condlst(&cfg, &opt->round_borders_blacklist, "round-borders-exclude");
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
	// -e (frame_opacity)
	config_lookup_float(&cfg, "frame-opacity", &opt->frame_opacity);
	// -c (shadow_enable)
	lcfg_lookup_bool(&cfg, "shadow", shadow_enable);
	// -m (menu_opacity)
	if (config_lookup_float(&cfg, "menu-opacity", &dval)) {
		log_warn("Option `menu-opacity` is deprecated, and will be removed."
		         "Please use the wintype option `opacity` of `popup_menu`"
		         "and `dropdown_menu` instead.");
		opt->wintype_option[WINTYPE_DROPDOWN_MENU].opacity = dval;
		opt->wintype_option[WINTYPE_POPUP_MENU].opacity = dval;
		winopt_mask[WINTYPE_DROPDOWN_MENU].opacity = true;
		winopt_mask[WINTYPE_POPUP_MENU].opacity = true;
	}
	// -f (fading_enable)
	if (config_lookup_bool(&cfg, "fading", &ival))
		*fading_enable = ival;
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
	if (config_lookup_string(&cfg, "shadow-exclude-reg", &sval))
		opt->shadow_exclude_reg_str = strdup(sval);
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
	lcfg_lookup_bool(&cfg, "experimental-backends", &opt->experimental_backends);
	if (config_lookup_string(&cfg, "backend", &sval)) {
		opt->backend = parse_backend(sval);
		if (opt->backend >= NUM_BKEND) {
			log_fatal("Cannot parse backend");
			goto err;
		}
	}
	// --log-level
	if (config_lookup_string(&cfg, "log-level", &sval)) {
		auto level = string_to_log_level(sval);
		if (level == LOG_LEVEL_INVALID) {
			log_warn("Invalid log level, defaults to WARN");
		} else {
			log_set_level_tls(level);
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
		log_warn("The sw-opti %s", deprecation_message);
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
	// --transparent-clipping-exclude
	parse_cfg_condlst(&cfg, &opt->transparent_clipping_blacklist,
	                  "transparent-clipping-exclude");
	// --shadow-exclude
	parse_cfg_condlst(&cfg, &opt->shadow_blacklist, "shadow-exclude");
	// --clip-shadow-above
	parse_cfg_condlst(&cfg, &opt->shadow_clip_list, "clip-shadow-above");
	// --fade-exclude
	parse_cfg_condlst(&cfg, &opt->fade_blacklist, "fade-exclude");
	// --focus-exclude
	parse_cfg_condlst(&cfg, &opt->focus_blacklist, "focus-exclude");
	// --invert-color-include
	parse_cfg_condlst(&cfg, &opt->invert_color_list, "invert-color-include");
	// --blur-background-exclude
	parse_cfg_condlst(&cfg, &opt->blur_background_blacklist, "blur-background-exclude");
	// --opacity-rule
	parse_cfg_condlst_opct(opt, &cfg, "opacity-rule");
	// --unredir-if-possible-exclude
	parse_cfg_condlst(&cfg, &opt->unredir_if_possible_blacklist,
	                  "unredir-if-possible-exclude");
	// --blur-method
	if (config_lookup_string(&cfg, "blur-method", &sval)) {
		enum blur_method method = parse_blur_method(sval);
		if (method >= BLUR_METHOD_INVALID) {
			log_fatal("Invalid blur method %s", sval);
			goto err;
		}
		opt->blur_method = method;
	}
	// --blur-size
	config_lookup_int(&cfg, "blur-size", &opt->blur_radius);
	// --blur-deviation
	config_lookup_float(&cfg, "blur-deviation", &opt->blur_deviation);
	// --blur-strength
<<<<<<< HEAD
	config_lookup_int(&cfg, "blur-strength", &opt->blur_strength);
=======
	if (config_lookup_int(&cfg, "blur-strength", &ival) && ival) {
		opt->blur_strength = parse_kawase_blur_strength(ival);
	}
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
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
		opt->blur_kerns =
		    parse_blur_kern_lst(sval, conv_kern_hasneg, &opt->blur_kernel_count);
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
<<<<<<< HEAD

	// --window-shader-fg-rule
	parse_cfg_condlst_shader(opt, &cfg, "window-shader-fg-rule",
	                         config_get_include_dir(&cfg));

	// --glx-use-gpushader4
	if (config_lookup_bool(&cfg, "glx-use-gpushader4", &ival)) {
		log_error("glx-use-gpushader4 has been removed, please remove it "
		          "from your config file");
		goto err;
=======
	// --xrender-sync
	if (config_lookup_bool(&cfg, "xrender-sync", &ival) && ival) {
		log_warn("Please use xrender-sync-fence instead of xrender-sync.");
		opt->xrender_sync_fence = true;
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
	}
	// --xrender-sync-fence
	lcfg_lookup_bool(&cfg, "xrender-sync-fence", &opt->xrender_sync_fence);

	if (lcfg_lookup_bool(&cfg, "clear-shadow", &bval))
		log_warn("\"clear-shadow\" is removed as an option, and is always"
		         " enabled now. Consider removing it from your config file");
<<<<<<< HEAD
=======
	if (lcfg_lookup_bool(&cfg, "paint-on-overlay", &bval))
		log_warn("\"paint-on-overlay\" has been removed as an option, and "
		         "is enabled whenever possible");

	if (config_lookup_float(&cfg, "alpha-step", &dval))
		log_warn("\"alpha-step\" has been removed, compton now tries to make use"
		         " of all alpha values");

	const char *deprecation_message =
	    "has been removed. If you encounter problems "
	    "without this feature, please feel free to open a bug report";
	if (lcfg_lookup_bool(&cfg, "glx-use-copysubbuffermesa", &bval) && bval) {
		log_error("\"glx-use-copysubbuffermesa\" %s", deprecation_message);
		return ERR_PTR(-1);
	}
	if (lcfg_lookup_bool(&cfg, "glx-copy-from-front", &bval) && bval) {
		log_error("\"glx-copy-from-front\" %s", deprecation_message);
		return ERR_PTR(-1);
	}
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45

	config_setting_t *blur_cfg = config_lookup(&cfg, "blur");
	if (blur_cfg) {
		if (config_setting_lookup_string(blur_cfg, "method", &sval)) {
			enum blur_method method = parse_blur_method(sval);
			if (method >= BLUR_METHOD_INVALID) {
				log_warn("Invalid blur method %s, ignoring.", sval);
			} else {
				opt->blur_method = method;
			}
		}

		config_setting_lookup_int(blur_cfg, "size", &opt->blur_radius);

		if (config_setting_lookup_string(blur_cfg, "kernel", &sval)) {
			opt->blur_kerns = parse_blur_kern_lst(sval, conv_kern_hasneg,
			                                      &opt->blur_kernel_count);
			if (!opt->blur_kerns) {
				log_warn("Failed to parse blur kernel: %s", sval);
			}
		}

		config_setting_lookup_float(blur_cfg, "deviation", &opt->blur_deviation);
<<<<<<< HEAD
		config_setting_lookup_int(blur_cfg, "strength", &opt->blur_strength);
	}

	// --write-pid-path
	if (config_lookup_string(&cfg, "write-pid-path", &sval)) {
		if (*sval != '/') {
			log_warn("The write-pid-path in your configuration file is not"
			         " an absolute path");
		}
		opt->write_pid_path = strdup(sval);
=======

		if (config_setting_lookup_int(blur_cfg, "strength", &ival) && ival) {
			opt->blur_strength = parse_kawase_blur_strength(ival);
		}
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
	}

	// Wintype settings

	// XXX ! Refactor all the wintype_* arrays into a struct
	for (wintype_t i = 0; i < NUM_WINTYPES; ++i) {
		parse_wintype_config(&cfg, WINTYPES[i], &opt->wintype_option[i],
		                     &winopt_mask[i]);
	}

	// Compatibility with the old name for notification windows.
	parse_wintype_config(&cfg, "notify", &opt->wintype_option[WINTYPE_NOTIFICATION],
	                     &winopt_mask[WINTYPE_NOTIFICATION]);

	config_destroy(&cfg);
	return path;

err:
	config_destroy(&cfg);
	free(path);
	return ERR_PTR(-1);
}
