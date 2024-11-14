#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/randr.h>
#include <xcb/xcb_event.h>

#include "log.h"
#include "misc.h"
#include "ui.h"
#include "x.h"

struct ui {
	xcb_fontable_t normal_font;
	xcb_fontable_t bold_font;
};

static xcb_pixmap_t
ui_message_box_draw_text(struct ui *ui, struct x_connection *c, xcb_window_t window,
                         struct ui_message_box_content *content) {
	xcb_pixmap_t pixmap = x_new_id(c);
	uint16_t width =
	    content->size.width > UINT16_MAX ? UINT16_MAX : (uint16_t)content->size.width;
	uint16_t height =
	    content->size.height > UINT16_MAX ? UINT16_MAX : (uint16_t)content->size.height;
	if (!XCB_AWAIT_VOID(xcb_create_pixmap, c, c->screen_info->root_depth, pixmap,
	                    window, width, height)) {
		return XCB_NONE;
	}

	xcb_gcontext_t gc = x_new_id(c);
	{
		uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
		uint32_t value_list[3] = {c->screen_info->black_pixel,
		                          c->screen_info->black_pixel};

		if (!XCB_AWAIT_VOID(xcb_create_gc, c, gc, pixmap, mask, value_list)) {
			return XCB_NONE;
		}
	}

	xcb_poly_fill_rectangle(
	    c->c, pixmap, gc, 1,
	    &(xcb_rectangle_t){.x = 0, .y = 0, .width = width, .height = height});

	const char yellow_name[] = "yellow";
	const char red_name[] = "red";
	auto r = XCB_AWAIT(xcb_alloc_named_color, c, c->screen_info->default_colormap,
	                   ARR_SIZE(yellow_name) - 1, yellow_name);
	if (r == NULL) {
		return XCB_NONE;
	}
	auto yellow_pixel = r->pixel;
	free(r);
	r = XCB_AWAIT(xcb_alloc_named_color, c, c->screen_info->default_colormap,
	              ARR_SIZE(red_name) - 1, red_name);
	if (r == NULL) {
		return XCB_NONE;
	}
	auto red_pixel = r->pixel;
	free(r);

	for (unsigned i = 0; i < content->num_lines; i++) {
		auto line = &content->lines[i];
		uint32_t color = 0;
		switch (line->color) {
		case UI_COLOR_WHITE: color = c->screen_info->white_pixel; break;
		case UI_COLOR_YELLOW: color = yellow_pixel; break;
		case UI_COLOR_RED: color = red_pixel; break;
		}
		uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_FONT;
		uint32_t value_list[2] = {
		    color, line->style == UI_STYLE_BOLD ? ui->bold_font : ui->normal_font};
		int16_t x = (int16_t)(line->position.x > INT16_MAX ? INT16_MAX
		                                                   : line->position.x),
		        y = (int16_t)(line->position.y > INT16_MAX ? INT16_MAX
		                                                   : line->position.y);
		xcb_change_gc(c->c, gc, mask, value_list);
		xcb_image_text_8(c->c, (uint8_t)strlen(line->text), pixmap, gc, x, y,
		                 line->text);
	}
	xcb_free_gc(c->c, gc);
	return pixmap;
}

void ui_message_box_place(struct x_connection *c, struct ui_message_box_content *content,
                          int16_t *x, int16_t *y) {
	auto r = XCB_AWAIT(xcb_randr_get_screen_resources_current, c, c->screen_info->root);
	if (r == NULL) {
		return;
	}
	auto pointer = XCB_AWAIT(xcb_query_pointer, c, c->screen_info->root);
	if (pointer == NULL) {
		free(r);
		return;
	}

	auto num_crtc = xcb_randr_get_screen_resources_current_crtcs_length(r);
	auto crtcs = xcb_randr_get_screen_resources_current_crtcs(r);
	for (int i = 0; i < num_crtc; i++) {
		auto crtc_info_ptr =
		    XCB_AWAIT(xcb_randr_get_crtc_info, c, crtcs[i], r->config_timestamp);
		if (crtc_info_ptr == NULL ||
		    crtc_info_ptr->status != XCB_RANDR_SET_CONFIG_SUCCESS) {
			free(crtc_info_ptr);
			continue;
		}

		auto crtc_info = *crtc_info_ptr;
		free(crtc_info_ptr);

		if (content->scale == 0) {
			content->scale = crtc_info.width / 1280.0;
		}

		vec2 size = {
		    (content->size.width + (int)content->margin * 2) * content->scale,
		    (content->size.height + (int)content->margin * 2) * content->scale,
		};

		if (pointer->root_x >= crtc_info.x &&
		    pointer->root_x < crtc_info.x + crtc_info.width &&
		    pointer->root_y >= crtc_info.y &&
		    pointer->root_y < crtc_info.y + crtc_info.height) {
			auto tmp_x =
			    crtc_info.x + max2((crtc_info.width - size.width) / 2, 0);
			auto tmp_y =
			    crtc_info.y + max2((crtc_info.height - size.height) / 2, 0);
			*x = (int16_t)clamp(tmp_x, INT16_MIN, INT16_MAX);
			*y = (int16_t)clamp(tmp_y, INT16_MIN, INT16_MAX);
			break;
		}
	}
	free(pointer);
	free(r);
}
const int64_t FPS = 60;
bool ui_message_box_show(struct ui *ui, struct x_connection *c,
                         struct ui_message_box_content *content, unsigned timeout) {
	struct timespec next_render;
	if (clock_gettime(CLOCK_MONOTONIC, &next_render) < 0) {
		log_error("Failed to get current time");
		return false;
	}
	struct timespec close_time = next_render;
	close_time.tv_sec += (long)timeout;

	int16_t x = 0, y = 0;
	ui_message_box_place(c, content, &x, &y);

	ivec2 size = ivec2_add(
	    content->size, (ivec2){(int)content->margin * 2, (int)content->margin * 2});
	size.width = (int)(size.width * content->scale);
	size.height = (int)(size.height * content->scale);

	xcb_window_t win = x_new_id(c);
	uint16_t width = to_u16_saturated(size.width),
	         height = to_u16_saturated(size.height),
	         inner_width = to_u16_saturated(content->size.width * content->scale),
	         inner_height = to_u16_saturated(content->size.height * content->scale);
	int16_t margin = to_i16_checked(content->margin * content->scale);

	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
	uint32_t values[3] = {c->screen_info->black_pixel, 1,
	                      XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS |
	                          XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_POINTER_MOTION |
	                          XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW};
	bool success = XCB_AWAIT_VOID(xcb_create_window, c, c->screen_info->root_depth,
	                              win, c->screen_info->root, x, y, width, height,
	                              /*border_width=*/0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
	                              c->screen_info->root_visual, mask, values);
	if (!success) {
		return success;
	}
	auto rendered_content = ui_message_box_draw_text(ui, c, win, content);
	xcb_render_picture_t content_picture = x_create_picture_with_visual_and_pixmap(
	    c, c->screen_info->root_visual, rendered_content, 0, NULL);
	xcb_render_picture_t target_picture = x_create_picture_with_visual_and_pixmap(
	    c, c->screen_info->root_visual, win, 0, NULL);
	if (content_picture == XCB_NONE || target_picture == XCB_NONE) {
		return false;
	}

	xcb_render_transform_t transform = {
	    .matrix11 = DOUBLE_TO_XFIXED(1.0F / content->scale),
	    .matrix22 = DOUBLE_TO_XFIXED(1.0F / content->scale),
	    .matrix33 = DOUBLE_TO_XFIXED(1.0),
	};
	if (!XCB_AWAIT_VOID(xcb_render_set_picture_transform, c, content_picture, transform)) {
		return false;
	}

	const char filter_name[] = "bilinear";
	if (!XCB_AWAIT_VOID(xcb_render_set_picture_filter, c, content_picture,
	                    ARR_SIZE(filter_name) - 1, filter_name, 0, NULL)) {
		return false;
	}

	if (!XCB_AWAIT_VOID(xcb_map_window, c, win)) {
		xcb_destroy_window(c->c, win);
		return false;
	}

	xcb_generic_event_t *event;
	bool quit = false;
	while (!quit) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		int wait_time = 0;
		double remaining = 0;
		bool should_render = false;
		if (timespec_cmp(now, next_render) < 0) {
			wait_time = (int)((next_render.tv_sec - now.tv_sec) * 1000 +
			                  (next_render.tv_nsec - now.tv_nsec) / 1000000);
		}
		struct pollfd fds = {.fd = xcb_get_file_descriptor(c->c), .events = POLLIN};
		poll(&fds, 1, wait_time);
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (timespec_cmp(next_render, now) <= 0) {
			should_render = true;
		}
		if (timespec_cmp(close_time, now) <= 0) {
			quit = true;
		} else {
			remaining = (double)(close_time.tv_sec - now.tv_sec) +
			            (double)(close_time.tv_nsec - now.tv_nsec) / 1000000000;
		}
		while ((event = xcb_poll_for_event(c->c)) != NULL) {
			switch (XCB_EVENT_RESPONSE_TYPE(event)) {
			case XCB_EXPOSE:
				xcb_render_fill_rectangles(
				    c->c, XCB_RENDER_PICT_OP_SRC, target_picture,
				    (xcb_render_color_t){.alpha = 0xffff}, 1,
				    (const xcb_rectangle_t[]){
				        {.x = 0, .y = 0, .width = width, .height = height}});
				xcb_render_composite(c->c, XCB_RENDER_PICT_OP_SRC,
				                     content_picture, XCB_NONE,
				                     target_picture, 0, 0, 0, 0, margin,
				                     margin, inner_width, inner_height);
				should_render = true;
				break;
			case XCB_KEY_RELEASE:;
				xcb_key_release_event_t *kr = (xcb_key_release_event_t *)event;

				switch (kr->detail) {
				case /*ESC*/ 9: quit = true; break;
				}
				break;
			case XCB_ENTER_NOTIFY:
				xcb_grab_keyboard(c->c, 0, win, XCB_CURRENT_TIME,
				                  XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
				break;
			case XCB_LEAVE_NOTIFY:
				xcb_ungrab_keyboard(c->c, XCB_CURRENT_TIME);
				break;
			}
			free(event);
		}
		if (should_render) {
			const uint16_t bar_height = (uint16_t)(10 * content->scale);
			const uint16_t bar_width =
			    to_u16_saturated(width * remaining / timeout);
			next_render.tv_sec = now.tv_sec;
			next_render.tv_nsec = now.tv_nsec + 1000000000 / FPS;
			xcb_render_fill_rectangles(
			    c->c, XCB_RENDER_PICT_OP_SRC, target_picture,
			    (xcb_render_color_t){}, 1,
			    (const xcb_rectangle_t[]){
			        {.x = 0,
			         .y = (int16_t)clamp(height - bar_height, 0, INT16_MAX),
			         .width = width,
			         .height = bar_height}});
			xcb_render_fill_rectangles(
			    c->c, XCB_RENDER_PICT_OP_SRC, target_picture,
			    (xcb_render_color_t){
			        .alpha = 0xffff, .red = 0xffff, .green = 0xffff, .blue = 0xffff},
			    1,
			    (const xcb_rectangle_t[]){
			        {.x = (int16_t)((width - bar_width) / 2),
			         .y = (int16_t)clamp(height - bar_height, 0, INT16_MAX),
			         .width = bar_width,
			         .height = bar_height}});
			if (next_render.tv_nsec >= 1000000000) {
				next_render.tv_sec++;
				next_render.tv_nsec -= 1000000000;
			}
		}
		xcb_flush(c->c);
	}
	return true;
}

static bool ui_message_box_line_extent(struct ui *ui, struct x_connection *c,
                                       struct ui_message_box_line *line) {
	auto len = (uint32_t)strlen(line->text);
	if (len > UINT8_MAX) {
		return false;
	}
	auto text16 = ccalloc(len, xcb_char2b_t);
	auto font = line->style == UI_STYLE_BOLD ? ui->bold_font : ui->normal_font;
	for (uint32_t i = 0; i < len; i++) {
		text16[i].byte1 = 0;
		text16[i].byte2 = (uint8_t)line->text[i];
	}
	auto r = XCB_AWAIT(xcb_query_text_extents, c, font, len, text16);
	free(text16);

	if (!r) {
		return false;
	}
	line->size.width = r->overall_width;
	line->size.height = r->font_ascent + r->font_descent;
	line->position.y = r->font_ascent;
	free(r);
	return true;
}

bool ui_message_box_content_plan(struct ui *ui, struct x_connection *c,
                                 struct ui_message_box_content *content) {
	if (content->margin > INT_MAX) {
		log_error("Margin is too large");
		return false;
	}
	ivec2 size = {};
	for (unsigned i = 0; i < content->num_lines; i++) {
		if (!ui_message_box_line_extent(ui, c, &content->lines[i])) {
			return false;
		}
		content->lines[i].position.y += size.height;
		size.height +=
		    content->lines[i].size.height + (int)content->lines[i].pad_bottom;
		if (content->lines[i].size.width > size.width) {
			size.width = content->lines[i].size.width;
		}
	}

	content->size = size;

	for (unsigned i = 0; i < content->num_lines; i++) {
		switch (content->lines[i].justify) {
		case UI_JUSTIFY_LEFT: content->lines[i].position.x = 0; break;
		case UI_JUSTIFY_CENTER:
			content->lines[i].position.x =
			    (size.width - content->lines[i].size.width) / 2;
			break;
		case UI_JUSTIFY_RIGHT:
			content->lines[i].position.x =
			    size.width - content->lines[i].size.width;
			break;
		}
	}
	return true;
}

struct ui *ui_new(struct x_connection *c) {
	const char normal_font[] = "fixed";
	const char bold_font[] = "-*-fixed-bold-*";
	auto ui = ccalloc(1, struct ui);
	ui->normal_font = x_new_id(c);
	ui->bold_font = x_new_id(c);
	auto cookie1 = xcb_open_font_checked(c->c, ui->normal_font,
	                                     ARR_SIZE(normal_font) - 1, normal_font);
	auto cookie2 =
	    xcb_open_font_checked(c->c, ui->bold_font, ARR_SIZE(bold_font) - 1, bold_font);

	xcb_generic_error_t *e = xcb_request_check(c->c, cookie1);
	if (e != NULL) {
		log_error_x_error(c, e, "Cannot open the fixed font");
		free(e);
		return NULL;
	}
	e = xcb_request_check(c->c, cookie2);
	if (e != NULL) {
		ui->bold_font = ui->normal_font;
		log_error_x_error(c, e,
		                  "Cannot open the bold font, falling back to normal "
		                  "font");
		free(e);
	}
	return ui;
}

void ui_destroy(struct ui *ui, struct x_connection *c) {
	xcb_close_font(c->c, ui->normal_font);
	if (ui->bold_font != ui->normal_font) {
		xcb_close_font(c->c, ui->bold_font);
	}
	xcb_flush(c->c);
}
