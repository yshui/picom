#pragma once
#include <xcb/xcb.h>

#include "cache.h"
#include "meta.h"

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
	_XSETROOT_ID

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

xcb_atom_t get_atom(struct atom *a, const char *key, xcb_connection_t *c);
xcb_atom_t get_atom_cached(struct atom *a, const char *key);
const char *get_atom_name(struct atom *a, xcb_atom_t, xcb_connection_t *c);
const char *get_atom_name_cached(struct atom *a, xcb_atom_t atom);

void destroy_atoms(struct atom *a);
