#pragma once
#include <stdlib.h>

#include <xcb/xcb.h>

#include "meta.h"
#include "cache.h"

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
	_NET_WM_WINDOW_TYPE

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

#define ATOM_LIST3 \
	_NET_CURRENT_DESKTOP, \
	_NET_WM_DESKTOP

// clang-format on

#define ATOM_DEF(x) xcb_atom_t a##x

struct atom {
	struct cache *c;
	LIST_APPLY(ATOM_DEF, SEP_COLON, ATOM_LIST1);
	LIST_APPLY(ATOM_DEF, SEP_COLON, ATOM_LIST2);
	LIST_APPLY(ATOM_DEF, SEP_COLON, ATOM_LIST3);
};

struct atom *init_atoms(xcb_connection_t *);

static inline xcb_atom_t get_atom(struct atom *a, const char *key) {
	return (xcb_atom_t)(intptr_t)cache_get(a->c, key, NULL);
}

static inline void destroy_atoms(struct atom *a) {
	cache_free(a->c);
	free(a);
}
