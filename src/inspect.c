// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2024 Yuxuan Shui <yshuiv7@gmail.com>

#include <X11/Xlib.h>
#include <stddef.h>
#include <stdint.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/xproto.h>

#include "inspect.h"

#include "atom.h"
#include "backend/backend.h"
#include "c2.h"
#include "common.h"
#include "config.h"
#include "log.h"
#include "options.h"
#include "utils/misc.h"
#include "wm/defs.h"
#include "wm/win.h"
#include "x.h"

static struct win *
setup_window(struct x_connection *c, struct atom *atoms, struct options *options,
             struct wm *wm, struct c2_state *state, xcb_window_t target) {
	// Pretend we are the compositor, and build up the window state
	auto cursor = wm_find(wm, target);
	if (cursor == NULL) {
		log_fatal("Could not find window %#010x", target);
		wm_free(wm);
		return NULL;
	}

	auto toplevel = wm_ref_toplevel_of(wm, cursor);
	BUG_ON_NULL(toplevel);
	struct win *w = ccalloc(1, struct win);
	w->state = WSTATE_MAPPED;
	w->tree_ref = toplevel;
	log_debug("Toplevel is %#010x", wm_ref_win_id(toplevel));
	log_debug("Client is %#010x", win_client_id(w, true));
	win_update_wintype(c, atoms, w);
	win_update_frame_extents(c, atoms, w, win_client_id(w, /*fallback_to_self=*/true),
	                         options->frame_opacity);
	// TODO(yshui) get leader
	win_update_name(c, atoms, w);
	win_update_class(c, atoms, w);
	win_update_role(c, atoms, w);

	auto geometry_reply = XCB_AWAIT(xcb_get_geometry, c->c, win_id(w));
	w->g = (struct win_geometry){
	    .x = geometry_reply->x,
	    .y = geometry_reply->y,
	    .width = geometry_reply->width,
	    .height = geometry_reply->height,
	};
	free(geometry_reply);

	auto shape_info = xcb_get_extension_data(c->c, &xcb_shape_id);
	win_on_win_size_change(w, options->shadow_offset_x, options->shadow_offset_y,
	                       options->shadow_radius);
	win_update_bounding_shape(c, w, shape_info->present, options->detect_rounded_corners);
	win_update_prop_fullscreen(c, atoms, w);

	// Determine if the window is focused
	xcb_window_t wid = XCB_NONE;
	bool exists;
	if (options->use_ewmh_active_win) {
		wid_get_prop_window(c, c->screen_info->root, atoms->a_NET_ACTIVE_WINDOW,
		                    &exists);
	} else {
		// Determine the currently focused window so we can apply appropriate
		// opacity on it
		xcb_get_input_focus_reply_t *reply =
		    xcb_get_input_focus_reply(c->c, xcb_get_input_focus(c->c), NULL);

		if (reply) {
			wid = reply->focus;
			free(reply);
		}
	}
	if (wid == win_id(w) || wid == win_client_id(w, /*fallback_to_self=*/false)) {
		w->focused = true;
	}

	auto attributes_reply = XCB_AWAIT(xcb_get_window_attributes, c->c, win_id(w));
	w->a = *attributes_reply;
	w->pictfmt = x_get_pictform_for_visual(c, w->a.visual);
	free(attributes_reply);

	c2_window_state_init(state, &w->c2_state);
	c2_window_state_update(state, &w->c2_state, c->c,
	                       win_client_id(w, /*fallback_to_self=*/true), win_id(w));
	return w;
}

xcb_window_t select_window(struct x_connection *c) {
	xcb_font_t font = x_new_id(c);
	xcb_cursor_t cursor = x_new_id(c);
	const char font_name[] = "cursor";
	static const uint16_t CROSSHAIR_CHAR = 34;
	XCB_AWAIT_VOID(xcb_open_font, c->c, font, sizeof(font_name) - 1, font_name);
	XCB_AWAIT_VOID(xcb_create_glyph_cursor, c->c, cursor, font, font, CROSSHAIR_CHAR,
	               CROSSHAIR_CHAR + 1, 0, 0, 0, 0xffff, 0xffff, 0xffff);
	auto grab_reply = XCB_AWAIT(
	    xcb_grab_pointer, c->c, false, c->screen_info->root,
	    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE, XCB_GRAB_MODE_SYNC,
	    XCB_GRAB_MODE_ASYNC, c->screen_info->root, cursor, XCB_CURRENT_TIME);
	if (grab_reply->status != XCB_GRAB_STATUS_SUCCESS) {
		log_fatal("Failed to grab pointer");
		return 1;
	}
	free(grab_reply);

	// Let the user pick a window by clicking on it, mostly stolen from
	// xprop
	xcb_window_t target = XCB_NONE;
	int buttons_pressed = 0;
	while ((target == XCB_NONE) || (buttons_pressed > 0)) {
		XCB_AWAIT_VOID(xcb_allow_events, c->c, XCB_ALLOW_ASYNC_POINTER,
		               XCB_CURRENT_TIME);
		xcb_generic_event_t *ev = xcb_wait_for_event(c->c);
		if (!ev) {
			log_fatal("Connection to X server lost");
			return 1;
		}
		switch (XCB_EVENT_RESPONSE_TYPE(ev)) {
		case XCB_BUTTON_PRESS: {
			xcb_button_press_event_t *e = (xcb_button_press_event_t *)ev;
			if (target == XCB_NONE) {
				target = e->child;
				if (target == XCB_NONE) {
					target = e->root;
				}
			}
			buttons_pressed++;
			break;
		}
		case XCB_BUTTON_RELEASE: {
			if (buttons_pressed > 0) {
				buttons_pressed--;
			}
			break;
		}
		default: break;
		}
		free(ev);
	}
	XCB_AWAIT_VOID(xcb_ungrab_pointer, c->c, XCB_CURRENT_TIME);
	return target;
}

struct c2_match_state {
	struct c2_state *state;
	struct win *w;
	bool print_value;
};

bool c2_match_once_and_log(const c2_lptr_t *cond, void *data) {
	struct c2_match_state *state = data;
	void *rule_data = NULL;
	printf("    %s ... ", c2_lptr_to_str(cond));
	bool matched = c2_match_one(state->state, state->w, cond, rule_data);
	printf("%s", matched ? "\033[1;32mmatched\033[0m" : "not matched");
	if (state->print_value && matched) {
		printf("/%lu", (unsigned long)(intptr_t)rule_data);
		state->print_value = false;
	}
	printf("\n");
	return false;
}

#define BOLD(str) "\033[1m" str "\033[0m"

int inspect_main(int argc, char **argv, const char *config_file) {
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		log_fatal("Can't open display");
		return 1;
	}
	struct x_connection c;
	x_connection_init(&c, dpy);

	xcb_prefetch_extension_data(c.c, &xcb_shape_id);

	struct options options;
	if (!parse_config(&options, config_file)) {
		return 1;
	}

	// Parse all of the rest command line options
	options.backend = backend_find("dummy");
	if (!get_cfg(&options, argc, argv)) {
		log_fatal("Failed to get configuration, usually mean you have specified "
		          "invalid options.");
		return 1;
	}

	auto atoms = init_atoms(c.c);
	auto state = c2_state_new(atoms);
	options_postprocess_c2_lists(state, &c, &options);

	struct wm *wm = wm_new();

	wm_import_start(wm, &c, atoms, c.screen_info->root, NULL);
	// Process events until the window tree is consistent
	while (x_has_pending_requests(&c)) {
		auto ev = x_poll_for_event(&c);
		if (ev == NULL) {
			continue;
		}
		switch (ev->response_type) {
		case XCB_CREATE_NOTIFY:;
			auto create = (xcb_create_notify_event_t *)ev;
			auto parent = wm_find(wm, create->parent);
			wm_import_start(wm, &c, atoms,
			                ((xcb_create_notify_event_t *)ev)->window, parent);
			break;
		case XCB_DESTROY_NOTIFY:
			wm_destroy(wm, ((xcb_destroy_notify_event_t *)ev)->window);
			break;
		case XCB_REPARENT_NOTIFY:;
			auto reparent = (xcb_reparent_notify_event_t *)ev;
			wm_reparent(wm, reparent->window, reparent->parent);
			break;
		default:
			// Ignore ConfigureNotify and CirculateNotify, because we don't
			// use stacking order for window rules.
			break;
		}
		free(ev);
	}

	auto target = select_window(&c);
	log_info("Target window: %#x", target);
	auto w = setup_window(&c, atoms, &options, wm, state, target);
	struct c2_match_state match_state = {
	    .state = state,
	    .w = w,
	};
	printf("Checking " BOLD("transparent-clipping-exclude") ":\n");
	c2_list_foreach(options.transparent_clipping_blacklist, c2_match_once_and_log,
	                &match_state);
	printf("Checking " BOLD("shadow-exclude") ":\n");
	c2_list_foreach(options.shadow_blacklist, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("inactive-dim-exclude") ":\n");
	c2_list_foreach(options.inactive_dim_blacklist, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("fade-exclude") ":\n");
	c2_list_foreach(options.fade_blacklist, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("clip-shadow-above") ":\n");
	c2_list_foreach(options.shadow_clip_list, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("focus-exclude") ":\n");
	c2_list_foreach(options.focus_blacklist, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("invert-color-include") ":\n");
	c2_list_foreach(options.invert_color_list, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("blur-background-exclude") ":\n");
	c2_list_foreach(options.blur_background_blacklist, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("unredir-if-possible-exclude") ":\n");
	c2_list_foreach(options.unredir_if_possible_blacklist, c2_match_once_and_log,
	                &match_state);
	printf("Checking " BOLD("rounded-corners-exclude") ":\n");
	c2_list_foreach(options.rounded_corners_blacklist, c2_match_once_and_log, &match_state);

	match_state.print_value = true;
	printf("Checking " BOLD("opacity-rule") ":\n");
	c2_list_foreach(options.opacity_rules, c2_match_once_and_log, &match_state);
	printf("Checking " BOLD("corner-radius-rule") ":\n");
	c2_list_foreach(options.corner_radius_rules, c2_match_once_and_log, &match_state);

	printf("\nHere are some rule(s) that match this window:\n");
	if (w->name != NULL) {
		printf("    name = '%s'\n", w->name);
	}
	if (w->class_instance != NULL) {
		printf("    class_i = '%s'\n", w->class_instance);
	}
	if (w->class_general != NULL) {
		printf("    class_g = '%s'\n", w->class_general);
	}
	if (w->role != NULL) {
		printf("    role = '%s'\n", w->role);
	}
	if (w->window_type != WINTYPE_UNKNOWN) {
		printf("    window_type = '%s'\n", WINTYPES[w->window_type].name);
	}
	printf("    %sfullscreen\n", w->is_fullscreen ? "" : "! ");
	if (w->bounding_shaped) {
		printf("    bounding_shaped\n");
	}
	printf("    border_width = %d\n", w->g.border_width);

	pixman_region32_fini(&w->bounding_shape);
	free(w->name);
	free(w->class_instance);
	free(w->class_general);
	free(w->role);
	c2_window_state_destroy(state, &w->c2_state);
	free(w);

	wm_free(wm);

	log_deinit_tls();
	c2_state_free(state);
	destroy_atoms(atoms);
	options_destroy(&options);
	XCloseDisplay(c.dpy);
	return 0;
}
