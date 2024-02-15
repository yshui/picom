#pragma once
#include <stdlib.h>

#include <xcb/xcb.h>

#include "cache.h"
#include "log.h"
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

struct atom {
	xcb_connection_t *conn;
	struct cache c;
	LIST_APPLY(ATOM_DEF, SEP_COLON, ATOM_LIST1);
	LIST_APPLY(ATOM_DEF, SEP_COLON, ATOM_LIST2);
};

struct atom_entry {
	struct cache_handle entry;
	xcb_atom_t atom;
};

/// Create a new atom object with a xcb connection, note that this atom object will hold a
/// reference to the connection, so the caller must keep the connection alive until the
/// atom object is destroyed.
struct atom *init_atoms(xcb_connection_t *);

static inline xcb_atom_t get_atom(struct atom *a, const char *key) {
	struct cache_handle *entry = NULL;
	if (cache_get_or_fetch(&a->c, key, &entry) < 0) {
		log_error("Failed to get atom %s", key);
		return XCB_NONE;
	}
	return cache_entry(entry, struct atom_entry, entry)->atom;
}

static inline xcb_atom_t get_atom_cached(struct atom *a, const char *key) {
	return cache_entry(cache_get(&a->c, key), struct atom_entry, entry)->atom;
}

void destroy_atoms(struct atom *a);
