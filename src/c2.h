// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <xcb/xproto.h>

typedef struct _c2_lptr c2_lptr_t;
typedef struct session session_t;
struct c2_state;
/// Per-window state used for c2 condition matching.
struct c2_window_state {
	/// An array of window properties. Exact how many
	/// properties there are is stored inside `struct c2_state`.
	struct c2_property_value *values;
};
struct atom;
struct managed_win;

typedef void (*c2_userdata_free)(void *);
c2_lptr_t *c2_parse(c2_lptr_t **pcondlst, const char *pattern, void *data);

/// Parse a condition that has a prefix. The prefix is parsed by `parse_prefix`. If
/// `free_value` is not NULL, it will be called to free the value returned by
/// `parse_prefix` when error occurs.
c2_lptr_t *
c2_parse_with_prefix(c2_lptr_t **pcondlst, const char *pattern,
                     void *(*parse_prefix)(const char *input, const char **end, void *),
                     void (*free_value)(void *), void *user_data);

c2_lptr_t *c2_free_lptr(c2_lptr_t *lp, c2_userdata_free f);

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

bool c2_match(struct c2_state *state, const struct managed_win *w,
              const c2_lptr_t *condlst, void **pdata);
bool c2_match_one(struct c2_state *state, const struct managed_win *w,
                  const c2_lptr_t *condlst, void **pdata);

bool c2_list_postprocess(struct c2_state *state, xcb_connection_t *c, c2_lptr_t *list);
typedef bool (*c2_list_foreach_cb_t)(const c2_lptr_t *cond, void *data);
bool c2_list_foreach(const c2_lptr_t *list, c2_list_foreach_cb_t cb, void *data);
/// Return user data stored in a condition.
void *c2_list_get_data(const c2_lptr_t *condlist);
/// Convert a c2_lptr_t to string. The returned string is only valid until the
/// next call to this function, and should not be freed.
const char *c2_lptr_to_str(const c2_lptr_t *);

/**
 * Destroy a condition list.
 */
static inline void c2_list_free(c2_lptr_t **pcondlst, c2_userdata_free f) {
	while ((*pcondlst = c2_free_lptr(*pcondlst, f))) {
	}
	*pcondlst = NULL;
}
