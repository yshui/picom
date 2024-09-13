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
struct win;
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

/// Create a new window management object.
/// Caller is expected to first call `wm_import_start` with the root window after
/// creating the object. Otherwise many operations will fail or crash.
struct wm *wm_new(void);
void wm_free(struct wm *wm);

/// The current focused toplevel, based on information from X server.
struct wm_ref *wm_focused_win(struct wm *wm);
/// The current cached focused leader. This is only update to date after a call to
/// `wm_refresh_leaders`, and before any changes to the wm tree. Otherwise this could
/// return a dangling pointer.
const struct wm_ref *wm_focused_leader(struct wm *wm);

// Note: `wm` keeps track of 2 lists of windows. One is the window stack, which includes
// all windows that might need to be rendered, which means it would include destroyed
// windows in case they have close animation. This list is accessed by `wm_stack_*` series
// of functions. The other is a hash table of windows, which does not include destroyed
// windows. This list is accessed by `wm_find_*`, `wm_foreach`, and `wm_num_windows`.
// Adding a window to the window stack also automatically adds it to the hash table.

/// Find a window in the hash table from window id.
struct wm_ref *attr_pure wm_find(const struct wm *wm, xcb_window_t id);
// Find the WM frame of a client window. `id` is the client window id.
struct wm_ref *attr_pure wm_find_by_client(const struct wm *wm, xcb_window_t client);
/// Find the toplevel of a window by going up the window tree.
struct wm_ref *attr_pure wm_ref_toplevel_of(const struct wm *wm, struct wm_ref *cursor);
/// Return the client window of a window. Must be called with a cursor to a toplevel.
/// Returns NULL if there is no client window.
struct wm_ref *attr_pure wm_ref_client_of(struct wm_ref *cursor);
/// Find the next window in the window stack. Returns NULL if `cursor` is the last window.
struct wm_ref *attr_pure wm_ref_below(const struct wm_ref *cursor);
struct wm_ref *attr_pure wm_ref_above(const struct wm_ref *cursor);
struct wm_ref *attr_pure wm_root_ref(const struct wm *wm);

struct wm_ref *attr_pure wm_ref_topmost_child(const struct wm_ref *cursor);
struct wm_ref *attr_pure wm_ref_bottommost_child(const struct wm_ref *cursor);

/// Move window `w` so it's right above `below`, if `below` is 0, `w` is moved
/// to the bottom of the stack
void wm_stack_move_to_above(struct wm *wm, struct wm_ref *cursor, xcb_window_t below);
/// Move window `w` to the top of the stack.
void wm_stack_move_to_end(struct wm *wm, struct wm_ref *cursor, bool to_bottom);

struct win *attr_pure wm_ref_deref(const struct wm_ref *cursor);
xcb_window_t attr_pure wm_ref_win_id(const struct wm_ref *cursor);
wm_treeid attr_pure wm_ref_treeid(const struct wm_ref *cursor);
/// Assign a window to a cursor. The cursor must not already have a window assigned.
void wm_ref_set(struct wm_ref *cursor, struct win *w);
/// Mark `cursor` as the focused window.
void wm_ref_set_focused(struct wm *wm, struct wm_ref *cursor);
void wm_ref_set_leader(struct wm *wm, struct wm_ref *cursor, xcb_window_t leader);
/// Get the current cached group leader of a window. This is only update to date after a
/// call to `wm_refresh_leaders`.
const struct wm_ref *wm_ref_leader(const struct wm_ref *cursor);
bool attr_pure wm_ref_is_zombie(const struct wm_ref *cursor);

/// Recalculate all cached leaders, and update `wm_focused_leader`.
void wm_refresh_leaders(struct wm *wm);

/// Destroy a window. Children of this window should already have been destroyed. This
/// will cause a `WM_TREE_CHANGE_TOPLEVEL_KILLED` event to be generated, and a zombie
/// window to be placed where the window was.
void wm_destroy(struct wm *wm, xcb_window_t wid);
/// Remove a zombie window from the window tree.
void wm_reap_zombie(struct wm_ref *zombie);
void wm_reparent(struct wm *wm, struct x_connection *c, struct atom *atoms,
                 xcb_window_t wid, xcb_window_t parent);
/// Disconnect `child` from its `parent`. If `new_parent_known` is true, the new parent
/// is a fully imported window in our tree. Otherwise, the new parent is either unknown,
/// or in the process of being imported.
void wm_disconnect(struct wm *wm, xcb_window_t child, xcb_window_t parent,
                   xcb_window_t new_parent);
void wm_set_has_wm_state(struct wm *wm, struct wm_ref *cursor, bool has_wm_state);

/// Start the import process for `wid`.
///
/// This function sets up event masks for `wid`, and start an async query tree request on
/// it. When the query tree request is completed, `wm_handle_query_tree_reply` will be
/// called to actually insert the window into the window tree.
/// `wm_handle_query_tree_reply` will also in turn start the import process for all
/// children of `wid`.
///
/// The reason for this two step process is because we want to catch all windows ever
/// created on X server's side. It's not enough to just set up event masks and wait for
/// events. Because at the point in time we set up the event mask, some child windows
/// could have already been created. It would have been nice if there is a way to listen
/// for events on the whole window tree, for all present and future windows. But alas, the
/// X11 protocol is lacking in this area.
///
/// The best thing we can do is set up the event mask to catch all future events, and then
/// query the current state. But there are complications with this approach, too. Because
/// there is no way to atomically do these two things in one go, things could happen
/// between these two steps, for which we will receive events. Some of these are easy to
/// deal with, e.g. if a window is created, we will get an event for that, and later we
/// will see that window again in the query tree reply. These are easy to ignore. Some are
/// more complex. Because there could be some child windows we are not aware of. We could
/// get events for windows that we don't know about. We try our best to ignore those
/// events.
///
/// Another problem with this is, the usual way we send a request then process the reply
/// is synchronous. i.e. with `xcb_<request>_reply(xcb_<request>(...))`. As previously
/// mentioned, we might receive events before the reply. And in this case we would process
/// the reply _before_ any of those events! This might be benign, but it is difficult to
/// audit all the possible cases and events to make sure this always work. One example is,
/// imagine a window A is being imported, and another window B, which is already in the
/// tree got reparented to A. We would think B appeared in two places if we processed
/// query tree reply before the reparent event. For that, we introduce the concept of
/// "async requests". Replies to these requests are received and processed like other X
/// events. With that, we don't need to worry about the order of events and replies.
///
/// (Now you have a glimpse of how much X11 sucks.)
void wm_import_start(struct wm *wm, struct x_connection *c, struct atom *atoms,
                     xcb_window_t wid, struct wm_ref *parent);

/// Check if there are tree change events
bool wm_has_tree_changes(const struct wm *wm);

struct wm_change wm_dequeue_change(struct wm *wm);

/// Whether the window tree should be in a consistent state. When the window tree is
/// consistent, we should not be receiving X events that refer to windows that we don't
/// know about. And we also should not have any orphaned windows in the tree.
bool wm_is_consistent(const struct wm *wm);

// Unit testing helpers

struct wm_ref *wm_new_mock_window(struct wm *wm, xcb_window_t wid);
void wm_free_mock_window(struct wm *wm, struct wm_ref *cursor);
