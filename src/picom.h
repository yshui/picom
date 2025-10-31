// SPDX-License-Identifier: MIT
// Copyright (c) 2011-2013, Christopher Jeffrey
// Copyright (c) 2018 Yuxuan Shui <yshuiv7@gmail.com>

// Throw everything in here.
// !!! DON'T !!!

// === Includes ===

#include <X11/Xutil.h>
#include <locale.h>
#include <stdbool.h>
#include <stdlib.h>
#include <xcb/xproto.h>

#include <picom/types.h>

#include "c2.h"
#include "common.h"
#include "config.h"
#include "log.h"        // XXX clean up
#include "wm/win.h"
#include "x.h"

// == Functions ==
// TODO(yshui) move static inline functions that are only used in picom.c, into picom.c

void root_damaged(session_t *ps);

void queue_redraw(session_t *ps);

void configure_root(session_t *ps);

void quit(session_t *ps);

xcb_window_t session_get_target_window(session_t *);

uint8_t session_redirection_mode(session_t *ps);

#ifdef CONFIG_DBUS
struct cdbus_data *session_get_cdbus(struct session *);
#else
static inline struct cdbus_data *session_get_cdbus(session_t *ps attr_unused) {
	return NULL;
}
#endif
