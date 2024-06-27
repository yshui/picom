#pragma once

#include <stdalign.h>

#include <uthash.h>
#include <xcb/xproto.h>

#include "utils/list.h"

#include "wm.h"

struct wm_tree {
	/// The generation of the wm tree. This number is incremented every time a new
	/// window is created.
	///
	/// Because X server recycles window IDs, X ID alone is not enough to uniquely
	/// identify a window. This generation number is incremented every time a window
	/// is created, so even if a window ID is reused, its generation number is
	/// guaranteed to be different from before. Unless, of course, the generation
	/// number overflows, but since we are using a uint64_t here, that won't happen
	/// for a very long time. Still, it is recommended that you restart the compositor
	/// at least once before the Universe collapse back on itself.
	uint64_t gen;
	/// wm tree nodes indexed by their X window ID.
	struct wm_tree_node *nodes;
	struct wm_tree_node *root;

	struct list_node changes;
	struct list_node free_changes;
};

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
struct wm_tree_node *attr_pure wm_tree_find(const struct wm_tree *tree, xcb_window_t id);
/// Find the toplevel that is an ancestor of `node` or `node` itself. Returns NULL if
/// `node` is part of an orphaned subtree.
struct wm_tree_node *attr_pure wm_tree_find_toplevel_for(const struct wm_tree *tree,
                                                         struct wm_tree_node *node);
struct wm_tree_node *attr_pure wm_tree_next(struct wm_tree_node *node,
                                            struct wm_tree_node *subroot);
/// Create a new window node in the tree, with X window ID `id`, and parent `parent`. If
/// `parent` is NULL, the new node will be the root window. Only one root window is
/// permitted, and the root window cannot be destroyed once created, until
/// `wm_tree_clear` is called. If `parent` is not NULL, the new node will be put at the
/// top of the stacking order among its siblings.
struct wm_tree_node *wm_tree_new_window(struct wm_tree *tree, xcb_window_t id);
void wm_tree_add_window(struct wm_tree *tree, struct wm_tree_node *node);
void wm_tree_destroy_window(struct wm_tree *tree, struct wm_tree_node *node);
/// Detach the subtree rooted at `subroot` from `tree`. The subtree root is removed from
/// its parent, and the disconnected tree nodes won't be able to be found via
/// `wm_tree_find`. Relevant events will be generated.
void wm_tree_detach(struct wm_tree *tree, struct wm_tree_node *subroot);
/// Attach `node` to `parent`. `node` becomes the topmost child of `parent`. If `parent`
/// is NULL, `node` becomes the root window.
void wm_tree_attach(struct wm_tree *tree, struct wm_tree_node *child,
                    struct wm_tree_node *parent);
void wm_tree_move_to_above(struct wm_tree *tree, struct wm_tree_node *node,
                           struct wm_tree_node *other);
/// Move `node` to the top or the bottom of its parent's child window stack.
void wm_tree_move_to_end(struct wm_tree *tree, struct wm_tree_node *node, bool to_bottom);
struct wm_tree_change wm_tree_dequeue_change(struct wm_tree *tree);
void wm_tree_reap_zombie(struct wm_tree_node *zombie);
void wm_tree_set_wm_state(struct wm_tree *tree, struct wm_tree_node *node, bool has_wm_state);

static inline void wm_tree_init(struct wm_tree *tree) {
	tree->nodes = NULL;
	tree->gen = 1;
	list_init_head(&tree->changes);
	list_init_head(&tree->free_changes);
}
