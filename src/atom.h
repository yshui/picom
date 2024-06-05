// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <string.h>
#include <xcb/xcb.h>

#include "utils/meta.h"

// clang-format off
// Splitted into 2 lists because of the limitation of our macros
#define ATOM_LIST1 \
	_NET_WM_WINDOW_OPACITY, \
	_NET_FRAME_EXTENTS, \
	WM_STATE, \
	_NET_WM_NAME, \
	_NET_WM_PID, \
	WM_NAME, \
	WM_CLASS, \
	WM_ICON_NAME, \
	WM_TRANSIENT_FOR, \
	WM_WINDOW_ROLE, \
	WM_CLIENT_LEADER, \
	WM_CLIENT_MACHINE, \
	_NET_ACTIVE_WINDOW, \
	_COMPTON_SHADOW, \
	COMPTON_VERSION, \
	_NET_WM_WINDOW_TYPE, \
	_XROOTPMAP_ID, \
	ESETROOT_PMAP_ID, \
	_XSETROOT_ID, \
	_NET_CURRENT_DESKTOP

#define ATOM_LIST2 \
	_NET_WM_WINDOW_TYPE_DESKTOP, \
	_NET_WM_WINDOW_TYPE_DOCK, \
	_NET_WM_WINDOW_TYPE_TOOLBAR, \
	_NET_WM_WINDOW_TYPE_MENU, \
	_NET_WM_WINDOW_TYPE_UTILITY, \
	_NET_WM_WINDOW_TYPE_SPLASH, \
	_NET_WM_WINDOW_TYPE_DIALOG, \
	_NET_WM_WINDOW_TYPE_NORMAL, \
	_NET_WM_WINDOW_TYPE_DROPDOWN_MENU, \
	_NET_WM_WINDOW_TYPE_POPUP_MENU, \
	_NET_WM_WINDOW_TYPE_TOOLTIP, \
	_NET_WM_WINDOW_TYPE_NOTIFICATION, \
	_NET_WM_WINDOW_TYPE_COMBO, \
	_NET_WM_WINDOW_TYPE_DND, \
	_NET_WM_STATE, \
	_NET_WM_STATE_FULLSCREEN, \
	_NET_WM_BYPASS_COMPOSITOR, \
	UTF8_STRING, \
	C_STRING
// clang-format on

#define ATOM_DEF(x) xcb_atom_t a##x

struct atom_entry;
struct atom {
	LIST_APPLY(ATOM_DEF, SEP_COLON, ATOM_LIST1);
	LIST_APPLY(ATOM_DEF, SEP_COLON, ATOM_LIST2);
};

/// Create a new atom object with a xcb connection. `struct atom` does not hold
/// a reference to the connection.
struct atom *init_atoms(xcb_connection_t *c);

xcb_atom_t get_atom(struct atom *a, const char *key, size_t keylen, xcb_connection_t *c);
static inline xcb_atom_t
get_atom_with_nul(struct atom *a, const char *key, xcb_connection_t *c) {
	return get_atom(a, key, strlen(key), c);
}
xcb_atom_t get_atom_cached(struct atom *a, const char *key, size_t keylen);
static inline xcb_atom_t get_atom_cached_with_nul(struct atom *a, const char *key) {
	return get_atom_cached(a, key, strlen(key));
}
const char *get_atom_name(struct atom *a, xcb_atom_t, xcb_connection_t *c);
const char *get_atom_name_cached(struct atom *a, xcb_atom_t atom);

void destroy_atoms(struct atom *a);

/// A mock atom object for unit testing. Successive calls to get_atom will return
/// secutive integers as atoms, starting from 1. Calling get_atom_name with atoms
/// previously seen will result in the string that was used to create the atom; if
/// the atom was never returned by get_atom, it will abort.
struct atom *init_mock_atoms(void);
