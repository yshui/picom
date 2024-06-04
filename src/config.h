// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

/// Common functions and definitions for configuration parsing
/// Used for command line arguments and config files

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include <libconfig.h>
#include <picom/types.h>

#include "compiler.h"
#include "log.h"
#include "utils/kernel.h"
#include "utils/list.h"
#include "wm/defs.h"

typedef struct session session_t;

/// @brief Possible backends
enum backend {
	BKEND_XRENDER,
	BKEND_GLX,
	BKEND_XR_GLX_HYBRID,
	BKEND_DUMMY,
	BKEND_EGL,
	NUM_BKEND,
};

typedef struct win_option_mask {
	bool shadow : 1;
	bool fade : 1;
	bool focus : 1;
	bool blur_background : 1;
	bool full_shadow : 1;
	bool redir_ignore : 1;
	bool opacity : 1;
	bool clip_shadow_above : 1;
} win_option_mask_t;

typedef struct win_option {
	bool shadow;
	bool fade;
	bool focus;
	bool blur_background;
	bool full_shadow;
	bool redir_ignore;
	double opacity;
	bool clip_shadow_above;
} win_option_t;

typedef struct _c2_lptr c2_lptr_t;

enum vblank_scheduler_type {
	/// X Present extension based vblank events
	VBLANK_SCHEDULER_PRESENT,
	/// GLX_SGI_video_sync based vblank events
	VBLANK_SCHEDULER_SGI_VIDEO_SYNC,
	/// An invalid scheduler, served as a scheduler count, and
	/// as a sentinel value.
	LAST_VBLANK_SCHEDULER,
};

enum animation_trigger {
	ANIMATION_TRIGGER_INVALID = -1,
	/// When a hidden window is shown
	ANIMATION_TRIGGER_SHOW = 0,
	/// When a window is hidden
	ANIMATION_TRIGGER_HIDE,
	/// When window opacity is increased
	ANIMATION_TRIGGER_INCREASE_OPACITY,
	/// When window opacity is decreased
	ANIMATION_TRIGGER_DECREASE_OPACITY,
	/// When a new window opens
	ANIMATION_TRIGGER_OPEN,
	/// When a window is closed
	ANIMATION_TRIGGER_CLOSE,
	ANIMATION_TRIGGER_LAST = ANIMATION_TRIGGER_CLOSE,
};

static const char *animation_trigger_names[] attr_unused = {
    [ANIMATION_TRIGGER_SHOW] = "show",
    [ANIMATION_TRIGGER_HIDE] = "hide",
    [ANIMATION_TRIGGER_INCREASE_OPACITY] = "increase-opacity",
    [ANIMATION_TRIGGER_DECREASE_OPACITY] = "decrease-opacity",
    [ANIMATION_TRIGGER_OPEN] = "open",
    [ANIMATION_TRIGGER_CLOSE] = "close",
};

struct script;
struct win_script {
	int output_indices[NUM_OF_WIN_SCRIPT_OUTPUTS];
	/// A running animation can be configured to prevent other animations from
	/// starting.
	uint64_t suppressions;
	struct script *script;
};

extern const char *vblank_scheduler_str[];

/// Internal, private options for debugging and development use.
struct debug_options {
	/// Try to reduce frame latency by using vblank interval and render time
	/// estimates. Right now it's not working well across drivers.
	int smart_frame_pacing;
	/// Override the vblank scheduler chosen by the compositor.
	int force_vblank_scheduler;
	/// Release then immediately rebind every window pixmap each frame.
	/// Useful when being traced under apitrace, to force it to pick up
	/// updated contents. WARNING, extremely slow.
	int always_rebind_pixmap;
	/// When using damage, replaying an apitrace becomes non-deterministic, because
	/// the buffer age we got when we rendered will be different from the buffer age
	/// apitrace gets when it replays. When this option is enabled, we saves the
	/// contents of each rendered frame, and at the beginning of each render, we
	/// restore the content of the back buffer based on the buffer age we get,
	/// ensuring no matter what buffer age apitrace gets during replay, the result
	/// will be the same.
	int consistent_buffer_age;
};

extern struct debug_options global_debug_options;

struct included_config_file {
	char *path;
	struct list_node siblings;
};

/// Structure representing all options.
typedef struct options {
	// === Config ===
	/// Path to the config file
	char *config_file_path;
	/// List of config files included by the main config file
	struct list_node included_config_files;
	// === Debugging ===
	bool monitor_repaint;
	bool print_diagnostics;
	/// Render to a separate window instead of taking over the screen
	bool debug_mode;
	// === General ===
	/// Use the legacy backends?
	bool use_legacy_backends;
	/// Path to write PID to.
	char *write_pid_path;
	/// Name of the backend
	struct backend_info *backend;
	/// The backend in use (for legacy backends).
	int legacy_backend;
	/// Log level.
	int log_level;
	/// Whether to sync X drawing with X Sync fence to avoid certain delay
	/// issues with GLX backend.
	bool xrender_sync_fence;
	/// Whether to avoid using stencil buffer under GLX backend. Might be
	/// unsafe.
	bool glx_no_stencil;
	/// Whether to avoid rebinding pixmap on window damage.
	bool glx_no_rebind_pixmap;
	/// Custom fragment shader for painting windows, as a string.
	char *glx_fshader_win_str;
	/// Whether to detect rounded corners.
	bool detect_rounded_corners;
	/// Force painting of window content with blending.
	bool force_win_blend;
	/// Resize damage for a specific number of pixels.
	int resize_damage;
	/// Whether to unredirect all windows if a full-screen opaque window
	/// is detected.
	bool unredir_if_possible;
	/// List of conditions of windows to ignore as a full-screen window
	/// when determining if a window could be unredirected.
	c2_lptr_t *unredir_if_possible_blacklist;
	/// Delay before unredirecting screen, in milliseconds.
	int unredir_if_possible_delay;
	/// Forced redirection setting through D-Bus.
	switch_t redirected_force;
	/// Whether to stop painting. Controlled through D-Bus.
	switch_t stoppaint_force;
	/// Whether to enable D-Bus support.
	bool dbus;
	/// Path to log file.
	char *logpath;
	/// Number of cycles to paint in benchmark mode. 0 for disabled.
	int benchmark;
	/// Window to constantly repaint in benchmark mode. 0 for full-screen.
	xcb_window_t benchmark_wid;
	/// A list of conditions of windows not to paint.
	c2_lptr_t *paint_blacklist;
	/// Whether to show all X errors.
	bool show_all_xerrors;
	/// Whether to avoid acquiring X Selection.
	bool no_x_selection;
	/// Window type option override.
	win_option_t wintype_option[NUM_WINTYPES];
	struct win_option_mask wintype_option_mask[NUM_WINTYPES];
	/// Whether to set realtime scheduling policy for the compositor process.
	bool use_realtime_scheduling;

	// === VSync & software optimization ===
	/// VSync method to use;
	bool vsync;
	/// Whether to use glFinish() instead of glFlush() for (possibly) better
	/// VSync yet probably higher CPU usage.
	bool vsync_use_glfinish;
	/// Whether use damage information to help limit the area to paint
	bool use_damage;
	/// Disable frame pacing
	bool frame_pacing;

	// === Shadow ===
	/// Red, green and blue tone of the shadow.
	double shadow_red, shadow_green, shadow_blue;
	int shadow_radius;
	int shadow_offset_x, shadow_offset_y;
	double shadow_opacity;
	/// Shadow blacklist. A linked list of conditions.
	c2_lptr_t *shadow_blacklist;
	/// Whether bounding-shaped window should be ignored.
	bool shadow_ignore_shaped;
	/// Whether to crop shadow to the very X RandR monitor.
	bool crop_shadow_to_monitor;
	/// Don't draw shadow over these windows. A linked list of conditions.
	c2_lptr_t *shadow_clip_list;
	bool shadow_enable;

	// === Fading ===
	/// How much to fade in in a single fading step.
	double fade_in_step;
	/// How much to fade out in a single fading step.
	double fade_out_step;
	/// Fading time delta. In milliseconds.
	int fade_delta;
	/// Whether to disable fading on window open/close.
	bool no_fading_openclose;
	/// Whether to disable fading on ARGB managed destroyed windows.
	bool no_fading_destroyed_argb;
	/// Fading blacklist. A linked list of conditions.
	c2_lptr_t *fade_blacklist;
	bool fading_enable;

	// === Opacity ===
	/// Default opacity for inactive windows.
	/// 32-bit integer with the format of _NET_WM_WINDOW_OPACITY.
	double inactive_opacity;
	/// Default opacity for inactive windows.
	double active_opacity;
	/// Whether inactive_opacity overrides the opacity set by window
	/// attributes.
	bool inactive_opacity_override;
	/// Frame opacity. Relative to window opacity, also affects shadow
	/// opacity.
	double frame_opacity;
	/// Whether to detect _NET_WM_WINDOW_OPACITY on client windows. Used on window
	/// managers that don't pass _NET_WM_WINDOW_OPACITY to frame windows.
	bool detect_client_opacity;

	// === Other window processing ===
	/// Blur method for background of semi-transparent windows
	enum blur_method blur_method;
	// Size of the blur kernel
	int blur_radius;
	// Standard deviation for the gaussian blur
	double blur_deviation;
	// Strength of the dual_kawase blur
	int blur_strength;
	/// Whether to blur background when the window frame is not opaque.
	/// Implies blur_background.
	bool blur_background_frame;
	/// Whether to use fixed blur strength instead of adjusting according
	/// to window opacity.
	bool blur_background_fixed;
	/// Background blur blacklist. A linked list of conditions.
	c2_lptr_t *blur_background_blacklist;
	/// Blur convolution kernel.
	struct conv **blur_kerns;
	/// Number of convolution kernels
	int blur_kernel_count;
	/// Custom fragment shader for painting windows
	char *window_shader_fg;
	/// Rules to change custom fragment shader for painting windows.
	c2_lptr_t *window_shader_fg_rules;
	/// How much to dim an inactive window. 0.0 - 1.0, 0 to disable.
	double inactive_dim;
	/// Whether to use fixed inactive dim opacity, instead of deciding
	/// based on window opacity.
	bool inactive_dim_fixed;
	/// Conditions of windows to have inverted colors.
	c2_lptr_t *invert_color_list;
	/// Rules to change window opacity.
	c2_lptr_t *opacity_rules;
	/// Limit window brightness
	double max_brightness;
	// Radius of rounded window corners
	int corner_radius;
	/// Rounded corners blacklist. A linked list of conditions.
	c2_lptr_t *rounded_corners_blacklist;
	/// Rounded corner rules. A linked list of conditions.
	c2_lptr_t *corner_radius_rules;

	// === Focus related ===
	/// Whether to try to detect WM windows and mark them as focused.
	bool mark_wmwin_focused;
	/// Whether to mark override-redirect windows as focused.
	bool mark_ovredir_focused;
	/// Whether to use EWMH _NET_ACTIVE_WINDOW to find active window.
	bool use_ewmh_active_win;
	/// A list of windows always to be considered focused.
	c2_lptr_t *focus_blacklist;
	/// Whether to do window grouping with <code>WM_TRANSIENT_FOR</code>.
	bool detect_transient;
	/// Whether to do window grouping with <code>WM_CLIENT_LEADER</code>.
	bool detect_client_leader;

	// === Calculated ===
	/// Whether we need to track window leaders.
	bool track_leader;

	// Don't use EWMH to detect fullscreen applications
	bool no_ewmh_fullscreen;

	// Make transparent windows clip other windows, instead of blending on top of
	// them
	bool transparent_clipping;
	/// A list of conditions of windows to which transparent clipping
	/// should not apply
	c2_lptr_t *transparent_clipping_blacklist;

	bool dithered_present;
	// === Animation ===
	struct win_script animations[ANIMATION_TRIGGER_LAST + 1];
	/// Array of all the scripts used in `animations`. This is a dynarr.
	struct script **all_scripts;
} options_t;

extern const char *const BACKEND_STRS[NUM_BKEND + 1];

bool load_plugin(const char *name, const char *include_dir);

bool must_use parse_long(const char *, long *);
bool must_use parse_int(const char *, int *);
struct conv **must_use parse_blur_kern_lst(const char *, int *count);
/// Parse the path prefix of a c2 rule. Then look for the specified file in the
/// given include directories. The include directories are passed via `user_data`.
void *parse_window_shader_prefix(const char *src, const char **end, void *user_data);
/// Same as `parse_window_shader_prefix`, but the path is relative to the current
/// working directory. `user_data` is ignored.
void *parse_window_shader_prefix_with_cwd(const char *src, const char **end, void *);
void *parse_numeric_prefix(const char *src, const char **end, void *user_data);
char *must_use locate_auxiliary_file(const char *scope, const char *path,
                                     const char *include_dir);
int must_use parse_blur_method(const char *src);
void parse_debug_options(struct debug_options *);

const char *xdg_config_home(void);
char **xdg_config_dirs(void);

/// Parse a configuration file
/// Returns the actually config_file name used, allocated on heap
/// Outputs:
///   shadow_enable = whether shadow is enabled globally
///   fading_enable = whether fading is enabled globally
///   win_option_mask = whether option overrides for specific window type is set for given
///                     options
///   hasneg = whether the convolution kernel has negative values
bool parse_config_libconfig(options_t *, const char *config_file);

/// Parse a configuration file is that is enabled, also initialize the winopt_mask with
/// default values
/// Outputs and returns:
///   same as parse_config_libconfig
bool parse_config(options_t *, const char *config_file);

/**
 * Parse a backend option argument.
 */
static inline attr_pure int parse_backend(const char *str) {
	for (int i = 0; BACKEND_STRS[i]; ++i) {
		if (!strcasecmp(str, BACKEND_STRS[i])) {
			return i;
		}
	}
	// Keep compatibility with an old revision containing a spelling mistake...
	if (!strcasecmp(str, "xr_glx_hybird")) {
		log_warn("backend xr_glx_hybird should be xr_glx_hybrid, the misspelt "
		         "version will be removed soon.");
		return BKEND_XR_GLX_HYBRID;
	}
	// cju wants to use dashes
	if (!strcasecmp(str, "xr-glx-hybrid")) {
		log_warn("backend xr-glx-hybrid should be xr_glx_hybrid, the alternative "
		         "version will be removed soon.");
		return BKEND_XR_GLX_HYBRID;
	}
	return NUM_BKEND;
}

/**
 * Parse a VSync option argument.
 */
static inline bool parse_vsync(const char *str) {
	if (strcmp(str, "no") == 0 || strcmp(str, "none") == 0 ||
	    strcmp(str, "false") == 0 || strcmp(str, "nah") == 0) {
		return false;
	}
	return true;
}

/// Generate animation script for legacy fading options
void generate_fading_config(struct options *opt);

// vim: set noet sw=8 ts=8 :
