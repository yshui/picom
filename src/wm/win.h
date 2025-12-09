// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdbool.h>
#include <xcb/damage.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

#include <picom/backend.h>
#include <picom/types.h>

#include "c2.h"
#include "compiler.h"
#include "config.h"
#include "defs.h"
#include "region.h"
#include "transition/script.h"
#include "utils/list.h"
#include "utils/misc.h"
#include "wm/wm.h"
#include "x.h"
#include "xcb/xproto.h"

struct backend_base;
typedef struct session session_t;
struct wm_cursor;

#define wm_stack_foreach(wm, i)                                                          \
	for (struct wm_ref * (i) = wm_ref_topmost_child(wm_root_ref(wm)); (i);           \
	     (i) = wm_ref_below(i))
#define wm_stack_foreach_rev(wm, i)                                                      \
	for (struct wm_ref * (i) = wm_ref_bottommost_child(wm_root_ref(wm)); (i);        \
	     (i) = wm_ref_above(i))

#define wm_stack_foreach_safe(wm, i, next_i)                                             \
	for (struct wm_ref * (i) = wm_ref_topmost_child(wm_root_ref(wm)),                \
	                     *(next_i) = (i) != NULL ? wm_ref_below(i) : NULL;           \
	     (i); (i) = (next_i), (next_i) = (i) != NULL ? wm_ref_below(i) : NULL)

struct wm;
/// An entry in the window stack. May or may not correspond to a window we know about.
struct window_stack_entry {
	struct list_node stack_neighbour;
	/// The actual window correspond to this stack entry. NULL if we didn't know about
	/// this window (e.g. an InputOnly window, or we haven't handled the window
	/// creation yet)
	struct win *win;
	/// The window id. Might not be unique in the stack, because there might be
	/// destroyed window still fading out in the stack.
	xcb_window_t id;
};

/**
 * About coordinate systems
 *
 * In general, X is the horizontal axis, Y is the vertical axis.
 * X goes from left to right, Y goes downwards.
 *
 * Global: the origin is the top left corner of the Xorg screen.
 * Local: the origin is the top left corner of the window, border is
 *        considered part of the window.
 */

struct win_geometry {
	int16_t x;
	int16_t y;
	uint16_t width;
	uint16_t height;
	uint16_t border_width;
};

/// These are changes of window state that might trigger an animation. We separate them
/// out and delay their application so determining which animation to run is easier.
///
/// These values are only hold for an instant, and once the animation is started they are
/// updated to reflect the latest state.
struct win_state_change {
	winstate_t state;
	double opacity;
	double blur_opacity;
	struct win_geometry g;
	struct color shadow_color;
};

struct win {
	/// Reference back to the position of this window inside the window tree.
	struct wm_ref *tree_ref;
	/// backend data attached to this window. Only available when
	/// `state` is not UNMAPPED
	image_handle win_image;
	/// The old window image before the window image is refreshed. This is used for
	/// animation, and is only kept alive for the duration of the animation.
	image_handle saved_win_image;
	/// How much to scale the saved_win_image, so that it is the same size as the
	/// current window image.
	vec2 saved_win_image_scale;
	/// A mask image for the shadow. This is usually a blurred `mask_image`, though
	/// for some backends this can be generated on the CPU.
	image_handle shadow_mask;
	/// A mask image for the shape of the window.
	image_handle mask_image;

	// Core members
	winstate_t state;
	/// Window attributes.
	xcb_get_window_attributes_reply_t a;
	/// The geometry of the window body, excluding the window border region.
	struct win_geometry g;
	/// Updated geometry received in events
	struct win_geometry pending_g;
	/// Window visual pict format
	const xcb_render_pictforminfo_t *pictfmt;
	/// Client window visual pict format
	const xcb_render_pictforminfo_t *client_pictfmt;
	/// Window painting mode.
	winmode_t mode;        // TODO(yshui) only used by legacy backends, remove.
	/// Whether the window has been damaged at least once since it
	/// was mapped. Unmapped windows that were previously mapped
	/// retain their `ever_damaged` state. Mapping a window resets
	/// this.
	bool ever_damaged;
	/// Whether the window was damaged after last paint.
	bool pixmap_damaged;
	/// Damage of the window.
	xcb_damage_damage_t damage;
	/// bitmap for properties which needs to be updated
	uint64_t *stale_props;
	/// number of uint64_ts that has been allocated for stale_props
	size_t stale_props_capacity;

	/// Bounding shape of the window. In local coordinates.
	/// See above about coordinate systems.
	region_t bounding_shape;
	/// Window flags. Definitions above.
	uint64_t flags;
	/// Cached width/height of the window including border.
	int widthb, heightb;
	/// Whether the window is bounding-shaped.
	bool bounding_shaped;
	/// Whether the window just have rounded corners.
	bool rounded_corners;
	/// Whether this window is to be painted.
	bool to_paint;
	/// Whether this window is in open/close state.
	bool in_openclose;

	// Client window related members
	/// A bitflag of window types. According to ICCCM, a window can have more than one
	/// type.
	uint32_t window_types;

	// Blacklist related members
	/// Name of the window.
	char *name;
	/// Window instance class of the window.
	char *class_instance;
	/// Window general class of the window.
	char *class_general;
	/// <code>WM_WINDOW_ROLE</code> value of the window.
	char *role;
	/// Whether the window sets the EWMH fullscreen property.
	bool is_ewmh_fullscreen;
	/// Whether the window should be considered fullscreen. Based on
	/// `is_ewmh_fullscreen`, or the windows spatial relation with the
	/// root window. Which one is used is determined by user configuration.
	bool is_fullscreen;
	/// Whether the window is the active window.
	bool is_focused;
	/// Whether the window group this window belongs to is focused.
	bool is_group_focused;

	// Opacity-related members
	/// The final window opacity if no animation is running
	double opacity;
	/// true if window (or client window, for broken window managers
	/// not transferring client window's _NET_WM_WINDOW_OPACITY value) has opacity
	/// prop
	bool has_opacity_prop;
	/// Cached value of opacity window attribute.
	opacity_t opacity_prop;
	/// Last window opacity value set by the rules.
	double opacity_set;

	float border_col[4];

	// Frame-opacity-related members
	/// Current window frame opacity. Affected by window opacity.
	double frame_opacity;
	/// Frame extents. Acquired from _NET_FRAME_EXTENTS.
	margin_t frame_extents;

	// Shadow-related members
	/// Window specific shadow factor. The final shadow opacity is a combination of
	/// this, the window opacity, and the window frame opacity.
	double shadow_opacity;
	/// X offset of shadow. Affected by command line argument.
	int shadow_dx;
	/// Y offset of shadow. Affected by command line argument.
	int shadow_dy;
	/// Width of shadow. Affected by window size and command line argument.
	int shadow_width;
	/// Height of shadow. Affected by window size and command line argument.
	int shadow_height;
	/// The value of _COMPTON_SHADOW attribute of the window. Below 0 for
	/// none.
	long long prop_shadow;

	struct c2_window_state c2_state;

	/// The damaged region of the window, in window local coordinates.
	region_t damaged;

	/// Per-window options coming from rules
	struct window_maybe_options options;
	/// Override of per-window options, used by dbus interface
	struct window_maybe_options options_override;
	/// Global per-window options default
	const struct window_options *options_default;

	/// Previous state of the window before state changed. This is used
	/// by `win_process_animation_and_state_change` to trigger appropriate
	/// animations.
	struct win_state_change previous;
	struct script_instance *running_animation_instance;
	struct win_script running_animation;

	/// Number of times each animation trigger is blocked
	unsigned int animation_block[ANIMATION_TRIGGER_COUNT];
};

struct win_script_context {
	double x, y, width, height;
	double x_before, y_before, width_before, height_before;
	double opacity_before, opacity;
	double blur_opacity_before, blur_opacity;
	double monitor_x, monitor_y;
	double monitor_width, monitor_height;
	struct color shadow_color, shadow_color_before;
};
// NOLINTNEXTLINE(bugprone-sizeof-expression)
static_assert(SCRIPT_CTX_PLACEHOLDER_BASE > sizeof(struct win_script_context),
              "win_script_context too large");

#define X(name) offsetof(struct win_script_context, name)
static const struct script_context_info win_script_context_info[] = {
    {"window-x", X(x)},
    {"window-y", X(y)},
    {"window-width", X(width)},
    {"window-height", X(height)},
    {"window-x-before", X(x_before)},
    {"window-y-before", X(y_before)},
    {"window-width-before", X(width_before)},
    {"window-height-before", X(height_before)},
    {"window-raw-opacity-before", X(opacity_before)},
    {"window-raw-opacity", X(opacity)},
    {"window-blur-opacity-before", X(blur_opacity_before)},
    {"window-blur-opacity", X(blur_opacity)},
    {"window-monitor-x", X(monitor_x)},
    {"window-monitor-y", X(monitor_y)},
    {"window-monitor-width", X(monitor_width)},
    {"window-monitor-height", X(monitor_height)},
    {"window-shadow-red", X(shadow_color.red)},
    {"window-shadow-green", X(shadow_color.green)},
    {"window-shadow-blue", X(shadow_color.blue)},
    {"window-shadow-red-before", X(shadow_color_before.red)},
    {"window-shadow-green-before", X(shadow_color_before.green)},
    {"window-shadow-blue-before", X(shadow_color_before.blue)},
    {NULL, 0}        //
};
#undef X

static const struct script_output_info win_script_outputs[] = {
    [WIN_SCRIPT_OFFSET_X] = {"offset-x"},
    [WIN_SCRIPT_OFFSET_Y] = {"offset-y"},
    [WIN_SCRIPT_SHADOW_OFFSET_X] = {"shadow-offset-x"},
    [WIN_SCRIPT_SHADOW_OFFSET_Y] = {"shadow-offset-y"},
    [WIN_SCRIPT_OPACITY] = {"opacity"},
    [WIN_SCRIPT_BLUR_OPACITY] = {"blur-opacity"},
    [WIN_SCRIPT_SHADOW_OPACITY] = {"shadow-opacity"},
    [WIN_SCRIPT_SCALE_X] = {"scale-x"},
    [WIN_SCRIPT_SCALE_Y] = {"scale-y"},
    [WIN_SCRIPT_SHADOW_SCALE_X] = {"shadow-scale-x"},
    [WIN_SCRIPT_SHADOW_SCALE_Y] = {"shadow-scale-y"},
    [WIN_SCRIPT_CROP_X] = {"crop-x"},
    [WIN_SCRIPT_CROP_Y] = {"crop-y"},
    [WIN_SCRIPT_CROP_WIDTH] = {"crop-width"},
    [WIN_SCRIPT_CROP_HEIGHT] = {"crop-height"},
    [WIN_SCRIPT_SAVED_IMAGE_BLEND] = {"saved-image-blend"},
    [WIN_SCRIPT_SHADOW_RED] = {"shadow-red"},
    [WIN_SCRIPT_SHADOW_GREEN] = {"shadow-green"},
    [WIN_SCRIPT_SHADOW_BLUE] = {"shadow-blue"},
    [NUM_OF_WIN_SCRIPT_OUTPUTS] = {NULL},
};

static const struct window_maybe_options WIN_MAYBE_OPTIONS_DEFAULT = {
    .blur_background = TRI_UNKNOWN,
    .clip_shadow_above = TRI_UNKNOWN,
    .shadow = TRI_UNKNOWN,
    .fade = TRI_UNKNOWN,
    .invert_color = TRI_UNKNOWN,
    .paint = TRI_UNKNOWN,
    .dim = NAN,
    .opacity = NAN,
    .blur_opacity = NAN,
    .shader = NULL,
    .corner_radius = -1,
    .unredir = WINDOW_UNREDIR_INVALID,
};

static inline void win_script_fold(const struct win_script *upper,
                                   const struct win_script *lower, struct win_script *output) {
	for (size_t i = 0; i < ANIMATION_TRIGGER_COUNT; i++) {
		output[i] = upper[i].script ? upper[i] : lower[i];
	}
}

/// Return `a` if it's not NaN, otherwise return `def`.
static inline double __attribute__((always_inline, const)) number_or_d(double a, double def) {
	return safe_isnan(a) ? def : a;
}

/// Combine two window options. The `upper` value has higher priority, the `lower` value
/// will only be used if the corresponding value in `upper` is not set (e.g. it is
/// TRI_UNKNOWN for tristate values, NaN for opacity, -1 for corner_radius).
static inline struct window_maybe_options __attribute__((always_inline, const))
win_maybe_options_fold(struct window_maybe_options upper, struct window_maybe_options lower) {
	struct window_maybe_options ret = {
	    .unredir = upper.unredir == WINDOW_UNREDIR_INVALID ? lower.unredir : upper.unredir,
	    .blur_background = tri_or(upper.blur_background, lower.blur_background),
	    .clip_shadow_above = tri_or(upper.clip_shadow_above, lower.clip_shadow_above),
	    .full_shadow = tri_or(upper.full_shadow, lower.full_shadow),
	    .shadow = tri_or(upper.shadow, lower.shadow),
	    .fade = tri_or(upper.fade, lower.fade),
	    .invert_color = tri_or(upper.invert_color, lower.invert_color),
	    .paint = tri_or(upper.paint, lower.paint),
	    .transparent_clipping =
	        tri_or(upper.transparent_clipping, lower.transparent_clipping),
	    .opacity = number_or_d(upper.opacity, lower.opacity),
	    .blur_opacity = number_or_d(upper.blur_opacity, lower.blur_opacity),
	    .dim = number_or_d(upper.dim, lower.dim),
	    .shader = upper.shader ? upper.shader : lower.shader,
	    .corner_radius = upper.corner_radius >= 0 ? upper.corner_radius : lower.corner_radius,
	    .is_shadow_color_set = upper.is_shadow_color_set || lower.is_shadow_color_set,
	    .shadow_color = upper.is_shadow_color_set ? upper.shadow_color : lower.shadow_color,
	};
	win_script_fold(upper.animations, lower.animations, ret.animations);
	return ret;
}

/// Unwrap a `window_maybe_options` to a `window_options`, using the default value for
/// values that are not set in the `window_maybe_options`.
static inline struct window_options __attribute__((always_inline, const))
win_maybe_options_or(struct window_maybe_options maybe, struct window_options def) {
	assert(def.unredir != WINDOW_UNREDIR_INVALID);
	struct window_options ret = {
	    .unredir = maybe.unredir == WINDOW_UNREDIR_INVALID ? def.unredir : maybe.unredir,
	    .blur_background = tri_or_bool(maybe.blur_background, def.blur_background),
	    .clip_shadow_above = tri_or_bool(maybe.clip_shadow_above, def.clip_shadow_above),
	    .full_shadow = tri_or_bool(maybe.full_shadow, def.full_shadow),
	    .shadow = tri_or_bool(maybe.shadow, def.shadow),
	    .corner_radius = maybe.corner_radius >= 0 ? (unsigned int)maybe.corner_radius
	                                              : def.corner_radius,
	    .fade = tri_or_bool(maybe.fade, def.fade),
	    .invert_color = tri_or_bool(maybe.invert_color, def.invert_color),
	    .paint = tri_or_bool(maybe.paint, def.paint),
	    .transparent_clipping =
	        tri_or_bool(maybe.transparent_clipping, def.transparent_clipping),
	    .opacity = number_or_d(maybe.opacity, def.opacity),
	    .blur_opacity = number_or_d(maybe.blur_opacity, def.blur_opacity),
	    .dim = number_or_d(maybe.dim, def.dim),
	    .shader = maybe.shader ? maybe.shader : def.shader,
	    .shadow_color = maybe.is_shadow_color_set ? maybe.shadow_color : def.shadow_color,
	};
	win_script_fold(maybe.animations, def.animations, ret.animations);
	return ret;
}

static inline struct window_options __attribute__((always_inline, const))
win_options(const struct win *w) {
	return win_maybe_options_or(
	    win_maybe_options_fold(w->options_override, w->options), *w->options_default);
}

/// Check if the window has changed in size. Border width is
/// not considered.
static inline bool win_size_changed(struct win_geometry a, struct win_geometry b) {
	return a.width != b.width || a.height != b.height;
}

/// Check if the window position has changed.
static inline bool win_position_changed(struct win_geometry a, struct win_geometry b) {
	if (win_size_changed(a, b)) {
		return false;
	}

	return a.x != b.x || a.y != b.y;
}

/// Process pending updates/images flags on a window. Has to be called in X critical
/// section. Returns true if the window had an animation running and it has just finished,
/// or if the window's states just changed and there is no animation defined for this
/// state change.
bool win_process_animation_and_state_change(struct session *ps, struct win *w, double delta_t);
double win_animatable_get(const struct win *w, enum win_script_output output);
void win_process_primary_flags(session_t *ps, struct win *w);
void win_process_secondary_flags(session_t *ps, struct win *w);
void win_process_image_flags(session_t *ps, struct win *w);

/// Start the unmap of a window. We cannot unmap immediately since we might need to fade
/// the window out.
void unmap_win_start(struct win *);
void unmap_win_finish(session_t *ps, struct win *w);
/// Start the destroying of a window. Windows cannot always be destroyed immediately
/// because of fading and such.
void win_destroy_start(struct win *w);
void win_map_start(struct session *ps, struct win *w);
void win_release_saved_win_image(backend_t *base, struct win *w);
/// Release images bound with a window, set the *_NONE flags on the window. Only to be
/// used when de-initializing the backend outside of win.c
void win_release_images(struct backend_base *backend, struct win *w);
winmode_t attr_pure win_calc_mode_raw(const struct win *w);
// TODO(yshui) `win_calc_mode` is only used by legacy backends
winmode_t attr_pure win_calc_mode(const struct win *w);
/**
 * Set real focused state of a window.
 */
void win_set_focused(session_t *ps, struct win *w);
void win_on_factor_change(session_t *ps, struct win *w);
void win_on_client_update(session_t *ps, struct win *w);

int attr_pure win_find_monitor(const struct x_monitors *monitors, const struct win *mw);

/// Recheck if a window is fullscreen
void win_update_is_fullscreen(const session_t *ps, struct win *w);
/**
 * Check if a window has BYPASS_COMPOSITOR property set
 */
bool win_is_bypassing_compositor(const session_t *ps, const struct win *w);
/**
 * Get a rectangular region in global coordinates a window (and possibly
 * its shadow) occupies.
 *
 * Note w->shadow and shadow geometry must be correct before calling this
 * function.
 */
void win_extents(const struct win *w, region_t *res);
region_t win_extents_by_val(const struct win *w);

/// Get the region for the frame of the window
void win_get_region_frame_local(const struct win *w, region_t *res);
/// Get the region for the frame of the window, by value
region_t win_get_region_frame_local_by_val(const struct win *w);
/// Query the Xorg for information about window `win`
/// `win` pointer might become invalid after this function returns
struct win *win_maybe_allocate(session_t *ps, struct wm_ref *cursor,
                               const xcb_get_window_attributes_reply_t *attrs);

/**
 * Release a destroyed window that is no longer needed.
 */
void win_destroy_finish(session_t *ps, struct win *w);

/// Return whether the group a window belongs to is really focused.
///
/// @param leader leader window ID
/// @return true if the window group is focused, false otherwise
bool win_is_group_focused(session_t *ps, struct win *w);

/// check if window has ARGB visual
bool attr_pure win_has_alpha(const struct win *w);

/// Whether it looks like a WM window. We consider a window WM window if
/// it does not have a decedent with WM_STATE and it is not override-
/// redirected itself.
static inline bool attr_pure win_is_wmwin(const struct win *w) {
	return wm_ref_client_of(w->tree_ref) == NULL && !w->a.override_redirect;
}

static inline xcb_window_t win_id(const struct win *w) {
	return wm_ref_win_id(w->tree_ref);
}

/// Returns the client window of a window. If a client window does not exist, returns the
/// window itself when `fallback_to_self` is true, otherwise returns XCB_NONE.
static inline xcb_window_t win_client_id(const struct win *w, bool fallback_to_self) {
	auto client_win = wm_ref_client_of(w->tree_ref);
	if (client_win == NULL) {
		return fallback_to_self ? win_id(w) : XCB_NONE;
	}
	return wm_ref_win_id(client_win);
}

/// Whether a given window is mapped on the X server side
bool win_is_mapped_in_x(const struct win *w);
/// Set flags on a window. Some sanity checks are performed
void win_set_flags(struct win *w, uint64_t flags);
/// Clear flags on a window. Some sanity checks are performed
void win_clear_flags(struct win *w, uint64_t flags);
/// Returns true if any of the flags in `flags` is set
bool win_check_flags_any(struct win *w, uint64_t flags);
/// Returns true if all of the flags in `flags` are set
bool win_check_flags_all(struct win *w, uint64_t flags);
/// Mark properties as stale for a window
void win_set_properties_stale(struct win *w, const xcb_atom_t *prop, int nprops);

static inline struct win_geometry
win_geometry_from_configure_notify(const xcb_configure_notify_event_t *ce) {
	return (struct win_geometry){
	    .x = ce->x,
	    .y = ce->y,
	    .width = ce->width,
	    .height = ce->height,
	    .border_width = ce->border_width,
	};
}
static inline struct win_geometry
win_geometry_from_get_geometry(const xcb_get_geometry_reply_t *g) {
	return (struct win_geometry){
	    .x = g->x,
	    .y = g->y,
	    .width = g->width,
	    .height = g->height,
	    .border_width = g->border_width,
	};
}
/// Set the pending geometry of a window. And set appropriate flags when the geometry
/// changes.
/// Returns true if the geometry has changed, false otherwise.
bool win_set_pending_geometry(struct win *w, struct win_geometry g);
bool win_update_wintype(struct x_connection *c, struct atom *atoms, struct win *w);
/**
 * Retrieve frame extents from a window.
 */
void win_update_frame_extents(struct x_connection *c, struct atom *atoms, struct win *w,
                              xcb_window_t client);
/**
 * Retrieve the <code>WM_CLASS</code> of a window and update its
 * <code>win</code> structure.
 */
bool win_update_class(struct x_connection *c, struct atom *atoms, struct win *w);
int win_update_role(struct x_connection *c, struct atom *atoms, struct win *w);
int win_update_name(struct x_connection *c, struct atom *atoms, struct win *w);
void win_on_win_size_change(struct win *w, int shadow_offset_x, int shadow_offset_y,
                            int shadow_radius);
void win_update_bounding_shape(struct x_connection *c, struct win *w,
                               bool detect_rounded_corners);
bool win_update_prop_fullscreen(struct x_connection *c, const struct atom *atoms,
                                struct win *w);

static inline attr_unused void win_set_property_stale(struct win *w, xcb_atom_t prop) {
	return win_set_properties_stale(w, (xcb_atom_t[]){prop}, 1);
}

/// Free all resources in a struct win
void free_win_res(session_t *ps, struct win *w);

/// Remove the corners of a scaled, rounded rectangle from region `res`. `origin` is the
/// top-left corner of the rectangle in `res`'s coordinate system. `size` is the size of
/// the rectangle, `scale` is the scaling factor. `corner_radius` is the _unscaled_ radius
/// of the rounded corners. The top-left corner of the rectangle is the scaling origin.
static inline void win_remove_region_corners(ivec2 size, vec2 scale, int corner_radius,
                                             ivec2 origin, region_t *res) {
	static const struct {
		int x, y;
	} corner_index[] = {
	    {0, 0},
	    {0, 1},
	    {1, 0},
	    {1, 1},
	};
	rect_t rectangles[4];
	for (size_t i = 0; i < ARR_SIZE(corner_index); i++) {
		double x1 = origin.x +
		            corner_index[i].x * (size.width - corner_radius) * scale.x,
		       y1 = origin.y +
		            corner_index[i].y * (size.height - corner_radius) * scale.y,
		       x2 = x1 + corner_radius * scale.x, y2 = y1 + corner_radius * scale.y;
		rectangles[i] = (rect_t){
		    .x1 = (int32_t)floor(x1),
		    .y1 = (int32_t)floor(y1),
		    .x2 = (int32_t)ceil(x2),
		    .y2 = (int32_t)ceil(y2),
		};
	}
	region_t corners;
	pixman_region32_init_rects(&corners, rectangles, 4);
	pixman_region32_subtract(res, res, &corners);
	pixman_region32_fini(&corners);
}

static inline region_t attr_unused win_get_bounding_shape_global_by_val(struct win *w) {
	region_t ret;
	pixman_region32_init(&ret);
	pixman_region32_copy(&ret, &w->bounding_shape);
	pixman_region32_translate(&ret, w->g.x, w->g.y);
	return ret;
}

/**
 * Calculate the extents of the frame of the given window based on EWMH
 * _NET_FRAME_EXTENTS and the X window border width.
 */
static inline margin_t attr_pure attr_unused win_calc_frame_extents(const struct win *w) {
	margin_t result = w->frame_extents;
	result.top = max2(result.top, w->g.border_width);
	result.left = max2(result.left, w->g.border_width);
	result.bottom = max2(result.bottom, w->g.border_width);
	result.right = max2(result.right, w->g.border_width);
	return result;
}

/**
 * Check whether a window has WM frames.
 */
static inline bool attr_pure attr_unused win_has_frame(const struct win *w) {
	return w->g.border_width || w->frame_extents.top || w->frame_extents.left ||
	       w->frame_extents.right || w->frame_extents.bottom;
}

static inline const char *attr_pure attr_unused win_wm_ref_name(const struct wm_ref *ref) {
	if (ref == NULL) {
		return NULL;
	}
	auto win = wm_ref_deref(ref);
	if (win == NULL) {
		return NULL;
	}
	return win->name;
}
