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

#include <assert.h>
#include <stdalign.h>

#include <uthash.h>
#include <xcb/xproto.h>

#include <picom/types.h>

#include "compiler.h"

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

enum wm_tree_change_type {
	/// The client window of a toplevel changed
	WM_TREE_CHANGE_CLIENT,
	/// A toplevel window is killed on the X server side
	/// A zombie will be left in its place.
	WM_TREE_CHANGE_TOPLEVEL_KILLED,
	/// A new toplevel window appeared
	WM_TREE_CHANGE_TOPLEVEL_NEW,

	// TODO(yshui): This is a stop-gap measure to make sure we invalidate `reg_ignore`
	// of windows. Once we get rid of `reg_ignore`, which is only used by the legacy
	// backends, this event should be removed.
	//
	// (`reg_ignore` is the cached cumulative opaque region of all windows above a
	// window in the stacking order. If it actually is performance critical, we
	// can probably cache it more cleanly in renderer layout.)

	/// The stacking order of toplevel windows changed. Note, toplevel gone/new
	/// changes also imply a restack.
	WM_TREE_CHANGE_TOPLEVEL_RESTACKED,

	/// Nothing changed
	WM_TREE_CHANGE_NONE,
};

typedef struct wm_treeid {
	/// The generation of the window ID. This is used to detect if the window ID is
	/// reused. Inherited from the wm_tree at cr
	uint64_t gen;
	/// The X window ID.
	xcb_window_t x;

	/// Explicit padding
	char padding[4];
} wm_treeid;

static const wm_treeid WM_TREEID_NONE = {.gen = 0, .x = XCB_NONE};

static_assert(sizeof(wm_treeid) == 16, "wm_treeid size is not 16 bytes");
static_assert(alignof(wm_treeid) == 8, "wm_treeid alignment is not 8 bytes");

struct wm_change {
	enum wm_tree_change_type type;
	/// The toplevel window this change is about. For
	/// `WM_TREE_CHANGE_TOPLEVEL_KILLED`, this is the zombie window left in place of
	/// the killed toplevel. For `WM_TREE_CHANGE_TOPLEVEL_RESTACKED`, this is NULL.
	struct wm_ref *toplevel;
	struct {
		wm_treeid old;
		wm_treeid new_;
	} client;
};

/// Reference to a window in the `struct wm`. Most of wm operations operate on wm_refs. If
/// the referenced window is managed, a `struct window` can be retrieved by
/// `wm_ref_deref`.
struct wm_ref;
struct atom;

static inline bool wm_treeid_eq(wm_treeid a, wm_treeid b) {
	return a.gen == b.gen && a.x == b.x;
}

struct wm *wm_new(void);
void wm_free(struct wm *wm, struct x_connection *c);

struct managed_win *wm_active_win(struct wm *wm);
void wm_set_active_win(struct wm *wm, struct managed_win *w);
xcb_window_t wm_active_leader(struct wm *wm);
void wm_set_active_leader(struct wm *wm, xcb_window_t leader);

// Note: `wm` keeps track of 2 lists of windows. One is the window stack, which includes
// all windows that might need to be rendered, which means it would include destroyed
// windows in case they have close animation. This list is accessed by `wm_stack_*` series
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
xcb_window_t attr_pure wm_ref_win_id(const struct wm_ref *cursor);

/// Destroy a window. Children of this window should already have been destroyed. This
/// will cause a `WM_TREE_CHANGE_TOPLEVEL_KILLED` event to be generated, and a zombie
/// window to be placed where the window was.
void wm_destroy(struct wm *wm, xcb_window_t wid);
void wm_reparent(struct wm *wm, xcb_window_t wid, xcb_window_t parent);

/// Create a tree node for `wid`, with `parent` as its parent. The parent must already
/// be in the window tree. This function creates a placeholder tree node, without
/// contacting the X server, thus can be called outside of the X critical section. The
/// expectation is that the caller will later call `wm_complete_import` inside the
/// X critical section to complete the import.
///
/// ## NOTE
///
/// The reason for this complicated dance is because we want to catch all windows ever
/// created on X server's side. For a newly created windows, we will setup a
/// SubstructureNotify event mask to catch any new windows created under it. But between
/// the time we received the creation event and the time we setup the event mask, if any
/// windows were created under the new window, we will miss them. Therefore we have to
/// scan the new windows in X critical section so they won't change as we scan.
///
/// On the other hand, we can't push everything to the X critical section, because
/// updating the window stack requires knowledge of all windows in the stack. Think
/// ConfigureNotify, if we don't know about the `above_sibling` window, we don't know
/// where to put the window. So we need to create an incomplete import first.
///
/// But wait, this is actually way more involved. Because we choose to not set up event
/// masks for incomplete imports (we can also choose otherwise, but that also has its own
/// set of problems), there is a whole subtree of windows we don't know about. And those
/// windows might involve in reparent events. To handle that, we essentially put "fog of
/// war" under any incomplete imports, anything reparented into the fog is lost, and will
/// be rediscovered later during subtree scans. If a window is reparented out of the fog,
/// then it's treated as if a brand new window was created.
///
/// But wait again, there's more. We can delete "lost" windows on our side and unset event
/// masks, but again because this is racy, we might still receive some events for those
/// windows. So we have to keep a list of "lost" windows, and correctly ignore events sent
/// for them. (And since we are actively ignoring events from these windows, we might as
/// well not unset their event masks, saves us a trip to the X server).
///
/// (Now you have a glimpse of how much X11 sucks.)
void wm_import_incomplete(struct wm *wm, xcb_window_t wid, xcb_window_t parent);

/// Check if there are any incomplete imports in the window tree.
bool wm_has_incomplete_imports(const struct wm *wm);

/// Check if there are tree change events
bool wm_has_tree_changes(const struct wm *wm);

/// Complete the previous incomplete imports by querying the X server. This function will
/// recursively import all children of previously created placeholders, and add them to
/// the window tree. This function must be called from the X critical section. This
/// function also subscribes to SubstructureNotify events for all newly imported windows,
/// as well as for the (now completed) placeholder windows.
void wm_complete_import(struct wm *wm, struct x_connection *c, struct atom *atoms);

bool wm_is_wid_masked(struct wm *wm, xcb_window_t wid);

struct wm_change wm_dequeue_change(struct wm *wm);
