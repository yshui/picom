// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2009 Lennart Poettering
// Copyright 2010 David Henningsson <diwic@ubuntu.com>
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

// Imported from https://github.com/heftig/rtkit/blob/master/rtkit.c

#include <errno.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__NetBSD__)
#include <lwp.h>
#elif defined(__FreeBSD__)
#include <sys/thr.h>
#elif defined(__DragonFly__)
#include <sys/lwp.h>
#endif

#include "log.h"
#include "rtkit.h"
#include "utils/misc.h"

#define RTKIT_SERVICE_NAME "org.freedesktop.RealtimeKit1"
#define RTKIT_OBJECT_PATH "/org/freedesktop/RealtimeKit1"
#define RTKIT_INTERFACE "org.freedesktop.RealtimeKit1"

static inline long compat_gettid(void) {
	long ret = -1;
#if defined(__linux__)
	ret = (pid_t)syscall(SYS_gettid);
#elif defined(__NetBSD__)
	ret = _lwp_self();
#elif defined(__FreeBSD__)
	long lwpid;
	thr_self(&lwpid);
	ret = lwpid;
#elif defined(__DragonFly__)
	ret = lwp_gettid();
#endif
	return ret;
}

static bool
rtkit_get_int_property(DBusConnection *connection, const char *propname, long long *propval) {
	DBusMessage *m = NULL, *r = NULL;
	DBusMessageIter iter, subiter;
	dbus_int64_t i64;
	dbus_int32_t i32;
	DBusError error;
	int current_type;
	int ret = 0;
	const char *interfacestr = RTKIT_INTERFACE;

	dbus_error_init(&error);

	m = dbus_message_new_method_call(RTKIT_SERVICE_NAME, RTKIT_OBJECT_PATH,
	                                 "org.freedesktop.DBus.Properties", "Get");
	if (!m) {
		ret = -ENOMEM;
		goto finish;
	}

	if (!dbus_message_append_args(m, DBUS_TYPE_STRING, &interfacestr,
	                              DBUS_TYPE_STRING, &propname, DBUS_TYPE_INVALID)) {
		ret = -ENOMEM;
		goto finish;
	}

	r = dbus_connection_send_with_reply_and_block(connection, m, -1, &error);
	if (!r) {
		goto finish;
	}

	if (dbus_set_error_from_message(&error, r)) {
		goto finish;
	}

	ret = -EBADMSG;
	dbus_message_iter_init(r, &iter);
	while ((current_type = dbus_message_iter_get_arg_type(&iter)) != DBUS_TYPE_INVALID) {
		if (current_type == DBUS_TYPE_VARIANT) {
			dbus_message_iter_recurse(&iter, &subiter);

			while ((current_type = dbus_message_iter_get_arg_type(&subiter)) !=
			       DBUS_TYPE_INVALID) {

				if (current_type == DBUS_TYPE_INT32) {
					dbus_message_iter_get_basic(&subiter, &i32);
					*propval = i32;
					ret = 0;
				}

				if (current_type == DBUS_TYPE_INT64) {
					dbus_message_iter_get_basic(&subiter, &i64);
					*propval = i64;
					ret = 0;
				}

				dbus_message_iter_next(&subiter);
			}
		}
		dbus_message_iter_next(&iter);
	}

finish:
	if (m) {
		dbus_message_unref(m);
	}
	if (r) {
		dbus_message_unref(r);
	}
	if (dbus_error_is_set(&error)) {
		log_debug("Couldn't get property %s from rtkit: (dbus) %s", propname,
		          error.message);
		dbus_error_free(&error);
		return false;
	}
	if (ret != 0) {
		log_debug("Couldn't get property %s from rtkit: %s", propname, strerror(-ret));
		return false;
	}

	return true;
}

static bool rtkit_get_rttime_usec_max(DBusConnection *connection, long long *retval) {
	return rtkit_get_int_property(connection, "RTTimeUSecMax", retval);
}

static inline void free_dbus_connection(DBusConnection **connection) {
	if (*connection) {
		dbus_connection_close(*connection);
		dbus_connection_unref(*connection);
		*connection = NULL;
	}
}

static inline void free_dbus_message(DBusMessage **message) {
	if (*message) {
		dbus_message_unref(*message);
		*message = NULL;
	}
}

bool rtkit_make_realtime(long thread, int priority) {
	cleanup(free_dbus_message) DBusMessage *m = NULL;
	cleanup(free_dbus_message) DBusMessage *r = NULL;
	dbus_uint64_t u64;
	dbus_uint32_t u32;
	DBusError error;
	int ret = 0;
	long long rttime_usec_max = 0;
	bool succeeded = true;

	dbus_error_init(&error);

	cleanup(free_dbus_connection) DBusConnection *connection =
	    dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
	if (dbus_error_is_set(&error)) {
		log_info("Couldn't get system bus: %s", error.message);
		dbus_error_free(&error);
		return false;
	}
	dbus_connection_set_exit_on_disconnect(connection, false);

	if (thread == 0) {
		thread = compat_gettid();
	}

	if (!rtkit_get_rttime_usec_max(connection, &rttime_usec_max)) {
		log_debug("Couldn't get RTTimeUSecMax from rtkit.");
		return false;
	}
	if (rttime_usec_max <= 0) {
		log_debug("Unreasonable RTTimeUSecMax from rtkit: %lld", rttime_usec_max);
		return false;
	}

#if defined(RLIMIT_RTTIME)
	struct rlimit old_rlim, new_rlim;
	// For security reasons, rtkit requires us to set RLIMIT_RTTIME before it will
	// give us realtime priority.
	if (getrlimit(RLIMIT_RTTIME, &old_rlim) != 0) {
		log_debug("Couldn't get RLIMIT_RTTIME.");
		return false;
	}
	new_rlim = old_rlim;
	new_rlim.rlim_cur = min3(new_rlim.rlim_max, (rlim_t)rttime_usec_max, 100000);        // 100ms
	new_rlim.rlim_max = new_rlim.rlim_cur;
	if (setrlimit(RLIMIT_RTTIME, &new_rlim) != 0) {
		log_debug("Couldn't set RLIMIT_RTTIME.");
		return false;
	}
#endif

	m = dbus_message_new_method_call(RTKIT_SERVICE_NAME, RTKIT_OBJECT_PATH,
	                                 RTKIT_INTERFACE, "MakeThreadRealtime");
	if (!m) {
		ret = -ENOMEM;
		goto finish;
	}

	u64 = (dbus_uint64_t)thread;
	u32 = (dbus_uint32_t)priority;

	if (!dbus_message_append_args(m, DBUS_TYPE_UINT64, &u64, DBUS_TYPE_UINT32, &u32,
	                              DBUS_TYPE_INVALID)) {
		ret = -ENOMEM;
		goto finish;
	}

	r = dbus_connection_send_with_reply_and_block(connection, m, -1, &error);
	if (!r) {
		goto finish;
	}

	if (dbus_set_error_from_message(&error, r)) {
		goto finish;
	}

	ret = 0;

finish:
	if (dbus_error_is_set(&error)) {
		log_info("Couldn't make thread realtime with rtkit: (dbus) %s", error.message);
		dbus_error_free(&error);
		succeeded = false;
	} else if (ret != 0) {
		log_info("Couldn't make thread realtime with rtkit: %s", strerror(-ret));
		succeeded = false;
	}
#if defined(RLIMIT_RTTIME)
	if (!succeeded) {
		// Restore RLIMIT_RTTIME
		setrlimit(RLIMIT_RTTIME, &old_rlim);
	}
#endif

	return succeeded;
}
