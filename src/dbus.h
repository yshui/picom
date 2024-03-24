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

typedef struct session session_t;
struct win;
struct cdbus_data;

#ifdef CONFIG_DBUS
#include <dbus/dbus.h>
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
void cdbus_ev_win_added(struct cdbus_data *cd, struct win *w);

/// Generate dbus win_destroyed signal
void cdbus_ev_win_destroyed(struct cdbus_data *cd, struct win *w);

/// Generate dbus win_mapped signal
void cdbus_ev_win_mapped(struct cdbus_data *cd, struct win *w);

/// Generate dbus win_unmapped signal
void cdbus_ev_win_unmapped(struct cdbus_data *cd, struct win *w);

/// Generate dbus win_focusout signal
void cdbus_ev_win_focusout(struct cdbus_data *cd, struct win *w);

/// Generate dbus win_focusin signal
void cdbus_ev_win_focusin(struct cdbus_data *cd, struct win *w);

#else

static inline void
cdbus_ev_win_unmapped(struct cdbus_data *cd attr_unused, struct win *w attr_unused) {
}

static inline void
cdbus_ev_win_mapped(struct cdbus_data *cd attr_unused, struct win *w attr_unused) {
}

static inline void
cdbus_ev_win_destroyed(struct cdbus_data *cd attr_unused, struct win *w attr_unused) {
}

static inline void
cdbus_ev_win_added(struct cdbus_data *cd attr_unused, struct win *w attr_unused) {
}

static inline void
cdbus_ev_win_focusout(struct cdbus_data *cd attr_unused, struct win *w attr_unused) {
}

static inline void
cdbus_ev_win_focusin(struct cdbus_data *cd attr_unused, struct win *w attr_unused) {
}

#endif

// vim: set noet sw=8 ts=8 :
