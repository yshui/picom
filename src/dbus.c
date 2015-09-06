/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE for more information.
 *
 */

#include "dbus.h"

/**
 * Initialize D-Bus connection.
 */
bool
cdbus_init(session_t *ps) {
  DBusError err = { };

  // Initialize
  dbus_error_init(&err);

  // Connect to D-Bus
  // Use dbus_bus_get_private() so we can fully recycle it ourselves
  ps->dbus_conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
  if (dbus_error_is_set(&err)) {
    printf_errf("(): D-Bus connection failed (%s).", err.message);
    dbus_error_free(&err);
    return false;
  }

  if (!ps->dbus_conn) {
    printf_errf("(): D-Bus connection failed for unknown reason.");
    return false;
  }

  // Avoid exiting on disconnect
  dbus_connection_set_exit_on_disconnect(ps->dbus_conn, false);

  // Request service name
  {
    // Build service name
    char *service = mstrjoin3(CDBUS_SERVICE_NAME, ".", ps->o.display_repr);
    ps->dbus_service = service;

    // Request for the name
    int ret = dbus_bus_request_name(ps->dbus_conn, service,
        DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);

    if (dbus_error_is_set(&err)) {
      printf_errf("(): Failed to obtain D-Bus name (%s).", err.message);
      dbus_error_free(&err);
    }

    if (DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER != ret
        && DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER != ret) {
      printf_errf("(): Failed to become the primary owner of requested "
          "D-Bus name (%d).", ret);
    }
  }


  // Add watch handlers
  if (!dbus_connection_set_watch_functions(ps->dbus_conn,
        cdbus_callback_add_watch, cdbus_callback_remove_watch,
        cdbus_callback_watch_toggled, ps, NULL)) {
    printf_errf("(): Failed to add D-Bus watch functions.");
    return false;
  }

  // Add timeout handlers
  if (!dbus_connection_set_timeout_functions(ps->dbus_conn,
        cdbus_callback_add_timeout, cdbus_callback_remove_timeout,
        cdbus_callback_timeout_toggled, ps, NULL)) {
    printf_errf("(): Failed to add D-Bus timeout functions.");
    return false;
  }

  // Add match
  dbus_bus_add_match(ps->dbus_conn,
      "type='method_call',interface='" CDBUS_INTERFACE_NAME "'", &err);
  if (dbus_error_is_set(&err)) {
    printf_errf("(): Failed to add D-Bus match.");
    dbus_error_free(&err);
    return false;
  }

  return true;
}

/**
 * Destroy D-Bus connection.
 */
void
cdbus_destroy(session_t *ps) {
  if (ps->dbus_conn) {
    // Release DBus name firstly
    if (ps->dbus_service) {
      DBusError err = { };
      dbus_error_init(&err);

      dbus_bus_release_name(ps->dbus_conn, ps->dbus_service, &err);
      if (dbus_error_is_set(&err)) {
        printf_errf("(): Failed to release DBus name (%s).",
            err.message);
        dbus_error_free(&err);
      }
    }

    // Close and unref the connection
    dbus_connection_close(ps->dbus_conn);
    dbus_connection_unref(ps->dbus_conn);
  }
}

/** @name DBusTimeout handling
 */
///@{

/**
 * Callback for adding D-Bus timeout.
 */
static dbus_bool_t
cdbus_callback_add_timeout(DBusTimeout *timeout, void *data) {
  session_t *ps = data;

  timeout_t *ptmout = timeout_insert(ps, dbus_timeout_get_interval(timeout),
      cdbus_callback_handle_timeout, timeout);
  if (ptmout)
    dbus_timeout_set_data(timeout, ptmout, NULL);

  return (bool) ptmout;
}

/**
 * Callback for removing D-Bus timeout.
 */
static void
cdbus_callback_remove_timeout(DBusTimeout *timeout, void *data) {
  session_t *ps = data;

  timeout_t *ptmout = dbus_timeout_get_data(timeout);
  assert(ptmout);
  if (ptmout)
    timeout_drop(ps, ptmout);
}

/**
 * Callback for toggling a D-Bus timeout.
 */
static void
cdbus_callback_timeout_toggled(DBusTimeout *timeout, void *data) {
  timeout_t *ptmout = dbus_timeout_get_data(timeout);

  assert(ptmout);
  if (ptmout) {
    ptmout->enabled = dbus_timeout_get_enabled(timeout);
    // Refresh interval as libdbus doc says: "Whenever a timeout is toggled,
    // its interval may change."
    ptmout->interval = dbus_timeout_get_interval(timeout);
  }
}

/**
 * Callback for handling a D-Bus timeout.
 */
static bool
cdbus_callback_handle_timeout(session_t *ps, timeout_t *ptmout) {
  assert(ptmout && ptmout->data);
  if (ptmout && ptmout->data)
    return dbus_timeout_handle(ptmout->data);

  return false;
}

///@}

/** @name DBusWatch handling
 */
///@{

/**
 * Callback for adding D-Bus watch.
 */
static dbus_bool_t
cdbus_callback_add_watch(DBusWatch *watch, void *data) {
  // Leave disabled watches alone
  if (!dbus_watch_get_enabled(watch))
    return TRUE;

  session_t *ps = data;

  // Insert the file descriptor
  fds_insert(ps, dbus_watch_get_unix_fd(watch),
      cdbus_get_watch_cond(watch));

  // Always return true
  return TRUE;
}

/**
 * Callback for removing D-Bus watch.
 */
static void
cdbus_callback_remove_watch(DBusWatch *watch, void *data) {
  session_t *ps = data;

  fds_drop(ps, dbus_watch_get_unix_fd(watch),
      cdbus_get_watch_cond(watch));
}

/**
 * Callback for toggling D-Bus watch status.
 */
static void
cdbus_callback_watch_toggled(DBusWatch *watch, void *data) {
  if (dbus_watch_get_enabled(watch)) {
    cdbus_callback_add_watch(watch, data);
  }
  else {
    cdbus_callback_remove_watch(watch, data);
  }
}

///@}

/** @name Message argument appending callbacks
 */
///@{

/**
 * Callback to append a bool argument to a message.
 */
static bool
cdbus_apdarg_bool(session_t *ps, DBusMessage *msg, const void *data) {
  assert(data);

  dbus_bool_t val = *(const bool *) data;

  if (!dbus_message_append_args(msg, DBUS_TYPE_BOOLEAN, &val,
        DBUS_TYPE_INVALID)) {
    printf_errf("(): Failed to append argument.");
    return false;
  }

  return true;
}

/**
 * Callback to append an int32 argument to a message.
 */
static bool
cdbus_apdarg_int32(session_t *ps, DBusMessage *msg, const void *data) {
  if (!dbus_message_append_args(msg, DBUS_TYPE_INT32, data,
        DBUS_TYPE_INVALID)) {
    printf_errf("(): Failed to append argument.");
    return false;
  }

  return true;
}

/**
 * Callback to append an uint32 argument to a message.
 */
static bool
cdbus_apdarg_uint32(session_t *ps, DBusMessage *msg, const void *data) {
  if (!dbus_message_append_args(msg, DBUS_TYPE_UINT32, data,
        DBUS_TYPE_INVALID)) {
    printf_errf("(): Failed to append argument.");
    return false;
  }

  return true;
}

/**
 * Callback to append a double argument to a message.
 */
static bool
cdbus_apdarg_double(session_t *ps, DBusMessage *msg, const void *data) {
  if (!dbus_message_append_args(msg, DBUS_TYPE_DOUBLE, data,
        DBUS_TYPE_INVALID)) {
    printf_errf("(): Failed to append argument.");
    return false;
  }

  return true;
}

/**
 * Callback to append a Window argument to a message.
 */
static bool
cdbus_apdarg_wid(session_t *ps, DBusMessage *msg, const void *data) {
  assert(data);
  cdbus_window_t val = *(const Window *)data;

  if (!dbus_message_append_args(msg, CDBUS_TYPE_WINDOW, &val,
        DBUS_TYPE_INVALID)) {
    printf_errf("(): Failed to append argument.");
    return false;
  }

  return true;
}

/**
 * Callback to append an cdbus_enum_t argument to a message.
 */
static bool
cdbus_apdarg_enum(session_t *ps, DBusMessage *msg, const void *data) {
  assert(data);
  if (!dbus_message_append_args(msg, CDBUS_TYPE_ENUM, data,
        DBUS_TYPE_INVALID)) {
    printf_errf("(): Failed to append argument.");
    return false;
  }

  return true;
}

/**
 * Callback to append a string argument to a message.
 */
static bool
cdbus_apdarg_string(session_t *ps, DBusMessage *msg, const void *data) {
  const char *str = data;
  if (!str)
    str = "";

  if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &str,
        DBUS_TYPE_INVALID)) {
    printf_errf("(): Failed to append argument.");
    return false;
  }

  return true;
}

/**
 * Callback to append all window IDs to a message.
 */
static bool
cdbus_apdarg_wids(session_t *ps, DBusMessage *msg, const void *data) {
  // Get the number of wids we are to include
  unsigned count = 0;
  for (win *w = ps->list; w; w = w->next) {
    if (!w->destroyed)
      ++count;
  }

  // Allocate memory for an array of window IDs
  cdbus_window_t *arr = malloc(sizeof(cdbus_window_t) * count);
  if (!arr) {
    printf_errf("(): Failed to allocate memory for window ID array.");
    return false;
  }

  // Build the array
  {
    cdbus_window_t *pcur = arr;
    for (win *w = ps->list; w; w = w->next) {
      if (!w->destroyed) {
        *pcur = w->id;
        ++pcur;
        assert(pcur <= arr + count);
      }
    }
    assert(pcur == arr + count);
  }

  // Append arguments
  if (!dbus_message_append_args(msg, DBUS_TYPE_ARRAY, CDBUS_TYPE_WINDOW,
        &arr, count, DBUS_TYPE_INVALID)) {
    printf_errf("(): Failed to append argument.");
    free(arr);
    return false;
  }

  free(arr);
  return true;
}
///@}

/**
 * Send a D-Bus signal.
 *
 * @param ps current session
 * @param name signal name
 * @param func a function that modifies the built message, to, for example,
 *        add an argument
 * @param data data pointer to pass to the function
 */
static bool
cdbus_signal(session_t *ps, const char *name,
    bool (*func)(session_t *ps, DBusMessage *msg, const void *data),
    const void *data) {
  DBusMessage* msg = NULL;

  // Create a signal
  msg = dbus_message_new_signal(CDBUS_OBJECT_NAME, CDBUS_INTERFACE_NAME,
      name);
  if (!msg) {
    printf_errf("(): Failed to create D-Bus signal.");
    return false;
  }

  // Append arguments onto message
  if (func && !func(ps, msg, data)) {
    dbus_message_unref(msg);
    return false;
  }

  // Send the message and flush the connection
  if (!dbus_connection_send(ps->dbus_conn, msg, NULL)) {
    printf_errf("(): Failed to send D-Bus signal.");
    dbus_message_unref(msg);
    return false;
  }
  dbus_connection_flush(ps->dbus_conn);

  // Free the message
  dbus_message_unref(msg);

  return true;
}

/**
 * Send a D-Bus reply.
 *
 * @param ps current session
 * @param srcmsg original message
 * @param func a function that modifies the built message, to, for example,
 *        add an argument
 * @param data data pointer to pass to the function
 */
static bool
cdbus_reply(session_t *ps, DBusMessage *srcmsg,
    bool (*func)(session_t *ps, DBusMessage *msg, const void *data),
    const void *data) {
  DBusMessage* msg = NULL;

  // Create a reply
  msg = dbus_message_new_method_return(srcmsg);
  if (!msg) {
    printf_errf("(): Failed to create D-Bus reply.");
    return false;
  }

  // Append arguments onto message
  if (func && !func(ps, msg, data)) {
    dbus_message_unref(msg);
    return false;
  }

  // Send the message and flush the connection
  if (!dbus_connection_send(ps->dbus_conn, msg, NULL)) {
    printf_errf("(): Failed to send D-Bus reply.");
    dbus_message_unref(msg);
    return false;
  }
  dbus_connection_flush(ps->dbus_conn);

  // Free the message
  dbus_message_unref(msg);

  return true;
}

/**
 * Send a D-Bus error reply.
 *
 * @param ps current session
 * @param msg the new error DBusMessage
 */
static bool
cdbus_reply_errm(session_t *ps, DBusMessage *msg) {
  if (!msg) {
    printf_errf("(): Failed to create D-Bus reply.");
    return false;
  }

  // Send the message and flush the connection
  if (!dbus_connection_send(ps->dbus_conn, msg, NULL)) {
    printf_errf("(): Failed to send D-Bus reply.");
    dbus_message_unref(msg);
    return false;
  }
  dbus_connection_flush(ps->dbus_conn);

  // Free the message
  dbus_message_unref(msg);

  return true;
}

/**
 * Get n-th argument of a D-Bus message.
 *
 * @param count the position of the argument to get, starting from 0
 * @param type libdbus type number of the type
 * @param pdest pointer to the target
 * @return true if successful, false otherwise.
 */
static bool
cdbus_msg_get_arg(DBusMessage *msg, int count, const int type, void *pdest) {
  assert(count >= 0);

  DBusMessageIter iter = { };
  if (!dbus_message_iter_init(msg, &iter)) {
    printf_errf("(): Message has no argument.");
    return false;
  }

  {
    const int oldcount = count;
    while (count) {
      if (!dbus_message_iter_next(&iter)) {
        printf_errf("(): Failed to find argument %d.", oldcount);
        return false;
      }
      --count;
    }
  }

  if (type != dbus_message_iter_get_arg_type(&iter)) {
    printf_errf("(): Argument has incorrect type.");
    return false;
  }

  dbus_message_iter_get_basic(&iter, pdest);

  return true;
}

void
cdbus_loop(session_t *ps) {
  dbus_connection_read_write(ps->dbus_conn, 0);
  DBusMessage *msg = NULL;
  while ((msg = dbus_connection_pop_message(ps->dbus_conn)))
    cdbus_process(ps, msg);
}

/** @name Message processing
 */
///@{

/**
 * Process a message from D-Bus.
 */
static void
cdbus_process(session_t *ps, DBusMessage *msg) {
  bool success = false;

#define cdbus_m_ismethod(method) \
  dbus_message_is_method_call(msg, CDBUS_INTERFACE_NAME, method)

  if (cdbus_m_ismethod("reset")) {
    ps->reset = true;
    if (!dbus_message_get_no_reply(msg))
      cdbus_reply_bool(ps, msg, true);
    success = true;
  }
  else if (cdbus_m_ismethod("repaint")) {
    force_repaint(ps);
    if (!dbus_message_get_no_reply(msg))
      cdbus_reply_bool(ps, msg, true);
    success = true;
  }
  else if (cdbus_m_ismethod("list_win")) {
    success = cdbus_process_list_win(ps, msg);
  }
  else if (cdbus_m_ismethod("win_get")) {
    success = cdbus_process_win_get(ps, msg);
  }
  else if (cdbus_m_ismethod("win_set")) {
    success = cdbus_process_win_set(ps, msg);
  }
  else if (cdbus_m_ismethod("find_win")) {
    success = cdbus_process_find_win(ps, msg);
  }
  else if (cdbus_m_ismethod("opts_get")) {
    success = cdbus_process_opts_get(ps, msg);
  }
  else if (cdbus_m_ismethod("opts_set")) {
    success = cdbus_process_opts_set(ps, msg);
  }
#undef cdbus_m_ismethod
  else if (dbus_message_is_method_call(msg,
        "org.freedesktop.DBus.Introspectable", "Introspect")) {
    success = cdbus_process_introspect(ps, msg);
  }
  else if (dbus_message_is_method_call(msg,
        "org.freedesktop.DBus.Peer", "Ping")) {
    cdbus_reply(ps, msg, NULL, NULL);
    success = true;
  }
  else if (dbus_message_is_method_call(msg,
        "org.freedesktop.DBus.Peer", "GetMachineId")) {
    char *uuid = dbus_get_local_machine_id();
    if (uuid) {
      cdbus_reply_string(ps, msg, uuid);
      dbus_free(uuid);
      success = true;
    }
  }
  else if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameAcquired")
      || dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameLost")) {
    success = true;
  }
  else {
    if (DBUS_MESSAGE_TYPE_ERROR == dbus_message_get_type(msg)) {
      printf_errf("(): Error message of path \"%s\" "
          "interface \"%s\", member \"%s\", error \"%s\"",
          dbus_message_get_path(msg), dbus_message_get_interface(msg),
          dbus_message_get_member(msg), dbus_message_get_error_name(msg));
    }
    else {
      printf_errf("(): Illegal message of type \"%s\", path \"%s\" "
          "interface \"%s\", member \"%s\"",
          cdbus_repr_msgtype(msg), dbus_message_get_path(msg),
          dbus_message_get_interface(msg), dbus_message_get_member(msg));
    }
    if (DBUS_MESSAGE_TYPE_METHOD_CALL == dbus_message_get_type(msg)
        && !dbus_message_get_no_reply(msg))
      cdbus_reply_err(ps, msg, CDBUS_ERROR_BADMSG, CDBUS_ERROR_BADMSG_S);
    success = true;
  }

  // If the message could not be processed, and an reply is expected, return
  // an empty reply.
  if (!success && DBUS_MESSAGE_TYPE_METHOD_CALL == dbus_message_get_type(msg)
      && !dbus_message_get_no_reply(msg))
    cdbus_reply_err(ps, msg, CDBUS_ERROR_UNKNOWN, CDBUS_ERROR_UNKNOWN_S);

  // Free the message
  dbus_message_unref(msg);
}

/**
 * Process a list_win D-Bus request.
 */
static bool
cdbus_process_list_win(session_t *ps, DBusMessage *msg) {
  cdbus_reply(ps, msg, cdbus_apdarg_wids, NULL);

  return true;
}

/**
 * Process a win_get D-Bus request.
 */
static bool
cdbus_process_win_get(session_t *ps, DBusMessage *msg) {
  cdbus_window_t wid = None;
  const char *target = NULL;
  DBusError err = { };

  if (!dbus_message_get_args(msg, &err,
        CDBUS_TYPE_WINDOW, &wid,
        DBUS_TYPE_STRING, &target,
        DBUS_TYPE_INVALID)) {
    printf_errf("(): Failed to parse argument of \"win_get\" (%s).",
        err.message);
    dbus_error_free(&err);
    return false;
  }

  win *w = find_win(ps, wid);

  if (!w) {
    printf_errf("(): Window %#010x not found.", wid);
    cdbus_reply_err(ps, msg, CDBUS_ERROR_BADWIN, CDBUS_ERROR_BADWIN_S, wid);
    return true;
  }

#define cdbus_m_win_get_do(tgt, apdarg_func) \
  if (!strcmp(MSTR(tgt), target)) { \
    apdarg_func(ps, msg, w->tgt); \
    return true; \
  }

  cdbus_m_win_get_do(id, cdbus_reply_wid);

  // next
  if (!strcmp("next", target)) {
    cdbus_reply_wid(ps, msg, (w->next ? w->next->id: 0));
    return true;
  }

  // map_state
  if (!strcmp("map_state", target)) {
    cdbus_reply_bool(ps, msg, w->a.map_state);
    return true;
  }

  cdbus_m_win_get_do(mode, cdbus_reply_enum);
  cdbus_m_win_get_do(client_win, cdbus_reply_wid);
  cdbus_m_win_get_do(damaged, cdbus_reply_bool);
  cdbus_m_win_get_do(destroyed, cdbus_reply_bool);
  cdbus_m_win_get_do(window_type, cdbus_reply_enum);
  cdbus_m_win_get_do(wmwin, cdbus_reply_bool);
  cdbus_m_win_get_do(leader, cdbus_reply_wid);
  // focused_real
  if (!strcmp("focused_real", target)) {
    cdbus_reply_bool(ps, msg, win_is_focused_real(ps, w));
    return true;
  }
  cdbus_m_win_get_do(fade_force, cdbus_reply_enum);
  cdbus_m_win_get_do(shadow_force, cdbus_reply_enum);
  cdbus_m_win_get_do(focused_force, cdbus_reply_enum);
  cdbus_m_win_get_do(invert_color_force, cdbus_reply_enum);
  cdbus_m_win_get_do(name, cdbus_reply_string);
  cdbus_m_win_get_do(class_instance, cdbus_reply_string);
  cdbus_m_win_get_do(class_general, cdbus_reply_string);
  cdbus_m_win_get_do(role, cdbus_reply_string);

  cdbus_m_win_get_do(opacity, cdbus_reply_uint32);
  cdbus_m_win_get_do(opacity_tgt, cdbus_reply_uint32);
  cdbus_m_win_get_do(opacity_prop, cdbus_reply_uint32);
  cdbus_m_win_get_do(opacity_prop_client, cdbus_reply_uint32);
  cdbus_m_win_get_do(opacity_set, cdbus_reply_uint32);

  cdbus_m_win_get_do(frame_opacity, cdbus_reply_double);
  if (!strcmp("left_width", target)) {
    cdbus_reply_uint32(ps, msg, w->frame_extents.left);
    return true;
  }
  if (!strcmp("right_width", target)) {
    cdbus_reply_uint32(ps, msg, w->frame_extents.right);
    return true;
  }
  if (!strcmp("top_width", target)) {
    cdbus_reply_uint32(ps, msg, w->frame_extents.top);
    return true;
  }
  if (!strcmp("bottom_width", target)) {
    cdbus_reply_uint32(ps, msg, w->frame_extents.bottom);
    return true;
  }

  cdbus_m_win_get_do(shadow, cdbus_reply_bool);
  cdbus_m_win_get_do(fade, cdbus_reply_bool);
  cdbus_m_win_get_do(invert_color, cdbus_reply_bool);
  cdbus_m_win_get_do(blur_background, cdbus_reply_bool);
#undef cdbus_m_win_get_do

  printf_errf("(): " CDBUS_ERROR_BADTGT_S, target);
  cdbus_reply_err(ps, msg, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);

  return true;
}

/**
 * Process a win_set D-Bus request.
 */
static bool
cdbus_process_win_set(session_t *ps, DBusMessage *msg) {
  cdbus_window_t wid = None;
  const char *target = NULL;
  DBusError err = { };

  if (!dbus_message_get_args(msg, &err,
        CDBUS_TYPE_WINDOW, &wid,
        DBUS_TYPE_STRING, &target,
        DBUS_TYPE_INVALID)) {
    printf_errf("(): Failed to parse argument of \"win_set\" (%s).",
        err.message);
    dbus_error_free(&err);
    return false;
  }

  win *w = find_win(ps, wid);

  if (!w) {
    printf_errf("(): Window %#010x not found.", wid);
    cdbus_reply_err(ps, msg, CDBUS_ERROR_BADWIN, CDBUS_ERROR_BADWIN_S, wid);
    return true;
  }

#define cdbus_m_win_set_do(tgt, type, real_type) \
  if (!strcmp(MSTR(tgt), target)) { \
    real_type val; \
    if (!cdbus_msg_get_arg(msg, 2, type, &val)) \
      return false; \
    w->tgt = val; \
    goto cdbus_process_win_set_success; \
  }

  if (!strcmp("shadow_force", target)) {
    cdbus_enum_t val = UNSET;
    if (!cdbus_msg_get_arg(msg, 2, CDBUS_TYPE_ENUM, &val))
      return false;
    win_set_shadow_force(ps, w, val);
    goto cdbus_process_win_set_success;
  }

  if (!strcmp("fade_force", target)) {
    cdbus_enum_t val = UNSET;
    if (!cdbus_msg_get_arg(msg, 2, CDBUS_TYPE_ENUM, &val))
      return false;
    win_set_fade_force(ps, w, val);
    goto cdbus_process_win_set_success;
  }

  if (!strcmp("focused_force", target)) {
    cdbus_enum_t val = UNSET;
    if (!cdbus_msg_get_arg(msg, 2, CDBUS_TYPE_ENUM, &val))
      return false;
    win_set_focused_force(ps, w, val);
    goto cdbus_process_win_set_success;
  }

  if (!strcmp("invert_color_force", target)) {
    cdbus_enum_t val = UNSET;
    if (!cdbus_msg_get_arg(msg, 2, CDBUS_TYPE_ENUM, &val))
      return false;
    win_set_invert_color_force(ps, w, val);
    goto cdbus_process_win_set_success;
  }
#undef cdbus_m_win_set_do

  printf_errf("(): " CDBUS_ERROR_BADTGT_S, target);
  cdbus_reply_err(ps, msg, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);

  return true;

cdbus_process_win_set_success:
  if (!dbus_message_get_no_reply(msg))
    cdbus_reply_bool(ps, msg, true);
  return true;
}

/**
 * Process a find_win D-Bus request.
 */
static bool
cdbus_process_find_win(session_t *ps, DBusMessage *msg) {
  const char *target = NULL;

  if (!cdbus_msg_get_arg(msg, 0, DBUS_TYPE_STRING, &target))
    return false;

  Window wid = None;

  // Find window by client window
  if (!strcmp("client", target)) {
    cdbus_window_t client = None;
    if (!cdbus_msg_get_arg(msg, 1, CDBUS_TYPE_WINDOW, &client))
      return false;
    win *w = find_toplevel(ps, client);
    if (w)
      wid = w->id;
  }
  // Find focused window
  else if (!strcmp("focused", target)) {
    win *w = find_focused(ps);
    if (w)
      wid = w->id;
  }
  else {
    printf_errf("(): " CDBUS_ERROR_BADTGT_S, target);
    cdbus_reply_err(ps, msg, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);

    return true;
  }

  cdbus_reply_wid(ps, msg, wid);

  return true;
}

/**
 * Process a opts_get D-Bus request.
 */
static bool
cdbus_process_opts_get(session_t *ps, DBusMessage *msg) {
  const char *target = NULL;

  if (!cdbus_msg_get_arg(msg, 0, DBUS_TYPE_STRING, &target))
    return false;

#define cdbus_m_opts_get_do(tgt, apdarg_func) \
  if (!strcmp(MSTR(tgt), target)) { \
    apdarg_func(ps, msg, ps->o.tgt); \
    return true; \
  }

  // version
  if (!strcmp("version", target)) {
    cdbus_reply_string(ps, msg, COMPTON_VERSION);
    return true;
  }

  // pid
  if (!strcmp("pid", target)) {
    cdbus_reply_int32(ps, msg, getpid());
    return true;
  }

  // display
  if (!strcmp("display", target)) {
    cdbus_reply_string(ps, msg, DisplayString(ps->dpy));
    return true;
  }

  cdbus_m_opts_get_do(config_file, cdbus_reply_string);
  cdbus_m_opts_get_do(display_repr, cdbus_reply_string);
  cdbus_m_opts_get_do(write_pid_path, cdbus_reply_string);
  cdbus_m_opts_get_do(mark_wmwin_focused, cdbus_reply_bool);
  cdbus_m_opts_get_do(mark_ovredir_focused, cdbus_reply_bool);
  cdbus_m_opts_get_do(fork_after_register, cdbus_reply_bool);
  cdbus_m_opts_get_do(detect_rounded_corners, cdbus_reply_bool);
  cdbus_m_opts_get_do(paint_on_overlay, cdbus_reply_bool);
  // paint_on_overlay_id: Get ID of the X composite overlay window
  if (!strcmp("paint_on_overlay_id", target)) {
    cdbus_reply_uint32(ps, msg, ps->overlay);
    return true;
  }
  cdbus_m_opts_get_do(unredir_if_possible, cdbus_reply_bool);
  cdbus_m_opts_get_do(unredir_if_possible_delay, cdbus_reply_int32);
  cdbus_m_opts_get_do(redirected_force, cdbus_reply_enum);
  cdbus_m_opts_get_do(stoppaint_force, cdbus_reply_enum);
  cdbus_m_opts_get_do(logpath, cdbus_reply_string);
  cdbus_m_opts_get_do(synchronize, cdbus_reply_bool);

  cdbus_m_opts_get_do(refresh_rate, cdbus_reply_int32);
  cdbus_m_opts_get_do(sw_opti, cdbus_reply_bool);
  if (!strcmp("vsync", target)) {
    assert(ps->o.vsync < sizeof(VSYNC_STRS) / sizeof(VSYNC_STRS[0]));
    cdbus_reply_string(ps, msg, VSYNC_STRS[ps->o.vsync]);
    return true;
  }
  if (!strcmp("backend", target)) {
    assert(ps->o.backend < sizeof(BACKEND_STRS) / sizeof(BACKEND_STRS[0]));
    cdbus_reply_string(ps, msg, BACKEND_STRS[ps->o.backend]);
    return true;
  }
  cdbus_m_opts_get_do(dbe, cdbus_reply_bool);
  cdbus_m_opts_get_do(vsync_aggressive, cdbus_reply_bool);

  cdbus_m_opts_get_do(shadow_red, cdbus_reply_double);
  cdbus_m_opts_get_do(shadow_green, cdbus_reply_double);
  cdbus_m_opts_get_do(shadow_blue, cdbus_reply_double);
  cdbus_m_opts_get_do(shadow_radius, cdbus_reply_int32);
  cdbus_m_opts_get_do(shadow_offset_x, cdbus_reply_int32);
  cdbus_m_opts_get_do(shadow_offset_y, cdbus_reply_int32);
  cdbus_m_opts_get_do(shadow_opacity, cdbus_reply_double);
  cdbus_m_opts_get_do(clear_shadow, cdbus_reply_bool);
  cdbus_m_opts_get_do(xinerama_shadow_crop, cdbus_reply_bool);

  cdbus_m_opts_get_do(fade_delta, cdbus_reply_int32);
  cdbus_m_opts_get_do(fade_in_step, cdbus_reply_int32);
  cdbus_m_opts_get_do(fade_out_step, cdbus_reply_int32);
  cdbus_m_opts_get_do(no_fading_openclose, cdbus_reply_bool);

  cdbus_m_opts_get_do(blur_background, cdbus_reply_bool);
  cdbus_m_opts_get_do(blur_background_frame, cdbus_reply_bool);
  cdbus_m_opts_get_do(blur_background_fixed, cdbus_reply_bool);

  cdbus_m_opts_get_do(inactive_dim, cdbus_reply_double);
  cdbus_m_opts_get_do(inactive_dim_fixed, cdbus_reply_bool);

  cdbus_m_opts_get_do(use_ewmh_active_win, cdbus_reply_bool);
  cdbus_m_opts_get_do(detect_transient, cdbus_reply_bool);
  cdbus_m_opts_get_do(detect_client_leader, cdbus_reply_bool);

#ifdef CONFIG_VSYNC_OPENGL
  cdbus_m_opts_get_do(glx_no_stencil, cdbus_reply_bool);
  cdbus_m_opts_get_do(glx_copy_from_front, cdbus_reply_bool);
  cdbus_m_opts_get_do(glx_use_copysubbuffermesa, cdbus_reply_bool);
  cdbus_m_opts_get_do(glx_no_rebind_pixmap, cdbus_reply_bool);
  cdbus_m_opts_get_do(glx_swap_method, cdbus_reply_int32);
#endif

  cdbus_m_opts_get_do(track_focus, cdbus_reply_bool);
  cdbus_m_opts_get_do(track_wdata, cdbus_reply_bool);
  cdbus_m_opts_get_do(track_leader, cdbus_reply_bool);
#undef cdbus_m_opts_get_do

  printf_errf("(): " CDBUS_ERROR_BADTGT_S, target);
  cdbus_reply_err(ps, msg, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);

  return true;
}

/**
 * Process a opts_set D-Bus request.
 */
static bool
cdbus_process_opts_set(session_t *ps, DBusMessage *msg) {
  const char *target = NULL;

  if (!cdbus_msg_get_arg(msg, 0, DBUS_TYPE_STRING, &target))
    return false;

#define cdbus_m_opts_set_do(tgt, type, real_type) \
  if (!strcmp(MSTR(tgt), target)) { \
    real_type val; \
    if (!cdbus_msg_get_arg(msg, 1, type, &val)) \
      return false; \
    ps->o.tgt = val; \
    goto cdbus_process_opts_set_success; \
  }

  // fade_delta
  if (!strcmp("fade_delta", target)) {
    int32_t val = 0.0;
    if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_INT32, &val))
      return false;
    ps->o.fade_delta = max_i(val, 1);
    goto cdbus_process_opts_set_success;
  }

  // fade_in_step
  if (!strcmp("fade_in_step", target)) {
    double val = 0.0;
    if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_DOUBLE, &val))
      return false;
    ps->o.fade_in_step = normalize_d(val) * OPAQUE;
    goto cdbus_process_opts_set_success;
  }

  // fade_out_step
  if (!strcmp("fade_out_step", target)) {
    double val = 0.0;
    if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_DOUBLE, &val))
      return false;
    ps->o.fade_out_step = normalize_d(val) * OPAQUE;
    goto cdbus_process_opts_set_success;
  }

  // no_fading_openclose
  if (!strcmp("no_fading_openclose", target)) {
    dbus_bool_t val = FALSE;
    if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_BOOLEAN, &val))
      return false;
    opts_set_no_fading_openclose(ps, val);
    goto cdbus_process_opts_set_success;
  }

  // unredir_if_possible
  if (!strcmp("unredir_if_possible", target)) {
    dbus_bool_t val = FALSE;
    if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_BOOLEAN, &val))
      return false;
    if (ps->o.unredir_if_possible != val) {
      ps->o.unredir_if_possible = val;
      ps->ev_received = true;
    }
    goto cdbus_process_opts_set_success;
  }

  // clear_shadow
  if (!strcmp("clear_shadow", target)) {
    dbus_bool_t val = FALSE;
    if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_BOOLEAN, &val))
      return false;
    if (ps->o.clear_shadow != val) {
      ps->o.clear_shadow = val;
      force_repaint(ps);
    }
    goto cdbus_process_opts_set_success;
  }

  // track_focus
  if (!strcmp("track_focus", target)) {
    dbus_bool_t val = FALSE;
    if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_BOOLEAN, &val))
      return false;
    // You could enable this option, but never turn if off
    if (val) {
      opts_init_track_focus(ps);
    }
    goto cdbus_process_opts_set_success;
  }

  // vsync
  if (!strcmp("vsync", target)) {
    const char * val = NULL;
    if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_STRING, &val))
      return false;
    vsync_deinit(ps);
    if (!parse_vsync(ps, val)) {
      printf_errf("(): " CDBUS_ERROR_BADARG_S, 1, "Value invalid.");
      cdbus_reply_err(ps, msg, CDBUS_ERROR_BADARG, CDBUS_ERROR_BADARG_S, 1, "Value invalid.");
    }
    else if (!vsync_init(ps)) {
      printf_errf("(): " CDBUS_ERROR_CUSTOM_S, "Failed to initialize specified VSync method.");
      cdbus_reply_err(ps, msg, CDBUS_ERROR_CUSTOM, CDBUS_ERROR_CUSTOM_S, "Failed to initialize specified VSync method.");
    }
    else
      goto cdbus_process_opts_set_success;
    return true;
  }

  // redirected_force
  if (!strcmp("redirected_force", target)) {
    cdbus_enum_t val = UNSET;
    if (!cdbus_msg_get_arg(msg, 1, CDBUS_TYPE_ENUM, &val))
      return false;
    ps->o.redirected_force = val;
    force_repaint(ps);
    goto cdbus_process_opts_set_success;
  }

  // stoppaint_force
  cdbus_m_opts_set_do(stoppaint_force, CDBUS_TYPE_ENUM, cdbus_enum_t);

#undef cdbus_m_opts_set_do

  printf_errf("(): " CDBUS_ERROR_BADTGT_S, target);
  cdbus_reply_err(ps, msg, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);

  return true;

cdbus_process_opts_set_success:
  if (!dbus_message_get_no_reply(msg))
    cdbus_reply_bool(ps, msg, true);
  return true;
}

/**
 * Process an Introspect D-Bus request.
 */
static bool
cdbus_process_introspect(session_t *ps, DBusMessage *msg) {
  const static char *str_introspect =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node name='" CDBUS_OBJECT_NAME "'>\n"
    "  <interface name='org.freedesktop.DBus.Introspectable'>\n"
    "    <method name='Introspect'>\n"
    "      <arg name='data' direction='out' type='s' />\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name='org.freedesktop.DBus.Peer'>\n"
    "    <method name='Ping' />\n"
    "    <method name='GetMachineId'>\n"
    "      <arg name='machine_uuid' direction='out' type='s' />\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name='" CDBUS_INTERFACE_NAME "'>\n"
    "    <signal name='win_added'>\n"
    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
    "    </signal>\n"
    "    <signal name='win_destroyed'>\n"
    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
    "    </signal>\n"
    "    <signal name='win_mapped'>\n"
    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
    "    </signal>\n"
    "    <signal name='win_unmapped'>\n"
    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
    "    </signal>\n"
    "    <signal name='win_focusin'>\n"
    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
    "    </signal>\n"
    "    <signal name='win_focusout'>\n"
    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
    "    </signal>\n"
    "    <method name='reset' />\n"
    "    <method name='repaint' />\n"
    "  </interface>\n"
    "</node>\n";

  cdbus_reply_string(ps, msg, str_introspect);

  return true;
}
///@}

/** @name Core callbacks
 */
///@{
void
cdbus_ev_win_added(session_t *ps, win *w) {
  if (ps->dbus_conn)
    cdbus_signal_wid(ps, "win_added", w->id);
}

void
cdbus_ev_win_destroyed(session_t *ps, win *w) {
  if (ps->dbus_conn)
    cdbus_signal_wid(ps, "win_destroyed", w->id);
}

void
cdbus_ev_win_mapped(session_t *ps, win *w) {
  if (ps->dbus_conn)
    cdbus_signal_wid(ps, "win_mapped", w->id);
}

void
cdbus_ev_win_unmapped(session_t *ps, win *w) {
  if (ps->dbus_conn)
    cdbus_signal_wid(ps, "win_unmapped", w->id);
}

void
cdbus_ev_win_focusout(session_t *ps, win *w) {
  if (ps->dbus_conn)
    cdbus_signal_wid(ps, "win_focusout", w->id);
}

void
cdbus_ev_win_focusin(session_t *ps, win *w) {
  if (ps->dbus_conn)
    cdbus_signal_wid(ps, "win_focusin", w->id);
}
//!@}
