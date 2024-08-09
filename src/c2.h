// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <xcb/xproto.h>

#include "utils/list.h"

typedef struct c2_condition c2_condition;
typedef struct session session_t;
struct c2_state;
/// Per-window state used for c2 condition matching.
struct c2_window_state {
	/// An array of window properties. Exact how many
	/// properties there are is stored inside `struct c2_state`.
	struct c2_property_value *values;
};
struct atom;
struct win;
struct list_node;

typedef void (*c2_userdata_free)(void *);
struct c2_condition *c2_parse(struct list_node *list, const char *pattern, void *data);

/// Parse a condition that has a prefix. The prefix is parsed by `parse_prefix`. If
/// `free_value` is not NULL, it will be called to free the value returned by
/// `parse_prefix` when error occurs.
c2_condition *
c2_parse_with_prefix(struct list_node *list, const char *pattern,
                     void *(*parse_prefix)(const char *input, const char **end, void *),
                     void (*free_value)(void *), void *user_data);

void c2_free_condition(c2_condition *lp, c2_userdata_free f);

/// Create a new c2_state object. This is used for maintaining the internal state
/// used for c2 condition matching. This state object holds a reference to the
/// pass atom object, thus the atom object should be kept alive as long as the
/// state object is alive.
struct c2_state *c2_state_new(struct atom *atoms);
void c2_state_free(struct c2_state *state);
/// Returns true if value of the property is used in any condition.
bool c2_state_is_property_tracked(struct c2_state *state, xcb_atom_t property);
void c2_window_state_init(const struct c2_state *state, struct c2_window_state *window_state);
void c2_window_state_destroy(const struct c2_state *state, struct c2_window_state *window_state);
void c2_window_state_mark_dirty(const struct c2_state *state,
                                struct c2_window_state *window_state, xcb_atom_t property,
                                bool is_on_client);
void c2_window_state_update(struct c2_state *state, struct c2_window_state *window_state,
                            xcb_connection_t *c, xcb_window_t client_win,
                            xcb_window_t frame_win);

bool c2_match(struct c2_state *state, const struct win *w,
              const struct list_node *conditions, void **pdata);
bool c2_match_one(const struct c2_state *state, const struct win *w,
                  const c2_condition *condlst, void **pdata);

bool c2_list_postprocess(struct c2_state *state, xcb_connection_t *c, struct list_node *list);
/// Return user data stored in a condition.
void *c2_condition_get_data(const c2_condition *condition);
/// Set user data stored in a condition. Return the old user data.
void *c2_condition_set_data(c2_condition *condlist, void *data);
/// Convert a c2_condition to string. The returned string is only valid until the
/// next call to this function, and should not be freed.
const char *c2_condition_to_str(const c2_condition *);
c2_condition *c2_condition_list_next(struct list_node *list, c2_condition *condition);
c2_condition *c2_condition_list_prev(struct list_node *list, c2_condition *condition);
c2_condition *c2_condition_list_entry(struct list_node *list);
/// Create a new condition list with a single condition that is always true.
c2_condition *c2_new_true(struct list_node *list);

#define c2_condition_list_foreach(list, i)                                               \
	for (c2_condition *i =                                                           \
	         list_is_empty((list)) ? NULL : c2_condition_list_entry((list)->next);   \
	     i; i = c2_condition_list_next(list, i))
#define c2_condition_list_foreach_rev(list, i)                                           \
	for (c2_condition *i =                                                           \
	         list_is_empty((list)) ? NULL : c2_condition_list_entry((list)->prev);   \
	     i; i = c2_condition_list_prev(list, i))

#define c2_condition_list_foreach_safe(list, i, n)                                       \
	for (c2_condition *i =                                                           \
	         list_is_empty((list)) ? NULL : c2_condition_list_entry((list)->next),   \
	                  *n = c2_condition_list_next(list, i);                          \
	     i; i = n, n = c2_condition_list_next(list, i))

/**
 * Destroy a condition list.
 */
static inline void c2_list_free(struct list_node *list, c2_userdata_free f) {
	c2_condition_list_foreach_safe(list, i, ni) {
		c2_free_condition(i, f);
	}
	list_init_head(list);
}
