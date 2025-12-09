// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX

#include <picom/types.h>
#include <test.h>

#include "common.h"
#include "log.h"
#include "utils/dynarr.h"
#include "utils/kernel.h"
#include "utils/str.h"

#include "config.h"

struct debug_options global_debug_options;

const char *xdg_config_home(void) {
	char *xdgh = getenv("XDG_CONFIG_HOME");
	char *home = getenv("HOME");
	const char *default_dir = "/.config";

	if (!xdgh) {
		if (!home) {
			return NULL;
		}

		xdgh = mstrjoin(home, default_dir);
	} else {
		xdgh = strdup(xdgh);
	}

	return xdgh;
}

char **xdg_config_dirs(void) {
	char *xdgd = getenv("XDG_CONFIG_DIRS");
	size_t count = 0;

	if (!xdgd) {
		xdgd = "/etc/xdg";
	}

	for (int i = 0; xdgd[i]; i++) {
		if (xdgd[i] == ':') {
			count++;
		}
	}

	// Store the string and the result pointers together so they can be
	// freed together
	char **dir_list = cvalloc(sizeof(char *) * (count + 2) + strlen(xdgd) + 1);
	auto dirs = strcpy((char *)dir_list + sizeof(char *) * (count + 2), xdgd);
	auto path = dirs;

	for (size_t i = 0; i < count; i++) {
		dir_list[i] = path;
		path = strchr(path, ':');
		*path = '\0';
		path++;
	}
	dir_list[count] = path;

	size_t fill = 0;
	for (size_t i = 0; i <= count; i++) {
		if (dir_list[i][0] == '/') {
			dir_list[fill] = dir_list[i];
			fill++;
		}
	}

	dir_list[fill] = NULL;

	return dir_list;
}

TEST_CASE(xdg_config_dirs) {
	auto old_var = getenv("XDG_CONFIG_DIRS");
	if (old_var) {
		old_var = strdup(old_var);
	}
	unsetenv("XDG_CONFIG_DIRS");

	auto result = xdg_config_dirs();
	TEST_STREQUAL(result[0], "/etc/xdg");
	TEST_EQUAL(result[1], NULL);
	free(result);

	setenv("XDG_CONFIG_DIRS", ".:.:/etc/xdg:.:/:", 1);
	result = xdg_config_dirs();
	TEST_STREQUAL(result[0], "/etc/xdg");
	TEST_STREQUAL(result[1], "/");
	TEST_EQUAL(result[2], NULL);
	free(result);

	setenv("XDG_CONFIG_DIRS", ":", 1);
	result = xdg_config_dirs();
	TEST_EQUAL(result[0], NULL);
	free(result);

	if (old_var) {
		setenv("XDG_CONFIG_DIRS", old_var, 1);
		free(old_var);
	}
}

/**
 * Parse a long number.
 */
bool parse_long(const char *s, long *dest) {
	const char *endptr = NULL;
	long val = strtol(s, (char **)&endptr, 0);
	if (!endptr || endptr == s) {
		log_error("Invalid number: %s", s);
		return false;
	}
	while (isspace((unsigned char)*endptr)) {
		++endptr;
	}
	if (*endptr) {
		log_error("Trailing characters: %s", s);
		return false;
	}
	*dest = val;
	return true;
}

/**
 * Parse an int  number.
 */
bool parse_int(const char *s, int *dest) {
	long val;
	if (!parse_long(s, &val)) {
		return false;
	}
	if (val > INT_MAX || val < INT_MIN) {
		log_error("Number exceeded int limits: %ld", val);
		return false;
	}
	*dest = (int)val;
	return true;
}

/**
 * Parse a floating-point number in from a string,
 * also strips the trailing space and comma after the number.
 *
 * @param[in]  src  string to parse
 * @param[out] dest return the number parsed from the string
 * @return          pointer to the last character parsed
 */
const char *parse_readnum(const char *src, double *dest) {
	const char *pc = NULL;
	double val = strtod_simple(src, &pc);
	if (!pc || pc == src) {
		log_error("No number found: %s", src);
		return src;
	}
	while (*pc && (isspace((unsigned char)*pc) || *pc == ',')) {
		++pc;
	}
	*dest = val;
	return pc;
}

int parse_blur_method(const char *src) {
	if (strcmp(src, "box") == 0) {
		return BLUR_METHOD_BOX;
	}
	if (strcmp(src, "dual_kawase") == 0) {
		return BLUR_METHOD_DUAL_KAWASE;
	}
	if (strcmp(src, "gaussian") == 0) {
		return BLUR_METHOD_GAUSSIAN;
	}
	if (strcmp(src, "kernel") == 0) {
		return BLUR_METHOD_KERNEL;
	}
	if (strcmp(src, "none") == 0) {
		return BLUR_METHOD_NONE;
	}
	return BLUR_METHOD_INVALID;
}

/**
 * Parse a matrix.
 *
 * @param[in]  src    the blur kernel string
 * @param[out] endptr return where the end of kernel is in the string
 * @param[out] hasneg whether the kernel has negative values
 */
static conv *parse_blur_kern(const char *src, const char **endptr) {
	int width = 0, height = 0;

	const char *pc = NULL;

	// Get matrix width and height
	double val = 0.0;
	pc = parse_readnum(src, &val);
	if (src == pc) {
		goto err1;
	}
	src = pc;
	width = (int)val;
	pc = parse_readnum(src, &val);
	if (src == pc) {
		goto err1;
	}
	src = pc;
	height = (int)val;

	// Validate matrix width and height
	if (width <= 0 || height <= 0) {
		log_error("Blue kernel width/height can't be negative.");
		goto err1;
	}
	if (!(width % 2 && height % 2)) {
		log_error("Blur kernel width/height must be odd.");
		goto err1;
	}
	if (width > 16 || height > 16) {
		log_warn("Blur kernel width/height too large, may slow down"
		         "rendering, and/or consume lots of memory");
	}

	// Allocate memory
	conv *matrix = cvalloc(sizeof(conv) + (size_t)(width * height) * sizeof(double));

	// Read elements
	int skip = height / 2 * width + width / 2;
	for (int i = 0; i < width * height; ++i) {
		// Ignore the center element
		if (i == skip) {
			matrix->data[i] = 1;
			continue;
		}
		pc = parse_readnum(src, &val);
		if (src == pc) {
			goto err2;
		}
		src = pc;
		matrix->data[i] = val;
	}

	// Detect trailing characters
	for (; *pc && *pc != ';'; pc++) {
		if (!isspace((unsigned char)*pc) && *pc != ',') {
			// TODO(yshui) isspace is locale aware, be careful
			log_error("Trailing characters in blur kernel string.");
			goto err2;
		}
	}

	// Jump over spaces after ';'
	if (*pc == ';') {
		pc++;
		while (*pc && isspace((unsigned char)*pc)) {
			++pc;
		}
	}

	// Require an end of string if endptr is not provided, otherwise
	// copy end pointer to endptr
	if (endptr) {
		*endptr = pc;
	} else if (*pc) {
		log_error("Only one blur kernel expected.");
		goto err2;
	}

	// Fill in width and height
	matrix->w = width;
	matrix->h = height;
	return matrix;

err2:
	free(matrix);
err1:
	return NULL;
}

/**
 * Parse a list of convolution kernels.
 *
 * @param[in]  src    string to parse
 * @param[out] hasneg whether any of the kernels have negative values
 * @return            the kernels
 */
struct conv **parse_blur_kern_lst(const char *src, int *count) {
	// TODO(yshui) just return a predefined kernels, not parse predefined strings...
	static const struct {
		const char *name;
		const char *kern_str;
	} CONV_KERN_PREDEF[] = {
	    {"3x3box", "3,3,1,1,1,1,1,1,1,1,"},
	    {"5x5box", "5,5,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"},
	    {"7x7box", "7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"
	               "1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"},
	    {"3x3gaussian", "3,3,0.243117,0.493069,0.243117,0.493069,0.493069,0.243117,0."
	                    "493069,0.243117,"},
	    {"5x5gaussian", "5,5,0.003493,0.029143,0.059106,0.029143,0.003493,0.029143,0."
	                    "243117,0.493069,0.243117,0.029143,0.059106,0.493069,0."
	                    "493069,0.059106,0.029143,0.243117,0.493069,0.243117,0."
	                    "029143,0.003493,0.029143,0.059106,0.029143,0.003493,"},
	    {"7x7gaussian", "7,7,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0."
	                    "000003,0.000102,0.003493,0.029143,0.059106,0.029143,0."
	                    "003493,0.000102,0.000849,0.029143,0.243117,0.493069,0."
	                    "243117,0.029143,0.000849,0.001723,0.059106,0.493069,0."
	                    "493069,0.059106,0.001723,0.000849,0.029143,0.243117,0."
	                    "493069,0.243117,0.029143,0.000849,0.000102,0.003493,0."
	                    "029143,0.059106,0.029143,0.003493,0.000102,0.000003,0."
	                    "000102,0.000849,0.001723,0.000849,0.000102,0.000003,"},
	    {"9x9gaussian",
	     "9,9,0.000000,0.000000,0.000001,0.000006,0.000012,0.000006,0.000001,0."
	     "000000,0.000000,0.000000,0.000003,0.000102,0.000849,0.001723,0.000849,0."
	     "000102,0.000003,0.000000,0.000001,0.000102,0.003493,0.029143,0.059106,0."
	     "029143,0.003493,0.000102,0.000001,0.000006,0.000849,0.029143,0.243117,0."
	     "493069,0.243117,0.029143,0.000849,0.000006,0.000012,0.001723,0.059106,0."
	     "493069,0.493069,0.059106,0.001723,0.000012,0.000006,0.000849,0.029143,0."
	     "243117,0.493069,0.243117,0.029143,0.000849,0.000006,0.000001,0.000102,0."
	     "003493,0.029143,0.059106,0.029143,0.003493,0.000102,0.000001,0.000000,0."
	     "000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003,0.000000,0."
	     "000000,0.000000,0.000001,0.000006,0.000012,0.000006,0.000001,0.000000,0."
	     "000000,"},
	    {"11x11gaussian",
	     "11,11,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0."
	     "000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000001,0."
	     "000006,0.000012,0.000006,0.000001,0.000000,0.000000,0.000000,0.000000,0."
	     "000000,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003,0."
	     "000000,0.000000,0.000000,0.000001,0.000102,0.003493,0.029143,0.059106,0."
	     "029143,0.003493,0.000102,0.000001,0.000000,0.000000,0.000006,0.000849,0."
	     "029143,0.243117,0.493069,0.243117,0.029143,0.000849,0.000006,0.000000,0."
	     "000000,0.000012,0.001723,0.059106,0.493069,0.493069,0.059106,0.001723,0."
	     "000012,0.000000,0.000000,0.000006,0.000849,0.029143,0.243117,0.493069,0."
	     "243117,0.029143,0.000849,0.000006,0.000000,0.000000,0.000001,0.000102,0."
	     "003493,0.029143,0.059106,0.029143,0.003493,0.000102,0.000001,0.000000,0."
	     "000000,0.000000,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0."
	     "000003,0.000000,0.000000,0.000000,0.000000,0.000000,0.000001,0.000006,0."
	     "000012,0.000006,0.000001,0.000000,0.000000,0.000000,0.000000,0.000000,0."
	     "000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000,0."
	     "000000,"},
	};

	*count = 0;
	for (unsigned int i = 0;
	     i < sizeof(CONV_KERN_PREDEF) / sizeof(CONV_KERN_PREDEF[0]); ++i) {
		if (strcmp(CONV_KERN_PREDEF[i].name, src) == 0) {
			return parse_blur_kern_lst(CONV_KERN_PREDEF[i].kern_str, count);
		}
	}

	int nkernels = 1;
	for (int i = 0; src[i]; i++) {
		if (src[i] == ';') {
			nkernels++;
		}
	}

	struct conv **ret = ccalloc(nkernels, struct conv *);

	int i = 0;
	const char *pc = src;

	// Continue parsing until the end of source string
	i = 0;
	while (pc && *pc) {
		assert(i < nkernels);
		ret[i] = parse_blur_kern(pc, &pc);
		if (!ret[i]) {
			for (int j = 0; j < i; j++) {
				free(ret[j]);
			}
			free(ret);
			return NULL;
		}
		i++;
	}

	if (i > 1) {
		log_warn("You are seeing this message because you are using "
		         "multipass blur. Please report an issue to us so we know "
		         "multipass blur is actually been used. Otherwise it might be "
		         "removed in future releases");
	}

	*count = i;

	return ret;
}

void *parse_numeric_prefix(const char *src, const char **end, void *user_data) {
	int *minmax = user_data;
	*end = NULL;
	if (!src) {
		return NULL;
	}

	// Find numeric value
	char *endptr = NULL;
	long val = strtol(src, &endptr, 0);
	if (!endptr || endptr == src) {
		log_error("No number specified: %s", src);
		return NULL;
	}

	if (val < minmax[0] || val > minmax[1]) {
		log_error("Number not in range (%d <= n <= %d): %s", minmax[0], minmax[1], src);
		return NULL;
	}

	// Skip over spaces
	while (*endptr && isspace((unsigned char)*endptr)) {
		++endptr;
	}
	if (':' != *endptr) {
		log_error("Number separator (':') not found: %s", src);
		return NULL;
	}
	++endptr;

	*end = endptr;

	return (void *)val;
}

/// Search for auxiliary file under a base directory
static char *locate_auxiliary_file_at(const char *base, const char *scope, const char *file) {
	scoped_charp path = mstrjoin(base, scope);
	mstrextend(&path, "/");
	mstrextend(&path, file);
	if (access(path, O_RDONLY) == 0) {
		// Canonicalize path to avoid duplicates
		char *abspath = realpath(path, NULL);
		return abspath;
	}
	return NULL;
}

/**
 * Get a path of an auxiliary file to read, could be a shader file, or any supplementary
 * file.
 *
 * Follows the XDG specification to search for the shader file in configuration locations.
 *
 * The search order is:
 *   1) If an absolute path is given, use it directly.
 *   2) Search for the file directly under `include_dir`.
 *   3) Search for the file in the XDG configuration directories, under path
 *      /picom/<scope>/
 */
char *locate_auxiliary_file(const char *scope, const char *path, const char *include_dir) {
	if (!path || strlen(path) == 0) {
		return NULL;
	}

	// Filename is absolute path, so try to load from there
	if (path[0] == '/') {
		if (access(path, O_RDONLY) == 0) {
			return realpath(path, NULL);
		}
	}

	// First try to load file from the include directory (i.e. relative to the
	// config file)
	if (include_dir && strlen(include_dir)) {
		char *ret = locate_auxiliary_file_at(include_dir, "", path);
		if (ret) {
			return ret;
		}
	}

	// Fall back to searching in user config directory
	scoped_charp picom_scope = mstrjoin("/picom/", scope);
	scoped_charp config_home = (char *)xdg_config_home();
	if (config_home) {
		char *ret = locate_auxiliary_file_at(config_home, picom_scope, path);
		if (ret) {
			return ret;
		}
	}

	// Fall back to searching in system config directory
	auto config_dirs = xdg_config_dirs();
	for (int i = 0; config_dirs[i]; i++) {
		char *ret = locate_auxiliary_file_at(config_dirs[i], picom_scope, path);
		if (ret) {
			free(config_dirs);
			return ret;
		}
	}
	free(config_dirs);

	return NULL;
}

struct debug_options_entry {
	const char *name;
	const char **choices;
	size_t offset;
};

// clang-format off
const char *vblank_scheduler_str[] = {
	[VBLANK_SCHEDULER_PRESENT] = "present",
	[VBLANK_SCHEDULER_SGI_VIDEO_SYNC] = "sgi_video_sync",
	[LAST_VBLANK_SCHEDULER] = NULL
};
static const struct debug_options_entry debug_options_entries[] = {
    {"always_rebind_pixmap" , NULL                , offsetof(struct debug_options, always_rebind_pixmap)},
    {"smart_frame_pacing"   , NULL                , offsetof(struct debug_options, smart_frame_pacing)},
    {"force_vblank_sched"   , vblank_scheduler_str, offsetof(struct debug_options, force_vblank_scheduler)},
    {"consistent_buffer_age", NULL                , offsetof(struct debug_options, consistent_buffer_age)},
};
// clang-format on

void parse_debug_option_single(char *setting, struct debug_options *debug_options) {
	char *equal = strchr(setting, '=');
	size_t name_len = equal ? (size_t)(equal - setting) : strlen(setting);
	for (size_t i = 0; i < ARR_SIZE(debug_options_entries); i++) {
		if (strncmp(setting, debug_options_entries[i].name, name_len) != 0) {
			continue;
		}
		if (debug_options_entries[i].name[name_len] != '\0') {
			continue;
		}
		auto value = (int *)((void *)debug_options + debug_options_entries[i].offset);
		if (equal) {
			const char *const arg = equal + 1;
			if (debug_options_entries[i].choices != NULL) {
				for (size_t j = 0; debug_options_entries[i].choices[j]; j++) {
					if (strcmp(arg, debug_options_entries[i].choices[j]) ==
					    0) {
						*value = (int)j;
						return;
					}
				}
			}
			if (!parse_int(arg, value)) {
				log_error("Invalid value for debug option %s: %s, it "
				          "will be ignored.",
				          debug_options_entries[i].name, arg);
			}
		} else if (debug_options_entries[i].choices == NULL) {
			*value = 1;
		} else {
			log_error(
			    "Missing value for debug option %s, it will be ignored.", setting);
		}
		return;
	}
	log_error("Invalid debug option: %s", setting);
}

/// Parse debug options from environment variable `PICOM_DEBUG`.
void parse_debug_options(struct debug_options *debug_options) {
	const char *debug = getenv("PICOM_DEBUG");
	const struct debug_options default_debug_options = {
	    .force_vblank_scheduler = LAST_VBLANK_SCHEDULER,
	};

	*debug_options = default_debug_options;
	if (!debug) {
		return;
	}

	scoped_charp debug_copy = strdup(debug);
	char *tmp, *needle = strtok_r(debug_copy, ";", &tmp);
	while (needle) {
		parse_debug_option_single(needle, debug_options);
		needle = strtok_r(NULL, ";", &tmp);
	}
}

void *parse_window_shader_prefix(const char *src, const char **end, void *user_data) {
	const char *include_dir = user_data;
	*end = NULL;
	if (!src) {
		return NULL;
	}

	// Find custom shader terminator
	const char *endptr = strchr(src, ':');
	if (!endptr) {
		log_error("Custom shader terminator not found: %s", src);
		return NULL;
	}

	// Parse and create custom shader
	scoped_charp untrimed_shader_source = strdup(src);
	if (!untrimed_shader_source) {
		return NULL;
	}
	auto source_end = strchr(untrimed_shader_source, ':');
	*source_end = '\0';

	size_t length;
	char *tmp = (char *)trim_both(untrimed_shader_source, &length);
	tmp[length] = '\0';
	struct shader_specification *shader_source = NULL;

	if (strcasecmp(tmp, "default") != 0) {
		char *full_path = locate_auxiliary_file("shaders", tmp, include_dir);
		if (!full_path) {
			log_error("Custom shader file \"%s\" not found for rule: %s", tmp, src);
			return NULL;
		}
		shader_source = shader_spec_from_path(full_path);
		free(full_path);
	}

	*end = endptr + 1;
	return shader_source;
}
void *parse_window_shader_prefix_with_cwd(const char *src, const char **end, void * /*data*/) {
	scoped_charp cwd = getcwd(NULL, 0);
	return parse_window_shader_prefix(src, end, cwd);
}

bool load_plugin(const char *name, const char *include_dir) {
	scoped_charp path = locate_auxiliary_file("plugins", optarg, include_dir);
	void *handle = NULL;
	if (!path) {
		handle = dlopen(name, RTLD_LAZY);
	} else {
		log_debug("Plugin %s resolved to %s", name, path);
		handle = dlopen(path, RTLD_LAZY);
	}
	return handle != NULL;
}

bool parse_config(options_t *opt, const char *config_file) {
	// clang-format off
	*opt = (struct options){
	    .glx_no_stencil = false,
	    .mark_wmwin_focused = false,
	    .mark_ovredir_focused = false,
	    .detect_rounded_corners = false,
	    .resize_damage = 0,
	    .unredir_if_possible = false,
	    .unredir_if_possible_blacklist = NULL,
	    .unredir_if_possible_delay = 0,
	    .redirected_force = UNSET,
	    .stoppaint_force = UNSET,
	    .dbus = false,
	    .benchmark = 0,
	    .benchmark_wid = XCB_NONE,
	    .logpath = NULL,
	    .log_level = LOG_LEVEL_WARN,

	    .use_damage = true,
	    .frame_pacing = true,

	    .shadow_red = 0.0,
	    .shadow_green = 0.0,
	    .shadow_blue = 0.0,
	    .shadow_radius = 18,
	    .shadow_offset_x = -15,
	    .shadow_offset_y = -15,
	    .shadow_opacity = .75,
	    .shadow_blacklist = NULL,
	    .shadow_ignore_shaped = false,
	    .crop_shadow_to_monitor = false,
	    .shadow_clip_list = NULL,

	    .corner_radius = 0,

	    .fade_in_step = 0.028,
	    .fade_out_step = 0.03,
	    .fade_delta = 10,
	    .no_fading_openclose = false,
	    .no_fading_destroyed_argb = false,
	    .fade_blacklist = NULL,

	    .inactive_opacity = 1.0,
	    .inactive_opacity_override = false,
	    .active_opacity = 1.0,
	    .frame_opacity = 1.0,
	    .detect_client_opacity = false,

	    .blur_method = BLUR_METHOD_NONE,
	    .blur_radius = 3,
	    .blur_deviation = 0.84089642,
	    .blur_strength = 5,
	    .blur_background_frame = false,
	    .blur_background_fixed = false,
	    .blur_background_blacklist = NULL,
	    .blur_kerns = NULL,
	    .blur_kernel_count = 0,
	    .window_shader_fg = NULL,
	    .window_shader_fg_rules = NULL,
	    .inactive_dim = 0.0,
	    .inactive_dim_fixed = false,
	    .invert_color_list = NULL,
	    .opacity_rules = NULL,
	    .max_brightness = 1.0,

	    .use_ewmh_active_win = false,
	    .focus_blacklist = NULL,
	    .detect_transient = false,
	    .detect_client_leader = false,
	    .no_ewmh_fullscreen = false,

	    .track_leader = false,

	    .rounded_corners_blacklist = NULL,

	    .rules = NULL,
	};
	// clang-format on

	list_init_head(&opt->included_config_files);
	list_init_head(&opt->unredir_if_possible_blacklist);
	list_init_head(&opt->paint_blacklist);
	list_init_head(&opt->shadow_blacklist);
	list_init_head(&opt->shadow_clip_list);
	list_init_head(&opt->fade_blacklist);
	list_init_head(&opt->blur_background_blacklist);
	list_init_head(&opt->invert_color_list);
	list_init_head(&opt->window_shader_fg_rules);
	list_init_head(&opt->opacity_rules);
	list_init_head(&opt->rounded_corners_blacklist);
	list_init_head(&opt->corner_radius_rules);
	list_init_head(&opt->focus_blacklist);
	list_init_head(&opt->transparent_clipping_blacklist);
	list_init_head(&opt->rules);

	opt->all_scripts = dynarr_new(struct script *, 4);
	return parse_config_libconfig(opt, config_file);
}
