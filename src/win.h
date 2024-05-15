// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2013 Richard Grenville <pyxlcy@gmail.com>
#pragma once
#include <stdbool.h>
#include <xcb/damage.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

#include <backend/backend.h>

#include "uthash_extra.h"

#include "c2.h"
#include "compiler.h"
#include "list.h"
#include "region.h"
#include "render.h"
#include "transition.h"
#include "types.h"
#include "utils.h"
#include "win_defs.h"
#include "x.h"
#include "xcb/xproto.h"

struct backend_base;
typedef struct session session_t;
typedef struct _glx_texture glx_texture_t;

#define win_stack_foreach_managed(w, win_stack)                                          \
	list_foreach(struct managed_win, w, win_stack,                                   \
	             base.stack_neighbour) if ((w)->base.managed)

#define win_stack_foreach_managed_safe(w, win_stack)                                     \
	list_foreach_safe(struct managed_win, w, win_stack,                              \
	                  base.stack_neighbour) if ((w)->base.managed)

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

/// Structure representing a top-level managed window.
struct win {
	UT_hash_handle hh;
	struct list_node stack_neighbour;
	/// ID of the top-level frame window.
	xcb_window_t id;
	/// Generation of the window.
	/// (see `struct wm::generation` for explanation of what a generation is)
	uint64_t generation;
	/// Whether the window is destroyed from Xorg's perspective
	bool destroyed : 1;
	/// True if we just received CreateNotify, and haven't queried X for any info
	/// about the window
	bool is_new : 1;
	/// True if this window is managed, i.e. this struct is actually a `managed_win`.
	/// Always false if `is_new` is true.
	bool managed : 1;
};

struct win_geometry {
	int16_t x;
	int16_t y;
	uint16_t width;
	uint16_t height;
	uint16_t border_width;
};

struct managed_win {
	struct win base;
	/// backend data attached to this window. Only available when
	/// `state` is not UNMAPPED
	image_handle win_image;
	image_handle shadow_image;
	image_handle mask_image;
	/// Pointer to the next higher window to paint.
	struct managed_win *prev_trans;
	/// Number of windows above this window
	int stacking_rank;
	// TODO(yshui) rethink reg_ignore

	// Core members
	/// The "mapped state" of this window, doesn't necessary
	/// match X mapped state, because of fading.
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
	winmode_t mode;
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
	/// ID of the top-level client window of the window.
	xcb_window_t client_win;
	/// Type of the window.
	wintype_t window_type;
	/// Leader window ID of the window.
	xcb_window_t leader;
	/// Cached topmost window ID of the window.
	xcb_window_t cache_leader;

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
	struct animatable opacity;
	/// Opacity of the window's background blur
	/// Used to gracefully fade in/out the window, otherwise the blur
	/// would be at full/zero intensity immediately which will be jarring.
	struct animatable blur_opacity;
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
	/// Number of animations currently in progress
	unsigned int number_of_animations;

#ifdef CONFIG_OPENGL
	/// Textures and FBO background blur use.
	glx_blur_cache_t glx_blur_cache;
	/// Background texture of the window
	glx_texture_t *glx_texture_bg;
#endif

	/// The damaged region of the window, in window local coordinates.
	region_t damaged;
};

/// Process pending updates/images flags on a window. Has to be called in X critical
/// section
void win_process_update_flags(session_t *ps, struct managed_win *w);
void win_process_image_flags(session_t *ps, struct managed_win *w);

/// Start the unmap of a window. We cannot unmap immediately since we might need to fade
/// the window out.
void unmap_win_start(struct session *, struct managed_win *);

/// Start the mapping of a window. We cannot map immediately since we might need to fade
/// the window in.
void map_win_start(struct session *, struct managed_win *);

/// Start the destroying of a window. Windows cannot always be destroyed immediately
/// because of fading and such.
void destroy_win_start(session_t *ps, struct win *w);

/// Release images bound with a window, set the *_NONE flags on the window. Only to be
/// used when de-initializing the backend outside of win.c
void win_release_images(struct backend_base *base, struct managed_win *w);
winmode_t attr_pure win_calc_mode_raw(const struct managed_win *w);
// TODO(yshui) `win_calc_mode` is only used by legacy backends
winmode_t attr_pure win_calc_mode(const struct managed_win *w);
void win_set_shadow_force(session_t *ps, struct managed_win *w, switch_t val);
void win_set_fade_force(struct managed_win *w, switch_t val);
void win_set_focused_force(session_t *ps, struct managed_win *w, switch_t val);
void win_set_invert_color_force(session_t *ps, struct managed_win *w, switch_t val);
/**
 * Set real focused state of a window.
 */
void win_set_focused(session_t *ps, struct managed_win *w);
bool attr_pure win_should_fade(session_t *ps, const struct managed_win *w);
void win_on_factor_change(session_t *ps, struct managed_win *w);
void win_unmark_client(struct managed_win *w);

bool attr_pure win_should_dim(session_t *ps, const struct managed_win *w);

void win_update_monitor(struct x_monitors *monitors, struct managed_win *mw);

/// Recheck if a window is fullscreen
void win_update_is_fullscreen(const session_t *ps, struct managed_win *w);
/**
 * Check if a window has BYPASS_COMPOSITOR property set
 */
bool win_is_bypassing_compositor(const session_t *ps, const struct managed_win *w);
/**
 * Get a rectangular region in global coordinates a window (and possibly
 * its shadow) occupies.
 *
 * Note w->shadow and shadow geometry must be correct before calling this
 * function.
 */
void win_extents(const struct managed_win *w, region_t *res);
region_t win_extents_by_val(const struct managed_win *w);
/**
 * Add a window to damaged area.
 *
 * @param ps current session
 * @param w struct _win element representing the window
 */
void add_damage_from_win(session_t *ps, const struct managed_win *w);
/**
 * Get a rectangular region a window occupies, excluding frame and shadow.
 *
 * Return region in global coordinates.
 */
void win_get_region_noframe_local(const struct managed_win *w, region_t *);
void win_get_region_noframe_local_without_corners(const struct managed_win *w, region_t *);

/// Get the region for the frame of the window
void win_get_region_frame_local(const struct managed_win *w, region_t *res);
/// Get the region for the frame of the window, by value
region_t win_get_region_frame_local_by_val(const struct managed_win *w);
/// Query the Xorg for information about window `win`
/// `win` pointer might become invalid after this function returns
struct win *attr_ret_nonnull maybe_allocate_managed_win(session_t *ps, struct win *win);

/**
 * Release a destroyed window that is no longer needed.
 */
void destroy_win_finish(session_t *ps, struct win *w);

/// Skip the current in progress fading of window,
/// transition the window straight to its end state
void win_skip_fading(struct managed_win *w);

/**
 * Check if a window is focused, without using any focus rules or forced focus settings
 */
bool attr_pure win_is_focused_raw(const struct managed_win *w);

/// check if window has ARGB visual
bool attr_pure win_has_alpha(const struct managed_win *w);

/// Whether it looks like a WM window. We consider a window WM window if
/// it does not have a decedent with WM_STATE and it is not override-
/// redirected itself.
static inline bool attr_pure win_is_wmwin(const struct managed_win *w) {
	return w->base.id == w->client_win && !w->a.override_redirect;
}

static inline struct managed_win *win_as_managed(struct win *w) {
	BUG_ON(!w->managed);
	return (struct managed_win *)w;
}

static inline const char *win_get_name_if_managed(const struct win *w) {
	if (!w->managed) {
		return "(unmanaged)";
	}
	auto mw = (struct managed_win *)w;
	return mw->name;
}

/// check if reg_ignore_valid is true for all windows above us
bool attr_pure win_is_region_ignore_valid(session_t *ps, const struct managed_win *w);

/// Whether a given window is mapped on the X server side
bool win_is_mapped_in_x(const struct managed_win *w);
/// Set flags on a window. Some sanity checks are performed
void win_set_flags(struct managed_win *w, uint64_t flags);
/// Clear flags on a window. Some sanity checks are performed
void win_clear_flags(struct managed_win *w, uint64_t flags);
/// Returns true if any of the flags in `flags` is set
bool win_check_flags_any(struct managed_win *w, uint64_t flags);
/// Returns true if all of the flags in `flags` are set
bool win_check_flags_all(struct managed_win *w, uint64_t flags);
/// Mark properties as stale for a window
void win_set_properties_stale(struct managed_win *w, const xcb_atom_t *prop, int nprops);

xcb_window_t win_get_client_window(struct x_connection *c, struct wm *wm,
                                   struct atom *atoms, const struct managed_win *w);
bool win_update_wintype(struct x_connection *c, struct atom *atoms, struct managed_win *w);
/**
 * Retrieve frame extents from a window.
 */
void win_update_frame_extents(struct x_connection *c, struct atom *atoms,
                              struct managed_win *w, xcb_window_t client,
                              double frame_opacity);
/**
 * Retrieve the <code>WM_CLASS</code> of a window and update its
 * <code>win</code> structure.
 */
bool win_update_class(struct x_connection *c, struct atom *atoms, struct managed_win *w);
int win_update_role(struct x_connection *c, struct atom *atoms, struct managed_win *w);
int win_update_name(struct x_connection *c, struct atom *atoms, struct managed_win *w);
void win_on_win_size_change(struct managed_win *w, int shadow_offset_x,
                            int shadow_offset_y, int shadow_radius);
void win_update_bounding_shape(struct x_connection *c, struct managed_win *w,
                               bool shape_exists, bool detect_rounded_corners);
bool win_update_prop_fullscreen(struct x_connection *c, const struct atom *atoms,
                                struct managed_win *w);

static inline attr_unused void win_set_property_stale(struct managed_win *w, xcb_atom_t prop) {
	return win_set_properties_stale(w, (xcb_atom_t[]){prop}, 1);
}

/// Free all resources in a struct win
void free_win_res(session_t *ps, struct managed_win *w);

static inline void win_region_remove_corners(const struct managed_win *w, region_t *res) {
	region_t corners;
	pixman_region32_init_rects(
	    &corners,
	    (rect_t[]){
	        {.x1 = 0, .y1 = 0, .x2 = w->corner_radius, .y2 = w->corner_radius},
	        {.x1 = 0, .y1 = w->heightb - w->corner_radius, .x2 = w->corner_radius, .y2 = w->heightb},
	        {.x1 = w->widthb - w->corner_radius, .y1 = 0, .x2 = w->widthb, .y2 = w->corner_radius},
	        {.x1 = w->widthb - w->corner_radius,
	         .y1 = w->heightb - w->corner_radius,
	         .x2 = w->widthb,
	         .y2 = w->heightb},
	    },
	    4);
	pixman_region32_subtract(res, res, &corners);
	pixman_region32_fini(&corners);
}

static inline region_t attr_unused win_get_bounding_shape_global_by_val(struct managed_win *w) {
	region_t ret;
	pixman_region32_init(&ret);
	pixman_region32_copy(&ret, &w->bounding_shape);
	pixman_region32_translate(&ret, w->g.x, w->g.y);
	return ret;
}

static inline region_t
win_get_bounding_shape_global_without_corners_by_val(struct managed_win *w) {
	region_t ret;
	pixman_region32_init(&ret);
	pixman_region32_copy(&ret, &w->bounding_shape);
	win_region_remove_corners(w, &ret);
	pixman_region32_translate(&ret, w->g.x, w->g.y);
	return ret;
}

/**
 * Calculate the extents of the frame of the given window based on EWMH
 * _NET_FRAME_EXTENTS and the X window border width.
 */
static inline margin_t attr_pure attr_unused win_calc_frame_extents(const struct managed_win *w) {
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
static inline bool attr_pure attr_unused win_has_frame(const struct managed_win *w) {
	return w->g.border_width || w->frame_extents.top || w->frame_extents.left ||
	       w->frame_extents.right || w->frame_extents.bottom;
}
