#pragma once

#include <assert.h>
#include <stdalign.h>

#include <uthash.h>
#include <xcb/xproto.h>

#include "utils/list.h"

#include "wm.h"

struct wm_tree {
	/// The generation of the wm tree. This number is incremented every time a new
	/// window is created.
	uint64_t gen;
	/// wm tree nodes indexed by their X window ID.
	struct wm_tree_node *nodes;

	struct list_node changes;
	struct list_node free_changes;
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

struct wm_tree_node {
	UT_hash_handle hh;

	struct wm_tree_node *parent;
	struct win *win;

	struct list_node siblings;
	struct list_node children;

	wm_treeid id;
	/// The client window. Only a toplevel can have a client window.
	struct wm_tree_node *client_window;

	bool has_wm_state : 1;
	/// Whether this window exists only on our side. A zombie window is a toplevel
	/// that has been destroyed or reparented (i.e. no long a toplevel) on the X
	/// server side, but is kept on our side for things like animations. A zombie
	/// window cannot be found in the wm_tree hash table.
	bool is_zombie : 1;
};

/// Describe a change of a toplevel's client window.
/// A `XCB_NONE` in either `old_client` or `new_client` means a missing client window.
/// i.e. if `old_client` is `XCB_NONE`, it means the toplevel window did not have a client
/// window before the change, and if `new_client` is `XCB_NONE`, it means the toplevel
/// window lost its client window after the change.
struct wm_tree_change {
	wm_treeid toplevel;
	union {
		/// Information for `WM_TREE_CHANGE_CLIENT`.
		struct {
			struct wm_tree_node *toplevel;
			/// The old and new client windows.
			wm_treeid old, new_;
		} client;
		/// Information for `WM_TREE_CHANGE_TOPLEVEL_KILLED`.
		/// The zombie window left in place of the killed toplevel.
		struct wm_tree_node *killed;
		struct wm_tree_node *new_;
	};

	enum wm_tree_change_type type;
};

/// Free all tree nodes and changes, without generating any change events. Used when
/// shutting down.
void wm_tree_clear(struct wm_tree *tree);
struct wm_tree_node *wm_tree_find(struct wm_tree *tree, xcb_window_t id);
struct wm_tree_node *wm_tree_find_toplevel_for(struct wm_tree_node *node);
/// Detach the subtree rooted at `subroot` from `tree`. The subtree root is removed from
/// its parent, and the disconnected tree nodes won't be able to be found via
/// `wm_tree_find`. Relevant events will be generated.
void wm_tree_detach(struct wm_tree *tree, struct wm_tree_node *subroot);

static inline void wm_tree_init(struct wm_tree *tree) {
	tree->nodes = NULL;
	list_init_head(&tree->changes);
	list_init_head(&tree->free_changes);
}

static inline bool wm_treeid_eq(wm_treeid a, wm_treeid b) {
	return a.gen == b.gen && a.x == b.x;
}
