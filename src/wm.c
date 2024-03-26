// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui

#include <uthash.h>
#include <xcb/xproto.h>

#include "list.h"
#include "log.h"
#include "uthash_extra.h"
#include "win.h"
#include "wm.h"
#include "x.h"

struct wm {
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
};

unsigned int wm_get_window_count(struct wm *wm) {
	unsigned int count = 0;
	HASH_ITER2(wm->windows, w) {
		assert(!w->destroyed);
		++count;
	}
	return count;
}

struct managed_win *wm_active_win(struct wm *wm) {
	return wm->active_win;
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

	free(wm);
}
