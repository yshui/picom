// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui

#include <stddef.h>
#include <uthash.h>
#include <xcb/xproto.h>

#include "common.h"
#include "log.h"
#include "utils/dynarr.h"
#include "utils/list.h"
#include "utils/uthash_extra.h"
#include "x.h"

#include "win.h"
#include "wm.h"
#include "wm_internal.h"

struct wm {
	/// Current window generation, start from 1. 0 is reserved for using as
	/// an invalid generation number.
	///
	/// Because X server recycles window IDs, `id` along
	/// is not enough to uniquely identify a window. This generation number is
	/// incremented every time a window is destroyed, so that if a window ID is
	/// reused, its generation number will be different from before.
	/// Unless, of course, if the generation number overflows, but since we are
	/// using a uint64_t here, that won't happen for a very long time. Still,
	/// it is recommended that you restart the compositor at least once before
	/// the Universe collapse back on itself.
	uint64_t generation;
	/// A hash table of all windows.
	struct win *windows;
	/// Windows in their stacking order
	struct list_node window_stack;
	/// Pointer to <code>win</code> of current active window. Used by
	/// EWMH <code>_NET_ACTIVE_WINDOW</code> focus detection. In theory,
	/// it's more reliable to store the window ID directly here, just in
	/// case the WM does something extraordinary, but caching the pointer
	/// means another layer of complexity.
	struct managed_win *active_win;
	/// Window ID of leader window of currently active window. Used for
	/// subsidiary window detection.
	xcb_window_t active_leader;
	struct subwin *subwins;
	struct wm_tree tree;

	struct wm_tree_node *root;

	/// Incomplete imports. See `wm_import_incomplete` for an explanation.
	/// This is a dynarr.
	struct wm_tree_node **incompletes;
	/// Tree nodes that we have chosen to forget, but we might still receive some
	/// events from, we keep them here to ignore those events.
	struct wm_tree_node **masked;
};

unsigned int wm_get_window_count(struct wm *wm) {
	unsigned int count = 0;
	HASH_ITER2(wm->windows, w) {
		assert(!w->destroyed);
		++count;
	}
	return count;
}
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

xcb_window_t wm_ref_win_id(const struct wm_ref *cursor) {
	return to_tree_node(cursor)->id.x;
}

struct managed_win *wm_active_win(struct wm *wm) {
	return wm->active_win;
}

static ptrdiff_t wm_find_masked(struct wm *wm, xcb_window_t wid) {
	dynarr_foreach(wm->masked, m) {
		if ((*m)->id.x == wid) {
			return m - wm->masked;
		}
	}
	return -1;
}

void wm_set_active_win(struct wm *wm, struct managed_win *w) {
	wm->active_win = w;
}

xcb_window_t wm_active_leader(struct wm *wm) {
	return wm->active_leader;
}

void wm_set_active_leader(struct wm *wm, xcb_window_t leader) {
	wm->active_leader = leader;
}

struct win *wm_stack_next(struct wm *wm, struct list_node *cursor) {
	if (!list_node_is_last(&wm->window_stack, cursor)) {
		return list_entry(cursor->next, struct win, stack_neighbour);
	}
	return NULL;
}

// Find the managed window immediately below `i` in the window stack
struct managed_win *
wm_stack_next_managed(const struct wm *wm, const struct list_node *cursor) {
	while (!list_node_is_last(&wm->window_stack, cursor)) {
		auto next = list_entry(cursor->next, struct win, stack_neighbour);
		if (next->managed) {
			return (struct managed_win *)next;
		}
		cursor = &next->stack_neighbour;
	}
	return NULL;
}

/// Find a managed window from window id in window linked list of the session.
struct win *wm_find(struct wm *wm, xcb_window_t id) {
	if (!id) {
		return NULL;
	}

	struct win *w = NULL;
	HASH_FIND_INT(wm->windows, &id, w);
	assert(w == NULL || !w->destroyed);
	return w;
}

void wm_remove(struct wm *wm, struct win *w) {
	wm->generation++;
	HASH_DEL(wm->windows, w);
}

int wm_foreach(struct wm *wm, int (*func)(struct win *, void *), void *data) {
	HASH_ITER2(wm->windows, w) {
		assert(!w->destroyed);
		int ret = func(w, data);
		if (ret) {
			return ret;
		}
	}
	return 0;
}

void wm_stack_replace(struct wm *wm, struct win *old, struct win *new_) {
	list_replace(&old->stack_neighbour, &new_->stack_neighbour);
	struct win *replaced = NULL;
	HASH_REPLACE_INT(wm->windows, id, new_, replaced);
	assert(replaced == old);
	free(old);
}

/// Insert a new window after list_node `prev`
/// New window will be in unmapped state
static struct win *
wm_stack_insert_after(struct wm *wm, xcb_window_t id, struct list_node *prev) {
	log_debug("Adding window %#010x", id);
	struct win *old_w = NULL;
	HASH_FIND_INT(wm->windows, &id, old_w);
	assert(old_w == NULL);

	auto new_w = cmalloc(struct win);
	list_insert_after(prev, &new_w->stack_neighbour);
	new_w->id = id;
	new_w->managed = false;
	new_w->is_new = true;
	new_w->destroyed = false;
	new_w->generation = wm->generation;

	HASH_ADD_INT(wm->windows, id, new_w);
	return new_w;
}

struct win *wm_stack_add_top(struct wm *wm, xcb_window_t id) {
	return wm_stack_insert_after(wm, id, &wm->window_stack);
}

struct win *wm_stack_add_above(struct wm *wm, xcb_window_t id, xcb_window_t below) {
	struct win *w = NULL;
	HASH_FIND_INT(wm->windows, &below, w);
	if (!w) {
		if (!list_is_empty(&wm->window_stack)) {
			// `below` window is not found even if the window stack is
			// not empty
			return NULL;
		}
		return wm_stack_add_top(wm, id);
	}
	// we found something from the hash table, so if the stack is
	// empty, we are in an inconsistent state.
	assert(!list_is_empty(&wm->window_stack));
	return wm_stack_insert_after(wm, id, w->stack_neighbour.prev);
}

/// Move window `w` so it's before `next` in the list
static inline void wm_stack_move_before(struct wm *wm, struct win *w, struct list_node *next) {
	struct managed_win *mw = NULL;
	if (w->managed) {
		mw = (struct managed_win *)w;
	}

	if (mw) {
		// This invalidates all reg_ignore below the new stack position of
		// `w`
		mw->reg_ignore_valid = false;
		rc_region_unref(&mw->reg_ignore);

		// This invalidates all reg_ignore below the old stack position of
		// `w`
		auto next_w = wm_stack_next_managed(wm, &w->stack_neighbour);
		if (next_w) {
			next_w->reg_ignore_valid = false;
			rc_region_unref(&next_w->reg_ignore);
		}
	}

	list_move_before(&w->stack_neighbour, next);

#ifdef DEBUG_RESTACK
	log_trace("Window stack modified. Current stack:");
	for (auto c = &wm->window_stack; c; c = c->next) {
		const char *desc = "";
		if (c->state == WSTATE_DESTROYING) {
			desc = "(D) ";
		}
		log_trace("%#010x \"%s\" %s", c->id, c->name, desc);
	}
#endif
}

struct list_node *wm_stack_end(struct wm *wm) {
	return &wm->window_stack;
}

/// Move window `w` so it's right above `below`, if `below` is 0, `w` is moved
/// to the bottom of the stack
void wm_stack_move_above(struct wm *wm, struct win *w, xcb_window_t below) {
	xcb_window_t old_below;

	if (!list_node_is_last(&wm->window_stack, &w->stack_neighbour)) {
		old_below = list_next_entry(w, stack_neighbour)->id;
	} else {
		old_below = XCB_NONE;
	}
	log_debug("Restack %#010x (%s), old_below: %#010x, new_below: %#010x", w->id,
	          win_get_name_if_managed(w), old_below, below);

	if (old_below != below) {
		struct list_node *new_next;
		if (!below) {
			new_next = &wm->window_stack;
		} else {
			struct win *tmp_w = NULL;
			HASH_FIND_INT(wm->windows, &below, tmp_w);

			if (!tmp_w) {
				log_error("Failed to found new below window %#010x.", below);
				return;
			}

			new_next = &tmp_w->stack_neighbour;
		}
		wm_stack_move_before(wm, w, new_next);
	}
}

void wm_stack_move_to_top(struct wm *wm, struct win *w) {
	if (&w->stack_neighbour == wm->window_stack.next) {
		// already at top
		return;
	}
	wm_stack_move_before(wm, w, wm->window_stack.next);
}

unsigned wm_stack_num_managed_windows(const struct wm *wm) {
	unsigned count = 0;
	list_foreach(struct win, w, &wm->window_stack, stack_neighbour) {
		if (w->managed) {
			count += 1;
		}
	}
	return count;
}

struct managed_win *wm_find_by_client(struct wm *wm, xcb_window_t client) {
	if (!client) {
		return NULL;
	}

	HASH_ITER2(wm->windows, w) {
		assert(!w->destroyed);
		if (!w->managed) {
			continue;
		}

		auto mw = (struct managed_win *)w;
		if (mw->client_win == client) {
			return mw;
		}
	}

	return NULL;
}

struct managed_win *wm_find_managed(struct wm *wm, xcb_window_t id) {
	struct win *w = wm_find(wm, id);
	if (!w || !w->managed) {
		return NULL;
	}

	auto mw = (struct managed_win *)w;
	assert(mw->state != WSTATE_DESTROYED);
	return mw;
}

unsigned wm_num_windows(const struct wm *wm) {
	return HASH_COUNT(wm->windows);
}

struct subwin *wm_subwin_add_and_subscribe(struct wm *wm, struct x_connection *c,
                                           xcb_window_t id, xcb_window_t parent) {
	struct subwin *subwin = NULL;
	HASH_FIND_INT(wm->subwins, &id, subwin);
	BUG_ON(subwin != NULL);

	subwin = ccalloc(1, struct subwin);
	subwin->id = id;
	subwin->toplevel = parent;
	HASH_ADD_INT(wm->subwins, id, subwin);

	log_debug("Allocated subwin %p for window %#010x, toplevel %#010x, total: %d",
	          subwin, id, parent, HASH_COUNT(wm->subwins));
	XCB_AWAIT_VOID(xcb_change_window_attributes, c->c, id, XCB_CW_EVENT_MASK,
	               (const uint32_t[]){XCB_EVENT_MASK_PROPERTY_CHANGE});
	return subwin;
}

struct subwin *wm_subwin_find(struct wm *wm, xcb_window_t id) {
	struct subwin *subwin = NULL;
	HASH_FIND_INT(wm->subwins, &id, subwin);
	return subwin;
}

void wm_subwin_remove(struct wm *wm, struct subwin *subwin) {
	log_debug("Freeing subwin %p for window %#010x, toplevel %#010x", subwin,
	          subwin->id, subwin->toplevel);
	HASH_DEL(wm->subwins, subwin);
	free(subwin);
}

void wm_subwin_remove_and_unsubscribe(struct wm *wm, struct x_connection *c,
                                      struct subwin *subwin) {
	log_debug("Freeing subwin %p for window %#010x", subwin, subwin->id);
	XCB_AWAIT_VOID(xcb_change_window_attributes, c->c, subwin->id, XCB_CW_EVENT_MASK,
	               (const uint32_t[]){0});
	wm_subwin_remove(wm, subwin);
}
void wm_subwin_remove_and_unsubscribe_for_toplevel(struct wm *wm, struct x_connection *c,
                                                   xcb_window_t toplevel) {
	struct subwin *subwin, *next_subwin;
	HASH_ITER(hh, wm->subwins, subwin, next_subwin) {
		if (subwin->toplevel == toplevel) {
			wm_subwin_remove_and_unsubscribe(wm, c, subwin);
		}
	}
}

struct wm *wm_new(void) {
	auto wm = ccalloc(1, struct wm);
	list_init_head(&wm->window_stack);
	wm->generation = 1;
	wm_tree_init(&wm->tree);
	wm->incompletes = dynarr_new(struct wm_tree_node *, 4);
	wm->masked = dynarr_new(struct wm_tree_node *, 8);
	return wm;
}

void wm_free(struct wm *wm, struct x_connection *c) {
	list_foreach_safe(struct win, w, &wm->window_stack, stack_neighbour) {
		if (w->managed) {
			XCB_AWAIT_VOID(xcb_change_window_attributes, c->c, w->id,
			               XCB_CW_EVENT_MASK, (const uint32_t[]){0});
		}
		if (!w->destroyed) {
			HASH_DEL(wm->windows, w);
		}
		free(w);
	}
	list_init_head(&wm->window_stack);

	struct subwin *subwin, *next_subwin;
	HASH_ITER(hh, wm->subwins, subwin, next_subwin) {
		wm_subwin_remove_and_unsubscribe(wm, c, subwin);
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
		if (!list_is_empty(&curr->children)) {
			log_error("Newly imported subtree root at %#010x already has "
			          "children. "
			          "Expect malfunction.",
			          curr->id.x);
		}

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
