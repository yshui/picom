#pragma once
#include <stdlib.h>

#include <xcb/xcb.h>

#include "meta.h"
#include "cache.h"

// Splitted into 2 lists because of the limitation of our macros
#define ATOM_LIST \
	_NET_WM_WINDOW_OPACITY, \
	_NET_FRAME_EXTENTS, \
	WM_STATE, \
	_NET_WM_NAME, \
	_NET_WM_PID, \
	WM_NAME, \
	WM_CLASS, \
	WM_TRANSIENT_FOR, \
	WM_WINDOW_ROLE, \
	WM_CLIENT_LEADER, \
	_NET_ACTIVE_WINDOW, \
	_COMPTON_SHADOW, \
	_NET_WM_WINDOW_TYPE, \
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
	_NET_WM_STATE_FULLSCREEN

#define ATOM_DEF(x) xcb_atom_t a##x

struct atom {
	struct cache *c;
	LIST_APPLY(ATOM_DEF, SEP_COLON, ATOM_LIST);
};

struct atom *init_atoms(xcb_connection_t *);

static inline xcb_atom_t get_atom(struct atom *a, const char *key) {
	return (xcb_atom_t)(intptr_t)cache_get(a->c, key, NULL);
}

static inline void destroy_atoms(struct atom *a) {
	cache_free(a->c);
	free(a);
}
