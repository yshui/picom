// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui.

// === Includes ===
#pragma once
#include <stdbool.h>
#include <xcb/xproto.h>

#include <X11/Xutil.h>
#include "backend/backend.h"
#include "backend/driver.h"
#include "c2.h"
#include "common.h"
#include "config.h"
#include "ev.h"
#include "list.h"
#include "region.h"
#include "render.h"
#include "types.h"
#include "win.h"
#include "x.h"
#include "xcb/render.h"

enum root_flags {
	ROOT_FLAGS_SCREEN_CHANGE = 1,        // Received RandR screen change notify, we
	                                     // use this to track refresh rate changes
	ROOT_FLAGS_CONFIGURED = 2        // Received configure notify on the root window
};

void add_damage(session_t *ps, const region_t *damage);
uint32_t determine_evmask(session_t *ps, xcb_window_t wid, win_evmode_t mode);
void circulate_win(session_t *ps, xcb_circulate_notify_event_t *ce);
void root_damaged(session_t *ps);
void queue_redraw(session_t *ps);
void discard_pending(session_t *ps, uint32_t sequence);
void set_root_flags(session_t *ps, uint64_t flags);
void quit(session_t *ps);
xcb_window_t session_get_target_window(session_t *);
uint8_t session_redirection_mode(session_t *ps);
struct options *session_get_options(session_t *ps);
struct ev_loop *session_get_mainloop(session_t *ps);
struct x_connection *session_get_x_connection(session_t *ps);
xcb_window_t session_get_overlay(session_t *ps);
struct atom *session_get_atoms(session_t *ps);
struct c2_state *session_get_c2(session_t *ps);
void session_mark_updates_pending(session_t *ps);
// TODO(yshui) can we lump these 3 backend related functions together?
struct backend_base *session_get_backend_data(session_t *ps);
struct backend_shadow_context *session_get_backend_shadow_context(session_t *ps);
void *session_get_backend_blur_context(session_t *ps);
struct shader_info *session_get_shader_info(session_t *ps, const char *key);
bool session_is_redirected(session_t *ps);
void session_xsync_wait_fence(session_t *ps);
enum driver session_get_driver(session_t *ps);
struct damage_ring *session_get_damage_ring(session_t *ps);
void session_assert_server_grabbed(session_t *ps);
image_handle session_get_root_image(session_t *ps);
/// Record the amount of CPU time spent in the render cycle. This is called in
/// backend.c just before we start sending backend rendering commands. `struct session`
/// keeps track when each render call started, when this function is called, the time
/// difference is used as the CPU time spent in the render cycle.
void session_record_cpu_time(session_t *ps);

// TODO(yshui) move window related data out of session_t, and remove these getters
void session_delete_win(session_t *ps, struct win *w);
struct list_node *session_get_win_stack(session_t *ps);
struct managed_win *session_get_active_win(session_t *ps);
void session_set_active_win(session_t *ps, struct managed_win *w);
struct win *session_get_next_win_in_stack(session_t *ps, struct win *w);
unsigned int session_get_window_count(session_t *ps);
void session_foreach_win(session_t *ps, void (*func)(struct win *w, void *data), void *data);
xcb_window_t session_get_active_leader(session_t *ps);
void session_set_active_leader(session_t *ps, xcb_window_t leader);
void session_clear_cache_win_leaders(session_t *ps);
bool session_is_win_region_ignore_valid(session_t *ps, const struct managed_win *w);
struct win *session_find_win(session_t *ps, xcb_window_t id);
/// Insert a new window above window with id `below`, if there is no window, add to top
/// New window will be in unmapped state
struct win *session_add_win_above(session_t *ps, xcb_window_t id, xcb_window_t below);
/// Insert a new win entry at the top of the stack
struct win *session_add_win_top(session_t *ps, xcb_window_t id);
/// Move window `w` to be right above `below`
void session_restack_above(session_t *ps, struct win *w, xcb_window_t below);
/// Move window `w` to the bottom of the stack
void session_restack_bottom(session_t *ps, struct win *w);
/// Move window `w` to the top of the stack
void session_restack_top(session_t *ps, struct win *w);
/**
 * Find out the WM frame of a client window using existing data.
 *
 * @param id window ID
 * @return struct win object of the found window, NULL if not found
 */
struct managed_win *session_find_toplevel(session_t *ps, xcb_window_t id);
// Find the managed window immediately below `w` in the window stack
struct managed_win *
session_get_next_managed_win_in_stack(const session_t *ps, const struct list_node *cursor);

// TODO(yshui) Legacy backend stuff, should probably lump them together. Also remove them
// once the legacy backends are removed.
#ifdef CONFIG_OPENGL
struct glx_session *session_get_psglx(session_t *ps);
void session_set_psglx(session_t *ps, struct glx_session *psglx);
#endif
struct {
	int height, width;
} session_get_root_extent(session_t *ps);
xcb_render_picture_t *session_get_alpha_pictures(session_t *ps);
struct x_monitors *session_get_monitors(session_t *ps);
paint_t *session_get_tgt_buffer(session_t *ps);
xcb_render_picture_t session_get_tgt_picture(session_t *ps);
paint_t *session_get_root_tile_paint(session_t *ps);
bool session_get_root_tile_fill(session_t *ps);
void session_set_root_tile_fill(session_t *ps, bool fill);
void session_vsync_wait(session_t *ps);
void session_set_vsync_wait(session_t *ps, int (*vsync_wait)(session_t *));
xcb_render_picture_t session_get_white_picture(session_t *ps);
xcb_render_picture_t session_get_black_picture(session_t *ps);
xcb_render_picture_t session_get_cshadow_picture(session_t *ps);
void session_set_black_picture(session_t *ps, xcb_render_picture_t p);
void session_set_white_picture(session_t *ps, xcb_render_picture_t p);
void session_set_cshadow_picture(session_t *ps, xcb_render_picture_t p);
region_t *session_get_screen_reg(session_t *ps);
region_t *session_get_shadow_exclude_reg(session_t *ps);
struct x_convolution_kernel **session_get_blur_kern_cache(session_t *ps);
void session_set_blur_kern_cache(session_t *ps, struct x_convolution_kernel **cache);

// TODO(yshui) has_extension and get_*_extension_error should probably be in struct
// x_connection, or just use xcb functions instead - those are cached as well.
bool session_has_glx_extension(session_t *ps);
bool session_has_shape_extension(session_t *ps);
bool session_has_present_extension(session_t *ps);
bool session_has_randr_extension(session_t *ps);
bool session_has_xsync_extension(session_t *ps);
int session_get_xfixes_extension_error(session_t *ps);
int session_get_render_extension_error(session_t *ps);
int session_get_damage_extension_error(session_t *ps);
int session_get_glx_extension_error(session_t *ps);
int session_get_xsync_extension_error(session_t *ps);
int session_get_xsync_extention_event(session_t *ps);
int session_get_damage_extention_event(session_t *ps);
int session_get_shape_extention_event(session_t *ps);
int session_get_randr_extention_event(session_t *ps);

#ifdef CONFIG_DBUS
struct cdbus_data *session_get_cdbus(struct session *);
#else
static inline struct cdbus_data *session_get_cdbus(session_t *ps attr_unused) {
	return NULL;
}
#endif

/**
 * Set a <code>switch_t</code> array of all unset wintypes to true.
 */
static inline void wintype_arr_enable_unset(switch_t arr[]) {
	wintype_t i;

	for (i = 0; i < NUM_WINTYPES; ++i) {
		if (UNSET == arr[i]) {
			arr[i] = ON;
		}
	}
}

/**
 * Check if a window ID exists in an array of window IDs.
 *
 * @param arr the array of window IDs
 * @param count amount of elements in the array
 * @param wid window ID to search for
 */
static inline bool array_wid_exists(const xcb_window_t *arr, int count, xcb_window_t wid) {
	while (count--) {
		if (arr[count] == wid) {
			return true;
		}
	}

	return false;
}

#ifndef CONFIG_OPENGL
static inline void free_paint_glx(session_t *ps attr_unused, paint_t *p attr_unused) {
}
static inline void
free_win_res_glx(session_t *ps attr_unused, struct managed_win *w attr_unused) {
}
#endif
