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

#include "c2.h"
#include "common.h"
#include "config.h"
#include "log.h"
#include "utils/console.h"
#include "utils/dynarr.h"
#include "utils/str.h"
#include "wm/defs.h"
#include "wm/win.h"
#include "x.h"

xcb_window_t inspect_select_window(struct x_connection *c) {
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
	const struct c2_state *state;
	const struct win *w;
	bool print_value;
};

static bool c2_match_and_log(const struct list_node *list, const struct c2_state *state,
                             const struct win *w, bool print_value) {
	void *rule_data = NULL;
	c2_condition_list_foreach((struct list_node *)list, i) {
		printf("    %s ... ", c2_condition_to_str(i));
		bool matched = c2_match_one(state, w, i, rule_data);
		printf("%s", matched ? "\033[1;32mmatched\033[0m" : "not matched");
		if (print_value && matched) {
			printf("/%lu", (unsigned long)(intptr_t)rule_data);
			print_value = false;
		}
		printf("\n");
	}
	return false;
}

void inspect_dump_window(const struct c2_state *state, const struct options *opts,
                         const struct win *w) {
	if (list_is_empty(&opts->rules)) {
		printf("Checking " BOLD("transparent-clipping-exclude") ":\n");
		c2_match_and_log(&opts->transparent_clipping_blacklist, state, w, false);
		printf("Checking " BOLD("shadow-exclude") ":\n");
		c2_match_and_log(&opts->shadow_blacklist, state, w, false);
		printf("Checking " BOLD("fade-exclude") ":\n");
		c2_match_and_log(&opts->fade_blacklist, state, w, false);
		printf("Checking " BOLD("clip-shadow-above") ":\n");
		c2_match_and_log(&opts->shadow_clip_list, state, w, true);
		printf("Checking " BOLD("focus-exclude") ":\n");
		c2_match_and_log(&opts->focus_blacklist, state, w, false);
		printf("Checking " BOLD("invert-color-include") ":\n");
		c2_match_and_log(&opts->invert_color_list, state, w, false);
		printf("Checking " BOLD("blur-background-exclude") ":\n");
		c2_match_and_log(&opts->blur_background_blacklist, state, w, false);
		printf("Checking " BOLD("unredir-if-possible-exclude") ":\n");
		c2_match_and_log(&opts->unredir_if_possible_blacklist, state, w, false);
		printf("Checking " BOLD("rounded-corners-exclude") ":\n");
		c2_match_and_log(&opts->rounded_corners_blacklist, state, w, false);

		printf("Checking " BOLD("opacity-rule") ":\n");
		c2_match_and_log(&opts->opacity_rules, state, w, true);
		printf("Checking " BOLD("corner-radius-rule") ":\n");
		c2_match_and_log(&opts->corner_radius_rules, state, w, true);
	}

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
	if (w->window_types != 0) {
		for (int i = 0; i < NUM_WINTYPES; i++) {
			if (w->window_types & (1 << i)) {
				printf("    window_type = '%s'\n", WINTYPES[i].name);
			}
		}
	}
	printf("    %sfullscreen\n", w->is_fullscreen ? "" : "! ");
	if (w->bounding_shaped) {
		printf("    bounding_shaped\n");
	}
	printf("    border_width = %d\n", w->g.border_width);
}

void inspect_dump_window_maybe_options(struct window_maybe_options wopts) {
	bool nothing = true;
	printf("      Applying:\n");
	if (wopts.shadow != TRI_UNKNOWN) {
		printf("        shadow = %s\n", wopts.shadow == TRI_TRUE ? "true" : "false");
		nothing = false;
	}
	if (wopts.fade != TRI_UNKNOWN) {
		printf("        fade = %s\n", wopts.fade == TRI_TRUE ? "true" : "false");
		nothing = false;
	}
	if (wopts.blur_background != TRI_UNKNOWN) {
		printf("        blur_background = %s\n",
		       wopts.blur_background == TRI_TRUE ? "true" : "false");
		nothing = false;
	}
	if (wopts.invert_color != TRI_UNKNOWN) {
		printf("        invert_color = %s\n",
		       wopts.invert_color == TRI_TRUE ? "true" : "false");
		nothing = false;
	}
	if (wopts.clip_shadow_above != TRI_UNKNOWN) {
		printf("        clip_shadow_above = %s\n",
		       wopts.clip_shadow_above == TRI_TRUE ? "true" : "false");
		nothing = false;
	}
	if (wopts.transparent_clipping != TRI_UNKNOWN) {
		printf("        transparent_clipping = %s\n",
		       wopts.transparent_clipping == TRI_TRUE ? "true" : "false");
		nothing = false;
	}
	if (wopts.full_shadow != TRI_UNKNOWN) {
		printf("        full_shadow = %s\n",
		       wopts.full_shadow == TRI_TRUE ? "true" : "false");
		nothing = false;
	}
	if (wopts.unredir != WINDOW_UNREDIR_INVALID) {
		const char *str = NULL;
		switch (wopts.unredir) {
		case WINDOW_UNREDIR_WHEN_POSSIBLE_ELSE_TERMINATE: str = "true"; break;
		case WINDOW_UNREDIR_TERMINATE: str = "false"; break;
		case WINDOW_UNREDIR_FORCED: str = "\"forced\""; break;
		case WINDOW_UNREDIR_PASSIVE: str = "\"passive\""; break;
		case WINDOW_UNREDIR_WHEN_POSSIBLE: str = "\"preferred\""; break;
		default: unreachable();
		}
		printf("        unredir = %s\n", str);
		nothing = false;
	}
	if (!safe_isnan(wopts.opacity)) {
		printf("        opacity = %f\n", wopts.opacity);
		nothing = false;
	}
	if (!safe_isnan(wopts.dim)) {
		printf("        dim = %f\n", wopts.dim);
		nothing = false;
	}
	if (wopts.corner_radius >= 0) {
		printf("        corner_radius = %d\n", wopts.corner_radius);
		nothing = false;
	}

	char **animation_triggers = dynarr_new(char *, 0);
	for (int i = 0; i < ANIMATION_TRIGGER_COUNT; i++) {
		if (wopts.animations[i].script != NULL) {
			char *name = NULL;
			casprintf(&name, "\"%s\"", animation_trigger_names[i]);
			dynarr_push(animation_triggers, name);
		}
	}
	if (dynarr_len(animation_triggers) > 0) {
		char *animation_triggers_str = dynarr_join(animation_triggers, ", ");
		printf("        animations = { triggers = [%s]; }\n", animation_triggers_str);
		free(animation_triggers_str);
		nothing = false;
	} else {
		dynarr_free_pod(animation_triggers);
	}

	if (nothing) {
		printf("        (nothing)\n");
	}
}
