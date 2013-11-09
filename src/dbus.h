/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE for more information.
 *
 */

#include "common.h"
#include <ctype.h>
#include <sys/types.h>
#include <unistd.h>

#define CDBUS_SERVICE_NAME      "com.github.chjj.compton"
#define CDBUS_INTERFACE_NAME    CDBUS_SERVICE_NAME
#define CDBUS_OBJECT_NAME       "/com/github/chjj/compton"
#define CDBUS_ERROR_PREFIX      CDBUS_INTERFACE_NAME ".error"
#define CDBUS_ERROR_UNKNOWN     CDBUS_ERROR_PREFIX ".unknown"
#define CDBUS_ERROR_UNKNOWN_S   "Well, I don't know what happened. Do you?"
#define CDBUS_ERROR_BADMSG      CDBUS_ERROR_PREFIX ".bad_message"
#define CDBUS_ERROR_BADMSG_S    "Unrecognized command. Beware compton " \
                                "cannot make you a sandwich."
#define CDBUS_ERROR_BADARG      CDBUS_ERROR_PREFIX ".bad_argument"
#define CDBUS_ERROR_BADARG_S    "Failed to parse argument %d: %s"
#define CDBUS_ERROR_BADWIN      CDBUS_ERROR_PREFIX ".bad_window"
#define CDBUS_ERROR_BADWIN_S    "Requested window %#010lx not found."
#define CDBUS_ERROR_BADTGT      CDBUS_ERROR_PREFIX ".bad_target"
#define CDBUS_ERROR_BADTGT_S    "Target \"%s\" not found."
#define CDBUS_ERROR_FORBIDDEN   CDBUS_ERROR_PREFIX ".forbidden"
#define CDBUS_ERROR_FORBIDDEN_S "Incorrect password, access denied."
#define CDBUS_ERROR_CUSTOM      CDBUS_ERROR_PREFIX ".custom"
#define CDBUS_ERROR_CUSTOM_S    "%s"

// Window type
typedef uint32_t cdbus_window_t;
#define CDBUS_TYPE_WINDOW       DBUS_TYPE_UINT32
#define CDBUS_TYPE_WINDOW_STR   DBUS_TYPE_UINT32_AS_STRING

typedef uint16_t cdbus_enum_t;
#define CDBUS_TYPE_ENUM         DBUS_TYPE_UINT16
#define CDBUS_TYPE_ENUM_STR     DBUS_TYPE_UINT16_AS_STRING

static dbus_bool_t
cdbus_callback_add_timeout(DBusTimeout *timeout, void *data);

static void
cdbus_callback_remove_timeout(DBusTimeout *timeout, void *data);

static void
cdbus_callback_timeout_toggled(DBusTimeout *timeout, void *data);

static bool
cdbus_callback_handle_timeout(session_t *ps, timeout_t *ptmout);

/**
 * Determine the poll condition of a DBusWatch.
 */
static inline short
cdbus_get_watch_cond(DBusWatch *watch) {
  const unsigned flags = dbus_watch_get_flags(watch);
  short condition = POLLERR | POLLHUP;
  if (flags & DBUS_WATCH_READABLE)
    condition |= POLLIN;
  if (flags & DBUS_WATCH_WRITABLE)
    condition |= POLLOUT;

  return condition;
}

static dbus_bool_t
cdbus_callback_add_watch(DBusWatch *watch, void *data);

static void
cdbus_callback_remove_watch(DBusWatch *watch, void *data);

static void
cdbus_callback_watch_toggled(DBusWatch *watch, void *data);

static bool
cdbus_apdarg_bool(session_t *ps, DBusMessage *msg, const void *data);

static bool
cdbus_apdarg_int32(session_t *ps, DBusMessage *msg, const void *data);

static bool
cdbus_apdarg_uint32(session_t *ps, DBusMessage *msg, const void *data);

static bool
cdbus_apdarg_double(session_t *ps, DBusMessage *msg, const void *data);

static bool
cdbus_apdarg_wid(session_t *ps, DBusMessage *msg, const void *data);

static bool
cdbus_apdarg_enum(session_t *ps, DBusMessage *msg, const void *data);

static bool
cdbus_apdarg_string(session_t *ps, DBusMessage *msg, const void *data);

static bool
cdbus_apdarg_wids(session_t *ps, DBusMessage *msg, const void *data);

/** @name DBus signal sending
 */
///@{

static bool
cdbus_signal(session_t *ps, const char *name,
    bool (*func)(session_t *ps, DBusMessage *msg, const void *data),
    const void *data);

/**
 * Send a signal with no argument.
 */
static inline bool
cdbus_signal_noarg(session_t *ps, const char *name) {
  return cdbus_signal(ps, name, NULL, NULL);
}

/**
 * Send a signal with a Window ID as argument.
 */
static inline bool
cdbus_signal_wid(session_t *ps, const char *name, Window wid) {
  return cdbus_signal(ps, name, cdbus_apdarg_wid, &wid);
}

///@}

/** @name DBus reply sending
 */
///@{

static bool
cdbus_reply(session_t *ps, DBusMessage *srcmsg,
    bool (*func)(session_t *ps, DBusMessage *msg, const void *data),
    const void *data);

static bool
cdbus_reply_errm(session_t *ps, DBusMessage *msg);

#define cdbus_reply_err(ps, srcmsg, err_name, err_format, ...) \
  cdbus_reply_errm((ps), dbus_message_new_error_printf((srcmsg), (err_name), (err_format), ## __VA_ARGS__))

/**
 * Send a reply with no argument.
 */
static inline bool
cdbus_reply_noarg(session_t *ps, DBusMessage *srcmsg) {
  return cdbus_reply(ps, srcmsg, NULL, NULL);
}

/**
 * Send a reply with a bool argument.
 */
static inline bool
cdbus_reply_bool(session_t *ps, DBusMessage *srcmsg, bool bval) {
  return cdbus_reply(ps, srcmsg, cdbus_apdarg_bool, &bval);
}

/**
 * Send a reply with an int32 argument.
 */
static inline bool
cdbus_reply_int32(session_t *ps, DBusMessage *srcmsg, int32_t val) {
  return cdbus_reply(ps, srcmsg, cdbus_apdarg_int32, &val);
}

/**
 * Send a reply with an uint32 argument.
 */
static inline bool
cdbus_reply_uint32(session_t *ps, DBusMessage *srcmsg, uint32_t val) {
  return cdbus_reply(ps, srcmsg, cdbus_apdarg_uint32, &val);
}

/**
 * Send a reply with a double argument.
 */
static inline bool
cdbus_reply_double(session_t *ps, DBusMessage *srcmsg, double val) {
  return cdbus_reply(ps, srcmsg, cdbus_apdarg_double, &val);
}

/**
 * Send a reply with a wid argument.
 */
static inline bool
cdbus_reply_wid(session_t *ps, DBusMessage *srcmsg, Window wid) {
  return cdbus_reply(ps, srcmsg, cdbus_apdarg_wid, &wid);
}

/**
 * Send a reply with a string argument.
 */
static inline bool
cdbus_reply_string(session_t *ps, DBusMessage *srcmsg, const char *str) {
  return cdbus_reply(ps, srcmsg, cdbus_apdarg_string, str);
}

/**
 * Send a reply with a enum argument.
 */
static inline bool
cdbus_reply_enum(session_t *ps, DBusMessage *srcmsg, cdbus_enum_t eval) {
  return cdbus_reply(ps, srcmsg, cdbus_apdarg_enum, &eval);
}

///@}

static bool
cdbus_msg_get_arg(DBusMessage *msg, int count, const int type, void *pdest);

/**
 * Return a string representation of a D-Bus message type.
 */
static inline const char *
cdbus_repr_msgtype(DBusMessage *msg) {
  return dbus_message_type_to_string(dbus_message_get_type(msg));
}

/** @name Message processing
 */
///@{

static void
cdbus_process(session_t *ps, DBusMessage *msg);

static bool
cdbus_process_list_win(session_t *ps, DBusMessage *msg);

static bool
cdbus_process_win_get(session_t *ps, DBusMessage *msg);

static bool
cdbus_process_win_set(session_t *ps, DBusMessage *msg);

static bool
cdbus_process_find_win(session_t *ps, DBusMessage *msg);

static bool
cdbus_process_opts_get(session_t *ps, DBusMessage *msg);

static bool
cdbus_process_opts_set(session_t *ps, DBusMessage *msg);

static bool
cdbus_process_introspect(session_t *ps, DBusMessage *msg);

///@}
