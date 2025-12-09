// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <getopt.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX

#include "backend/backend.h"
#include "c2.h"
#include "config.h"
#include "log.h"
#include "options.h"
#include "transition/script.h"
#include "utils/dynarr.h"
#include "utils/misc.h"
#include "utils/str.h"
#include "x.h"

#pragma GCC diagnostic error "-Wunused-parameter"

struct picom_option;

struct picom_arg {
	const char *name;
	ptrdiff_t offset;

	const void *user_data;
	bool (*handler)(const struct picom_option *, const struct picom_arg *,
	                const char *optarg, void *output);
};

struct picom_arg_parser {
	int (*parse)(const char *);
	int invalid_value;
};

struct picom_rules_parser {
	void *(*parse_prefix)(const char *, const char **end, void *data);
	void (*free_value)(void *);
	void *user_data;
};

struct picom_deprecated_arg {
	const char *message;
	struct picom_arg inner;
	bool error;
};

struct picom_option {
	const char *long_name;
	int has_arg;
	struct picom_arg arg;
	const char *help;
	const char *argv0;
};

static bool set_flag(const struct picom_option * /*opt*/, const struct picom_arg *arg,
                     const char * /*arg_str*/, void *output) {
	*(bool *)(output + arg->offset) = true;
	return true;
}

static bool set_rule_flag(const struct picom_option *arg_opt, const struct picom_arg *arg,
                          const char * /*arg_str*/, void *output) {
	auto opt = (struct options *)output;
	if (!list_is_empty(&opt->rules)) {
		log_warn_both_style_of_rules(opt, arg_opt->long_name);
		return true;
	}
	*(bool *)(output + arg->offset) = true;
	return true;
}
static bool unset_flag(const struct picom_option * /*opt*/, const struct picom_arg *arg,
                       const char * /*arg_str*/, void *output) {
	*(bool *)(output + arg->offset) = false;
	return true;
}

static bool parse_with(const struct picom_option *opt, const struct picom_arg *arg,
                       const char *arg_str, void *output) {
	const struct picom_arg_parser *parser = arg->user_data;
	int *dst = (int *)(output + arg->offset);
	*dst = parser->parse(arg_str);
	if (*dst == parser->invalid_value) {
		log_error("Invalid argument for option `--%s`: %s", opt->long_name, arg_str);
		return false;
	}
	return true;
}

static bool store_float(const struct picom_option *opt, const struct picom_arg *arg,
                        const char *arg_str, void *output) {
	double *dst = (double *)(output + arg->offset);
	const double *minmax = (const double *)arg->user_data;
	const char *endptr = NULL;
	*dst = strtod_simple(arg_str, &endptr);
	if (!endptr || *endptr != '\0') {
		log_error("Argument for option `--%s` is not a valid float number: %s",
		          opt->long_name, arg_str);
		return false;
	}
	*dst = max2(minmax[0], min2(*dst, minmax[1]));
	return true;
}

static bool store_rule_float(const struct picom_option *arg_opt, const struct picom_arg *arg,
                             const char *arg_str, void *output) {
	auto opt = (struct options *)output;
	if (!list_is_empty(&opt->rules)) {
		log_warn_both_style_of_rules(opt, arg_opt->long_name);
		return true;
	}
	return store_float(arg_opt, arg, arg_str, output);
}

static bool store_int(const struct picom_option *opt, const struct picom_arg *arg,
                      const char *arg_str, void *output) {
	const int *minmax = (const int *)arg->user_data;
	int *dst = (int *)(output + arg->offset);
	if (!parse_int(arg_str, dst)) {
		log_error("Argument for option `--%s` is not a valid integer: %s",
		          opt->long_name, arg_str);
		return false;
	}
	*dst = max2(minmax[0], min2(*dst, minmax[1]));
	return true;
}

static bool store_string(const struct picom_option * /*opt*/, const struct picom_arg *arg,
                         const char *arg_str, void *output) {
	char **dst = (char **)(output + arg->offset);
	free(*dst);
	*dst = strdup(arg_str);
	return true;
}

static bool store_shader(const struct picom_option *opt, const struct picom_arg *arg,
                         const char *arg_str, void *output) {
	scoped_charp cwd = getcwd(NULL, 0);
	scoped_charp full_path = locate_auxiliary_file("shaders", arg_str, cwd);
	if (!full_path) {
		log_error("Couldn't find shader file \"%s\" for %s", arg_str, opt->long_name);
		return false;
	}

	auto dst = (struct shader_specification **)(output + arg->offset);
	free(*dst);
	*dst = shader_spec_from_path(full_path);
	return true;
}

static bool
store_fixed_string(const struct picom_option * /*opt*/, const struct picom_arg *arg,
                   const char * /*arg_str*/, void *output) {
	char **dst = (char **)(output + arg->offset);
	free(*dst);
	*dst = strdup((const char *)arg->user_data);
	return true;
}

static bool store_rules(const struct picom_option *arg_opt, const struct picom_arg *arg,
                        const char *arg_str, void *output) {
	const struct picom_rules_parser *parser = arg->user_data;
	struct options *opt = (struct options *)output;
	if (!list_is_empty(&opt->rules)) {
		log_warn_both_style_of_rules(opt, arg_opt->long_name);
		return true;
	}
	auto rules = (struct list_node *)(output + arg->offset);
	bool deprecated = false, succeeded;
	if (!parser->parse_prefix) {
		succeeded = c2_parse(rules, arg_str, NULL, &deprecated) != NULL;
	} else {
		succeeded = c2_parse_with_prefix(rules, arg_str, parser->parse_prefix,
		                                 parser->free_value, parser->user_data,
		                                 &deprecated);
	}
	if (deprecated) {
		log_warn("Rule option --%s contains deprecated syntax (see above).",
		         arg_opt->long_name);
		record_problematic_option(opt, arg_opt->long_name);
	}
	return succeeded;
}

static bool store_fixed_enum(const struct picom_option * /*opt*/, const struct picom_arg *arg,
                             const char * /*arg_str*/, void *output) {
	const int *value = (const int *)arg->user_data;
	*(int *)(output + arg->offset) = *value;
	return true;
}

static bool noop(const struct picom_option * /*opt*/, const struct picom_arg * /*arg*/,
                 const char * /*arg_str*/, void * /*output*/) {
	return true;
}

static bool reject(const struct picom_option * /*opt*/, const struct picom_arg * /*arg*/,
                   const char * /*arg_str*/, void * /*output*/) {
	return false;
}

static bool say_deprecated(const struct picom_option *opt, const struct picom_arg *arg,
                           const char *arg_str, void *output) {
	const struct picom_deprecated_arg *deprecation = arg->user_data;
	report_deprecated_option(output, opt->long_name, deprecation->error);
	return deprecation->inner.handler(opt, &deprecation->inner, arg_str, output);
}

#define OFFSET(member) offsetof(struct options, member)

#define ENABLE(member)                                                                   \
	no_argument, {                                                                   \
		.offset = OFFSET(member), .handler = set_flag,                           \
	}

/// A true or false option that functions like a window rule. Which is superseded by the
/// `rules` option.
#define ENABLE_RULE(member)                                                              \
	no_argument, {                                                                   \
		.offset = OFFSET(member), .handler = set_rule_flag,                      \
	}

#define DISABLE(member)                                                                  \
	no_argument, {                                                                   \
		.offset = OFFSET(member), .handler = unset_flag,                         \
	}

#define IGNORE(has_arg)                                                                  \
	has_arg, {                                                                       \
		.handler = noop,                                                         \
	}

#define REJECT(has_arg)                                                                  \
	has_arg, {                                                                       \
		.handler = reject,                                                       \
	}

#define DO(fn)                                                                           \
	required_argument, {                                                             \
		.handler = (fn),                                                         \
	}

#define PARSE_WITH(fn, invalid, member)                                                  \
	required_argument, {                                                             \
		.offset = OFFSET(member), .handler = parse_with,                         \
		.user_data = (struct picom_arg_parser[]){{                               \
		    .invalid_value = (invalid),                                          \
		    .parse = (fn),                                                       \
		}},                                                                      \
	}

#define FLOAT(member, min, max)                                                          \
	required_argument, {                                                             \
		.offset = OFFSET(member), .handler = store_float,                        \
		.user_data = (double[]){min, max},                                       \
	}

/// A float option that functions like a window rule. Which is superseded by the `rules`
/// option.
#define FLOAT_RULE(member, min, max)                                                     \
	required_argument, {                                                             \
		.offset = OFFSET(member), .handler = store_rule_float,                   \
		.user_data = (double[]){min, max},                                       \
	}

#define INTEGER(member, min, max)                                                        \
	required_argument, {                                                             \
		.offset = OFFSET(member), .handler = store_int,                          \
		.user_data = (int[]){min, max},                                          \
	}

#define NAMED_STRING(member, name_)                                                      \
	required_argument, {                                                             \
		.offset = OFFSET(member), .handler = store_string, .name = (name_)       \
	}

#define SHADER(member)                                                                   \
	required_argument, {                                                             \
		.offset = OFFSET(member), .handler = store_shader, .name = "PATH",       \
	}

#define STRING(member) NAMED_STRING(member, NULL)

#define NAMED_RULES(member, name_, ...)                                                  \
	required_argument, {                                                             \
		.offset = OFFSET(member), .handler = store_rules, .name = (name_),       \
		.user_data = (struct picom_rules_parser[]) {                             \
			__VA_ARGS__                                                      \
		}                                                                        \
	}
#define NUMERIC_RULES(member, value, min, max)                                           \
	NAMED_RULES(member, value ":COND",                                               \
	            {.parse_prefix = parse_numeric_prefix, .user_data = (int[]){min, max}})
#define RULES(member) NAMED_RULES(member, "COND", {})

#define FIXED(member, value)                                                             \
	no_argument, {                                                                   \
		.offset = OFFSET(member), .handler = store_fixed_enum,                   \
		.user_data = (int[]){value},                                             \
	}
#define FIXED_STR(member, str)                                                           \
	no_argument, {                                                                   \
		.offset = OFFSET(member), .handler = store_fixed_string,                 \
		.user_data = (void *)(str)                                               \
	}

#define SAY_DEPRECATED_(error_, msg, has_arg, ...)                                        \
	has_arg, {                                                                        \
		.handler = say_deprecated, .user_data = (struct picom_deprecated_arg[]) { \
			{.message = (msg), .inner = __VA_ARGS__, .error = error_},        \
		}                                                                         \
	}
#define SAY_DEPRECATED(error_, msg, ...) SAY_DEPRECATED_(error_, msg, __VA_ARGS__)

#define WARN_DEPRECATED(...)                                                             \
	SAY_DEPRECATED_(false,                                                           \
	                "If you encounter problems without this feature, please "        \
	                "feel free to open a bug report.",                               \
	                __VA_ARGS__)

#define WARN_DEPRECATED_ENABLED(...)                                                     \
	SAY_DEPRECATED_(false, "Its functionality will always be enabled. ", __VA_ARGS__)

#define ERROR_DEPRECATED(has_arg) SAY_DEPRECATED(true, "", REJECT(has_arg))

static bool
store_shadow_color(const struct picom_option * /*opt*/, const struct picom_arg * /*arg*/,
                   const char *arg_str, void *output) {
	struct options *opt = (struct options *)output;
	struct color rgb;
	rgb = hex_to_rgb(arg_str);
	opt->shadow_red = rgb.red;
	opt->shadow_green = rgb.green;
	opt->shadow_blue = rgb.blue;
	return true;
}

static bool
store_blur_kern(const struct picom_option * /*opt*/, const struct picom_arg * /*arg*/,
                const char *arg_str, void *output) {
	struct options *opt = (struct options *)output;
	opt->blur_kerns = parse_blur_kern_lst(arg_str, &opt->blur_kernel_count);
	return opt->blur_kerns != NULL;
}

static bool
store_benchmark_wid(const struct picom_option * /*opt*/, const struct picom_arg * /*arg*/,
                    const char *arg_str, void *output) {
	struct options *opt = (struct options *)output;
	const char *endptr = NULL;
	opt->benchmark_wid = (xcb_window_t)strtoul(arg_str, (char **)&endptr, 0);
	if (!endptr || *endptr != '\0') {
		log_error("Invalid window ID for `--benchmark-wid`: %s", arg_str);
		return false;
	}
	return true;
}

static bool store_backend(const struct picom_option * /*opt*/, const struct picom_arg * /*arg*/,
                          const char *arg_str, void *output) {
	struct options *opt = (struct options *)output;
	opt->backend = backend_find(arg_str);
	if (opt->backend == NULL) {
		log_error("Invalid backend: %s", arg_str);
		return false;
	}
	return true;
}

#define WINDOW_SHADER_RULE                                                               \
	{.parse_prefix = parse_window_shader_prefix_with_cwd, .free_value = free}

#ifdef CONFIG_OPENGL
#define BACKENDS "xrender, glx"
#else
#define BACKENDS "xrender"
#endif

// clang-format off
static const struct option *longopts = NULL;
static const struct picom_option picom_options[] = {
    // As you can see, aligning this table is difficult...

    // Rejected options, we shouldn't be able to reach `get_cfg` when these are set
    ['h'] = {"help"   , REJECT(no_argument), "Print this help message and exit."},
    [318] = {"version", REJECT(no_argument), "Print version number and exit."},

    // Ignored options, these are already handled by `get_early_cfg`
    [314] = {"show-all-xerrors", IGNORE(no_argument)},
    ['b'] = {"daemon"          , IGNORE(no_argument)      , "Daemonize process."},
    [256] = {"config"          , IGNORE(required_argument), "Path to the configuration file."},
    [307] = {"plugins"         , IGNORE(required_argument), "Plugins to load. Can be specified multiple times, each time with a single plugin."},

    // "Rule-like" options
    [262] = {"mark-wmwin-focused"       , ENABLE_RULE(mark_wmwin_focused)       , "Try to detect WM windows and mark them as active."},
    [264] = {"mark-ovredir-focused"     , ENABLE_RULE(mark_ovredir_focused)     , "Mark windows that have no WM frame as active."},
    [266] = {"shadow-ignore-shaped"     , ENABLE_RULE(shadow_ignore_shaped)     , "Do not paint shadows on shaped windows. (Deprecated, use --shadow-exclude "
                                                                                  "\'bounding_shaped\' or --shadow-exclude \'bounding_shaped && "
                                                                                  "!rounded_corners\' instead.)"},
    [260] = {"inactive-opacity-override", ENABLE_RULE(inactive_opacity_override), "Inactive opacity set by -i overrides value of _NET_WM_WINDOW_OPACITY."},
    [297] = {"active-opacity"           , FLOAT_RULE(active_opacity, 0, 1)      , "Default opacity for active windows. (0.0 - 1.0)"},
    [261] = {"inactive-dim"             , FLOAT_RULE(inactive_dim, 0, 1)        , "Dim inactive windows. (0.0 - 1.0, defaults to 0)"},
    ['i'] = {"inactive-opacity"         , FLOAT_RULE(inactive_opacity, 0, 1)    , "Opacity of inactive windows. (0.0 - 1.0)"},

    // Simple flags
    ['c'] = {"shadow"                   , ENABLE(shadow_enable)            , "Enabled client-side shadows on windows."},
    ['f'] = {"fading"                   , ENABLE(fading_enable)            , "Fade windows in/out when opening/closing and when opacity changes, "
                                                                             "unless --no-fading-openclose is used."},
    [265] = {"no-fading-openclose"      , ENABLE(no_fading_openclose)      , "Do not fade on window open/close."},
    [268] = {"detect-client-opacity"    , ENABLE(detect_client_opacity)    , "Detect _NET_WM_WINDOW_OPACITY on client windows, useful for window "
                                                                             "managers not passing _NET_WM_WINDOW_OPACITY of client windows to frame"},
    [270] = {"vsync"                    , ENABLE(vsync)                    , "Enable VSync"},
    [271] = {"crop-shadow-to-monitor"   , ENABLE(crop_shadow_to_monitor)   , "Crop shadow of a window fully on a particular monitor to that monitor. "
                                                                             "This is currently implemented using the X RandR extension"},
    [276] = {"use-ewmh-active-win"      , ENABLE(use_ewmh_active_win)      , "Use _NET_WM_ACTIVE_WINDOW on the root window to determine which window is "
                                                                             "focused instead of using FocusIn/Out events"},
    [278] = {"unredir-if-possible"      , ENABLE(unredir_if_possible)      , "Unredirect all windows if a full-screen opaque window is detected, to "
                                                                             "maximize performance for full-screen applications."},
    [280] = {"inactive-dim-fixed"       , ENABLE(inactive_dim_fixed)       , "Use fixed inactive dim value."},
    [281] = {"detect-transient"         , ENABLE(detect_transient)         , "Use WM_TRANSIENT_FOR to group windows, and consider windows in the same "
                                                                             "group focused at the same time."},
    [282] = {"detect-client-leader"     , ENABLE(detect_client_leader)     , "Use WM_CLIENT_LEADER to group windows, and consider windows in the same group "
                                                                             "focused at the same time. This usually means windows from the same application "
                                                                             "will be considered focused or unfocused at the same time. WM_TRANSIENT_FOR has "
                                                                             "higher priority if --detect-transient is enabled, too."},
    [284] = {"blur-background-frame"    , ENABLE(blur_background_frame)    , "Blur background of windows when the window frame is not opaque. Implies "
                                                                             "--blur-background."},
    [285] = {"blur-background-fixed"    , ENABLE(blur_background_fixed)    , "Use fixed blur strength instead of adjusting according to window opacity."},
#ifdef CONFIG_DBUS
    [286] = {"dbus"                     , ENABLE(dbus)                     , "Enable remote control via D-Bus. See the D-BUS API section in the man page "
                                                                             "for more details."},
#endif
    [311] = {"vsync-use-glfinish"       , ENABLE(vsync_use_glfinish)},
    [313] = {"xrender-sync-fence"       , ENABLE(xrender_sync_fence)       , "Additionally use X Sync fence to sync clients' draw calls. Needed on "
                                                                             "nvidia-drivers with GLX backend for some users."},
    [315] = {"no-fading-destroyed-argb" , ENABLE(no_fading_destroyed_argb) , "Do not fade destroyed ARGB windows with WM frame. Workaround bugs in Openbox, "
                                                                             "Fluxbox, etc."},
    [316] = {"force-win-blend"          , ENABLE(force_win_blend)          , "Force all windows to be painted with blending. Useful if you have a custom "
                                                                             "shader that could turn opaque pixels transparent."},
    [319] = {"no-x-selection"           , ENABLE(no_x_selection)},
    [323] = {"use-damage"               , ENABLE(use_damage)               , "Render only the damaged (changed) part of the screen"},
    [324] = {"no-use-damage"            , DISABLE(use_damage)              , "Disable the use of damage information. This cause the whole screen to be"
                                                                             "redrawn every time, instead of the part of the screen that has actually "
                                                                             "changed. Potentially degrades the performance, but might fix some artifacts."},
    [267] = {"detect-rounded-corners"   , ENABLE(detect_rounded_corners)   , "Try to detect windows with rounded corners and don't consider them shaped "
                                                                             "windows. Affects --shadow-ignore-shaped, --unredir-if-possible, and "
                                                                             "possibly others. You need to turn this on manually if you want to match "
                                                                             "against rounded_corners in conditions."},
    [298] = {"glx-no-rebind-pixmap"     , WARN_DEPRECATED(ENABLE(glx_no_rebind_pixmap))},
    [291] = {"glx-no-stencil"           , WARN_DEPRECATED(ENABLE(glx_no_stencil))},
    [325] = {"no-vsync"                 , DISABLE(vsync)                   , "Disable VSync"},
    [327] = {"transparent-clipping"     , ENABLE(transparent_clipping)     , "Make transparent windows clip other windows like non-transparent windows do, "
                                                                             "instead of blending on top of them"},
    [339] = {"dithered-present"         , ENABLE(dithered_present)         , "Use higher precision during rendering, and apply dither when presenting the "
                                                                             "rendered screen. Reduces banding artifacts, but might cause performance "
                                                                             "degradation. Only works with OpenGL."},
    [341] = {"no-frame-pacing"          , DISABLE(frame_pacing)            , "Disable frame pacing. This might increase the latency."},
    [733] = {"legacy-backends"          , WARN_DEPRECATED(ENABLE(use_legacy_backends)), NULL},
    [800] = {"monitor-repaint"          , ENABLE(monitor_repaint)          , "Highlight the updated area of the screen. For debugging."},
    [801] = {"diagnostics"              , ENABLE(print_diagnostics)        , "Print diagnostic information"},
    [802] = {"debug-mode"               , ENABLE(debug_mode)               , "Render into a separate window, and don't take over the screen. Useful when "
                                                                             "you want to attach a debugger to picom"},
    [803] = {"no-ewmh-fullscreen"       , ENABLE(no_ewmh_fullscreen)       , "Do not use EWMH to detect fullscreen windows. Reverts to checking if a "
                                                                             "window is fullscreen based only on its size and coordinates."},
    [804] = {"realtime"                 , ENABLE(use_realtime_scheduling)  , "Enable realtime scheduling. This might reduce latency, but might also cause "
                                                                             "other issues. Disable this if you see the compositor being killed."},
    [805] = {"monitor"                  , ENABLE(inspect_monitor)          , "For picom-inspect, run in a loop and dump information every time something "
                                                                             "changed about a window.", "picom-inspect"},

    // Flags that takes an argument
    ['r'] = {"shadow-radius"               , INTEGER(shadow_radius, 0, INT_MAX)             , "The blur radius for shadows. (default 12)"},
    ['o'] = {"shadow-opacity"              , FLOAT(shadow_opacity, 0, 1)                    , "The translucency for shadows. (default .75)"},
    ['l'] = {"shadow-offset-x"             , INTEGER(shadow_offset_x, INT_MIN, INT_MAX)     , "The left offset for shadows. (default -15)"},
    ['t'] = {"shadow-offset-y"             , INTEGER(shadow_offset_y, INT_MIN, INT_MAX)     , "The top offset for shadows. (default -15)"},
    ['I'] = {"fade-in-step"                , FLOAT(fade_in_step, 0, 1)                      , "Opacity change between steps while fading in. (default 0.028)"},
    ['O'] = {"fade-out-step"               , FLOAT(fade_out_step, 0, 1)                     , "Opacity change between steps while fading out. (default 0.03)"},
    ['D'] = {"fade-delta"                  , INTEGER(fade_delta, 1, INT_MAX)                , "The time between steps in a fade in milliseconds. (default 10)"},
    ['e'] = {"frame-opacity"               , FLOAT(frame_opacity, 0, 1)                     , "Opacity of window titlebars and borders. (0.0 - 1.0)"},
    [257] = {"shadow-red"                  , FLOAT(shadow_red, 0, 1)                        , "Red color value of shadow (0.0 - 1.0, defaults to 0)."},
    [258] = {"shadow-green"                , FLOAT(shadow_green, 0, 1)                      , "Green color value of shadow (0.0 - 1.0, defaults to 0)."},
    [259] = {"shadow-blue"                 , FLOAT(shadow_blue, 0, 1)                       , "Blue color value of shadow (0.0 - 1.0, defaults to 0)."},
    [283] = {"blur-background"             , FIXED(blur_method, BLUR_METHOD_KERNEL)         , "Blur background of semi-transparent / ARGB windows. May impact performance"},
    [290] = {"backend"                     , DO(store_backend)                              , "Backend. Possible values are: " BACKENDS},
    [293] = {"benchmark"                   , INTEGER(benchmark, 0, INT_MAX)                 , "Benchmark mode. Repeatedly paint until reaching the specified cycles."},
    [302] = {"resize-damage"               , WARN_DEPRECATED(INTEGER(resize_damage, INT_MIN, INT_MAX))},       // only used by legacy backends
    [309] = {"unredir-if-possible-delay"   , INTEGER(unredir_if_possible_delay, 0, INT_MAX) , "Delay before unredirecting the window, in milliseconds. Defaults to 0."},
    [310] = {"write-pid-path"              , NAMED_STRING(write_pid_path, "PATH")           , "Write process ID to a file."},
    [322] = {"log-file"                    , STRING(logpath)                                , "Path to the log file."},
    [326] = {"max-brightness"              , FLOAT(max_brightness, 0, 1)                    , "Dims windows which average brightness is above this threshold. Requires "
                                                                                              "--no-use-damage. (default: 1.0, meaning no dimming)"},
    [329] = {"blur-size"                   , INTEGER(blur_radius, 0, INT_MAX)               , "The radius of the blur kernel for 'box' and 'gaussian' blur method."},
    [330] = {"blur-deviation"              , FLOAT(blur_deviation, 0, INFINITY)             , "The standard deviation for the 'gaussian' blur method."},
    [331] = {"blur-strength"               , INTEGER(blur_strength, 0, INT_MAX)             , "The strength level of the 'dual_kawase' blur method."},
    [333] = {"corner-radius"               , INTEGER(corner_radius, 0, INT_MAX)             , "Sets the radius of rounded window corners. When > 0, the compositor will "
                                                                                              "round the corners of windows. (defaults to 0)."},
    [336] = {"window-shader-fg"            , SHADER(window_shader_fg)                       , "Specify GLSL fragment shader path for rendering window contents. See man "
                                                                                              "page for more details."},
    [342] = {"root-pixmap-shader"          , SHADER(root_pixmap_shader)                     , "Specify GLSL fragment shader path for rendering the root pixmap."},
    [294] = {"benchmark-wid"               , DO(store_benchmark_wid)                        , "Specify window ID to repaint in benchmark mode. If omitted or is 0, the whole"
                                                                                              " screen is repainted."},
    [301] = {"blur-kern"                   , DO(store_blur_kern)                            , "Specify the blur convolution kernel, see man page for more details"},
    [332] = {"shadow-color"                , DO(store_shadow_color)                         , "Color of shadow, as a hex RGB string (defaults to #000000)"},

    // Rules
    [263] = {"shadow-exclude"              , RULES(shadow_blacklist)              , "Exclude conditions for shadows."},
    [279] = {"focus-exclude"               , RULES(focus_blacklist)               , "Specify a list of conditions of windows that should always be considered focused."},
    [288] = {"invert-color-include"        , RULES(invert_color_list)             , "Specify a list of conditions of windows that should be painted with "
                                                                                    "inverted color."},
    [296] = {"blur-background-exclude"     , RULES(blur_background_blacklist)     , "Exclude conditions for background blur."},
    [300] = {"fade-exclude"                , RULES(fade_blacklist)                , "Exclude conditions for fading."},
    [306] = {"paint-exclude"               , RULES(paint_blacklist)               , NULL},
    [308] = {"unredir-if-possible-exclude" , RULES(unredir_if_possible_blacklist) , "Conditions of windows that shouldn't be considered full-screen for "
                                                                                    "unredirecting screen."},
    [334] = {"rounded-corners-exclude"     , RULES(rounded_corners_blacklist)     , "Exclude conditions for rounded corners."},
    [335] = {"clip-shadow-above"           , RULES(shadow_clip_list)              , "Specify a list of conditions of windows to not paint a shadow over, such "
                                                                                    "as a dock window."},
    [338] = {"transparent-clipping-exclude", RULES(transparent_clipping_blacklist), "Specify a list of conditions of windows that should never have "
                                                                                    "transparent clipping applied. Useful for screenshot tools, where you "
                                                                                    "need to be able to see through transparent parts of the window."},

    // Rules that are too long to fit in one line
    [304] = {"opacity-rule"                , NUMERIC_RULES(opacity_rules, "OPACITY", 0, 100),
             "Specify a list of opacity rules, see man page for more details"},
    [337] = {"window-shader-fg-rule"       , NAMED_RULES(window_shader_fg_rules, "PATH", WINDOW_SHADER_RULE),
             "Specify GLSL fragment shader path for rendering window contents using patterns. Pattern should be "
             "in the format of SHADER_PATH:PATTERN, similar to --opacity-rule. SHADER_PATH can be \"default\", "
             "in which case the default shader will be used. See man page for more details"},
    [340] = {"corner-radius-rules"         , NUMERIC_RULES(corner_radius_rules, "RADIUS", 0, INT_MAX),
             "Window rules for specific rounded corner radii."},

    // Options that are too long to fit in one line
    [321] = {"log-level"  , PARSE_WITH(string_to_log_level, LOG_LEVEL_INVALID, log_level),
             "Log level, possible values are: trace, debug, info, warn, error"},
    [328] = {"blur-method", PARSE_WITH(parse_blur_method, BLUR_METHOD_INVALID, blur_method),
             "The algorithm used for background bluring. Available choices are: 'none' to disable, 'gaussian', "
	     "'box' or 'kernel' for custom convolution blur with --blur-kern."},

    // Deprecated options
    [269] = {"refresh-rate"       , WARN_DEPRECATED(IGNORE(required_argument))},
    [272] = {"xinerama-shadow-crop", SAY_DEPRECATED(false, "Use --crop-shadow-to-monitor instead.", ENABLE(crop_shadow_to_monitor))},
    [287] = {"logpath"             , SAY_DEPRECATED(false, "Use --log-file instead."              , STRING(logpath))},
    [289] = {"opengl"              , SAY_DEPRECATED(false, "Use --backend=glx instead."           , FIXED_STR(backend, "glx"))},
    [305] = {"shadow-exclude-reg"  , SAY_DEPRECATED(true,  "Use --clip-shadow-above instead."     , REJECT(required_argument))},
};
// clang-format on

static void setup_longopts(void) {
	auto opts = ccalloc(ARR_SIZE(picom_options) + 1, struct option);
	int option_count = 0;
	for (size_t i = 0; i < ARR_SIZE(picom_options); i++) {
		if (picom_options[i].arg.handler == NULL) {
			continue;
		}
		opts[option_count].name = picom_options[i].long_name;
		opts[option_count].has_arg = picom_options[i].has_arg;
		opts[option_count].flag = NULL;
		opts[option_count].val = (int)i;
		option_count++;
	}
	longopts = opts;
}

void print_help(const char *help, size_t indent, size_t curr_indent, size_t line_wrap,
                FILE *f) {
	if (curr_indent > indent) {
		fputs("\n", f);
		curr_indent = 0;
	}

	if (line_wrap - indent <= 1) {
		line_wrap = indent + 2;
	}

	size_t pos = 0;
	size_t len = strlen(help);
	while (pos < len) {
		fprintf(f, "%*s", (int)(indent - curr_indent), "");
		curr_indent = 0;
		size_t towrite = line_wrap - indent;
		while (help[pos] == ' ') {
			pos++;
		}
		if (pos + towrite > len) {
			towrite = len - pos;
			fwrite(help + pos, 1, towrite, f);
		} else {
			auto space_break = towrite;
			while (space_break > 0 && help[pos + space_break - 1] != ' ') {
				space_break--;
			}

			bool print_hyphen = false;
			if (space_break == 0) {
				print_hyphen = true;
				towrite--;
			} else {
				towrite = space_break;
			}

			fwrite(help + pos, 1, towrite, f);

			if (print_hyphen) {
				fputs("-", f);
			}
		}

		fputs("\n", f);
		pos += towrite;
	}
}

/**
 * Print usage text.
 */
static void usage(const char *argv0, int ret) {
	FILE *f = (ret ? stderr : stdout);
	fprintf(f, "picom " PICOM_FULL_VERSION "\n");
	fprintf(f, "Standalone X11 compositor\n");
	fprintf(f, "Please report bugs to https://github.com/yshui/picom\n\n");

	fprintf(f, "Usage: %s [OPTION]...\n\n", argv0);
	fprintf(f, "OPTIONS:\n");

	int line_wrap = 80;
	struct winsize window_size = {0};
	if (ioctl(fileno(f), TIOCGWINSZ, &window_size) != -1) {
		line_wrap = window_size.ws_col;
	}

	const char *basename = strrchr(argv0, '/') ? strrchr(argv0, '/') + 1 : argv0;

	size_t help_indent = 0;
	for (size_t i = 0; i < ARR_SIZE(picom_options); i++) {
		if (picom_options[i].help == NULL) {
			// Hide options with no help message.
			continue;
		}
		if (picom_options[i].argv0 != NULL &&
		    strcmp(picom_options[i].argv0, basename) != 0) {
			// Hide options that are not for this program.
			continue;
		}
		auto option_len = strlen(picom_options[i].long_name) + 2 + 4;
		if (picom_options[i].arg.name) {
			option_len += strlen(picom_options[i].arg.name) + 1;
		}
		if (option_len > help_indent && option_len < 30) {
			help_indent = option_len;
		}
	}
	help_indent += 6;

	for (size_t i = 0; i < ARR_SIZE(picom_options); i++) {
		if (picom_options[i].help == NULL) {
			continue;
		}
		if (picom_options[i].argv0 != NULL &&
		    strcmp(picom_options[i].argv0, basename) != 0) {
			// Hide options that are not for this program.
			continue;
		}
		size_t option_len = 8;
		fprintf(f, "    ");
		if ((i > 'a' && i < 'z') || (i > 'A' && i < 'Z')) {
			fprintf(f, "-%c, ", (char)i);
		} else {
			fprintf(f, "    ");
		}
		fprintf(f, "--%s", picom_options[i].long_name);
		option_len += strlen(picom_options[i].long_name) + 2;
		if (picom_options[i].arg.name) {
			fprintf(f, "=%s", picom_options[i].arg.name);
			option_len += strlen(picom_options[i].arg.name) + 1;
		}
		fprintf(f, "  ");
		option_len += 2;
		print_help(picom_options[i].help, help_indent, option_len,
		           (size_t)line_wrap, f);
	}
}

static void set_default_winopts(options_t *opt) {
	auto mask = opt->wintype_option_mask;
	// Apply default wintype options.
	if (!mask[WINTYPE_DESKTOP].shadow) {
		// Desktop windows are always drawn without shadow by default.
		mask[WINTYPE_DESKTOP].shadow = true;
		opt->wintype_option[WINTYPE_DESKTOP].shadow = false;
	}

	// Focused/unfocused state only apply to a few window types, all other windows
	// are always considered focused.
	const wintype_t nofocus_type[] = {WINTYPE_UNKNOWN, WINTYPE_NORMAL, WINTYPE_UTILITY};
	for (unsigned long i = 0; i < ARR_SIZE(nofocus_type); i++) {
		if (!mask[nofocus_type[i]].focus) {
			mask[nofocus_type[i]].focus = true;
			opt->wintype_option[nofocus_type[i]].focus = false;
		}
	}
	for (unsigned long i = 0; i < NUM_WINTYPES; i++) {
		if (!mask[i].shadow) {
			mask[i].shadow = true;
			opt->wintype_option[i].shadow = opt->shadow_enable;
		}
		if (!mask[i].fade) {
			mask[i].fade = true;
			opt->wintype_option[i].fade = opt->fading_enable;
		}
		if (!mask[i].focus) {
			mask[i].focus = true;
			opt->wintype_option[i].focus = true;
		}
		if (!mask[i].blur_background) {
			mask[i].blur_background = true;
			opt->wintype_option[i].blur_background =
			    opt->blur_method != BLUR_METHOD_NONE;
		}
		if (!mask[i].full_shadow) {
			mask[i].full_shadow = true;
			opt->wintype_option[i].full_shadow = false;
		}
		if (!mask[i].redir_ignore) {
			mask[i].redir_ignore = true;
			opt->wintype_option[i].redir_ignore = false;
		}
		if (!mask[i].opacity) {
			mask[i].opacity = true;
			// Opacity is not set to a concrete number here because the
			// opacity logic is complicated, and needs an "unset" state
			opt->wintype_option[i].opacity = NAN;
		}
		if (!mask[i].clip_shadow_above) {
			mask[i].clip_shadow_above = true;
			opt->wintype_option[i].clip_shadow_above = false;
		}
	}
}

static const char *shortopts = "D:I:O:r:o:m:l:t:i:e:hcfCzGb";

/// Get config options that are needed to parse the rest of the options
/// Return true if we should quit
bool get_early_config(int argc, char *const *argv, char **config_file, bool *all_xerrors,
                      bool *fork, int *exit_code) {
	setup_longopts();

	scoped_charp current_working_dir = getcwd(NULL, 0);
	int o = 0, longopt_idx = -1;

	// Pre-parse the command line arguments to check for --config and invalid
	// switches
	// Must reset optind to 0 here in case we reread the command line
	// arguments
	optind = 1;
	*config_file = NULL;
	*exit_code = 0;
	while (-1 != (o = getopt_long(argc, argv, shortopts, longopts, &longopt_idx))) {
		if (o == 256) {
			*config_file = strdup(optarg);
		} else if (o == 'h') {
			usage(argv[0], 0);
			return true;
		} else if (o == 'b') {
			*fork = true;
		} else if (o == 314) {
			*all_xerrors = true;
		} else if (o == 318) {
			printf(PICOM_FULL_VERSION "\n");
			return true;
		} else if (o == 307) {
			// --plugin
			if (!load_plugin(optarg, current_working_dir)) {
				log_error("Failed to load plugin %s", optarg);
				goto err;
			}
		} else if (o == '?' || o == ':') {
			usage(argv[0], 1);
			goto err;
		}
		// TODO(yshui) maybe log-level should be handled here.
	}

	// Check for abundant positional arguments
	if (optind < argc) {
		// log is not initialized here yet
		fprintf(stderr, "picom doesn't accept positional arguments.\n");
		goto err;
	}

	return false;
err:
	*exit_code = 1;
	return true;
}

static void script_ptr_deinit(struct script **ptr) {
	if (*ptr) {
		script_free(*ptr);
		*ptr = NULL;
	}
}

static bool sanitize_options(struct options *opt) {
	if (opt->backend == NULL) {
		log_error("Backend not specified. You must choose one "
		          "explicitly. Valid ones are: ");
		for (auto i = backend_iter(); i; i = backend_iter_next(i)) {
			log_error("\t%s", backend_name(i));
		}
		return false;
	}

	if (opt->max_brightness < 1.0 && opt->use_damage) {
		log_warn("--max-brightness requires --no-use-damage. "
		         "Falling back to 1.0.");
		opt->max_brightness = 1.0;
	}

	if (opt->write_pid_path && *opt->write_pid_path != '/') {
		log_warn("--write-pid-path is not an absolute path");
	}

	// Sanitize parameters for dual-filter kawase blur
	if (opt->blur_method == BLUR_METHOD_DUAL_KAWASE) {
		if (opt->blur_strength <= 0 && opt->blur_radius > 500) {
			log_warn("Blur radius >500 not supported by dual_kawase "
			         "method, "
			         "capping to 500.");
			opt->blur_radius = 500;
		}
		if (opt->blur_strength > 20) {
			log_warn("Blur strength >20 not supported by dual_kawase "
			         "method, "
			         "capping to 20.");
			opt->blur_strength = 20;
		}
	}

	if (opt->resize_damage < 0) {
		log_warn("Negative --resize-damage will not work correctly.");
	}

	if (opt->has_both_style_of_rules) {
		log_warn("You have set both \"rules\", as well as old-style rule options "
		         "in your configuration. The old-style rule options will have no "
		         "effect. It is recommended that you remove the old-style rule "
		         "options, and use only \"rules\" for all your window rules. If "
		         "you do genuinely need to use the old-style rule options, you "
		         "must not set \"rules\".");
	}

	return true;
}

/**
 * Process arguments and configuration files.
 */
bool get_cfg(options_t *opt, int argc, char *const *argv) {
	int o = 0, longopt_idx = -1;
	bool failed = false;
	optind = 1;
	const char *basename = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	while (-1 != (o = getopt_long(argc, argv, shortopts, longopts, &longopt_idx))) {
		if (o == '?' || o == ':' || picom_options[o].arg.handler == NULL) {
			usage(argv[0], 1);
			failed = true;
		} else if (picom_options[o].argv0 != NULL &&
		           strcmp(picom_options[o].argv0, basename) != 0) {
			log_error("Invalid option %s", argv[optind - 1]);
			failed = true;
		} else if (!picom_options[o].arg.handler(
		               &picom_options[o], &picom_options[o].arg, optarg, opt)) {
			failed = true;
		}

		if (failed) {
			// Parsing this option has failed, break the loop
			break;
		}
	}

	if (failed) {
		return false;
	}

	log_set_level_tls(opt->log_level);

	if (!sanitize_options(opt)) {
		return false;
	}

	// --blur-background-frame implies --blur-background
	if (opt->blur_background_frame && opt->blur_method == BLUR_METHOD_NONE) {
		opt->blur_method = BLUR_METHOD_KERNEL;
	}

	// Apply default wintype options that are dependent on global options
	set_default_winopts(opt);

	// Determine whether we track window grouping
	if (opt->detect_transient || opt->detect_client_leader) {
		opt->track_leader = true;
	}

	// Fill default blur kernel
	if (opt->blur_method == BLUR_METHOD_KERNEL &&
	    (!opt->blur_kerns || !opt->blur_kerns[0])) {
		opt->blur_kerns = parse_blur_kern_lst("3x3box", &opt->blur_kernel_count);
		CHECK(opt->blur_kerns);
		CHECK(opt->blur_kernel_count);
	}

	if (opt->fading_enable) {
		generate_fading_config(opt);
	}
	return true;
}

void options_postprocess_c2_lists(struct c2_state *state, struct x_connection *c,
                                  struct options *option) {
	if (!list_is_empty(&option->rules)) {
		if (!c2_list_postprocess(state, c->c, &option->rules)) {
			log_error("Post-processing of rules failed, some of your rules "
			          "might not work");
		}
		return;
	}

	if (!(c2_list_postprocess(state, c->c, &option->unredir_if_possible_blacklist) &&
	      c2_list_postprocess(state, c->c, &option->paint_blacklist) &&
	      c2_list_postprocess(state, c->c, &option->shadow_blacklist) &&
	      c2_list_postprocess(state, c->c, &option->shadow_clip_list) &&
	      c2_list_postprocess(state, c->c, &option->fade_blacklist) &&
	      c2_list_postprocess(state, c->c, &option->blur_background_blacklist) &&
	      c2_list_postprocess(state, c->c, &option->invert_color_list) &&
	      c2_list_postprocess(state, c->c, &option->window_shader_fg_rules) &&
	      c2_list_postprocess(state, c->c, &option->opacity_rules) &&
	      c2_list_postprocess(state, c->c, &option->rounded_corners_blacklist) &&
	      c2_list_postprocess(state, c->c, &option->corner_radius_rules) &&
	      c2_list_postprocess(state, c->c, &option->focus_blacklist) &&
	      c2_list_postprocess(state, c->c, &option->transparent_clipping_blacklist))) {
		log_error("Post-processing of conditionals failed, some of your "
		          "rules might not work");
	}
}

static void free_window_maybe_options(void *data) {
	auto wopts = (struct window_maybe_options *)data;
	if (wopts) {
		free((void *)wopts->shader);
		free(wopts);
	}
}

void options_destroy(struct options *options) {
	// Free blacklists
	c2_list_free(&options->shadow_blacklist, NULL);
	c2_list_free(&options->shadow_clip_list, NULL);
	c2_list_free(&options->fade_blacklist, NULL);
	c2_list_free(&options->focus_blacklist, NULL);
	c2_list_free(&options->invert_color_list, NULL);
	c2_list_free(&options->blur_background_blacklist, NULL);
	c2_list_free(&options->opacity_rules, NULL);
	c2_list_free(&options->paint_blacklist, NULL);
	c2_list_free(&options->unredir_if_possible_blacklist, NULL);
	c2_list_free(&options->rounded_corners_blacklist, NULL);
	c2_list_free(&options->corner_radius_rules, NULL);
	c2_list_free(&options->window_shader_fg_rules, free);
	c2_list_free(&options->transparent_clipping_blacklist, NULL);
	c2_list_free(&options->rules, free_window_maybe_options);

	free(options->config_file_path);
	free(options->write_pid_path);
	free(options->logpath);
	free(options->window_shader_fg);
	free(options->root_pixmap_shader);

	for (int i = 0; i < options->blur_kernel_count; ++i) {
		free(options->blur_kerns[i]);
	}
	free(options->blur_kerns);

	dynarr_free(options->all_scripts, script_ptr_deinit);
	memset(options->animations, 0, sizeof(options->animations));

	list_foreach_safe(struct included_config_file, i, &options->included_config_files,
	                  siblings) {
		free(i->path);
		list_remove(&i->siblings);
		free(i);
	}

	struct option_name *bad_opt, *next_bad_opt;
	HASH_ITER(hh, options->problematic_options, bad_opt, next_bad_opt) {
		HASH_DEL(options->problematic_options, bad_opt);
		free(bad_opt);
	}
}

// vim: set noet sw=8 ts=8 :
