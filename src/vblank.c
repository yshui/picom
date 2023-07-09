#include <assert.h>

#include <ev.h>
#include <inttypes.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include "compiler.h"
#include "config.h"
#include "list.h"        // for container_of
#include "log.h"
#include "vblank.h"
#include "x.h"

struct vblank_closure {
	vblank_callback_t fn;
	void *user_data;
};

struct vblank_scheduler {
	struct x_connection *c;
	size_t callback_capacity, callback_count;
	struct vblank_closure *callbacks;
	struct ev_loop *loop;
	xcb_window_t target_window;
	enum vblank_scheduler_type type;
	bool vblank_event_requested;
};

struct present_vblank_scheduler {
	struct vblank_scheduler base;

	uint64_t last_msc;
	/// The timestamp for the end of last vblank.
	uint64_t last_ust;
	ev_timer callback_timer;
	xcb_present_event_t event_id;
	xcb_special_event_t *event;
};

struct vblank_scheduler_ops {
	void (*init)(struct vblank_scheduler *self);
	void (*deinit)(struct vblank_scheduler *self);
	void (*schedule)(struct vblank_scheduler *self);
	bool (*handle_x_events)(struct vblank_scheduler *self);
};

static void
vblank_scheduler_invoke_callbacks(struct vblank_scheduler *self, struct vblank_event *event);

static void present_vblank_scheduler_schedule(struct vblank_scheduler *base) {
	auto self = (struct present_vblank_scheduler *)base;
	log_verbose("Requesting vblank event for window 0x%08x, msc %" PRIu64,
	            base->target_window, self->last_msc + 1);
	assert(!base->vblank_event_requested);
	x_request_vblank_event(base->c, base->target_window, self->last_msc + 1);
	base->vblank_event_requested = true;
}

static void present_vblank_callback(EV_P attr_unused, ev_timer *w, int attr_unused revents) {
	auto sched = container_of(w, struct present_vblank_scheduler, callback_timer);
	auto event = (struct vblank_event){
	    .msc = sched->last_msc,
	    .ust = sched->last_ust,
	};
	sched->base.vblank_event_requested = false;
	vblank_scheduler_invoke_callbacks(&sched->base, &event);
}

static void present_vblank_scheduler_init(struct vblank_scheduler *base) {
	auto self = (struct present_vblank_scheduler *)base;
	base->type = VBLANK_SCHEDULER_PRESENT;
	ev_timer_init(&self->callback_timer, present_vblank_callback, 0, 0);

	self->event_id = x_new_id(base->c);
	auto select_input =
	    xcb_present_select_input(base->c->c, self->event_id, base->target_window,
	                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
	set_cant_fail_cookie(base->c, select_input);
	self->event =
	    xcb_register_for_special_xge(base->c->c, &xcb_present_id, self->event_id, NULL);
}

static void present_vblank_scheduler_deinit(struct vblank_scheduler *base) {
	auto self = (struct present_vblank_scheduler *)base;
	ev_timer_stop(base->loop, &self->callback_timer);
	auto select_input =
	    xcb_present_select_input(base->c->c, self->event_id, base->target_window, 0);
	set_cant_fail_cookie(base->c, select_input);
	xcb_unregister_for_special_event(base->c->c, self->event);
}

/// Handle PresentCompleteNotify events
///
/// Schedule the registered callback to be called when the current vblank ends.
static void handle_present_complete_notify(struct present_vblank_scheduler *self,
                                           xcb_present_complete_notify_event_t *cne) {
	assert(self->base.type == VBLANK_SCHEDULER_PRESENT);

	if (cne->kind != XCB_PRESENT_COMPLETE_KIND_NOTIFY_MSC) {
		return;
	}

	assert(self->base.vblank_event_requested);

	// X sometimes sends duplicate/bogus MSC events, when screen has just been turned
	// off. Don't use the msc value in these events. We treat this as not receiving a
	// vblank event at all, and try to get a new one.
	//
	// See:
	// https://gitlab.freedesktop.org/xorg/xserver/-/issues/1418
	bool event_is_invalid = cne->msc <= self->last_msc || cne->ust == 0;
	if (event_is_invalid) {
		log_debug("Invalid PresentCompleteNotify event, %" PRIu64 " %" PRIu64,
		          cne->msc, cne->ust);
		x_request_vblank_event(self->base.c, cne->window, self->last_msc + 1);
		return;
	}

	self->last_ust = cne->ust;
	self->last_msc = cne->msc;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	auto now_us = (unsigned long)(now.tv_sec * 1000000L + now.tv_nsec / 1000);
	double delay_sec = 0.0;
	if (now_us < cne->ust) {
		log_trace("The end of this vblank is %lu us into the "
		          "future",
		          cne->ust - now_us);
		delay_sec = (double)(cne->ust - now_us) / 1000000.0;
	}
	// Wait until the end of the current vblank to invoke callbacks. If we
	// call it too early, it can mistakenly think the render missed the
	// vblank, and doesn't schedule render for the next vblank, causing frame
	// drops.
	assert(!ev_is_active(&self->callback_timer));
	ev_timer_set(&self->callback_timer, delay_sec, 0);
	ev_timer_start(self->base.loop, &self->callback_timer);
}

static bool handle_present_events(struct vblank_scheduler *base) {
	auto self = (struct present_vblank_scheduler *)base;
	xcb_present_generic_event_t *ev;
	while ((ev = (void *)xcb_poll_for_special_event(base->c->c, self->event))) {
		if (ev->event != self->event_id) {
			// This event doesn't have the right event context, it's not meant
			// for us.
			goto next;
		}

		// We only subscribed to the complete notify event.
		assert(ev->evtype == XCB_PRESENT_EVENT_COMPLETE_NOTIFY);
		handle_present_complete_notify(self, (void *)ev);
	next:
		free(ev);
	}
	return true;
}

static const struct vblank_scheduler_ops vblank_scheduler_ops[LAST_VBLANK_SCHEDULER] = {
    [VBLANK_SCHEDULER_PRESENT] =
        {
            .init = present_vblank_scheduler_init,
            .deinit = present_vblank_scheduler_deinit,
            .schedule = present_vblank_scheduler_schedule,
            .handle_x_events = handle_present_events,
        },
};

static void vblank_scheduler_schedule_internal(struct vblank_scheduler *self) {
	assert(self->type < LAST_VBLANK_SCHEDULER);
	auto fn = vblank_scheduler_ops[self->type].schedule;
	assert(fn != NULL);
	fn(self);
}

bool vblank_scheduler_schedule(struct vblank_scheduler *self,
                               vblank_callback_t vblank_callback, void *user_data) {
	if (self->callback_count == 0) {
		vblank_scheduler_schedule_internal(self);
	}
	if (self->callback_count == self->callback_capacity) {
		size_t new_capacity =
		    self->callback_capacity ? self->callback_capacity * 2 : 1;
		void *new_buffer =
		    realloc(self->callbacks, new_capacity * sizeof(*self->callbacks));
		if (!new_buffer) {
			return false;
		}
		self->callbacks = new_buffer;
		self->callback_capacity = new_capacity;
	}
	self->callbacks[self->callback_count++] = (struct vblank_closure){
	    .fn = vblank_callback,
	    .user_data = user_data,
	};
	return true;
}

static void
vblank_scheduler_invoke_callbacks(struct vblank_scheduler *self, struct vblank_event *event) {
	// callbacks might be added during callback invocation, so we need to
	// copy the callback_count.
	size_t count = self->callback_count, write_head = 0;
	for (size_t i = 0; i < count; i++) {
		auto action = self->callbacks[i].fn(event, self->callbacks[i].user_data);
		switch (action) {
		case VBLANK_CALLBACK_AGAIN:
			if (i != write_head) {
				self->callbacks[write_head] = self->callbacks[i];
			}
			write_head++;
		case VBLANK_CALLBACK_DONE:
		default:        // nothing to do
			break;
		}
	}
	memset(self->callbacks + write_head, 0,
	       (count - write_head) * sizeof(*self->callbacks));
	assert(count == self->callback_count && "callbacks should not be added when "
	                                        "callbacks are being invoked.");
	self->callback_count = write_head;
	if (self->callback_count) {
		vblank_scheduler_schedule_internal(self);
	}
}

void vblank_scheduler_free(struct vblank_scheduler *self) {
	assert(self->type < LAST_VBLANK_SCHEDULER);
	auto fn = vblank_scheduler_ops[self->type].deinit;
	if (fn != NULL) {
		fn(self);
	}
	free(self->callbacks);
	free(self);
}

struct vblank_scheduler *vblank_scheduler_new(struct ev_loop *loop, struct x_connection *c,
                                              xcb_window_t target_window) {
	struct vblank_scheduler *self = calloc(1, sizeof(struct present_vblank_scheduler));
	self->target_window = target_window;
	self->c = c;
	self->loop = loop;
	vblank_scheduler_ops[VBLANK_SCHEDULER_PRESENT].init(self);
	return self;
}

bool vblank_handle_x_events(struct vblank_scheduler *self) {
	assert(self->type < LAST_VBLANK_SCHEDULER);
	auto fn = vblank_scheduler_ops[self->type].handle_x_events;
	if (fn != NULL) {
		return fn(self);
	}
	return true;
}