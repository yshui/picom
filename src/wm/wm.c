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

struct wm_query_tree_request {
	struct x_async_request_base base;
	struct wm_tree_node *node;
	struct wm *wm;
	struct atom *atoms;
	size_t pending_index;
};

struct wm_get_property_request {
	struct x_async_request_base base;
	struct wm *wm;
	xcb_window_t wid;
};

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

	/// This is a virtual root for all "orphaned" windows. A window is orphaned
	/// if it is not reachable from the root node. This can only be non-empty if
	/// the tree is not consistent, i.e. there are pending async query tree requests.
	///
	/// Note an orphaned window cannot be a toplevel. This is trivially true because
	/// a toplevel has the root window as its parent, and once the root window is
	/// created its list of children is always up to date.
	struct wm_tree_node orphan_root;

	/// Tree nodes that have pending async query tree requests. We also have async get
	/// property requests, but they are not tracked because they don't affect the tree
	/// structure. We guarantee that when there are pending query tree requests, no
	/// tree nodes will be freed. This is a dynarr.
	struct wm_query_tree_request **pending_query_trees;
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
	return (struct wm_ref *)&wm->tree.root->siblings;
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
	if (node == NULL) {
		return NULL;
	}
	auto toplevel = wm_tree_find_toplevel_for(&wm->tree, node);
	return toplevel != NULL ? (struct wm_ref *)&toplevel->siblings : NULL;
}

struct wm_ref *wm_ref_toplevel_of(const struct wm *wm, struct wm_ref *cursor) {
	auto toplevel = wm_tree_find_toplevel_for(&wm->tree, to_tree_node_mut(cursor));
	return toplevel != NULL ? (struct wm_ref *)&toplevel->siblings : NULL;
}

struct wm_ref *wm_ref_client_of(struct wm_ref *cursor) {
	auto client = to_tree_node(cursor)->client_window;
	return client != NULL ? (struct wm_ref *)&client->siblings : NULL;
}

struct wm_ref *wm_stack_end(struct wm *wm) {
	return (struct wm_ref *)&wm->tree.root->children;
}

static long wm_find_pending_query_tree(struct wm *wm, struct wm_tree_node *node) {
	dynarr_foreach(wm->pending_query_trees, i) {
		if ((*i)->node == node) {
			return i - wm->pending_query_trees;
		}
	}
	return -1;
}

/// Move window `w` so it's right above `below`, if `below` is 0, `w` is moved
/// to the bottom of the stack
void wm_stack_move_to_above(struct wm *wm, struct wm_ref *cursor, struct wm_ref *below) {
	auto node = to_tree_node_mut(cursor);
	if (node->parent == &wm->orphan_root) {
		// If this window is orphaned, moving it around its siblings is
		// meaningless. Same below.
		return;
	}
	wm_tree_move_to_above(&wm->tree, node, to_tree_node_mut(below));
}

void wm_stack_move_to_end(struct wm *wm, struct wm_ref *cursor, bool to_bottom) {
	auto node = to_tree_node_mut(cursor);
	if (node->parent == &wm->orphan_root) {
		return;
	}
	wm_tree_move_to_end(&wm->tree, node, to_bottom);
}

struct wm *wm_new(void) {
	auto wm = ccalloc(1, struct wm);
	wm_tree_init(&wm->tree);
	list_init_head(&wm->orphan_root.children);
	wm->pending_query_trees = dynarr_new(struct wm_query_tree_request *, 0);
	return wm;
}

void wm_free(struct wm *wm) {
	// Free all `struct win`s associated with tree nodes, this leaves dangling
	// pointers, but we are freeing the tree nodes immediately after, so everything
	// is fine (TM).
	if (wm->tree.root != NULL) {
		wm_stack_foreach_safe(wm, i, next) {
			auto w = wm_ref_deref(i);
			auto tree_node = to_tree_node_mut(i);
			free(w);

			if (tree_node->is_zombie) {
				// This mainly happens on `session_destroy`, e.g. when
				// there's ongoing animations.
				log_debug("Leftover zombie node for window %#010x",
				          tree_node->id.x);
				wm_tree_reap_zombie(tree_node);
			}
		}
	}
	wm_tree_clear(&wm->tree);
	assert(wm_is_consistent(wm));
	assert(list_is_empty(&wm->orphan_root.children));
	dynarr_free_pod(wm->pending_query_trees);

	free(wm);
}

/// Once the window tree reaches a consistent state, we know any tree nodes that are not
/// reachable from the root must have been destroyed, so we can safely free them.
///
/// There are cases where we won't receive DestroyNotify events for these windows. For
/// example, if a window is reparented to a window that is not yet in our tree, then
/// destroyed, we won't receive a DestroyNotify event for it.
static void wm_reap_orphans(struct wm *wm) {
	// Reap orphaned windows
	while (!list_is_empty(&wm->orphan_root.children)) {
		auto node =
		    list_entry(wm->orphan_root.children.next, struct wm_tree_node, siblings);
		list_remove(&node->siblings);
		if (!list_is_empty(&node->children)) {
			log_error("Orphaned window %#010x still has children", node->id.x);
			list_splice(&node->children, &wm->orphan_root.children);
		}
		HASH_DEL(wm->tree.nodes, node);
		free(node);
	}
}

void wm_destroy(struct wm *wm, xcb_window_t wid) {
	struct wm_tree_node *node = wm_tree_find(&wm->tree, wid);
	if (!node) {
		if (wm_is_consistent(wm)) {
			log_error("Window %#010x destroyed, but it's not in our tree.", wid);
		}
		return;
	}

	log_debug("Destroying window %#010x", wid);

	if (!list_is_empty(&node->children)) {
		log_error("Window %#010x is destroyed but it still has children", wid);
	}
	wm_tree_detach(&wm->tree, node);
	// There could be an in-flight query tree request for this window, we orphan it.
	// It will be reaped when all query tree requests are completed. (Or right now if
	// the tree is already consistent.)
	wm_tree_attach(&wm->tree, node, &wm->orphan_root);
	if (wm_is_consistent(wm)) {
		wm_reap_orphans(wm);
	}
}

void wm_reap_zombie(struct wm_ref *zombie) {
	wm_tree_reap_zombie(to_tree_node_mut(zombie));
}

void wm_reparent(struct wm *wm, xcb_window_t wid, xcb_window_t parent) {
	auto window = wm_tree_find(&wm->tree, wid);
	auto new_parent = wm_tree_find(&wm->tree, parent);

	// We orphan the window here if parent is not found. We will reconnect
	// this window later as query tree requests being completed.
	if (window == NULL) {
		if (wm_is_consistent(wm)) {
			log_error("Window %#010x reparented,  but it's not in "
			          "our tree.",
			          wid);
		}
		return;
	}

	if (window->parent == new_parent) {
		// Reparent to the same parent moves the window to the top of the
		// stack
		wm_tree_move_to_end(&wm->tree, window, false);
		return;
	}

	wm_tree_detach(&wm->tree, window);

	// Attaching `window` to `new_parent` will change the children list of
	// `new_parent`, if there is a pending query tree request for `new_parent`, doing
	// so will create an overlap. In other words, `window` will appear in the query
	// tree reply too. Generally speaking, we want to keep a node's children list
	// empty while there is a pending query tree request for it. (Imagine sending the
	// query tree "locks" the children list until the reply is processed). Same logic
	// applies to `wm_import_start`.
	//
	// Alternatively if the new parent isn't in our tree yet, we orphan the window
	// too.
	if (new_parent == NULL || wm_find_pending_query_tree(wm, new_parent) != -1) {
		log_debug("Window %#010x is attached to window %#010x which is "
		          "currently been queried, orphaning.",
		          window->id.x, new_parent->id.x);
		wm_tree_attach(&wm->tree, window, &wm->orphan_root);
	} else {
		wm_tree_attach(&wm->tree, window, new_parent);
	}
}

void wm_set_has_wm_state(struct wm *wm, struct wm_ref *cursor, bool has_wm_state) {
	wm_tree_set_wm_state(&wm->tree, to_tree_node_mut(cursor), has_wm_state);
}

static const xcb_event_mask_t WM_IMPORT_EV_MASK =
    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;

static void wm_import_start_no_flush(struct wm *wm, struct x_connection *c, struct atom *atoms,
                                     xcb_window_t wid, struct wm_tree_node *parent);

static void
wm_handle_query_tree_reply(struct x_connection *c, struct x_async_request_base *base,
                           xcb_raw_generic_event_t *reply_or_error) {
	auto req = (struct wm_query_tree_request *)base;
	auto wm = req->wm;
	{
		auto last_req = dynarr_last(wm->pending_query_trees);
		dynarr_remove_swap(req->wm->pending_query_trees, req->pending_index);
		last_req->pending_index = req->pending_index;
	}

	if (reply_or_error == NULL) {
		goto out;
	}

	auto node = req->node;

	if (reply_or_error->response_type == 0) {
		// This is an error, most likely the window is gone when we tried
		// to query it.
		xcb_generic_error_t *err = (xcb_generic_error_t *)reply_or_error;
		log_debug("Query tree request for window %#010x failed with "
		          "error %s",
		          node->id.x, x_strerror(err));
		goto out;
	}

	xcb_query_tree_reply_t *reply = (xcb_query_tree_reply_t *)reply_or_error;
	log_debug("Finished querying tree for window %#010x", node->id.x);

	auto children = xcb_query_tree_children(reply);
	log_debug("Window %#010x has %d children", node->id.x,
	          xcb_query_tree_children_length(reply));
	for (int i = 0; i < xcb_query_tree_children_length(reply); i++) {
		auto child = children[i];
		auto child_node = wm_tree_find(&wm->tree, child);
		if (child_node == NULL) {
			wm_import_start_no_flush(wm, c, req->atoms, child, node);
			continue;
		}

		// If child node exists, it must be a previously orphaned node.
		assert(child_node->parent == &wm->orphan_root);
		wm_tree_detach(&wm->tree, child_node);
		wm_tree_attach(&wm->tree, child_node, node);
	}

out:
	free(req);
	xcb_flush(c->c);        // Actually send the requests
	if (wm_is_consistent(wm)) {
		wm_reap_orphans(wm);
	}
}

static void wm_handle_get_wm_state_reply(struct x_connection * /*c*/,
                                         struct x_async_request_base *base,
                                         xcb_raw_generic_event_t *reply_or_error) {
	auto req = (struct wm_get_property_request *)base;
	if (reply_or_error == NULL) {
		free(req);
		return;
	}

	// We guarantee that if a query tree request is pending, its corresponding
	// window tree node won't be reaped. But we don't guarantee the same for
	// get property requests. So we need to search the node by window ID again.

	if (reply_or_error->response_type == 0) {
		// This is an error, most likely the window is gone when we tried
		// to query it. (Note the tree node might have been freed at this
		// point if the query tree request also failed earlier.)
		xcb_generic_error_t *err = (xcb_generic_error_t *)reply_or_error;
		log_debug("Get WM_STATE request for window %#010x failed with "
		          "error %s",
		          req->wid, x_strerror(err));
		free(req);
		return;
	}

	auto node = wm_tree_find(&req->wm->tree, req->wid);
	BUG_ON_NULL(node);        // window must exist at this point, but it might be
	                          // freed then recreated while we were waiting for the
	                          // reply.
	auto reply = (xcb_get_property_reply_t *)reply_or_error;
	wm_tree_set_wm_state(&req->wm->tree, node, reply->type != XCB_NONE);
	free(req);
}

static void wm_import_start_no_flush(struct wm *wm, struct x_connection *c, struct atom *atoms,
                                     xcb_window_t wid, struct wm_tree_node *parent) {
	log_debug("Starting import process for window %#010x", wid);
	x_set_error_action_ignore(
	    c, xcb_change_window_attributes(c->c, wid, XCB_CW_EVENT_MASK,
	                                    (const uint32_t[]){WM_IMPORT_EV_MASK}));

	// Try to see if any orphaned window has the same window ID, if so, it must
	// have been destroyed without us knowing, so we should reuse the node.
	auto new = wm_tree_find(&wm->tree, wid);
	if (new == NULL) {
		new = wm_tree_new_window(&wm->tree, wid);
		wm_tree_add_window(&wm->tree, new);
	} else {
		if (new->parent == parent) {
			// What's going on???
			log_error("Importing window %#010x a second time", wid);
			assert(false);
			return;
		}
		if (new->parent != &wm->orphan_root) {
			log_error("Window %#010x appeared in the children list of both "
			          "%#010x (previous) and %#010x (current).",
			          wid, new->parent->id.x, parent->id.x);
			assert(false);
		}

		wm_tree_detach(&wm->tree, new);
		// Need to bump the generation number, as `new` is actually an entirely
		// new window, just reusing the same window ID.
		new->id.gen = wm->tree.gen++;
	}
	wm_tree_attach(&wm->tree, new, parent);
	// In theory, though very very unlikely, a window could be reparented (so we won't
	// receive its DestroyNotify), destroyed, and then a new window was created with
	// the same window ID, all before the previous query tree request is completed. In
	// this case, we shouldn't resend another query tree request. (And we also know in
	// this case the previous get property request isn't completed either.)
	if (wm_find_pending_query_tree(wm, new) != -1) {
		return;
	}

	{
		auto cookie = xcb_query_tree(c->c, wid);
		auto req = ccalloc(1, struct wm_query_tree_request);
		req->base.callback = wm_handle_query_tree_reply;
		req->base.sequence = cookie.sequence;
		req->node = new;
		req->wm = wm;
		req->atoms = atoms;
		req->pending_index = dynarr_len(wm->pending_query_trees);
		dynarr_push(wm->pending_query_trees, req);
		x_await_request(c, &req->base);
	}

	// (It's OK to resend the get property request even if one is already in-flight,
	// unlike query tree.)
	{
		auto cookie =
		    xcb_get_property(c->c, 0, wid, atoms->aWM_STATE, XCB_ATOM_ANY, 0, 2);
		auto req = ccalloc(1, struct wm_get_property_request);
		req->base.callback = wm_handle_get_wm_state_reply;
		req->base.sequence = cookie.sequence;
		req->wm = wm;
		req->wid = wid;
		x_await_request(c, &req->base);
	}
}

void wm_import_start(struct wm *wm, struct x_connection *c, struct atom *atoms,
                     xcb_window_t wid, struct wm_ref *parent) {
	struct wm_tree_node *parent_node = parent != NULL ? to_tree_node_mut(parent) : NULL;
	if (parent_node != NULL && wm_find_pending_query_tree(wm, parent_node) != -1) {
		// Parent node is currently being queried, we can't attach the new window
		// to it as that will change its children list.
		return;
	}
	wm_import_start_no_flush(wm, c, atoms, wid, parent_node);
	xcb_flush(c->c);        // Actually send the requests
}

bool wm_is_consistent(const struct wm *wm) {
	return dynarr_is_empty(wm->pending_query_trees);
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

struct wm_ref *wm_new_mock_window(struct wm *wm, xcb_window_t wid) {
	auto node = wm_tree_new_window(&wm->tree, wid);
	return (struct wm_ref *)&node->siblings;
}
void wm_free_mock_window(struct wm * /*wm*/, struct wm_ref *cursor) {
	free(to_tree_node_mut(cursor));
}
