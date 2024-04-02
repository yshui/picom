// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui

/// Yeah... We have our own window manager inside the compositor. As a compositor, we do
/// need to do a little bit of what a window manager does, to correctly render windows.
/// But our window manager is a lot less sophisticated than a average window manager. We
/// only keep track of a list of top-level windows, and the order they are stacked.
/// But OTOH doing window managing here is also somewhat more challenging. As we are not
/// a window manager, we don't actually know what window is an application window, what
/// is not. We have to rely on the real window manager playing nice and following the
/// ICCCM and EWMH standards.

#pragma once

#include <uthash.h>
#include <xcb/xproto.h>

#include "compiler.h"
#include "utils.h"

struct wm;
struct managed_win;
struct list_node;
struct x_connection;

/// Direct children of a toplevel.
struct subwin {
	xcb_window_t id;
	xcb_window_t toplevel;
	enum tristate has_wm_state;
	UT_hash_handle hh;
};

struct wm *wm_new(void);
void wm_free(struct wm *wm, struct x_connection *c);

struct managed_win *wm_active_win(struct wm *wm);
void wm_set_active_win(struct wm *wm, struct managed_win *w);
xcb_window_t wm_active_leader(struct wm *wm);
void wm_set_active_leader(struct wm *wm, xcb_window_t leader);

// Note: `wm` keeps track of 2 lists of windows. One is the window stack, which includes
// all windows that might need to be rendered, which means it would include destroyed
// windows in case they need to be faded out. This list is accessed by `wm_stack_*` series
// of functions. The other is a hash table of windows, which does not include destroyed
// windows. This list is accessed by `wm_find_*`, `wm_foreach`, and `wm_num_windows`.
// Adding a window to the window stack also automatically adds it to the hash table.

/// Find a window in the hash table from window id.
struct win *wm_find(struct wm *wm, xcb_window_t id);
/// Remove a window from the hash table.
void wm_remove(struct wm *wm, struct win *w);
/// Find a managed window from window id in window linked list of the session.
struct managed_win *wm_find_managed(struct wm *wm, xcb_window_t id);
// Find the WM frame of a client window. `id` is the client window id.
struct managed_win *wm_find_by_client(struct wm *wm, xcb_window_t client);
/// Call `func` on each toplevel window. `func` should return 0 if the iteration
/// should continue. If it returns anything else, the iteration will stop and the
/// return value will be returned from `wm_foreach`. If the iteration finishes
/// naturally, 0 will be returned.
int wm_foreach(struct wm *wm, int (*func)(struct win *, void *), void *data);
/// Returns the number of windows in the hash table.
unsigned attr_const wm_num_windows(const struct wm *wm);

/// Returns the cursor past the last window in the stack (the `end`). The window stack is
/// a cyclic linked list, so the next element after `end` is the first element. The `end`
/// itself does not point to a valid window. The address of `end` is stable as long as
/// the `struct wm` itself is not freed.
struct list_node *attr_const wm_stack_end(struct wm *wm);
/// Insert a new win entry at the top of the stack
struct win *wm_stack_add_top(struct wm *wm, xcb_window_t id);
/// Insert a new window above window with id `below`, if there is no window, add
/// to top New window will be in unmapped state
struct win *wm_stack_add_above(struct wm *wm, xcb_window_t id, xcb_window_t below);
// Find the managed window immediately below `i` in the window stack
struct managed_win *attr_pure wm_stack_next_managed(const struct wm *wm,
                                                    const struct list_node *cursor);
/// Move window `w` so it's right above `below`, if `below` is 0, `w` is moved
/// to the bottom of the stack
void wm_stack_move_above(struct wm *wm, struct win *w, xcb_window_t below);
/// Move window `w` to the bottom of the stack.
static inline void wm_stack_move_to_bottom(struct wm *wm, struct win *w) {
	wm_stack_move_above(wm, w, 0);
}
/// Move window `w` to the top of the stack.
void wm_stack_move_to_top(struct wm *wm, struct win *w);
/// Replace window `old` with `new_` in the stack, also replace the window in the hash
/// table. `old` will be freed.
void wm_stack_replace(struct wm *wm, struct win *old, struct win *new_);
unsigned attr_const wm_stack_num_managed_windows(const struct wm *wm);

struct subwin *wm_subwin_add_and_subscribe(struct wm *wm, struct x_connection *c,
                                           xcb_window_t id, xcb_window_t parent);
struct subwin *wm_subwin_find(struct wm *wm, xcb_window_t id);
void wm_subwin_remove(struct wm *wm, struct subwin *subwin);
void wm_subwin_remove_and_unsubscribe(struct wm *wm, struct x_connection *c,
                                      struct subwin *subwin);
/// Remove all subwins associated with a toplevel window
void wm_subwin_remove_and_unsubscribe_for_toplevel(struct wm *wm, struct x_connection *c,
                                                   xcb_window_t toplevel);
