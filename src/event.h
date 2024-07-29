// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2019, Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <xcb/xcb.h>

#include "common.h"

void ev_handle(session_t *ps, xcb_generic_event_t *ev);
void ev_update_focused(struct session *ps);
