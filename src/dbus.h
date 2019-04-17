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

#include <stdbool.h>

#include <dbus/dbus.h>

typedef struct session session_t;
struct win;

/**
 * Return a string representation of a D-Bus message type.
 */
static inline const char *cdbus_repr_msgtype(DBusMessage *msg) {
	return dbus_message_type_to_string(dbus_message_get_type(msg));
}

/**
 * Initialize D-Bus connection.
 */
bool cdbus_init(session_t *ps, const char *uniq_name);

/**
 * Destroy D-Bus connection.
 */
void cdbus_destroy(session_t *ps);

/// Generate dbus win_added signal
void cdbus_ev_win_added(session_t *ps, struct win *w);

/// Generate dbus win_destroyed signal
void cdbus_ev_win_destroyed(session_t *ps, struct win *w);

/// Generate dbus win_mapped signal
void cdbus_ev_win_mapped(session_t *ps, struct win *w);

/// Generate dbus win_unmapped signal
void cdbus_ev_win_unmapped(session_t *ps, struct win *w);

/// Generate dbus win_focusout signal
void cdbus_ev_win_focusout(session_t *ps, struct win *w);

/// Generate dbus win_focusin signal
void cdbus_ev_win_focusin(session_t *ps, struct win *w);

// vim: set noet sw=8 ts=8 :
