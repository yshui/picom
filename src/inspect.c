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
#include "c2.h"
#include "common.h"
#include "config.h"
#include "err.h"
#include "log.h"
#include "options.h"
#include "utils.h"
#include "win.h"
#include "win_defs.h"
#include "x.h"

static struct managed_win *
setup_window(struct x_connection *c, struct atom *atoms, struct options *options,
             struct c2_state *state, xcb_window_t target) {
	// Pretend we are the compositor, and build up the window state
	struct managed_win *w = ccalloc(1, struct managed_win);
	w->state = WSTATE_MAPPED;
	w->base.id = target;
	w->client_win = win_get_client_window(c, NULL, atoms, w);
	win_update_wintype(c, atoms, w);
	win_update_frame_extents(c, atoms, w, w->client_win, options->frame_opacity);
	// TODO(yshui) get leader
	win_update_name(c, atoms, w);
	win_update_class(c, atoms, w);
	win_update_role(c, atoms, w);

	auto geometry_reply = XCB_AWAIT(xcb_get_geometry, c->c, w->base.id);
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
	if (options->use_ewmh_active_win) {
		wid_get_prop_window(c, c->screen_info->root, atoms->a_NET_ACTIVE_WINDOW);
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
	if (wid == w->base.id || wid == w->client_win) {
		w->focused = true;
	}

	auto attributes_reply = XCB_AWAIT(xcb_get_window_attributes, c->c, w->base.id);
	w->a = *attributes_reply;
	w->pictfmt = x_get_pictform_for_visual(c, w->a.visual);
	free(attributes_reply);

	c2_window_state_init(state, &w->c2_state);
	c2_window_state_update(state, &w->c2_state, c->c, w->client_win, w->base.id);
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
	struct managed_win *w;
	bool print_value;
};

bool c2_match_once_and_log(const c2_lptr_t *cond, void *data) {
	struct c2_match_state *state = data;
	void *rule_data = NULL;
	printf("    %s ... ", c2_lptr_to_str(cond));
	bool matched = c2_match(state->state, state->w, cond, &rule_data);
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
	auto stderr_logger = stderr_logger_new();
	if (stderr_logger) {
		log_add_target_tls(stderr_logger);
	}

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		log_fatal("Can't open display");
		return 1;
	}
	struct x_connection c;
	x_connection_init(&c, dpy);

	xcb_prefetch_extension_data(c.c, &xcb_shape_id);

	char *config_file_to_free = NULL;
	struct options options;
	bool shadow_enabled, fading_enable, hasneg;
	win_option_mask_t winopt_mask[NUM_WINTYPES] = {0};
	config_file = config_file_to_free = parse_config(
	    &options, config_file, &shadow_enabled, &fading_enable, &hasneg, winopt_mask);

	if (IS_ERR(config_file_to_free)) {
		return 1;
	}

	// Parse all of the rest command line options
	if (!get_cfg(&options, argc, argv, shadow_enabled, fading_enable, hasneg, winopt_mask)) {
		log_fatal("Failed to get configuration, usually mean you have specified "
		          "invalid options.");
		return 1;
	}

	auto atoms attr_unused = init_atoms(c.c);
	auto state = c2_state_new(atoms);
	options_postprocess_c2_lists(state, &c, &options);

	auto target = select_window(&c);
	log_info("Target window: %#x", target);
	auto w = setup_window(&c, atoms, &options, state, target);
	struct c2_match_state match_state = {
	    .state = state,
	    .w = w,
	};
	printf("Checking " BOLD("transparent-clipping-exclude") ":\n");
	c2_list_foreach(options.transparent_clipping_blacklist, c2_match_once_and_log,
	                &match_state);
	printf("Checking " BOLD("shadow-exclude") ":\n");
	c2_list_foreach(options.shadow_blacklist, c2_match_once_and_log, &match_state);
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

	log_deinit_tls();
	free(config_file_to_free);
	c2_state_free(state);
	destroy_atoms(atoms);
	options_destroy(&options);
	XCloseDisplay(c.dpy);
	return 0;
}
