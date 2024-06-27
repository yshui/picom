// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

/// In my ideal world, the compositor shouldn't be concerned with the X window tree. It
/// should only need to care about the toplevel windows. However, because we support
/// window rules based on window properties, which can be set on any descendant of a
/// toplevel, we need to keep track of the entire window tree.
///
/// For descendants of a toplevel window, what we actually care about is what's called a
/// "client" window. A client window is a window with the `WM_STATE` property set, in
/// theory and descendants of a toplevel can gain/lose this property at any time. So we
/// setup a minimal structure for every single window to keep track of this. And once
/// a window becomes a client window, it will have our full attention and have all of its
/// information stored in the toplevel `struct managed_win`.

#include <uthash.h>
#include <xcb/xproto.h>

#include "log.h"
#include "utils/list.h"
#include "utils/misc.h"

#include "wm.h"
#include "wm_internal.h"

struct wm_tree_change_list {
	struct wm_tree_change item;
	struct list_node siblings;
};

void wm_tree_reap_zombie(struct wm_tree_node *zombie) {
	BUG_ON(!zombie->is_zombie);
	list_remove(&zombie->siblings);
	free(zombie);
}

static void wm_tree_enqueue_change(struct wm_tree *tree, struct wm_tree_change change) {
	if (change.type == WM_TREE_CHANGE_TOPLEVEL_KILLED) {
		// A gone toplevel will cancel out a previous
		// `WM_TREE_CHANGE_TOPLEVEL_NEW` change in the queue.
		bool found = false;
		list_foreach_safe(struct wm_tree_change_list, i, &tree->changes, siblings) {
			if (!wm_treeid_eq(i->item.toplevel, change.toplevel)) {
				continue;
			}
			if (i->item.type == WM_TREE_CHANGE_TOPLEVEL_NEW) {
				list_remove(&i->siblings);
				list_insert_after(&tree->free_changes, &i->siblings);
				found = true;
			} else if (found) {
				// We also need to delete all other changes
				// related to this toplevel in between the new and
				// gone changes.
				list_remove(&i->siblings);
				list_insert_after(&tree->free_changes, &i->siblings);
			} else if (i->item.type == WM_TREE_CHANGE_CLIENT) {
				// Need to update client changes, so they points to the
				// zombie instead of the old toplevel node, since the old
				// toplevel node could be freed before tree changes are
				// processed.
				i->item.client.toplevel = change.killed;
			}
		}
		if (found) {
			wm_tree_reap_zombie(change.killed);
			return;
		}
	} else if (change.type == WM_TREE_CHANGE_CLIENT) {
		// A client change can coalesce with a previous client change.
		list_foreach_safe(struct wm_tree_change_list, i, &tree->changes, siblings) {
			if (!wm_treeid_eq(i->item.toplevel, change.toplevel) ||
			    i->item.type != WM_TREE_CHANGE_CLIENT) {
				continue;
			}

			if (!wm_treeid_eq(i->item.client.new_, change.client.old)) {
				log_warn("Inconsistent client change for toplevel "
				         "%#010x. Missing changes from %#010x to %#010x. "
				         "Possible bug.",
				         change.toplevel.x, i->item.client.new_.x,
				         change.client.old.x);
			}

			i->item.client.new_ = change.client.new_;
			if (wm_treeid_eq(i->item.client.old, change.client.new_)) {
				list_remove(&i->siblings);
				list_insert_after(&tree->free_changes, &i->siblings);
			}
			return;
		}
	} else if (change.type == WM_TREE_CHANGE_TOPLEVEL_RESTACKED) {
		list_foreach(struct wm_tree_change_list, i, &tree->changes, siblings) {
			if (i->item.type == WM_TREE_CHANGE_TOPLEVEL_RESTACKED ||
			    i->item.type == WM_TREE_CHANGE_TOPLEVEL_NEW ||
			    i->item.type == WM_TREE_CHANGE_TOPLEVEL_KILLED) {
				// Only need to keep one
				// `WM_TREE_CHANGE_TOPLEVEL_RESTACKED` change, and order
				// doesn't matter.
				return;
			}
		}
	}

	// We don't let a `WM_TREE_CHANGE_TOPLEVEL_NEW` cancel out a previous
	// `WM_TREE_CHANGE_TOPLEVEL_GONE`, because the new toplevel would be a different
	// window reusing the same ID. So we need to go through the proper destruction
	// process for the previous toplevel. Changes are not commutative (naturally).

	struct wm_tree_change_list *change_list;
	if (!list_is_empty(&tree->free_changes)) {
		change_list = list_entry(tree->free_changes.next,
		                         struct wm_tree_change_list, siblings);
		list_remove(&change_list->siblings);
	} else {
		change_list = cmalloc(struct wm_tree_change_list);
	}

	change_list->item = change;
	list_insert_before(&tree->changes, &change_list->siblings);
}

/// Dequeue the oldest change from the change queue. If the queue is empty, a change with
/// `toplevel` set to `XCB_NONE` will be returned.
struct wm_tree_change wm_tree_dequeue_change(struct wm_tree *tree) {
	if (list_is_empty(&tree->changes)) {
		return (struct wm_tree_change){.type = WM_TREE_CHANGE_NONE};
	}

	auto change = list_entry(tree->changes.next, struct wm_tree_change_list, siblings);
	list_remove(&change->siblings);
	list_insert_after(&tree->free_changes, &change->siblings);
	return change->item;
}

/// Return the next node in the subtree after `node` in a pre-order traversal. Returns
/// NULL if `node` is the last node in the traversal.
struct wm_tree_node *wm_tree_next(struct wm_tree_node *node, struct wm_tree_node *subroot) {
	if (!list_is_empty(&node->children)) {
		// Descend if there are children
		return list_entry(node->children.next, struct wm_tree_node, siblings);
	}

	while (node != subroot && node->siblings.next == &node->parent->children) {
		// If the current node has no more children, go back to the
		// parent.
		node = node->parent;
	}
	if (node == subroot) {
		// We've gone past the topmost node for our search, stop.
		return NULL;
	}
	return list_entry(node->siblings.next, struct wm_tree_node, siblings);
}

/// Find a client window under a toplevel window. If there are multiple windows with
/// `WM_STATE` set under the toplevel window, we will return an arbitrary one.
static struct wm_tree_node *attr_pure wm_tree_find_client(struct wm_tree_node *subroot) {
	if (subroot->has_wm_state) {
		log_debug("Toplevel %#010x has WM_STATE set, weird. Using itself as its "
		          "client window.",
		          subroot->id.x);
		return subroot;
	}

	BUG_ON(subroot->parent == NULL);        // Trying to find client window on the
	                                        // root window

	for (auto curr = subroot; curr != NULL; curr = wm_tree_next(curr, subroot)) {
		if (curr->has_wm_state) {
			return curr;
		}
	}

	return NULL;
}

struct wm_tree_node *wm_tree_find(const struct wm_tree *tree, xcb_window_t id) {
	struct wm_tree_node *node = NULL;
	HASH_FIND_INT(tree->nodes, &id, node);
	return node;
}

struct wm_tree_node *wm_tree_find_toplevel_for(struct wm_tree_node *node) {
	BUG_ON_NULL(node);
	BUG_ON_NULL(node->parent);        // Trying to find toplevel for the root
	                                  // window

	struct wm_tree_node *toplevel;
	for (auto curr = node; curr->parent != NULL; curr = curr->parent) {
		toplevel = curr;
	}
	return toplevel;
}

/// Change whether a tree node has the `WM_STATE` property set.
/// `destroyed` indicate whether `node` is about to be destroyed, in which case, the `old`
/// field of the change event will be set to NULL.
void wm_tree_set_wm_state(struct wm_tree *tree, struct wm_tree_node *node, bool has_wm_state) {
	BUG_ON(node == NULL);

	if (node->has_wm_state == has_wm_state) {
		log_debug("WM_STATE unchanged call (window %#010x, WM_STATE %d).",
		          node->id.x, has_wm_state);
		return;
	}

	node->has_wm_state = has_wm_state;
	BUG_ON(node->parent == NULL);        // Trying to set WM_STATE on the root window

	struct wm_tree_node *toplevel;
	for (auto cur = node; cur->parent != NULL; cur = cur->parent) {
		toplevel = cur;
	}

	if (toplevel == node) {
		log_debug("Setting WM_STATE on a toplevel window %#010x, weird.", node->id.x);
		return;
	}

	struct wm_tree_change change = {
	    .toplevel = toplevel->id,
	    .type = WM_TREE_CHANGE_CLIENT,
	    .client = {.toplevel = toplevel},
	};
	if (!has_wm_state && toplevel->client_window == node) {
		auto new_client = wm_tree_find_client(toplevel);
		toplevel->client_window = new_client;
		change.client.old = node->id;
		change.client.new_ = new_client != NULL ? new_client->id : WM_TREEID_NONE;
		wm_tree_enqueue_change(tree, change);
	} else if (has_wm_state && toplevel->client_window == NULL) {
		toplevel->client_window = node;
		change.client.old = WM_TREEID_NONE;
		change.client.new_ = node->id;
		wm_tree_enqueue_change(tree, change);
	} else if (has_wm_state) {
		// If the toplevel window already has a client window, we won't
		// try to usurp it.
		log_debug("Toplevel window %#010x already has a client window "
		          "%#010x, ignoring new client window %#010x. I don't "
		          "like your window manager.",
		          toplevel->id.x, toplevel->client_window->id.x, node->id.x);
	}
}

struct wm_tree_node *wm_tree_new_window(struct wm_tree *tree, xcb_window_t id) {
	auto node = ccalloc(1, struct wm_tree_node);
	node->id.x = id;
	node->id.gen = tree->gen++;
	node->has_wm_state = false;
	list_init_head(&node->children);
	return node;
}

void wm_tree_add_window(struct wm_tree *tree, struct wm_tree_node *node) {
	HASH_ADD_INT(tree->nodes, id.x, node);
}

static void
wm_tree_refresh_client_and_queue_change(struct wm_tree *tree, struct wm_tree_node *toplevel) {
	BUG_ON_NULL(toplevel);
	BUG_ON_NULL(toplevel->parent);
	BUG_ON(toplevel->parent->parent != NULL);
	auto new_client = wm_tree_find_client(toplevel);
	if (new_client != toplevel->client_window) {
		struct wm_tree_change change = {.toplevel = toplevel->id,
		                                .type = WM_TREE_CHANGE_CLIENT,
		                                .client = {.toplevel = toplevel,
		                                           .old = WM_TREEID_NONE,
		                                           .new_ = WM_TREEID_NONE}};
		if (toplevel->client_window != NULL) {
			change.client.old = toplevel->client_window->id;
		}
		if (new_client != NULL) {
			change.client.new_ = new_client->id;
		}
		toplevel->client_window = new_client;
		wm_tree_enqueue_change(tree, change);
	}
}

void wm_tree_detach(struct wm_tree *tree, struct wm_tree_node *subroot) {
	BUG_ON(subroot == NULL);
	BUG_ON(subroot->parent == NULL);        // Trying to detach the root window?!

	auto toplevel = wm_tree_find_toplevel_for(subroot);
	if (toplevel != subroot) {
		list_remove(&subroot->siblings);
		wm_tree_refresh_client_and_queue_change(tree, toplevel);
	} else {
		// Detached a toplevel, create a zombie for it
		auto zombie = ccalloc(1, struct wm_tree_node);
		zombie->parent = subroot->parent;
		zombie->id = subroot->id;
		zombie->is_zombie = true;
		zombie->win = subroot->win;
		subroot->win = NULL;
		list_init_head(&zombie->children);
		list_replace(&subroot->siblings, &zombie->siblings);
		wm_tree_enqueue_change(tree, (struct wm_tree_change){
		                                 .toplevel = subroot->id,
		                                 .type = WM_TREE_CHANGE_TOPLEVEL_KILLED,
		                                 .killed = zombie,
		                             });
	}
	subroot->parent = NULL;
}

void wm_tree_attach(struct wm_tree *tree, struct wm_tree_node *child,
                    struct wm_tree_node *parent) {
	BUG_ON_NULL(parent);
	BUG_ON(child->parent != NULL);        // Trying to attach a window that's already
	                                      // attached
	child->parent = parent;
	list_insert_after(&parent->children, &child->siblings);

	auto toplevel = wm_tree_find_toplevel_for(child);
	if (child == toplevel) {
		wm_tree_enqueue_change(tree, (struct wm_tree_change){
		                                 .toplevel = child->id,
		                                 .type = WM_TREE_CHANGE_TOPLEVEL_NEW,
		                                 .new_ = child,
		                             });
	} else {
		wm_tree_refresh_client_and_queue_change(tree, toplevel);
	}
}

void wm_tree_destroy_window(struct wm_tree *tree, struct wm_tree_node *node) {
	BUG_ON(node == NULL);
	BUG_ON(node->parent == NULL);        // Trying to destroy the root window?!

	if (node->has_wm_state) {
		wm_tree_set_wm_state(tree, node, false);
	}

	HASH_DEL(tree->nodes, node);

	if (!list_is_empty(&node->children)) {
		log_error("Window %#010x is destroyed, but it still has children. Expect "
		          "malfunction.",
		          node->id.x);
		list_foreach_safe(struct wm_tree_node, i, &node->children, siblings) {
			log_error("    Child window %#010x", i->id.x);
			wm_tree_destroy_window(tree, i);
		}
	}

	if (node->parent->parent == NULL) {
		node->is_zombie = true;
		wm_tree_enqueue_change(tree, (struct wm_tree_change){
		                                 .toplevel = node->id,
		                                 .type = WM_TREE_CHANGE_TOPLEVEL_KILLED,
		                                 .killed = node,
		                             });
	} else {
		list_remove(&node->siblings);
		free(node);
	}
}

void wm_tree_move_to_end(struct wm_tree *tree, struct wm_tree_node *node, bool to_bottom) {
	BUG_ON(node == NULL);
	BUG_ON(node->parent == NULL);        // Trying to move the root window

	if ((node->parent->children.next == &node->siblings && !to_bottom) ||
	    (node->parent->children.prev == &node->siblings && to_bottom)) {
		// Already at the target position
		return;
	}
	list_remove(&node->siblings);
	if (to_bottom) {
		list_insert_before(&node->parent->children, &node->siblings);
	} else {
		list_insert_after(&node->parent->children, &node->siblings);
	}
	if (node->parent->parent == NULL) {
		wm_tree_enqueue_change(tree, (struct wm_tree_change){
		                                 .type = WM_TREE_CHANGE_TOPLEVEL_RESTACKED,
		                             });
	}
}

/// Move `node` to above `other` in their parent's child window stack.
void wm_tree_move_to_above(struct wm_tree *tree, struct wm_tree_node *node,
                           struct wm_tree_node *other) {
	BUG_ON(node == NULL);
	BUG_ON(node->parent == NULL);        // Trying to move the root window
	BUG_ON(other == NULL);
	BUG_ON(node->parent != other->parent);

	if (node->siblings.next == &other->siblings) {
		// Already above `other`
		return;
	}

	list_remove(&node->siblings);
	list_insert_before(&other->siblings, &node->siblings);
	if (node->parent->parent == NULL) {
		wm_tree_enqueue_change(tree, (struct wm_tree_change){
		                                 .type = WM_TREE_CHANGE_TOPLEVEL_RESTACKED,
		                             });
	}
}

void wm_tree_clear(struct wm_tree *tree) {
	struct wm_tree_node *cur, *tmp;
	HASH_ITER(hh, tree->nodes, cur, tmp) {
		HASH_DEL(tree->nodes, cur);
		free(cur);
	}
	list_foreach_safe(struct wm_tree_change_list, i, &tree->changes, siblings) {
		list_remove(&i->siblings);
		free(i);
	}
	list_foreach_safe(struct wm_tree_change_list, i, &tree->free_changes, siblings) {
		list_remove(&i->siblings);
		free(i);
	}
}

TEST_CASE(tree_manipulation) {
	struct wm_tree tree;
	wm_tree_init(&tree);

	wm_tree_add_window(&tree, wm_tree_new_window(&tree, 1));
	auto root = wm_tree_find(&tree, 1);
	assert(root != NULL);
	assert(root->parent == NULL);

	auto change = wm_tree_dequeue_change(&tree);
	assert(change.type == WM_TREE_CHANGE_NONE);

	auto node2 = wm_tree_new_window(&tree, 2);
	wm_tree_add_window(&tree, node2);
	wm_tree_attach(&tree, node2, root);
	assert(node2 != NULL);
	assert(node2 == wm_tree_find(&tree, 2));
	assert(node2->parent == root);

	change = wm_tree_dequeue_change(&tree);
	assert(change.toplevel.x == 2);
	assert(change.type == WM_TREE_CHANGE_TOPLEVEL_NEW);
	assert(wm_treeid_eq(node2->id, change.toplevel));

	auto node3 = wm_tree_new_window(&tree, 3);
	wm_tree_add_window(&tree, node3);
	wm_tree_attach(&tree, node3, root);

	change = wm_tree_dequeue_change(&tree);
	assert(change.toplevel.x == 3);
	assert(change.type == WM_TREE_CHANGE_TOPLEVEL_NEW);

	wm_tree_detach(&tree, node2);
	wm_tree_attach(&tree, node2, node3);
	assert(node2->parent == node3);
	assert(node3->children.next == &node2->siblings);

	// node2 is now a child of node3, so it's no longer a toplevel
	change = wm_tree_dequeue_change(&tree);
	assert(change.toplevel.x == 2);
	assert(change.type == WM_TREE_CHANGE_TOPLEVEL_KILLED);
	wm_tree_reap_zombie(change.killed);

	wm_tree_set_wm_state(&tree, node2, true);
	change = wm_tree_dequeue_change(&tree);
	assert(change.toplevel.x == 3);
	assert(change.type == WM_TREE_CHANGE_CLIENT);
	assert(wm_treeid_eq(change.client.old, WM_TREEID_NONE));
	assert(change.client.new_.x == 2);

	auto node4 = wm_tree_new_window(&tree, 4);
	wm_tree_add_window(&tree, node4);
	wm_tree_attach(&tree, node4, node3);
	change = wm_tree_dequeue_change(&tree);
	assert(change.type == WM_TREE_CHANGE_NONE);

	wm_tree_set_wm_state(&tree, node4, true);
	change = wm_tree_dequeue_change(&tree);
	// node3 already has node2 as its client window, so the new one should be ignored.
	assert(change.type == WM_TREE_CHANGE_NONE);

	wm_tree_destroy_window(&tree, node2);
	change = wm_tree_dequeue_change(&tree);
	assert(change.toplevel.x == 3);
	assert(change.type == WM_TREE_CHANGE_CLIENT);
	assert(change.client.old.x == 2);
	assert(change.client.new_.x == 4);

	// Test window ID reuse
	wm_tree_destroy_window(&tree, node4);
	node4 = wm_tree_new_window(&tree, 4);
	wm_tree_add_window(&tree, node4);
	wm_tree_attach(&tree, node4, node3);
	wm_tree_set_wm_state(&tree, node4, true);

	change = wm_tree_dequeue_change(&tree);
	assert(change.toplevel.x == 3);
	assert(change.type == WM_TREE_CHANGE_CLIENT);
	assert(change.client.old.x == 4);
	assert(change.client.new_.x == 4);

	auto node5 = wm_tree_new_window(&tree, 5);
	wm_tree_add_window(&tree, node5);
	wm_tree_attach(&tree, node5, root);
	wm_tree_destroy_window(&tree, node5);
	change = wm_tree_dequeue_change(&tree);
	assert(change.type == WM_TREE_CHANGE_NONE);        // Changes cancelled out

	wm_tree_clear(&tree);
}
