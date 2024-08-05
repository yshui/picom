// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <assert.h>

#include <ev.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include "config.h"

#ifdef CONFIG_OPENGL
// Enable sgi_video_sync_vblank_scheduler
#include <X11/X.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <epoxy/glx.h>
#include <pthread.h>

#endif

#include "compiler.h"
#include "log.h"
#include "utils/dynarr.h"
#include "vblank.h"
#include "x.h"

struct vblank_closure {
	vblank_callback_t fn;
	void *user_data;
};

#define VBLANK_WIND_DOWN 4

struct vblank_scheduler {
	struct x_connection *c;
	/// List of scheduled vblank callbacks, this is a dynarr
	struct vblank_closure *callbacks;
	struct ev_loop *loop;
	/// Request extra vblank events even when no callbacks are scheduled.
	/// This is because when callbacks are scheduled too close to a vblank,
	/// we might send PresentNotifyMsc request too late and miss the vblank event.
	/// So we request extra vblank events right after the last vblank event
	/// to make sure this doesn't happen.
	unsigned int wind_down;
	xcb_window_t target_window;
	enum vblank_scheduler_type type;
	bool vblank_event_requested;
	bool use_realtime_scheduling;
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
	size_t size;
	bool (*init)(struct vblank_scheduler *self);
	void (*deinit)(struct vblank_scheduler *self);
	bool (*schedule)(struct vblank_scheduler *self);
	bool (*handle_x_events)(struct vblank_scheduler *self);
};

static void
vblank_scheduler_invoke_callbacks(struct vblank_scheduler *self, struct vblank_event *event);

#ifdef CONFIG_OPENGL
struct sgi_video_sync_vblank_scheduler {
	struct vblank_scheduler base;

	// Since glXWaitVideoSyncSGI blocks, we need to run it in a separate thread.
	// ... and all the thread shenanigans that come with it.
	_Atomic unsigned int current_msc;
	_Atomic uint64_t current_ust;
	ev_async notify;
	pthread_t sync_thread;
	bool running, error, vblank_requested;
	unsigned int last_msc;

	/// Protects `running`, and `vblank_requested`
	pthread_mutex_t vblank_requested_mtx;
	pthread_cond_t vblank_requested_cnd;
};

struct sgi_video_sync_thread_args {
	struct sgi_video_sync_vblank_scheduler *self;
	int start_status;
	bool use_realtime_scheduling;
	pthread_mutex_t start_mtx;
	pthread_cond_t start_cnd;
};

static bool check_sgi_video_sync_extension(Display *dpy, int screen) {
	const char *glx_ext = glXQueryExtensionsString(dpy, screen);
	const char *needle = "GLX_SGI_video_sync";
	char *found = strstr(glx_ext, needle);
	if (!found) {
		return false;
	}
	if (found != glx_ext && found[-1] != ' ') {
		return false;
	}
	if (found[strlen(needle)] != ' ' && found[strlen(needle)] != '\0') {
		return false;
	}

	return true;
}

static void *sgi_video_sync_thread(void *data) {
	auto args = (struct sgi_video_sync_thread_args *)data;
	auto self = args->self;
	Display *dpy = XOpenDisplay(NULL);
	int error_code = 0;
	GLXContext ctx = NULL;
	GLXDrawable drawable = None;
	Window root = DefaultRootWindow(dpy), dummy = None;
	if (!dpy) {
		error_code = 1;
		goto start_failed;
	}
	int screen = DefaultScreen(dpy);
	int ncfg = 0;
	GLXFBConfig *cfg_ = glXChooseFBConfig(
	    dpy, screen,
	    (int[]){GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, 0},
	    &ncfg);

	if (!cfg_) {
		error_code = 2;
		goto start_failed;
	}
	GLXFBConfig cfg = cfg_[0];
	XFree(cfg_);

	XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, cfg);
	if (!vi) {
		error_code = 3;
		goto start_failed;
	}

	Visual *visual = vi->visual;
	const int depth = vi->depth;
	XFree(vi);

	Colormap colormap = XCreateColormap(dpy, root, visual, AllocNone);
	XSetWindowAttributes attributes;
	attributes.colormap = colormap;

	dummy = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, depth, InputOutput, visual,
	                      CWColormap, &attributes);
	XFreeColormap(dpy, colormap);
	if (dummy == None) {
		error_code = 4;
		goto start_failed;
	}

	drawable = glXCreateWindow(dpy, cfg, dummy, NULL);
	if (drawable == None) {
		error_code = 5;
		goto start_failed;
	}

	ctx = glXCreateNewContext(dpy, cfg, GLX_RGBA_TYPE, 0, true);
	if (ctx == NULL) {
		error_code = 6;
		goto start_failed;
	}

	if (!glXMakeContextCurrent(dpy, drawable, drawable, ctx)) {
		error_code = 7;
		goto start_failed;
	}

	if (!check_sgi_video_sync_extension(dpy, screen)) {
		error_code = 8;
		goto start_failed;
	}

	log_init_tls();

	if (args->use_realtime_scheduling) {
		set_rr_scheduling();
	}

	pthread_mutex_lock(&args->start_mtx);
	args->start_status = 0;
	pthread_cond_signal(&args->start_cnd);
	pthread_mutex_unlock(&args->start_mtx);

	pthread_mutex_lock(&self->vblank_requested_mtx);
	while (self->running) {
		if (!self->vblank_requested) {
			pthread_cond_wait(&self->vblank_requested_cnd,
			                  &self->vblank_requested_mtx);
			continue;
		}
		pthread_mutex_unlock(&self->vblank_requested_mtx);

		unsigned int last_msc;
		glXWaitVideoSyncSGI(1, 0, &last_msc);

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		atomic_store(&self->current_msc, last_msc);
		atomic_store(&self->current_ust,
		             (uint64_t)(now.tv_sec * 1000000 + now.tv_nsec / 1000));
		pthread_mutex_lock(&self->vblank_requested_mtx);
		self->vblank_requested = false;
		ev_async_send(self->base.loop, &self->notify);
	}
	pthread_mutex_unlock(&self->vblank_requested_mtx);
	goto cleanup;

start_failed:
	pthread_mutex_lock(&args->start_mtx);
	args->start_status = error_code;
	pthread_cond_signal(&args->start_cnd);
	pthread_mutex_unlock(&args->start_mtx);

cleanup:
	log_deinit_tls();
	if (dpy) {
		glXMakeCurrent(dpy, None, NULL);
		if (ctx) {
			glXDestroyContext(dpy, ctx);
		}
		if (drawable) {
			glXDestroyWindow(dpy, drawable);
		}
		if (dummy) {
			XDestroyWindow(dpy, dummy);
		}
		XCloseDisplay(dpy);
	}
	return NULL;
}

static bool sgi_video_sync_scheduler_schedule(struct vblank_scheduler *base) {
	auto self = (struct sgi_video_sync_vblank_scheduler *)base;
	if (self->error) {
		return false;
	}
	assert(!base->vblank_event_requested);

	log_verbose("Requesting vblank event for msc %d", self->current_msc + 1);
	pthread_mutex_lock(&self->vblank_requested_mtx);
	self->vblank_requested = true;
	pthread_cond_signal(&self->vblank_requested_cnd);
	pthread_mutex_unlock(&self->vblank_requested_mtx);

	base->vblank_event_requested = true;
	return true;
}

static void
sgi_video_sync_scheduler_callback(EV_P attr_unused, ev_async *w, int attr_unused revents);

static bool sgi_video_sync_scheduler_init(struct vblank_scheduler *base) {
	auto self = (struct sgi_video_sync_vblank_scheduler *)base;
	auto args = (struct sgi_video_sync_thread_args){
	    .self = self,
	    .start_status = -1,
	    .use_realtime_scheduling = base->use_realtime_scheduling,
	};
	bool succeeded = true;
	pthread_mutex_init(&args.start_mtx, NULL);
	pthread_cond_init(&args.start_cnd, NULL);

	base->type = VBLANK_SCHEDULER_SGI_VIDEO_SYNC;
	ev_async_init(&self->notify, sgi_video_sync_scheduler_callback);
	ev_async_start(base->loop, &self->notify);
	pthread_mutex_init(&self->vblank_requested_mtx, NULL);
	pthread_cond_init(&self->vblank_requested_cnd, NULL);

	self->vblank_requested = false;
	self->running = true;
	pthread_create(&self->sync_thread, NULL, sgi_video_sync_thread, &args);

	pthread_mutex_lock(&args.start_mtx);
	while (args.start_status == -1) {
		pthread_cond_wait(&args.start_cnd, &args.start_mtx);
	}
	if (args.start_status != 0) {
		log_fatal("Failed to start sgi_video_sync_thread, error code: %d",
		          args.start_status);
		succeeded = false;
	} else {
		log_info("Started sgi_video_sync_thread");
	}
	self->error = !succeeded;
	self->last_msc = 0;
	pthread_mutex_destroy(&args.start_mtx);
	pthread_cond_destroy(&args.start_cnd);
	return succeeded;
}

static void sgi_video_sync_scheduler_deinit(struct vblank_scheduler *base) {
	auto self = (struct sgi_video_sync_vblank_scheduler *)base;
	ev_async_stop(base->loop, &self->notify);
	pthread_mutex_lock(&self->vblank_requested_mtx);
	self->running = false;
	pthread_cond_signal(&self->vblank_requested_cnd);
	pthread_mutex_unlock(&self->vblank_requested_mtx);

	pthread_join(self->sync_thread, NULL);

	pthread_mutex_destroy(&self->vblank_requested_mtx);
	pthread_cond_destroy(&self->vblank_requested_cnd);
}

static void
sgi_video_sync_scheduler_callback(EV_P attr_unused, ev_async *w, int attr_unused revents) {
	auto sched = container_of(w, struct sgi_video_sync_vblank_scheduler, notify);
	auto msc = atomic_load(&sched->current_msc);
	if (sched->last_msc == msc) {
		// NVIDIA spams us with duplicate vblank events after a suspend/resume
		// cycle. Recreating the X connection and GLX context seems to fix this.
		// Oh NVIDIA.
		log_warn("Duplicate vblank event found with msc %d. Possible NVIDIA bug?", msc);
		log_warn("Resetting the vblank scheduler");
		sgi_video_sync_scheduler_deinit(&sched->base);
		sched->base.vblank_event_requested = false;
		if (!sgi_video_sync_scheduler_init(&sched->base)) {
			log_error("Failed to reset the vblank scheduler");
		} else {
			sgi_video_sync_scheduler_schedule(&sched->base);
		}
		return;
	}
	auto event = (struct vblank_event){
	    .msc = msc,
	    .ust = atomic_load(&sched->current_ust),
	};
	sched->base.vblank_event_requested = false;
	sched->last_msc = msc;
	log_verbose("Received vblank event for msc %" PRIu64, event.msc);
	vblank_scheduler_invoke_callbacks(&sched->base, &event);
}
#endif

static bool present_vblank_scheduler_schedule(struct vblank_scheduler *base) {
	auto self = (struct present_vblank_scheduler *)base;
	log_verbose("Requesting vblank event for window 0x%08x, msc %" PRIu64,
	            base->target_window, self->last_msc + 1);
	assert(!base->vblank_event_requested);
	x_request_vblank_event(base->c, base->target_window, self->last_msc + 1);
	base->vblank_event_requested = true;
	return true;
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

static bool present_vblank_scheduler_init(struct vblank_scheduler *base) {
	auto self = (struct present_vblank_scheduler *)base;
	base->type = VBLANK_SCHEDULER_PRESENT;
	ev_timer_init(&self->callback_timer, present_vblank_callback, 0, 0);

	self->event_id = x_new_id(base->c);
	auto select_input =
	    xcb_present_select_input(base->c->c, self->event_id, base->target_window,
	                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
	x_set_error_action_abort(base->c, select_input);
	self->event =
	    xcb_register_for_special_xge(base->c->c, &xcb_present_id, self->event_id, NULL);
	return true;
}

static void present_vblank_scheduler_deinit(struct vblank_scheduler *base) {
	auto self = (struct present_vblank_scheduler *)base;
	ev_timer_stop(base->loop, &self->callback_timer);
	auto select_input =
	    xcb_present_select_input(base->c->c, self->event_id, base->target_window, 0);
	x_set_error_action_abort(base->c, select_input);
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

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	auto now_us = (unsigned long)(now.tv_sec * 1000000L + now.tv_nsec / 1000);

	// X sometimes sends duplicate/bogus MSC events, when screen has just been turned
	// off. Don't use the msc value in these events. We treat this as not receiving a
	// vblank event at all, and try to get a new one.
	//
	// See:
	// https://gitlab.freedesktop.org/xorg/xserver/-/issues/1418
	bool event_is_invalid = cne->msc <= self->last_msc || cne->ust == 0;
	if (event_is_invalid) {
		log_debug("Invalid PresentCompleteNotify event, %" PRIu64 " %" PRIu64
		          ". Trying to recover, reporting a fake vblank.",
		          cne->msc, cne->ust);
		self->last_ust = now_us;
		self->last_msc += 1;
	} else {
		self->last_ust = cne->ust;
		self->last_msc = cne->msc;
	}
	double delay_sec = 0.0;
	if (now_us < cne->ust) {
		log_trace("The end of this vblank is %" PRIu64 " us into the "
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
            .size = sizeof(struct present_vblank_scheduler),
            .init = present_vblank_scheduler_init,
            .deinit = present_vblank_scheduler_deinit,
            .schedule = present_vblank_scheduler_schedule,
            .handle_x_events = handle_present_events,
        },
#ifdef CONFIG_OPENGL
    [VBLANK_SCHEDULER_SGI_VIDEO_SYNC] =
        {
            .size = sizeof(struct sgi_video_sync_vblank_scheduler),
            .init = sgi_video_sync_scheduler_init,
            .deinit = sgi_video_sync_scheduler_deinit,
            .schedule = sgi_video_sync_scheduler_schedule,
            .handle_x_events = NULL,
        },
#endif
};

static bool vblank_scheduler_schedule_internal(struct vblank_scheduler *self) {
	assert(self->type < LAST_VBLANK_SCHEDULER);
	auto fn = vblank_scheduler_ops[self->type].schedule;
	assert(fn != NULL);
	return fn(self);
}

bool vblank_scheduler_schedule(struct vblank_scheduler *self,
                               vblank_callback_t vblank_callback, void *user_data) {
	// Schedule a new vblank event if there are no callbacks currently scheduled.
	if (dynarr_len(self->callbacks) == 0 && self->wind_down == 0 &&
	    !vblank_scheduler_schedule_internal(self)) {
		return false;
	}
	struct vblank_closure closure = {
	    .fn = vblank_callback,
	    .user_data = user_data,
	};
	dynarr_push(self->callbacks, closure);
	return true;
}

static void
vblank_scheduler_invoke_callbacks(struct vblank_scheduler *self, struct vblank_event *event) {
	// callbacks might be added during callback invocation, so we need to
	// copy the callback_count.
	size_t count = dynarr_len(self->callbacks), write_head = 0;
	if (count == 0) {
		self->wind_down--;
	} else {
		self->wind_down = VBLANK_WIND_DOWN;
	}
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
	assert(count == dynarr_len(self->callbacks) && "callbacks should not be added "
	                                               "when callbacks are being "
	                                               "invoked.");
	dynarr_len(self->callbacks) = write_head;
	if (write_head || self->wind_down) {
		vblank_scheduler_schedule_internal(self);
	}
}

void vblank_scheduler_free(struct vblank_scheduler *self) {
	assert(self->type < LAST_VBLANK_SCHEDULER);
	auto fn = vblank_scheduler_ops[self->type].deinit;
	if (fn != NULL) {
		fn(self);
	}
	dynarr_free_pod(self->callbacks);
	free(self);
}

struct vblank_scheduler *
vblank_scheduler_new(struct ev_loop *loop, struct x_connection *c, xcb_window_t target_window,
                     enum vblank_scheduler_type type, bool use_realtime_scheduling) {
	size_t object_size = vblank_scheduler_ops[type].size;
	auto init_fn = vblank_scheduler_ops[type].init;
	if (!object_size || !init_fn) {
		log_error("Unsupported or invalid vblank scheduler type: %d", type);
		return NULL;
	}

	assert(object_size >= sizeof(struct vblank_scheduler));
	struct vblank_scheduler *self = calloc(1, object_size);
	self->target_window = target_window;
	self->c = c;
	self->loop = loop;
	self->use_realtime_scheduling = use_realtime_scheduling;
	self->callbacks = dynarr_new(struct vblank_closure, 1);
	init_fn(self);
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
