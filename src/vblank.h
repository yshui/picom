// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <xcb/present.h>
#include <xcb/xcb.h>

#include <ev.h>
#include <xcb/xproto.h>

#include "config.h"
#include "x.h"

/// An object that schedule vblank events.
struct vblank_scheduler;

struct vblank_event {
	uint64_t msc;
	uint64_t ust;
};

enum vblank_callback_action {
	/// The callback should be called again in the next vblank.
	VBLANK_CALLBACK_AGAIN,
	/// The callback is done and should not be called again.
	VBLANK_CALLBACK_DONE,
};

typedef enum vblank_callback_action (*vblank_callback_t)(struct vblank_event *event,
                                                         void *user_data);

/// Schedule a vblank event.
///
/// Schedule for `cb` to be called when the current vblank ends. If this is called
/// from a callback function for the current vblank, the newly scheduled callback
/// will be called in the next vblank.
///
/// Returns whether the scheduling is successful. Scheduling can fail if there
/// is not enough memory.
bool vblank_scheduler_schedule(struct vblank_scheduler *self, vblank_callback_t cb,
                               void *user_data);
struct vblank_scheduler *
vblank_scheduler_new(struct ev_loop *loop, struct x_connection *c, xcb_window_t target_window,
                     enum vblank_scheduler_type type, bool use_realtime_scheduling);
void vblank_scheduler_free(struct vblank_scheduler *);

bool vblank_handle_x_events(struct vblank_scheduler *self);
