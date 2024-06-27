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
#include "defs.h"
#include "region.h"
#include "render.h"
#include "transition/script.h"
#include "utils/list.h"
#include "utils/misc.h"
#include "wm/wm.h"
#include "x.h"
#include "xcb/xproto.h"

struct backend_base;
typedef struct session session_t;
typedef struct _glx_texture glx_texture_t;
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

#ifdef CONFIG_OPENGL
// FIXME this type should be in opengl.h
//       it is very unideal for it to be here
typedef struct {
	/// Framebuffer used for blurring.
	GLuint fbo;
	/// Textures used for blurring.
	GLuint textures[2];
	/// Width of the textures.
	int width;
	/// Height of the textures.
	int height;
} glx_blur_cache_t;
#endif
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
/// out and delay their application so determine which animation to run is easier.
struct win_state_change {
	winstate_t state;
};

struct win {
	/// Reference back to the position of this window inside the window tree.
	struct wm_ref *tree_ref;
	/// backend data attached to this window. Only available when
	/// `state` is not UNMAPPED
	image_handle win_image;
	image_handle shadow_image;
	image_handle mask_image;
	// TODO(yshui) only used by legacy backends, remove.
	/// Pointer to the next higher window to paint.
	struct win *prev_trans;
	// TODO(yshui) rethink reg_ignore

	// Core members
	winstate_t state;
	/// Window attributes.
	xcb_get_window_attributes_reply_t a;
	/// The geometry of the window body, excluding the window border region.
	struct win_geometry g;
	/// Updated geometry received in events
	struct win_geometry pending_g;
	/// X RandR monitor this window is on.
	int randr_monitor;
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
	/// Paint info of the window.
	paint_t paint;
	/// bitmap for properties which needs to be updated
	uint64_t *stale_props;
	/// number of uint64_ts that has been allocated for stale_props
	size_t stale_props_capacity;

	/// Bounding shape of the window. In local coordinates.
	/// See above about coordinate systems.
	region_t bounding_shape;
	/// Window flags. Definitions above.
	uint64_t flags;
	/// The region of screen that will be obscured when windows above is painted,
	/// in global coordinates.
	/// We use this to reduce the pixels that needed to be paint when painting
	/// this window and anything underneath. Depends on window frame
	/// opacity state, window geometry, window mapped/unmapped state,
	/// window mode of the windows above. DOES NOT INCLUDE the body of THIS WINDOW.
	/// NULL means reg_ignore has not been calculated for this window.
	rc_region_t *reg_ignore;
	/// Whether the reg_ignore of all windows beneath this window are valid
	bool reg_ignore_valid;
	/// Cached width/height of the window including border.
	int widthb, heightb;
	/// Whether the window is bounding-shaped.
	bool bounding_shaped;
	/// Whether the window just have rounded corners.
	bool rounded_corners;
	/// Whether this window is to be painted.
	bool to_paint;
	/// Whether the window is painting excluded.
	bool paint_excluded;
	/// Whether the window is unredirect-if-possible excluded.
	bool unredir_if_possible_excluded;
	/// Whether this window is in open/close state.
	bool in_openclose;

	// Client window related members
	/// Type of the window.
	wintype_t window_type;
	/// Leader window ID of the window.
	xcb_window_t leader;
	/// Cached topmost window ID of the leader window.
	struct wm_ref *cache_leader;

	// Focus-related members
	/// Whether the window is to be considered focused.
	bool focused;
	/// Override value of window focus state. Set by D-Bus method calls.
	switch_t focused_force;

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
	/// Whether the window is the EWMH active window.
	bool is_ewmh_focused;

	// Opacity-related members
	/// Window opacity
	double opacity;
	/// true if window (or client window, for broken window managers
	/// not transferring client window's _NET_WM_WINDOW_OPACITY value) has opacity
	/// prop
	bool has_opacity_prop;
	/// Cached value of opacity window attribute.
	opacity_t opacity_prop;
	/// true if opacity is set by some rules
	bool opacity_is_set;
	/// Last window opacity value set by the rules.
	double opacity_set;

	/// Radius of rounded window corners
	int corner_radius;
	float border_col[4];

	// Fading-related members
	/// Override value of window fade state. Set by D-Bus method calls.
	switch_t fade_force;
	/// Whether fading is excluded by the rules. Calculated.
	bool fade_excluded;

	/// Whether transparent clipping is excluded by the rules.
	bool transparent_clipping;

	// Frame-opacity-related members
	/// Current window frame opacity. Affected by window opacity.
	double frame_opacity;
	/// Frame extents. Acquired from _NET_FRAME_EXTENTS.
	margin_t frame_extents;

	// Shadow-related members
	/// Whether a window has shadow. Calculated.
	bool shadow;
	/// Override value of window shadow state. Set by D-Bus method calls.
	switch_t shadow_force;
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
	/// Picture to render shadow. Affected by window size.
	paint_t shadow_paint;
	/// The value of _COMPTON_SHADOW attribute of the window. Below 0 for
	/// none.
	long long prop_shadow;
	/// Do not paint shadow over this window.
	bool clip_shadow_above;

	// Dim-related members
	/// Whether the window is to be dimmed.
	bool dim;

	/// Whether to invert window color.
	bool invert_color;
	/// Override value of window color inversion state. Set by D-Bus method
	/// calls.
	switch_t invert_color_force;

	/// Whether to blur window background.
	bool blur_background;

	/// The custom window shader to use when rendering.
	struct shader_info *fg_shader;

	struct c2_window_state c2_state;

	// Animation related

#ifdef CONFIG_OPENGL
	/// Textures and FBO background blur use.
	glx_blur_cache_t glx_blur_cache;
	/// Background texture of the window
	glx_texture_t *glx_texture_bg;
#endif

	/// The damaged region of the window, in window local coordinates.
	region_t damaged;

	/// Previous state of the window before state changed. This is used
	/// by `win_process_animation_and_state_change` to trigger appropriate
	/// animations.
	struct win_state_change previous;
	struct script_instance *running_animation;
	const int *running_animation_outputs;
	uint64_t running_animation_suppressions;
};

struct win_script_context {
	double x, y, width, height;
	double opacity_before, opacity;
	double monitor_x, monitor_y;
	double monitor_width, monitor_height;
};

static const struct script_context_info win_script_context_info[] = {
    {"window-x", offsetof(struct win_script_context, x)},
    {"window-y", offsetof(struct win_script_context, y)},
    {"window-width", offsetof(struct win_script_context, width)},
    {"window-height", offsetof(struct win_script_context, height)},
    {"window-raw-opacity-before", offsetof(struct win_script_context, opacity_before)},
    {"window-raw-opacity", offsetof(struct win_script_context, opacity)},
    {"window-monitor-x", offsetof(struct win_script_context, monitor_x)},
    {"window-monitor-y", offsetof(struct win_script_context, monitor_y)},
    {"window-monitor-width", offsetof(struct win_script_context, monitor_width)},
    {"window-monitor-height", offsetof(struct win_script_context, monitor_height)},
    {NULL, 0}        //
};

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
    [NUM_OF_WIN_SCRIPT_OUTPUTS] = {NULL},
};

/// Process pending updates/images flags on a window. Has to be called in X critical
/// section. Returns true if the window had an animation running and it has just finished,
/// or if the window's states just changed and there is no animation defined for this
/// state change.
bool win_process_animation_and_state_change(struct session *ps, struct win *w, double delta_t);
double win_animatable_get(const struct win *w, enum win_script_output output);
void win_process_update_flags(session_t *ps, struct win *w);
void win_process_image_flags(session_t *ps, struct win *w);

/// Start the unmap of a window. We cannot unmap immediately since we might need to fade
/// the window out.
void unmap_win_start(struct win *);
void unmap_win_finish(session_t *ps, struct win *w);
/// Start the destroying of a window. Windows cannot always be destroyed immediately
/// because of fading and such.
void win_destroy_start(session_t *ps, struct win *w);
void win_map_start(struct win *w);
/// Release images bound with a window, set the *_NONE flags on the window. Only to be
/// used when de-initializing the backend outside of win.c
void win_release_images(struct backend_base *backend, struct win *w);
winmode_t attr_pure win_calc_mode_raw(const struct win *w);
// TODO(yshui) `win_calc_mode` is only used by legacy backends
winmode_t attr_pure win_calc_mode(const struct win *w);
void win_set_shadow_force(session_t *ps, struct win *w, switch_t val);
void win_set_fade_force(struct win *w, switch_t val);
void win_set_focused_force(session_t *ps, struct win *w, switch_t val);
void win_set_invert_color_force(session_t *ps, struct win *w, switch_t val);
/**
 * Set real focused state of a window.
 */
void win_set_focused(session_t *ps, struct win *w);
void win_on_factor_change(session_t *ps, struct win *w);
void win_on_client_update(session_t *ps, struct win *w);

bool attr_pure win_should_dim(session_t *ps, const struct win *w);

void win_update_monitor(struct x_monitors *monitors, struct win *mw);

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
/**
 * Add a window to damaged area.
 *
 * @param ps current session
 * @param w struct _win element representing the window
 */
void add_damage_from_win(session_t *ps, const struct win *w);
/**
 * Get a rectangular region a window occupies, excluding frame and shadow.
 *
 * Return region in global coordinates.
 */
void win_get_region_noframe_local(const struct win *w, region_t *);
void win_get_region_noframe_local_without_corners(const struct win *w, region_t *);

/// Get the region for the frame of the window
void win_get_region_frame_local(const struct win *w, region_t *res);
/// Get the region for the frame of the window, by value
region_t win_get_region_frame_local_by_val(const struct win *w);
/// Query the Xorg for information about window `win`
/// `win` pointer might become invalid after this function returns
struct win *win_maybe_allocate(session_t *ps, struct wm_ref *cursor);

/**
 * Release a destroyed window that is no longer needed.
 */
void win_destroy_finish(session_t *ps, struct win *w);

/**
 * Check if a window is focused, without using any focus rules or forced focus settings
 */
bool attr_pure win_is_focused_raw(const struct win *w);

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

/// check if reg_ignore_valid is true for all windows above us
bool attr_pure win_is_region_ignore_valid(session_t *ps, const struct win *w);

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

bool win_update_wintype(struct x_connection *c, struct atom *atoms, struct win *w);
/**
 * Retrieve frame extents from a window.
 */
void win_update_frame_extents(struct x_connection *c, struct atom *atoms, struct win *w,
                              xcb_window_t client, double frame_opacity);
/**
 * Retrieve the <code>WM_CLASS</code> of a window and update its
 * <code>win</code> structure.
 */
bool win_update_class(struct x_connection *c, struct atom *atoms, struct win *w);
int win_update_role(struct x_connection *c, struct atom *atoms, struct win *w);
int win_update_name(struct x_connection *c, struct atom *atoms, struct win *w);
void win_on_win_size_change(struct win *w, int shadow_offset_x, int shadow_offset_y,
                            int shadow_radius);
void win_update_bounding_shape(struct x_connection *c, struct win *w, bool shape_exists,
                               bool detect_rounded_corners);
bool win_update_prop_fullscreen(struct x_connection *c, const struct atom *atoms,
                                struct win *w);

static inline attr_unused void win_set_property_stale(struct win *w, xcb_atom_t prop) {
	return win_set_properties_stale(w, (xcb_atom_t[]){prop}, 1);
}

/// Free all resources in a struct win
void free_win_res(session_t *ps, struct win *w);

/// Remove the corners of window `w` from region `res`. `origin` is the top-left corner of
/// `w` in `res`'s coordinate system.
static inline void
win_region_remove_corners(const struct win *w, ivec2 origin, region_t *res) {
	static const int corner_index[][2] = {
	    {0, 0},
	    {0, 1},
	    {1, 0},
	    {1, 1},
	};
	rect_t rectangles[4];
	for (size_t i = 0; i < ARR_SIZE(corner_index); i++) {
		rectangles[i] = (rect_t){
		    .x1 = origin.x + corner_index[i][0] * (w->widthb - w->corner_radius),
		    .y1 = origin.y + corner_index[i][1] * (w->heightb - w->corner_radius),
		};
		rectangles[i].x2 = rectangles[i].x1 + w->corner_radius;
		rectangles[i].y2 = rectangles[i].y1 + w->corner_radius;
	}
	region_t corners;
	pixman_region32_init_rects(&corners, rectangles, 4);
	pixman_region32_subtract(res, res, &corners);
	pixman_region32_fini(&corners);
}

/// Like `win_region_remove_corners`, but `origin` is (0, 0).
static inline void win_region_remove_corners_local(const struct win *w, region_t *res) {
	win_region_remove_corners(w, (ivec2){0, 0}, res);
}

static inline region_t attr_unused win_get_bounding_shape_global_by_val(struct win *w) {
	region_t ret;
	pixman_region32_init(&ret);
	pixman_region32_copy(&ret, &w->bounding_shape);
	pixman_region32_translate(&ret, w->g.x, w->g.y);
	return ret;
}

static inline region_t win_get_bounding_shape_global_without_corners_by_val(struct win *w) {
	region_t ret;
	pixman_region32_init(&ret);
	pixman_region32_copy(&ret, &w->bounding_shape);
	win_region_remove_corners_local(w, &ret);
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
