// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2024 Yuxuan Shui <yshuiv7@gmail.com>

#include <picom/types.h>

struct x_connection;
struct ui;

enum ui_colors {
	UI_COLOR_WHITE,
	UI_COLOR_YELLOW,
	UI_COLOR_RED,
};

enum ui_style { UI_STYLE_NORMAL, UI_STYLE_BOLD };

enum ui_justify { UI_JUSTIFY_LEFT, UI_JUSTIFY_CENTER, UI_JUSTIFY_RIGHT };

struct ui_message_box_line {
	enum ui_colors color;
	enum ui_style style;
	enum ui_justify justify;
	ivec2 position;
	ivec2 size;
	unsigned pad_bottom;
	const char *text;
};

struct ui_message_box_content {
	unsigned num_lines;
	ivec2 size;
	unsigned margin;
	double scale;
	struct ui_message_box_line lines[];
};

/// Layout the content of a message box.
/// @return true if the layout is successful, false if an error occurred.
bool ui_message_box_content_plan(struct ui *ui, struct x_connection *c,
                                 struct ui_message_box_content *content);
bool ui_message_box_show(struct ui *ui, struct x_connection *c,
                         struct ui_message_box_content *content, unsigned timeout);
/// Initialize necessary resources for displaying UI.
struct ui *ui_new(struct x_connection *c);
void ui_destroy(struct ui *ui, struct x_connection *c);
