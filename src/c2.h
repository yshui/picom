// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE-mit for more information.
 *
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <xcb/xproto.h>

typedef struct _c2_lptr c2_lptr_t;
typedef struct session session_t;
struct c2_state;
struct atom;
struct managed_win;

typedef void (*c2_userdata_free)(void *);
c2_lptr_t *c2_parse(c2_lptr_t **pcondlst, const char *pattern, void *data);

c2_lptr_t *c2_free_lptr(c2_lptr_t *lp, c2_userdata_free f);

/// Create a new c2_state object. This is used for maintaining the internal state
/// used for c2 condition matching. This state object holds a reference to the
/// pass atom object, thus the atom object should be kept alive as long as the
/// state object is alive.
struct c2_state *c2_state_new(struct atom *atoms);
void c2_state_free(struct c2_state *state);
/// Returns true if value of the property is used in any condition.
bool c2_is_property_tracked(struct c2_state *state, xcb_atom_t property);

bool c2_match(session_t *ps, const struct managed_win *w, const c2_lptr_t *condlst,
              void **pdata);

bool c2_list_postprocess(struct c2_state *state, xcb_connection_t *c, c2_lptr_t *list);
typedef bool (*c2_list_foreach_cb_t)(const c2_lptr_t *cond, void *data);
bool c2_list_foreach(const c2_lptr_t *list, c2_list_foreach_cb_t cb, void *data);
/// Return user data stored in a condition.
void *c2_list_get_data(const c2_lptr_t *condlist);

/**
 * Destroy a condition list.
 */
static inline void c2_list_free(c2_lptr_t **pcondlst, c2_userdata_free f) {
	while ((*pcondlst = c2_free_lptr(*pcondlst, f))) {
	}
	*pcondlst = NULL;
}
