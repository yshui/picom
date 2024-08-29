// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

#include <X11/Xlib.h>
#include <ctype.h>
#include <dbus/dbus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include <picom/types.h>

#include "backend/backend.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "picom.h"
#include "utils/misc.h"
#include "utils/str.h"
#include "wm/defs.h"
#include "wm/win.h"
#include "wm/wm.h"

#include "dbus.h"

struct cdbus_data {
	/// Mainloop
	struct ev_loop *loop;
	/// DBus connection.
	DBusConnection *dbus_conn;
	/// DBus service name.
	char *dbus_service;
};

// Window type
typedef uint32_t cdbus_window_t;
#define CDBUS_TYPE_WINDOW DBUS_TYPE_UINT32
#define CDBUS_TYPE_WINDOW_STR DBUS_TYPE_UINT32_AS_STRING

typedef uint32_t cdbus_enum_t;
#define CDBUS_TYPE_ENUM DBUS_TYPE_UINT32
#define CDBUS_TYPE_ENUM_STR DBUS_TYPE_UINT32_AS_STRING

#define CDBUS_SERVICE_NAME "com.github.chjj.compton"
#define CDBUS_INTERFACE_NAME CDBUS_SERVICE_NAME
#define CDBUS_OBJECT_NAME "/com/github/chjj/compton"
#define CDBUS_ERROR_PREFIX CDBUS_INTERFACE_NAME ".error"
#define CDBUS_ERROR_UNKNOWN CDBUS_ERROR_PREFIX ".unknown"
#define CDBUS_ERROR_UNKNOWN_S "Well, I don't know what happened. Do you?"
#define CDBUS_ERROR_BADMSG CDBUS_ERROR_PREFIX ".bad_message"
#define CDBUS_ERROR_BADMSG_S                                                             \
	"Unrecognized command. Beware compton "                                          \
	"cannot make you a sandwich."
#define CDBUS_ERROR_BADARG CDBUS_ERROR_PREFIX ".bad_argument"
#define CDBUS_ERROR_BADARG_S "Failed to parse argument %d: %s"
#define CDBUS_ERROR_BADWIN CDBUS_ERROR_PREFIX ".bad_window"
#define CDBUS_ERROR_BADWIN_S "Requested window %#010x not found."
#define CDBUS_ERROR_BADTGT CDBUS_ERROR_PREFIX ".bad_target"
#define CDBUS_ERROR_BADTGT_S "Target \"%s\" not found."
#define CDBUS_ERROR_FORBIDDEN CDBUS_ERROR_PREFIX ".forbidden"
#define CDBUS_ERROR_FORBIDDEN_S "Incorrect password, access denied."
#define CDBUS_ERROR_CUSTOM CDBUS_ERROR_PREFIX ".custom"
#define CDBUS_ERROR_CUSTOM_S "%s"

#define cdbus_reply_err(conn, srcmsg, err_name, err_format, ...)                         \
	cdbus_reply_errm(conn, dbus_message_new_error_printf(                            \
	                           (srcmsg), (err_name), (err_format), ##__VA_ARGS__))

#define PICOM_WINDOW_INTERFACE "picom.Window"
#define PICOM_COMPOSITOR_INTERFACE "picom.Compositor"

static DBusHandlerResult cdbus_process(DBusConnection *conn, DBusMessage *m, void *);
static DBusHandlerResult cdbus_process_windows(DBusConnection *c, DBusMessage *msg, void *ud);

static dbus_bool_t cdbus_callback_add_timeout(DBusTimeout *timeout, void *data);

static void cdbus_callback_remove_timeout(DBusTimeout *timeout, void *data);

static void cdbus_callback_timeout_toggled(DBusTimeout *timeout, void *data);

static dbus_bool_t cdbus_callback_add_watch(DBusWatch *watch, void *data);

static void cdbus_callback_remove_watch(DBusWatch *watch, void *data);

static void cdbus_callback_watch_toggled(DBusWatch *watch, void *data);

/**
 * Initialize D-Bus connection.
 */
struct cdbus_data *cdbus_init(session_t *ps, const char *uniq) {
	auto cd = cmalloc(struct cdbus_data);
	cd->dbus_service = NULL;

	DBusError err = {};

	// Initialize
	dbus_error_init(&err);

	// Connect to D-Bus
	// Use dbus_bus_get_private() so we can fully recycle it ourselves
	cd->dbus_conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (dbus_error_is_set(&err)) {
		log_error("D-Bus connection failed.");
		goto fail;
	}

	if (!cd->dbus_conn) {
		log_error("D-Bus connection failed for unknown reason.");
		goto fail;
	}

	// Avoid exiting on disconnect
	dbus_connection_set_exit_on_disconnect(cd->dbus_conn, false);

	// Request service name
	{
		// Build service name
		size_t service_len = strlen(CDBUS_SERVICE_NAME) + strlen(uniq) + 2;
		char *service = ccalloc(service_len, char);
		snprintf(service, service_len, "%s.%s", CDBUS_SERVICE_NAME, uniq);

		// Make a valid dbus name by converting non alphanumeric characters to
		// underscore
		char *tmp = service + strlen(CDBUS_SERVICE_NAME) + 1;
		while (*tmp) {
			if (!isalnum((unsigned char)*tmp)) {
				*tmp = '_';
			}
			tmp++;
		}
		cd->dbus_service = service;

		// Request for the name
		int ret = dbus_bus_request_name(cd->dbus_conn, service,
		                                DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);

		if (dbus_error_is_set(&err)) {
			log_error("Failed to obtain D-Bus name.");
			goto fail;
		}

		if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
		    ret != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
			log_error("Failed to become the primary owner of requested D-Bus "
			          "name (%d).",
			          ret);
			goto fail;
		}
	}

	// Add watch handlers
	cd->loop = ps->loop;
	if (!dbus_connection_set_watch_functions(cd->dbus_conn, cdbus_callback_add_watch,
	                                         cdbus_callback_remove_watch,
	                                         cdbus_callback_watch_toggled, cd, NULL)) {
		log_error("Failed to add D-Bus watch functions.");
		goto fail;
	}

	// Add timeout handlers
	if (!dbus_connection_set_timeout_functions(
	        cd->dbus_conn, cdbus_callback_add_timeout, cdbus_callback_remove_timeout,
	        cdbus_callback_timeout_toggled, cd, NULL)) {
		log_error("Failed to add D-Bus timeout functions.");
		goto fail;
	}

	// Add match
	dbus_bus_add_match(cd->dbus_conn,
	                   "type='method_call',interface='" CDBUS_INTERFACE_NAME "'", &err);
	if (dbus_error_is_set(&err)) {
		log_error("Failed to add D-Bus match.");
		goto fail;
	}
	dbus_connection_register_object_path(
	    cd->dbus_conn, CDBUS_OBJECT_NAME,
	    (DBusObjectPathVTable[]){{NULL, cdbus_process}}, ps);
	dbus_connection_register_fallback(
	    cd->dbus_conn, CDBUS_OBJECT_NAME "/windows",
	    (DBusObjectPathVTable[]){{NULL, cdbus_process_windows}}, ps);
	return cd;
fail:
	if (dbus_error_is_set(&err)) {
		log_error("D-Bus error %s: %s", err.name, err.message);
		dbus_error_free(&err);
	}
	free(cd->dbus_service);
	free(cd);
	return NULL;
}

/**
 * Destroy D-Bus connection.
 */
void cdbus_destroy(struct cdbus_data *cd) {
	if (cd->dbus_conn) {
		// Release DBus name firstly
		if (cd->dbus_service) {
			DBusError err = {};
			dbus_error_init(&err);

			dbus_bus_release_name(cd->dbus_conn, cd->dbus_service, &err);
			if (dbus_error_is_set(&err)) {
				log_error("Failed to release DBus name (%s).", err.message);
				dbus_error_free(&err);
			}
			free(cd->dbus_service);
		}

		// Close and unref the connection
		dbus_connection_close(cd->dbus_conn);
		dbus_connection_unref(cd->dbus_conn);
	}
	free(cd);
}

/** @name DBusTimeout handling
 */
///@{

typedef struct ev_dbus_timer {
	ev_timer w;
	DBusTimeout *t;
} ev_dbus_timer;

/**
 * Callback for handling a D-Bus timeout.
 */
static void
cdbus_callback_handle_timeout(EV_P attr_unused, ev_timer *w, int revents attr_unused) {
	ev_dbus_timer *t = (void *)w;
	dbus_timeout_handle(t->t);
}

/**
 * Callback for adding D-Bus timeout.
 */
static dbus_bool_t cdbus_callback_add_timeout(DBusTimeout *timeout, void *data) {
	struct cdbus_data *cd = data;

	auto t = ccalloc(1, ev_dbus_timer);
	double i = dbus_timeout_get_interval(timeout) / 1000.0;
	ev_timer_init(&t->w, cdbus_callback_handle_timeout, i, i);
	t->t = timeout;
	dbus_timeout_set_data(timeout, t, NULL);

	if (dbus_timeout_get_enabled(timeout)) {
		ev_timer_start(cd->loop, &t->w);
	}

	return true;
}

/**
 * Callback for removing D-Bus timeout.
 */
static void cdbus_callback_remove_timeout(DBusTimeout *timeout, void *data) {
	struct cdbus_data *cd = data;

	ev_dbus_timer *t = dbus_timeout_get_data(timeout);
	assert(t);
	ev_timer_stop(cd->loop, &t->w);
	free(t);
}

/**
 * Callback for toggling a D-Bus timeout.
 */
static void cdbus_callback_timeout_toggled(DBusTimeout *timeout, void *data) {
	struct cdbus_data *cd = data;
	ev_dbus_timer *t = dbus_timeout_get_data(timeout);

	assert(t);
	ev_timer_stop(cd->loop, &t->w);
	if (dbus_timeout_get_enabled(timeout)) {
		double i = dbus_timeout_get_interval(timeout) / 1000.0;
		ev_timer_set(&t->w, i, i);
		ev_timer_start(cd->loop, &t->w);
	}
}

///@}

/** @name DBusWatch handling
 */
///@{

typedef struct ev_dbus_io {
	ev_io w;
	struct cdbus_data *cd;
	DBusWatch *dw;
} ev_dbus_io;

void cdbus_io_callback(EV_P attr_unused, ev_io *w, int revents) {
	ev_dbus_io *dw = (void *)w;
	DBusWatchFlags flags = 0;
	if (revents & EV_READ) {
		flags |= DBUS_WATCH_READABLE;
	}
	if (revents & EV_WRITE) {
		flags |= DBUS_WATCH_WRITABLE;
	}
	dbus_watch_handle(dw->dw, flags);
	while (dbus_connection_dispatch(dw->cd->dbus_conn) != DBUS_DISPATCH_COMPLETE)
		;
}

/**
 * Determine the poll condition of a DBusWatch.
 */
static inline int cdbus_get_watch_cond(DBusWatch *watch) {
	const unsigned flags = dbus_watch_get_flags(watch);
	int condition = 0;
	if (flags & DBUS_WATCH_READABLE) {
		condition |= EV_READ;
	}
	if (flags & DBUS_WATCH_WRITABLE) {
		condition |= EV_WRITE;
	}

	return condition;
}

/**
 * Callback for adding D-Bus watch.
 */
static dbus_bool_t cdbus_callback_add_watch(DBusWatch *watch, void *data) {
	struct cdbus_data *cd = data;

	auto w = ccalloc(1, ev_dbus_io);
	w->dw = watch;
	w->cd = cd;
	ev_io_init(&w->w, cdbus_io_callback, dbus_watch_get_unix_fd(watch),
	           cdbus_get_watch_cond(watch));

	// Leave disabled watches alone
	if (dbus_watch_get_enabled(watch)) {
		ev_io_start(cd->loop, &w->w);
	}

	dbus_watch_set_data(watch, w, NULL);

	// Always return true
	return true;
}

/**
 * Callback for removing D-Bus watch.
 */
static void cdbus_callback_remove_watch(DBusWatch *watch, void *data) {
	struct cdbus_data *cd = data;
	ev_dbus_io *w = dbus_watch_get_data(watch);
	ev_io_stop(cd->loop, &w->w);
	free(w);
}

/**
 * Callback for toggling D-Bus watch status.
 */
static void cdbus_callback_watch_toggled(DBusWatch *watch, void *data) {
	struct cdbus_data *cd = data;
	ev_io *w = dbus_watch_get_data(watch);
	if (dbus_watch_get_enabled(watch)) {
		ev_io_start(cd->loop, w);
	} else {
		ev_io_stop(cd->loop, w);
	}
}

///@}

static bool cdbus_append_boolean(DBusMessage *msg, dbus_bool_t val) {
	return dbus_message_append_args(msg, DBUS_TYPE_BOOLEAN, &val, DBUS_TYPE_INVALID);
}

static bool cdbus_append_int32(DBusMessage *msg, int32_t val) {
	return dbus_message_append_args(msg, DBUS_TYPE_INT32, &val, DBUS_TYPE_INVALID);
}

static bool cdbus_append_uint32(DBusMessage *msg, uint32_t val) {
	return dbus_message_append_args(msg, DBUS_TYPE_UINT32, &val, DBUS_TYPE_INVALID);
}

static bool cdbus_append_double(DBusMessage *msg, double val) {
	return dbus_message_append_args(msg, DBUS_TYPE_DOUBLE, &val, DBUS_TYPE_INVALID);
}

static bool cdbus_append_wid(DBusMessage *msg, xcb_window_t val_) {
	cdbus_window_t val = val_;
	return dbus_message_append_args(msg, CDBUS_TYPE_WINDOW, &val, DBUS_TYPE_INVALID);
}

static bool cdbus_append_enum(DBusMessage *msg, cdbus_enum_t val) {
	return dbus_message_append_args(msg, CDBUS_TYPE_ENUM, &val, DBUS_TYPE_INVALID);
}

static bool cdbus_append_string(DBusMessage *msg, const char *data) {
	const char *str = data ? data : "";
	return dbus_message_append_args(msg, DBUS_TYPE_STRING, &str, DBUS_TYPE_INVALID);
}

/// Append a window ID to a D-Bus message as a variant
static bool cdbus_append_wid_variant(DBusMessage *msg, xcb_window_t val_) {
	cdbus_window_t val = val_;

	DBusMessageIter it, it2;
	dbus_message_iter_init_append(msg, &it);
	if (!dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT,
	                                      CDBUS_TYPE_WINDOW_STR, &it2)) {
		return false;
	}
	if (!dbus_message_iter_append_basic(&it2, CDBUS_TYPE_WINDOW, &val)) {
		return false;
	}
	if (!dbus_message_iter_close_container(&it, &it2)) {
		return false;
	}

	return true;
}

/// Append a boolean to a D-Bus message as a variant
static bool cdbus_append_bool_variant(DBusMessage *msg, dbus_bool_t val) {
	DBusMessageIter it, it2;
	dbus_message_iter_init_append(msg, &it);
	if (!dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT,
	                                      DBUS_TYPE_BOOLEAN_AS_STRING, &it2)) {
		return false;
	}
	if (!dbus_message_iter_append_basic(&it2, DBUS_TYPE_BOOLEAN, &val)) {
		return false;
	}
	if (!dbus_message_iter_close_container(&it, &it2)) {
		return false;
	}

	return true;
}

/// Append a string to a D-Bus message as a variant
static bool cdbus_append_string_variant(DBusMessage *msg, const char *data) {
	const char *str = data ? data : "";

	DBusMessageIter it, it2;
	dbus_message_iter_init_append(msg, &it);
	if (!dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT,
	                                      DBUS_TYPE_STRING_AS_STRING, &it2)) {
		return false;
	}
	if (!dbus_message_iter_append_basic(&it2, DBUS_TYPE_STRING, &str)) {
		log_error("Failed to append argument.");
		return false;
	}
	if (!dbus_message_iter_close_container(&it, &it2)) {
		return false;
	}

	return true;
}

/// Append all window IDs in the window list of a session to a D-Bus message
static bool cdbus_append_wids(DBusMessage *msg, session_t *ps) {
	DBusMessageIter it, subit;
	dbus_message_iter_init_append(msg, &it);
	if (!dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY,
	                                      DBUS_TYPE_UINT32_AS_STRING, &subit)) {
		log_error("Failed to open container.");
		return false;
	}

	bool failed = false;
	wm_stack_foreach(ps->wm, cursor) {
		if (wm_ref_is_zombie(cursor)) {
			continue;
		}
		auto w = wm_ref_deref(cursor);
		if (w == NULL) {
			continue;
		}

		auto wid = win_id(w);
		if (!dbus_message_iter_append_basic(&subit, CDBUS_TYPE_WINDOW, &wid)) {
			failed = true;
			break;
		}
	}
	if (!dbus_message_iter_close_container(&it, &subit)) {
		log_error("Failed to close container.");
		return false;
	}
	if (failed) {
		log_error("Failed to append argument.");
		return false;
	}
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
static bool cdbus_msg_get_arg(DBusMessage *msg, int count, const int type, void *pdest) {
	assert(count >= 0);

	DBusMessageIter iter = {};
	if (!dbus_message_iter_init(msg, &iter)) {
		log_error("Message has no argument.");
		return false;
	}

	{
		const int oldcount = count;
		while (count) {
			if (!dbus_message_iter_next(&iter)) {
				log_error("Failed to find argument %d.", oldcount);
				return false;
			}
			--count;
		}
	}

	if (type != dbus_message_iter_get_arg_type(&iter)) {
		log_error("Argument has incorrect type.");
		return false;
	}

	dbus_message_iter_get_basic(&iter, pdest);

	return true;
}

/** @name Message processing
 */
///@{

/**
 * Process a list_win D-Bus request.
 */
static DBusHandlerResult
cdbus_process_list_win(session_t *ps, DBusMessage *msg attr_unused, DBusMessage *reply,
                       DBusError *err attr_unused) {
	if (!cdbus_append_wids(reply, ps)) {
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

/// Process a property Get D-Bus request.
static DBusHandlerResult
cdbus_process_window_property_get(session_t *ps, DBusMessage *msg, cdbus_window_t wid,
                                  DBusMessage *reply, DBusError *e) {
	const char *target = NULL;
	const char *interface = NULL;

	if (reply == NULL) {
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!dbus_message_get_args(msg, e, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING,
	                           &target, DBUS_TYPE_INVALID)) {
		log_debug("Failed to parse argument of \"Get\" (%s).", e->message);
		dbus_set_error_const(e, DBUS_ERROR_INVALID_ARGS, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (interface[0] != '\0' && strcmp(interface, PICOM_WINDOW_INTERFACE) != 0) {
		dbus_set_error_const(e, DBUS_ERROR_UNKNOWN_INTERFACE, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	auto cursor = wm_find(ps->wm, wid);
	if (cursor == NULL) {
		log_debug("Window %#010x not found.", wid);
		dbus_set_error(e, CDBUS_ERROR_BADWIN, CDBUS_ERROR_BADWIN_S, wid);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	auto w = wm_ref_deref(cursor);
	if (w == NULL) {
		log_debug("Window %#010x is not managed.", wid);
		dbus_set_error(e, CDBUS_ERROR_BADWIN, CDBUS_ERROR_BADWIN_S, wid);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

#define append(tgt, type, expr)                                                          \
	if (strcmp(#tgt, target) == 0) {                                                 \
		if (!cdbus_append_##type(reply, (expr))) {                               \
			return DBUS_HANDLER_RESULT_NEED_MEMORY;                          \
		}                                                                        \
		return DBUS_HANDLER_RESULT_HANDLED;                                      \
	}
#define append_win_property(name, member, type) append(name, type, w->member)

	append(Mapped, bool_variant, w->state == WSTATE_MAPPED);
	append(Id, wid_variant, win_id(w));
	append(RawFocused, bool_variant,
	       w->a.map_state == XCB_MAP_STATE_VIEWABLE && w->is_focused);
	append(ClientWin, wid_variant, win_client_id(w, /*fallback_to_self=*/true));
	append(Leader, wid_variant, wm_ref_win_id(wm_ref_leader(w->tree_ref)));
	append_win_property(Name, name, string_variant);

	if (strcmp("Type", target) == 0) {
		DBusMessageIter iter, sub;
		dbus_message_iter_init_append(reply, &iter);
		if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &sub)) {
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
		for (int i = 0; i < NUM_WINTYPES; i++) {
			if ((w->window_types & (1 << i)) == 0) {
				continue;
			}
			if (!dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING,
			                                    &WINTYPES[i].name)) {
				return DBUS_HANDLER_RESULT_NEED_MEMORY;
			}
		}
		if (!dbus_message_iter_close_container(&iter, &sub)) {
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (strcmp("Next", target) == 0) {
		cdbus_window_t next_id = 0;
		auto below = wm_ref_below(cursor);
		if (below != NULL) {
			next_id = wm_ref_win_id(below);
		}
		if (!cdbus_append_wid_variant(reply, next_id)) {
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

#undef append_win_property
#undef append

	log_debug(CDBUS_ERROR_BADTGT_S, target);
	dbus_set_error(e, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult cdbus_process_reset(session_t *ps, DBusMessage *msg attr_unused,
                                             DBusMessage *reply, DBusError *e attr_unused) {
	// Reset the compositor
	log_info("picom is resetting...");
	ev_break(ps->loop, EVBREAK_ALL);
	if (reply != NULL && !cdbus_append_boolean(reply, true)) {
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult cdbus_process_repaint(session_t *ps, DBusMessage *msg attr_unused,
                                               DBusMessage *reply, DBusError *e attr_unused) {
	// Reset the compositor
	force_repaint(ps);
	if (reply != NULL && !cdbus_append_boolean(reply, true)) {
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

static inline cdbus_enum_t tristate_to_switch(enum tristate val) {
	switch (val) {
	case TRI_FALSE: return OFF;
	case TRI_TRUE: return ON;
	default: return UNSET;
	}
}

/**
 * Process a win_get D-Bus request.
 */
static DBusHandlerResult
cdbus_process_win_get(session_t *ps, DBusMessage *msg, DBusMessage *reply, DBusError *err) {
	cdbus_window_t wid = XCB_NONE;
	const char *target = NULL;

	if (reply == NULL) {
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!dbus_message_get_args(msg, err, CDBUS_TYPE_WINDOW, &wid, DBUS_TYPE_STRING,
	                           &target, DBUS_TYPE_INVALID)) {
		log_debug("Failed to parse argument of \"win_get\" (%s).", err->message);
		dbus_set_error_const(err, DBUS_ERROR_INVALID_ARGS, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	auto cursor = wm_find(ps->wm, wid);
	if (cursor == NULL) {
		log_debug("Window %#010x not found.", wid);
		dbus_set_error(err, CDBUS_ERROR_BADWIN, CDBUS_ERROR_BADWIN_S, wid);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	auto w = wm_ref_deref(cursor);
	if (w == NULL) {
		log_debug("Window %#010x is not managed.", wid);
		dbus_set_error(err, CDBUS_ERROR_BADWIN, CDBUS_ERROR_BADWIN_S, wid);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

#define append(tgt, type, expr)                                                          \
	if (strcmp(#tgt, target) == 0) {                                                 \
		if (!cdbus_append_##type(reply, (expr))) {                               \
			return DBUS_HANDLER_RESULT_NEED_MEMORY;                          \
		}                                                                        \
		return DBUS_HANDLER_RESULT_HANDLED;                                      \
	}
#define append_win_property(tgt, type) append(tgt, type, w->tgt)

	if (strcmp("next", target) == 0) {
		auto below = wm_ref_below(cursor);
		xcb_window_t next_id = below ? wm_ref_win_id(below) : XCB_NONE;
		if (!cdbus_append_wid(reply, next_id)) {
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	auto w_opts = win_options(w);

	append(id, wid, win_id(w));
	append(client_win, wid, win_client_id(w, /*fallback_to_self=*/true));
	append(map_state, boolean, w->a.map_state);
	append(wmwin, boolean, win_is_wmwin(w));
	append(focused_raw, boolean, w->a.map_state == XCB_MAP_STATE_VIEWABLE && w->is_focused);
	append(left_width, int32, w->frame_extents.left);
	append(right_width, int32, w->frame_extents.right);
	append(top_width, int32, w->frame_extents.top);
	append(bottom_width, int32, w->frame_extents.bottom);
	append(fade_force, enum, tristate_to_switch(w->options_override.fade));
	append(shadow_force, enum, tristate_to_switch(w->options_override.shadow));
	append(invert_color_force, enum, tristate_to_switch(w->options_override.invert_color));
	append(opacity_is_set, boolean, !safe_isnan(w->options.opacity));
	append(shadow, boolean, w_opts.shadow);
	append(fade, boolean, w_opts.fade);
	append(blur_background, boolean, w_opts.blur_background);
	append(leader, wid, wm_ref_win_id(wm_ref_leader(w->tree_ref)));

	append_win_property(mode, enum);
	append_win_property(opacity, double);
	append_win_property(ever_damaged, boolean);
	append_win_property(window_types, enum);
	append_win_property(name, string);
	append_win_property(class_instance, string);
	append_win_property(class_general, string);
	append_win_property(role, string);
	append_win_property(opacity, double);
	append_win_property(has_opacity_prop, boolean);
	append_win_property(opacity_prop, uint32);
	append_win_property(opacity_set, double);
	append_win_property(frame_opacity, double);

#undef append_win_property
#undef append

	log_debug(CDBUS_ERROR_BADTGT_S, target);
	dbus_set_error(err, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);
	return DBUS_HANDLER_RESULT_HANDLED;
}

/**
 * Process a win_set D-Bus request.
 */
static DBusHandlerResult
cdbus_process_win_set(session_t *ps, DBusMessage *msg, DBusMessage *reply, DBusError *err) {
	cdbus_window_t wid = XCB_NONE;
	const char *target = NULL;

	if (!dbus_message_get_args(msg, err, CDBUS_TYPE_WINDOW, &wid, DBUS_TYPE_STRING,
	                           &target, DBUS_TYPE_INVALID)) {
		log_debug("Failed to parse argument of \"win_set\" (%s).", err->message);
		dbus_set_error_const(err, DBUS_ERROR_INVALID_ARGS, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	auto cursor = wm_find(ps->wm, wid);
	if (cursor == NULL) {
		log_debug("Window %#010x not found.", wid);
		dbus_set_error(err, CDBUS_ERROR_BADWIN, CDBUS_ERROR_BADWIN_S, wid);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	auto w = wm_ref_deref(cursor);
	if (w == NULL) {
		log_debug("Window %#010x is not managed.", wid);
		dbus_set_error(err, CDBUS_ERROR_BADWIN, CDBUS_ERROR_BADWIN_S, wid);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	cdbus_enum_t val = UNSET;
	if (!cdbus_msg_get_arg(msg, 2, CDBUS_TYPE_ENUM, &val)) {
		dbus_set_error_const(err, DBUS_ERROR_INVALID_ARGS, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	bool changed = false;
	if (strcmp("shadow_force", target) == 0) {
		w->options_override.shadow =
		    val == UNSET ? TRI_UNKNOWN : (val == ON ? TRI_TRUE : TRI_FALSE);
		changed = true;
	} else if (strcmp("fade_force", target) == 0) {
		w->options_override.fade =
		    val == UNSET ? TRI_UNKNOWN : (val == ON ? TRI_TRUE : TRI_FALSE);
		changed = true;
	} else if (strcmp("invert_color_force", target) == 0) {
		w->options_override.invert_color =
		    val == UNSET ? TRI_UNKNOWN : (val == ON ? TRI_TRUE : TRI_FALSE);
		changed = true;
	} else {
		log_debug(CDBUS_ERROR_BADTGT_S, target);
		dbus_set_error(err, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (changed) {
		add_damage_from_win(ps, w);
		queue_redraw(ps);
	}

	if (reply != NULL && !cdbus_append_boolean(reply, true)) {
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

/**
 * Process a find_win D-Bus request.
 */
static DBusHandlerResult
cdbus_process_find_win(session_t *ps, DBusMessage *msg, DBusMessage *reply, DBusError *err) {
	if (reply == NULL) {
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	const char *target = NULL;
	if (!cdbus_msg_get_arg(msg, 0, DBUS_TYPE_STRING, &target)) {
		dbus_set_error_const(err, DBUS_ERROR_INVALID_ARGS, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	xcb_window_t wid = XCB_NONE;

	if (strcmp("client", target) == 0) {
		// Find window by client window
		cdbus_window_t client = XCB_NONE;
		if (!cdbus_msg_get_arg(msg, 1, CDBUS_TYPE_WINDOW, &client)) {
			dbus_set_error_const(err, DBUS_ERROR_INVALID_ARGS, NULL);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		auto w = wm_find_by_client(ps->wm, client);
		if (w) {
			wid = wm_ref_win_id(w);
		}
	} else if (strcmp("focused", target) == 0) {
		// Find focused window
		auto focused_win = wm_focused_win(ps->wm);
		auto w = wm_ref_deref(focused_win);
		if (focused_win && w && w->state == WSTATE_MAPPED) {
			wid = wm_ref_win_id(focused_win);
		}
	} else {
		log_debug(CDBUS_ERROR_BADTGT_S, target);
		dbus_set_error(err, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (reply != NULL && !cdbus_append_wid(reply, wid)) {
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

/**
 * Process a opts_get D-Bus request.
 */
static DBusHandlerResult
cdbus_process_opts_get(session_t *ps, DBusMessage *msg, DBusMessage *reply, DBusError *err) {
	if (reply == NULL) {
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	const char *target = NULL;
	if (!cdbus_msg_get_arg(msg, 0, DBUS_TYPE_STRING, &target)) {
		dbus_set_error_const(err, DBUS_ERROR_INVALID_ARGS, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

#define append(tgt, type, ret)                                                           \
	if (strcmp(#tgt, target) == 0) {                                                 \
		if (reply != NULL && !cdbus_append_##type(reply, ret)) {                 \
			return DBUS_HANDLER_RESULT_NEED_MEMORY;                          \
		}                                                                        \
		return DBUS_HANDLER_RESULT_HANDLED;                                      \
	}
#define append_session_option(tgt, type) append(tgt, type, ps->o.tgt)

	if (strcmp("backend", target) == 0) {
		assert(!ps->o.use_legacy_backends ||
		       (size_t)ps->o.legacy_backend < ARR_SIZE(BACKEND_STRS));
		const char *name = ps->o.use_legacy_backends
		                       ? BACKEND_STRS[ps->o.legacy_backend]
		                       : backend_name(ps->o.backend);
		if (reply != NULL && !cdbus_append_string(reply, name)) {
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	append(version, string, PICOM_FULL_VERSION);
	append(pid, int32, getpid());
	append(display, string, DisplayString(ps->c.dpy));
	append(config_file, string, "Unknown");
	append(paint_on_overlay, boolean, ps->overlay != XCB_NONE);
	append(paint_on_overlay_id, uint32, ps->overlay);        // Sending ID of the X
	                                                         // composite overlay
	                                                         // window
	append(unredir_if_possible_delay, int32, (int32_t)ps->o.unredir_if_possible_delay);
	append(refresh_rate, int32, 0);
	append(sw_opti, boolean, false);
	append(backend, string, BACKEND_STRS[ps->o.legacy_backend]);

	append_session_option(unredir_if_possible, boolean);
	append_session_option(write_pid_path, string);
	append_session_option(mark_wmwin_focused, boolean);
	append_session_option(mark_ovredir_focused, boolean);
	append_session_option(detect_rounded_corners, boolean);
	append_session_option(redirected_force, enum);
	append_session_option(stoppaint_force, enum);
	append_session_option(logpath, string);
	append_session_option(vsync, boolean);
	append_session_option(shadow_red, double);
	append_session_option(shadow_green, double);
	append_session_option(shadow_blue, double);
	append_session_option(shadow_radius, int32);
	append_session_option(shadow_offset_x, int32);
	append_session_option(shadow_offset_y, int32);
	append_session_option(shadow_opacity, double);
	append_session_option(crop_shadow_to_monitor, boolean);

	append_session_option(fade_delta, int32);
	append_session_option(fade_in_step, double);
	append_session_option(fade_out_step, double);
	append_session_option(no_fading_openclose, boolean);

	append_session_option(blur_method, boolean);
	append_session_option(blur_background_frame, boolean);
	append_session_option(blur_background_fixed, boolean);

	append_session_option(inactive_dim, double);
	append_session_option(inactive_dim_fixed, boolean);

	append_session_option(max_brightness, double);

	append_session_option(use_ewmh_active_win, boolean);
	append_session_option(detect_transient, boolean);
	append_session_option(detect_client_leader, boolean);
	append_session_option(use_damage, boolean);

#ifdef CONFIG_OPENGL
	append_session_option(glx_no_stencil, boolean);
	append_session_option(glx_no_rebind_pixmap, boolean);
#endif

#undef append_session_option
#undef append

	log_debug(CDBUS_ERROR_BADTGT_S, target);
	dbus_set_error(err, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);

	return DBUS_HANDLER_RESULT_HANDLED;
}

/**
 * Process a opts_set D-Bus request.
 */
static DBusHandlerResult
cdbus_process_opts_set(session_t *ps, DBusMessage *msg, DBusMessage *reply, DBusError *err) {
	const char *target = NULL;

	if (!cdbus_msg_get_arg(msg, 0, DBUS_TYPE_STRING, &target)) {
		log_error("Failed to parse argument of \"opts_set\" (%s).", err->message);
		dbus_set_error_const(err, DBUS_ERROR_INVALID_ARGS, NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

#define get_msg_arg(type, val)                                                           \
	if (!cdbus_msg_get_arg(msg, 1, DBUS_TYPE_##type, &(val))) {                      \
		dbus_set_error_const(err, DBUS_ERROR_INVALID_ARGS, NULL);                \
		return DBUS_HANDLER_RESULT_HANDLED;                                      \
	};
#define opts_set_do(tgt, dbus_type, type, expr)                                          \
	if (strcmp(#tgt, target) == 0) {                                                 \
		type val;                                                                \
		get_msg_arg(dbus_type, val);                                             \
		ps->o.tgt = expr;                                                        \
		goto cdbus_process_opts_set_success;                                     \
	}

	if (strcmp("clear_shadow", target) == 0 || strcmp("track_focus", target) == 0) {
		goto cdbus_process_opts_set_success;
	}

	opts_set_do(fade_delta, INT32, int32_t, max2(val, 1));
	opts_set_do(fade_in_step, DOUBLE, double, normalize_d(val));
	opts_set_do(fade_out_step, DOUBLE, double, normalize_d(val));
	opts_set_do(no_fading_openclose, BOOLEAN, bool, val);
	opts_set_do(stoppaint_force, UINT32, cdbus_enum_t, val);

	if (strcmp("unredir_if_possible", target) == 0) {
		dbus_bool_t val = FALSE;
		get_msg_arg(BOOLEAN, val);
		if (ps->o.unredir_if_possible != val) {
			ps->o.unredir_if_possible = val;
			queue_redraw(ps);
		}
		goto cdbus_process_opts_set_success;
	}

	if (strcmp("redirected_force", target) == 0) {
		cdbus_enum_t val = UNSET;
		get_msg_arg(UINT32, val);
		if (ps->o.redirected_force != val) {
			ps->o.redirected_force = val;
			force_repaint(ps);
		}
		goto cdbus_process_opts_set_success;
	}
#undef get_msg_arg

	log_error(CDBUS_ERROR_BADTGT_S, target);
	dbus_set_error(err, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S, target);
	return DBUS_HANDLER_RESULT_HANDLED;

cdbus_process_opts_set_success:
	if (reply != NULL && !cdbus_append_boolean(reply, true)) {
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

/**
 * Process an Introspect D-Bus request.
 */
static DBusHandlerResult cdbus_process_introspect(DBusMessage *reply) {
	static const char *str_introspect =
	    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection "
	    "1.0//EN\"\n"
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
	    "    <method name='list_win'>\n"
	    "      <arg name='wids' type='au' direction='out' />\n"
	    "    </method>\n"
	    "  </interface>\n"
	    "  <interface name='" PICOM_COMPOSITOR_INTERFACE "'>\n"
	    "    <signal name='WinAdded'>\n"
	    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
	    "    </signal>\n"
	    "    <signal name='WinDestroyed'>\n"
	    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
	    "    </signal>\n"
	    "    <signal name='WinMapped'>\n"
	    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
	    "    </signal>\n"
	    "    <signal name='WinUnmapped'>\n"
	    "      <arg name='wid' type='" CDBUS_TYPE_WINDOW_STR "'/>\n"
	    "    </signal>\n"
	    "  </interface>\n"
	    "  <node name='windows' />\n"
	    "</node>\n";

	if (reply != NULL && !cdbus_append_string(reply, str_introspect)) {
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}
///@}

/// Process an D-Bus Introspect request, for /windows.
static DBusHandlerResult
cdbus_process_windows_root_introspect(session_t *ps, DBusMessage *reply) {
	static const char *str_introspect =
	    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection "
	    "1.0//EN\"\n"
	    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
	    "<node>\n"
	    "  <interface name='org.freedesktop.DBus.Introspectable'>\n"
	    "    <method name='Introspect'>\n"
	    "      <arg name='data' direction='out' type='s' />\n"
	    "    </method>\n"
	    "  </interface>\n";

	if (reply == NULL) {
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	scoped_charp introspect = NULL;
	mstrextend(&introspect, str_introspect);
	wm_stack_foreach(ps->wm, cursor) {
		auto w = wm_ref_deref(cursor);
		if (w == NULL) {
			continue;
		}
		char *tmp = NULL;
		if (asprintf(&tmp, "<node name='%#010x'/>\n", win_id(w)) < 0) {
			log_fatal("Failed to allocate memory.");
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
		mstrextend(&introspect, tmp);
		free(tmp);
	}
	mstrextend(&introspect, "</node>");
	if (!cdbus_append_string(reply, introspect)) {
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

static bool cdbus_process_window_introspect(DBusMessage *reply) {
	// clang-format off
	static const char *str_introspect =
	    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection "
	    "1.0//EN\"\n"
	    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
	    "<node>\n"
	    "  <interface name='org.freedesktop.DBus.Introspectable'>\n"
	    "    <method name='Introspect'>\n"
	    "      <arg name='data' direction='out' type='s' />\n"
	    "    </method>\n"
	    "  </interface>\n"
	    "  <interface name='org.freedesktop.DBus.Properties'>\n"
	    "    <method name='Get'>\n"
	    "      <arg type='s' name='interface_name' direction='in'/>\n"
	    "      <arg type='s' name='property_name' direction='in'/>\n"
	    "      <arg type='v' name='value' direction='out'/>\n"
	    "    </method>\n"
	    "    <method name='GetAll'>\n"
	    "      <arg type='s' name='interface_name' direction='in'/>\n"
	    "      <arg type='a{sv}' name='properties' direction='out'/>\n"
	    "    </method>\n"
	    "    <method name='Set'>\n"
	    "      <arg type='s' name='interface_name' direction='in'/>\n"
	    "      <arg type='s' name='property_name' direction='in'/>\n"
	    "      <arg type='v' name='value' direction='in'/>\n"
	    "    </method>\n"
	    "    <signal name='PropertiesChanged'>\n"
	    "      <arg type='s' name='interface_name'/>\n"
	    "      <arg type='a{sv}' name='changed_properties'/>\n"
	    "      <arg type='as' name='invalidated_properties'/>\n"
	    "    </signal>\n"
	    "  </interface>\n"
	    "  <interface name='" PICOM_WINDOW_INTERFACE "'>\n"
	    "    <property type='" CDBUS_TYPE_WINDOW_STR "' name='Leader' access='read'/>\n"
	    "    <property type='" CDBUS_TYPE_WINDOW_STR "' name='ClientWin' access='read'/>\n"
	    "    <property type='" CDBUS_TYPE_WINDOW_STR "' name='Id' access='read'/>\n"
	    "    <property type='" CDBUS_TYPE_WINDOW_STR "' name='Next' access='read'/>\n"
	    "    <property type='b' name='RawFocused' access='read'/>\n"
	    "    <property type='b' name='Mapped' access='read'/>\n"
	    "    <property type='s' name='Name' access='read'/>\n"
	    "    <property type='as' name='Type' access='read'/>\n"
	    "    <method name='BlockUnblockAnimation'>\n"
	    "      <arg type='s' name='trigger' direction='in'/>\n"
	    "      <arg type='b' name='block' direction='in'/>\n"
	    "      <arg type='u' name='count' direction='out'/>\n"
	    "    </method>\n"
	    "  </interface>\n"
	    "</node>\n";
	// clang-format on
	if (reply != NULL && !cdbus_append_string(reply, str_introspect)) {
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

/// Send a reply or an error message for request `msg`, appropriately based on the
/// `result` and whether `err` is set. Frees the error message and the reply message, and
/// flushes the connection.
static inline DBusHandlerResult
cdbus_send_reply_or_error(DBusConnection *conn, DBusHandlerResult result,
                          DBusMessage *msg, DBusMessage *reply, DBusError *err) {
	if (dbus_error_is_set(err) && reply != NULL) {
		// If error is set, send the error instead of the reply
		dbus_message_unref(reply);
		reply = dbus_message_new_error(msg, err->name, err->message);
		if (reply == NULL) {
			result = DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
	}
	if (result != DBUS_HANDLER_RESULT_HANDLED && reply != NULL) {
		// We shouldn't send a reply if we didn't handle this message
		dbus_message_unref(reply);
		reply = NULL;
	}
	if (reply != NULL) {
		if (!dbus_connection_send(conn, reply, NULL)) {
			result = DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
		dbus_message_unref(reply);
	}
	dbus_error_free(err);
	dbus_connection_flush(conn);
	return result;
}

/**
 * Process a message from D-Bus.
 */
static DBusHandlerResult cdbus_process(DBusConnection *conn, DBusMessage *msg, void *ud) {
	session_t *ps = ud;

	if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameAcquired") ||
	    dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameLost")) {
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_ERROR) {
		log_debug("Error message of path \"%s\" "
		          "interface \"%s\", member \"%s\", error \"%s\"",
		          dbus_message_get_path(msg), dbus_message_get_interface(msg),
		          dbus_message_get_member(msg), dbus_message_get_error_name(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
		log_debug("Illegal message of type \"%s\", path \"%s\" "
		          "interface \"%s\", member \"%s\"",
		          cdbus_repr_msgtype(msg), dbus_message_get_path(msg),
		          dbus_message_get_interface(msg), dbus_message_get_member(msg));
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	DBusMessage *reply = NULL;
	DBusError err;
	DBusHandlerResult ret = DBUS_HANDLER_RESULT_HANDLED;
	const char *interface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);

	dbus_error_init(&err);
	if (!dbus_message_get_no_reply(msg)) {
		reply = dbus_message_new_method_return(msg);
		if (reply == NULL) {
			log_error("Failed to create reply message.");
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
	}

	if (dbus_message_is_method_call(msg, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
		ret = cdbus_process_introspect(reply);
	} else if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PEER, "Ping")) {
		// Intentionally left blank
	} else if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PEER, "GetMachineId")) {
		if (reply != NULL) {
			char *uuid = dbus_get_local_machine_id();
			if (uuid) {
				if (!cdbus_append_string(reply, uuid)) {
					ret = DBUS_HANDLER_RESULT_NEED_MEMORY;
				}
				dbus_free(uuid);
			} else {
				ret = DBUS_HANDLER_RESULT_NEED_MEMORY;
			}
		}
	} else if (strcmp(interface, CDBUS_INTERFACE_NAME) != 0) {
		dbus_set_error_const(&err, DBUS_ERROR_UNKNOWN_INTERFACE, NULL);
	} else {
		static const struct {
			const char *name;
			DBusHandlerResult (*func)(session_t *ps, DBusMessage *msg,
			                          DBusMessage *reply, DBusError *err);
		} handlers[] = {
		    {"reset", cdbus_process_reset},
		    {"repaint", cdbus_process_repaint},
		    {"list_win", cdbus_process_list_win},
		    {"win_get", cdbus_process_win_get},
		    {"win_set", cdbus_process_win_set},
		    {"find_win", cdbus_process_find_win},
		    {"opts_get", cdbus_process_opts_get},
		    {"opts_set", cdbus_process_opts_set},
		};

		size_t i;
		for (i = 0; i < ARR_SIZE(handlers); i++) {
			if (strcmp(handlers[i].name, member) == 0) {
				ret = handlers[i].func(ps, msg, reply, &err);
				break;
			}
		}
		if (i >= ARR_SIZE(handlers)) {
			log_debug("Unknown method \"%s\".", member);
			dbus_set_error_const(&err, CDBUS_ERROR_BADMSG, CDBUS_ERROR_BADMSG_S);
		}
	}

	return cdbus_send_reply_or_error(conn, ret, msg, reply, &err);
}

/**
 * Process a message from D-Bus, for /windows path.
 */
static DBusHandlerResult
cdbus_process_windows(DBusConnection *conn, DBusMessage *msg, void *ud) {
	session_t *ps = ud;
	DBusHandlerResult ret = DBUS_HANDLER_RESULT_HANDLED;
	const char *path = dbus_message_get_path(msg);
	const char *last_segment = strrchr(path, '/');
	DBusError err;

	dbus_error_init(&err);

	if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameAcquired") ||
	    dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameLost")) {
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_ERROR) {
		log_debug("Error message of path \"%s\" "
		          "interface \"%s\", member \"%s\", error \"%s\"",
		          dbus_message_get_path(msg), dbus_message_get_interface(msg),
		          dbus_message_get_member(msg), dbus_message_get_error_name(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
		log_debug("Illegal message of type \"%s\", path \"%s\" "
		          "interface \"%s\", member \"%s\"",
		          cdbus_repr_msgtype(msg), dbus_message_get_path(msg),
		          dbus_message_get_interface(msg), dbus_message_get_member(msg));
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	DBusMessage *reply = NULL;
	const char *interface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);
	if (!dbus_message_get_no_reply(msg)) {
		reply = dbus_message_new_method_return(msg);
		if (reply == NULL) {
			log_error("Failed to create reply message.");
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
	}

	if (last_segment == NULL) {
		dbus_set_error_const(&err, CDBUS_ERROR_BADMSG, CDBUS_ERROR_BADMSG_S);
		goto finished;
	}

	bool is_root = strncmp(last_segment, "/windows", 8) == 0;
	if (is_root) {
		if (strcmp(interface, DBUS_INTERFACE_INTROSPECTABLE) == 0 &&
		    strcmp(member, "Introspect") == 0) {
			ret = cdbus_process_windows_root_introspect(ps, reply);
		} else {
			log_debug("Illegal message of type \"%s\", path \"%s\" "
			          "interface \"%s\", member \"%s\"",
			          cdbus_repr_msgtype(msg), dbus_message_get_path(msg),
			          dbus_message_get_interface(msg),
			          dbus_message_get_member(msg));
			dbus_set_error_const(&err, CDBUS_ERROR_BADMSG, CDBUS_ERROR_BADMSG_S);
		}
		goto finished;
	}

	char *endptr = NULL;
	auto wid = (cdbus_window_t)strtol(last_segment + 1, &endptr, 0);
	if (*endptr != '\0') {
		log_error("Invalid window ID string \"%s\".", last_segment + 1);
		dbus_set_error_const(&err, DBUS_ERROR_INVALID_ARGS, NULL);
	} else if (strcmp(interface, DBUS_INTERFACE_INTROSPECTABLE) == 0 &&
	           strcmp(member, "Introspect") == 0) {
		ret = cdbus_process_window_introspect(reply);
	} else if (strcmp(interface, DBUS_INTERFACE_PROPERTIES) == 0) {
		if (strcmp(member, "GetAll") == 0 || strcmp(member, "Set") == 0) {
			dbus_set_error_const(&err, DBUS_ERROR_NOT_SUPPORTED, NULL);
		} else if (strcmp(member, "Get") == 0) {
			ret = cdbus_process_window_property_get(ps, msg, wid, reply, &err);
		} else {
			log_debug(
			    "Unexpected member \"%s\" of dbus properties interface.", member);
			dbus_set_error_const(&err, DBUS_ERROR_UNKNOWN_METHOD, NULL);
		}
	} else if (strcmp(interface, PICOM_WINDOW_INTERFACE) == 0 &&
	           strcmp(member, "BlockUnblockAnimation") == 0) {
		bool block = false;
		const char *trigger_str = NULL;
		if (!cdbus_msg_get_arg(msg, 0, DBUS_TYPE_STRING, &trigger_str) ||
		    !cdbus_msg_get_arg(msg, 1, DBUS_TYPE_BOOLEAN, &block)) {
			dbus_set_error_const(&err, DBUS_ERROR_INVALID_ARGS, NULL);
			goto finished;
		}
		auto trigger = parse_animation_trigger(trigger_str);
		if (trigger == ANIMATION_TRIGGER_INVALID) {
			dbus_set_error(&err, CDBUS_ERROR_BADTGT, CDBUS_ERROR_BADTGT_S,
			               trigger_str);
			goto finished;
		}
		auto cursor = wm_find(ps->wm, wid);
		if (cursor == NULL) {
			dbus_set_error(&err, CDBUS_ERROR_BADWIN, CDBUS_ERROR_BADWIN_S, wid);
			goto finished;
		}
		auto w = wm_ref_deref(cursor);
		unsigned count = 0;
		if (w != NULL) {
			if (block) {
				w->animation_block[trigger] += 1;
			} else if (w->animation_block[trigger] > 0) {
				w->animation_block[trigger] -= 1;
			}
			count = w->animation_block[trigger];
		}
		if (reply != NULL && !cdbus_append_uint32(reply, count)) {
			ret = DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
	} else {
		log_debug("Illegal message of type \"%s\", path \"%s\" "
		          "interface \"%s\", member \"%s\"",
		          cdbus_repr_msgtype(msg), dbus_message_get_path(msg),
		          dbus_message_get_interface(msg), dbus_message_get_member(msg));
		dbus_set_error_const(&err, CDBUS_ERROR_BADMSG, CDBUS_ERROR_BADMSG_S);
	}

finished:
	return cdbus_send_reply_or_error(conn, ret, msg, reply, &err);
}

/**
 * Send a signal with a Window ID as argument.
 *
 * @param ps current session
 * @param name signal name
 * @param wid window ID
 */
static bool cdbus_signal_wid(struct cdbus_data *cd, const char *interface,
                             const char *name, xcb_window_t wid) {
	DBusMessage *msg = dbus_message_new_signal(CDBUS_OBJECT_NAME, interface, name);
	if (!msg) {
		log_error("Failed to create D-Bus signal.");
		return false;
	}

	if (!cdbus_append_wid(msg, wid)) {
		dbus_message_unref(msg);
		return false;
	}

	if (!dbus_connection_send(cd->dbus_conn, msg, NULL)) {
		log_error("Failed to send D-Bus signal.");
		dbus_message_unref(msg);
		return false;
	}

	dbus_connection_flush(cd->dbus_conn);
	dbus_message_unref(msg);
	return true;
}

/** @name Core callbacks
 */
///@{
void cdbus_ev_win_added(struct cdbus_data *cd, struct win *w) {
	if (cd->dbus_conn) {
		cdbus_signal_wid(cd, CDBUS_INTERFACE_NAME, "win_added", win_id(w));
		cdbus_signal_wid(cd, PICOM_COMPOSITOR_INTERFACE, "WinAdded", win_id(w));
	}
}

void cdbus_ev_win_destroyed(struct cdbus_data *cd, struct win *w) {
	if (cd->dbus_conn) {
		cdbus_signal_wid(cd, CDBUS_INTERFACE_NAME, "win_destroyed", win_id(w));
		cdbus_signal_wid(cd, PICOM_COMPOSITOR_INTERFACE, "WinDestroyed", win_id(w));
	}
}

void cdbus_ev_win_mapped(struct cdbus_data *cd, struct win *w) {
	if (cd->dbus_conn) {
		cdbus_signal_wid(cd, CDBUS_INTERFACE_NAME, "win_mapped", win_id(w));
		cdbus_signal_wid(cd, PICOM_COMPOSITOR_INTERFACE, "WinMapped", win_id(w));
	}
}

void cdbus_ev_win_unmapped(struct cdbus_data *cd, struct win *w) {
	if (cd->dbus_conn) {
		cdbus_signal_wid(cd, CDBUS_INTERFACE_NAME, "win_unmapped", win_id(w));
		cdbus_signal_wid(cd, PICOM_COMPOSITOR_INTERFACE, "WinUnmapped", win_id(w));
	}
}

void cdbus_ev_win_focusout(struct cdbus_data *cd, struct win *w) {
	if (cd->dbus_conn) {
		cdbus_signal_wid(cd, CDBUS_INTERFACE_NAME, "win_focusout", win_id(w));
	}
}

void cdbus_ev_win_focusin(struct cdbus_data *cd, struct win *w) {
	if (cd->dbus_conn) {
		cdbus_signal_wid(cd, CDBUS_INTERFACE_NAME, "win_focusin", win_id(w));
	}
}
//!@}
