// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui

#include <stddef.h>
#include <uthash.h>
#include <xcb/xproto.h>

#include "common.h"
#include "log.h"
#include "utils/dynarr.h"
#include "utils/list.h"
#include "x.h"

#include "win.h"
#include "wm.h"
#include "wm_internal.h"

struct wm {
	/// Pointer to <code>win</code> of current active window. Used by
	/// EWMH <code>_NET_ACTIVE_WINDOW</code> focus detection. In theory,
	/// it's more reliable to store the window ID directly here, just in
	/// case the WM does something extraordinary, but caching the pointer
	/// means another layer of complexity.
	struct win *active_win;
	/// Window ID of leader window of currently active window. Used for
	/// subsidiary window detection.
	struct wm_tree_node *active_leader;
	struct wm_tree tree;

	struct wm_tree_node *root;

	/// Incomplete imports. See `wm_import_incomplete` for an explanation.
	/// This is a dynarr.
	struct wm_tree_node **incompletes;
	/// Tree nodes that we have chosen to forget, but we might still receive some
	/// events from, we keep them here to ignore those events.
	struct wm_tree_node **masked;
};

// TODO(yshui): this is a bit weird and I am not decided on it yet myself. Maybe we can
// expose `wm_tree_node` directly. But maybe we want to bundle some additional data with
// it. Anyway, this is probably easy to get rid of if we want to.
/// A wrapper of `wm_tree_node`. This points to the `siblings` `struct list_node` in a
/// `struct wm_tree_node`.
struct wm_ref {
	struct list_node inner;
};
static_assert(offsetof(struct wm_ref, inner) == 0, "wm_cursor should be usable as a "
                                                   "wm_tree_node");
static_assert(alignof(struct wm_ref) == alignof(struct list_node),
              "wm_cursor should have the same alignment as wm_tree_node");

static inline const struct wm_tree_node *to_tree_node(const struct wm_ref *cursor) {
	return cursor != NULL ? list_entry(&cursor->inner, struct wm_tree_node, siblings)
	                      : NULL;
}

static inline struct wm_tree_node *to_tree_node_mut(struct wm_ref *cursor) {
	return cursor != NULL ? list_entry(&cursor->inner, struct wm_tree_node, siblings)
	                      : NULL;
}

xcb_window_t wm_ref_win_id(const struct wm_ref *cursor) {
	return to_tree_node(cursor)->id.x;
}

wm_treeid wm_ref_treeid(const struct wm_ref *cursor) {
	return to_tree_node(cursor)->id;
}

struct win *wm_ref_deref(const struct wm_ref *cursor) {
	auto node = to_tree_node(cursor);
	if (node->parent == NULL) {
		log_error("Trying to dereference a root node. Expect malfunction.");
		return NULL;
	}
	if (node->parent->parent != NULL) {
		// Don't return the client window if this is not a toplevel node. This
		// saves us from needing to clear `->win` when a window is reparented.
		return NULL;
	}
	return node->win;
}

void wm_ref_set(struct wm_ref *cursor, struct win *w) {
	to_tree_node_mut(cursor)->win = w;
}

static ptrdiff_t wm_find_masked(struct wm *wm, xcb_window_t wid) {
	dynarr_foreach(wm->masked, m) {
		if ((*m)->id.x == wid) {
			return m - wm->masked;
		}
	}
	return -1;
}

bool wm_is_wid_masked(struct wm *wm, xcb_window_t wid) {
	return wm_find_masked(wm, wid) != -1;
}

struct win *wm_active_win(struct wm *wm) {
	return wm->active_win;
}

void wm_set_active_win(struct wm *wm, struct win *w) {
	wm->active_win = w;
}

struct wm_ref *wm_active_leader(struct wm *wm) {
	return wm->active_leader != NULL ? (struct wm_ref *)&wm->active_leader->siblings : NULL;
}

void wm_set_active_leader(struct wm *wm, struct wm_ref *leader) {
	wm->active_leader = to_tree_node_mut(leader);
}

bool wm_ref_is_zombie(const struct wm_ref *cursor) {
	return to_tree_node(cursor)->is_zombie;
}

struct wm_ref *wm_ref_below(const struct wm_ref *cursor) {
	return &to_tree_node(cursor)->parent->children != cursor->inner.next
	           ? (struct wm_ref *)cursor->inner.next
	           : NULL;
}

struct wm_ref *wm_ref_above(const struct wm_ref *cursor) {
	return &to_tree_node(cursor)->parent->children != cursor->inner.prev
	           ? (struct wm_ref *)cursor->inner.prev
	           : NULL;
}

struct wm_ref *wm_root_ref(const struct wm *wm) {
	return (struct wm_ref *)&wm->root->siblings;
}

struct wm_ref *wm_ref_topmost_child(const struct wm_ref *cursor) {
	auto node = to_tree_node(cursor);
	return !list_is_empty(&node->children) ? (struct wm_ref *)node->children.next : NULL;
}

struct wm_ref *wm_ref_bottommost_child(const struct wm_ref *cursor) {
	auto node = to_tree_node(cursor);
	return !list_is_empty(&node->children) ? (struct wm_ref *)node->children.prev : NULL;
}

struct wm_ref *wm_find(const struct wm *wm, xcb_window_t id) {
	auto node = wm_tree_find(&wm->tree, id);
	return node != NULL ? (struct wm_ref *)&node->siblings : NULL;
}

struct wm_ref *wm_find_by_client(const struct wm *wm, xcb_window_t client) {
	auto node = wm_tree_find(&wm->tree, client);
	return node != NULL ? (struct wm_ref *)&wm_tree_find_toplevel_for(node)->siblings
	                    : NULL;
}

struct wm_ref *wm_ref_toplevel_of(struct wm_ref *cursor) {
	return (struct wm_ref *)&wm_tree_find_toplevel_for(to_tree_node_mut(cursor))->siblings;
}

struct wm_ref *wm_ref_client_of(struct wm_ref *cursor) {
	auto client = to_tree_node(cursor)->client_window;
	return client != NULL ? (struct wm_ref *)&client->siblings : NULL;
}

void wm_remove(struct wm *wm, struct wm_ref *w) {
	wm_tree_destroy_window(&wm->tree, (struct wm_tree_node *)w);
}

struct wm_ref *wm_stack_end(struct wm *wm) {
	return (struct wm_ref *)&wm->root->children;
}

/// Move window `w` so it's right above `below`, if `below` is 0, `w` is moved
/// to the bottom of the stack
void wm_stack_move_to_above(struct wm *wm, struct wm_ref *cursor, struct wm_ref *below) {
	wm_tree_move_to_above(&wm->tree, to_tree_node_mut(cursor), to_tree_node_mut(below));
}

void wm_stack_move_to_end(struct wm *wm, struct wm_ref *cursor, bool to_bottom) {
	wm_tree_move_to_end(&wm->tree, to_tree_node_mut(cursor), to_bottom);
}

struct wm *wm_new(void) {
	auto wm = ccalloc(1, struct wm);
	wm_tree_init(&wm->tree);
	wm->incompletes = dynarr_new(struct wm_tree_node *, 4);
	wm->masked = dynarr_new(struct wm_tree_node *, 8);
	return wm;
}

void wm_free(struct wm *wm) {
	// Free all `struct win`s associated with tree nodes, this leaves dangling
	// pointers, but we are freeing the tree nodes immediately after, so everything
	// is fine (TM).
	wm_stack_foreach_safe(wm, i, next) {
		auto w = wm_ref_deref(i);
		auto tree_node = to_tree_node_mut(i);
		free(w);

		if (tree_node->is_zombie) {
			// This mainly happens on `session_destroy`, e.g. when there's
			// ongoing animations.
			log_debug("Leftover zombie node for window %#010x", tree_node->id.x);
			wm_tree_reap_zombie(tree_node);
		}
	}
	wm_tree_clear(&wm->tree);
	dynarr_free_pod(wm->incompletes);
	dynarr_free_pod(wm->masked);

	free(wm);
}

void wm_destroy(struct wm *wm, xcb_window_t wid) {
	auto masked = wm_find_masked(wm, wid);
	if (masked != -1) {
		free(wm->masked[masked]);
		dynarr_remove_swap(wm->masked, (size_t)masked);
		return;
	}

	struct wm_tree_node *node = wm_tree_find(&wm->tree, wid);
	if (!node) {
		log_error("Trying to destroy window %#010x, but it's not in the tree. "
		          "Expect malfunction.",
		          wid);
		return;
	}

	auto index = dynarr_find_pod(wm->incompletes, node);
	if (index != -1) {
		dynarr_remove_swap(wm->incompletes, (size_t)index);
	}

	wm_tree_destroy_window(&wm->tree, node);
}

void wm_reap_zombie(struct wm_ref *zombie) {
	wm_tree_reap_zombie(to_tree_node_mut(zombie));
}

void wm_reparent(struct wm *wm, xcb_window_t wid, xcb_window_t parent) {
	auto window = wm_tree_find(&wm->tree, wid);
	auto new_parent = wm_tree_find(&wm->tree, parent);
	// We delete the window here if parent is not found, or if the parent is
	// an incomplete import. We will rediscover this window later in
	// `wm_complete_import`. Keeping it around will only confuse us.
	bool should_forget =
	    new_parent == NULL || dynarr_find_pod(wm->incompletes, new_parent) != -1;
	if (window == NULL) {
		log_debug("Reparenting window %#010x which is not in our tree. Assuming "
		          "it came from fog of war.",
		          wid);
		if (!should_forget) {
			wm_import_incomplete(wm, wid, parent);
		}
		return;
	}
	if (should_forget) {
		log_debug("Window %#010x reparented to window %#010x which is "
		          "%s, forgetting and masking instead.",
		          window->id.x, parent,
		          new_parent == NULL ? "not in our tree" : "an incomplete import");

		wm_tree_detach(&wm->tree, window);
		for (auto curr = window; curr != NULL;) {
			auto next = wm_tree_next(curr, window);
			HASH_DEL(wm->tree.nodes, curr);
			auto incomplete_index = dynarr_find_pod(wm->incompletes, curr);
			if (incomplete_index != -1) {
				dynarr_remove_swap(wm->incompletes, (size_t)incomplete_index);
				// Incomplete imports cannot have children.
				assert(list_is_empty(&curr->children));

				// Incomplete imports won't have event masks set, so we
				// don't need to mask them.
				free(curr);
			} else {
				dynarr_push(wm->masked, curr);
			}
			curr = next;
		}
		return;
	}

	wm_tree_reparent(&wm->tree, window, new_parent);
}

void wm_set_has_wm_state(struct wm *wm, struct wm_ref *cursor, bool has_wm_state) {
	wm_tree_set_wm_state(&wm->tree, to_tree_node_mut(cursor), has_wm_state);
}

void wm_import_incomplete(struct wm *wm, xcb_window_t wid, xcb_window_t parent) {
	auto masked = wm_find_masked(wm, wid);
	if (masked != -1) {
		// A new window created with the same wid means the window we chose to
		// forget has been deleted (without us knowing), and its ID was then
		// reused.
		free(wm->masked[masked]);
		dynarr_remove_swap(wm->masked, (size_t)masked);
	}
	auto parent_cursor = NULL;
	if (parent != XCB_NONE) {
		parent_cursor = wm_tree_find(&wm->tree, parent);
		if (parent_cursor == NULL) {
			log_error("Importing window %#010x, but its parent %#010x is not "
			          "in our tree, ignoring. Expect malfunction.",
			          wid, parent);
			return;
		}
	}
	log_debug("Importing window %#010x with parent %#010x", wid, parent);
	auto new = wm_tree_new_window(&wm->tree, wid, parent_cursor);
	dynarr_push(wm->incompletes, new);
	if (parent == XCB_NONE) {
		BUG_ON(wm->root != NULL);        // Can't have more than one root
		wm->root = new;
	}
}
static const xcb_event_mask_t WM_IMPORT_EV_MASK =
    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
static void wm_complete_import_single(struct wm *wm, struct x_connection *c,
                                      struct atom *atoms, struct wm_tree_node *node) {
	log_debug("Finishing importing window %#010x with parent %#010lx.", node->id.x,
	          node->parent != NULL ? node->parent->id.x : XCB_NONE);
	set_cant_fail_cookie(
	    c, xcb_change_window_attributes(c->c, node->id.x, XCB_CW_EVENT_MASK,
	                                    (const uint32_t[]){WM_IMPORT_EV_MASK}));
	if (wid_has_prop(c->c, node->id.x, atoms->aWM_STATE)) {
		wm_tree_set_wm_state(&wm->tree, node, true);
	}
}

static void wm_complete_import_subtree(struct wm *wm, struct x_connection *c,
                                       struct atom *atoms, struct wm_tree_node *subroot) {
	wm_complete_import_single(wm, c, atoms, subroot);

	for (auto curr = subroot; curr != NULL; curr = wm_tree_next(curr, subroot)) {
		// Surprise! This newly imported window might already have children.
		// Although we haven't setup SubstructureNotify for it yet, it's still
		// possible for another window to be reparented to it.

		xcb_query_tree_reply_t *tree = XCB_AWAIT(xcb_query_tree, c->c, curr->id.x);
		if (!tree) {
			log_error("Disappearing window subtree rooted at %#010x. Expect "
			          "malfunction.",
			          curr->id.x);
			continue;
		}

		auto children = xcb_query_tree_children(tree);
		auto children_len = xcb_query_tree_children_length(tree);
		for (int i = 0; i < children_len; i++) {
			// `children` goes from bottom to top, and `wm_tree_new_window`
			// puts the new window at the top of the stacking order, which
			// means the windows will naturally be in the correct stacking
			// order.
			auto existing = wm_tree_find(&wm->tree, children[i]);
			if (existing != NULL) {
				// This should never happen: we haven't subscribed to
				// child creation events yet, and any window reparented to
				// an incomplete is deleted. report an error and try to
				// recover.
				auto index = dynarr_find_pod(wm->incompletes, existing);
				if (index != -1) {
					dynarr_remove_swap(wm->incompletes, (size_t)index);
				}
				log_error("Window %#010x already exists in the tree, but "
				          "it appeared again as a child of window "
				          "%#010x. Deleting the old one, but expect "
				          "malfunction.",
				          children[i], curr->id.x);
				wm_tree_destroy_window(&wm->tree, existing);
			}
			existing = wm_tree_new_window(&wm->tree, children[i], curr);
			wm_complete_import_single(wm, c, atoms, existing);
		}
		free(tree);
	}
}

void wm_complete_import(struct wm *wm, struct x_connection *c, struct atom *atoms) {
	// Unveil the fog of war
	dynarr_foreach(wm->masked, m) {
		free(*m);
	}
	dynarr_clear_pod(wm->masked);

	while (!dynarr_is_empty(wm->incompletes)) {
		auto i = dynarr_pop(wm->incompletes);
		// This function modifies `wm->incompletes`, so we can't use
		// `dynarr_foreach`.
		wm_complete_import_subtree(wm, c, atoms, i);
	}
}

bool wm_has_incomplete_imports(const struct wm *wm) {
	return !dynarr_is_empty(wm->incompletes);
}

bool wm_has_tree_changes(const struct wm *wm) {
	return !list_is_empty(&wm->tree.changes);
}

struct wm_change wm_dequeue_change(struct wm *wm) {
	auto tree_change = wm_tree_dequeue_change(&wm->tree);
	struct wm_change ret = {
	    .type = tree_change.type,
	    .toplevel = NULL,
	};
	switch (tree_change.type) {
	case WM_TREE_CHANGE_CLIENT:
		ret.client.old = tree_change.client.old;
		ret.client.new_ = tree_change.client.new_;
		ret.toplevel = (struct wm_ref *)&tree_change.client.toplevel->siblings;
		break;
	case WM_TREE_CHANGE_TOPLEVEL_KILLED:
		ret.toplevel = (struct wm_ref *)&tree_change.killed->siblings;
		break;
	case WM_TREE_CHANGE_TOPLEVEL_NEW:
		ret.toplevel = (struct wm_ref *)&tree_change.new_->siblings;
		break;
	default: break;
	}
	return ret;
}
