// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include "command_builder.h"

#include "common.h"
#include "layout.h"
#include "win.h"

/// Generate commands for rendering the body of the window in `layer`.
///
/// @param[in]  frame_region frame region of the window, in window local coordinates
/// @param[out] cmd          output commands, when multiple commands are generated,
///                          it's stored in `cmd` going backwards, i.e. cmd - 1, -2, ...
/// @return                  number of commands generated
static inline unsigned
commands_for_window_body(struct layer *layer, struct backend_command *cmd,
                         const region_t *frame_region, bool inactive_dim_fixed,
                         double inactive_dim, double max_brightness) {
	auto w = layer->win;
	auto mode = win_calc_mode(layer->win);
	int border_width = w->g.border_width;
	double dim = 0;
	if (w->dim) {
		dim = inactive_dim;
		if (!inactive_dim_fixed) {
			dim *= layer->opacity;
		}
	}
	if (border_width == 0) {
		// Some WM has borders implemented as WM frames
		border_width = min3(w->frame_extents.left, w->frame_extents.right,
		                    w->frame_extents.bottom);
	}
	cmd->op = BACKEND_COMMAND_BLIT;
	cmd->source = BACKEND_COMMAND_SOURCE_WINDOW;
	cmd->origin = layer->origin;
	cmd->blit = (struct backend_blit_args){
	    .border_width = border_width,
	    .mask = &cmd->mask,
	    .corner_radius = w->corner_radius,
	    .opacity = layer->opacity,
	    .dim = dim,
	    .ewidth = w->widthb,
	    .eheight = w->heightb,
	    .shader = w->fg_shader ? w->fg_shader->backend_shader : NULL,
	    .color_inverted = w->invert_color,
	    .max_brightness = max_brightness};
	cmd->mask.inverted = false;
	cmd->mask.corner_radius = 0;
	cmd->mask.origin = (struct coord){};
	pixman_region32_copy(&cmd->mask.region, &w->bounding_shape);
	if (w->frame_opacity < 1) {
		pixman_region32_subtract(&cmd->mask.region, &cmd->mask.region, frame_region);
	}
	cmd->need_mask_image = false;
	pixman_region32_init(&cmd->opaque_region);
	if (mode == WMODE_SOLID || mode == WMODE_FRAME_TRANS) {
		pixman_region32_copy(&cmd->opaque_region, &cmd->mask.region);
	}
	if (mode == WMODE_FRAME_TRANS) {
		pixman_region32_subtract(&cmd->opaque_region, &cmd->opaque_region, frame_region);
	}
	if (w->corner_radius > 0) {
		win_region_remove_corners(w, &cmd->opaque_region);
	}
	pixman_region32_translate(&cmd->opaque_region, layer->origin.x, layer->origin.y);
	if (w->frame_opacity == 1 || w->frame_opacity == 0) {
		return 1;
	}

	cmd -= 1;
	cmd->op = BACKEND_COMMAND_BLIT;
	cmd->origin = layer->origin;
	cmd->source = BACKEND_COMMAND_SOURCE_WINDOW;
	cmd->need_mask_image = false;
	cmd->blit = cmd[1].blit;
	cmd->blit.mask = &cmd->mask;
	cmd->blit.opacity *= w->frame_opacity;
	cmd->mask.origin = (struct coord){};
	cmd->mask.inverted = false;
	cmd->mask.corner_radius = 0;
	pixman_region32_copy(&cmd->mask.region, frame_region);
	pixman_region32_init(&cmd->opaque_region);
	return 2;
}

/// Generate render command for the shadow in `layer`
///
/// @param[in] end the end of the commands generated for this `layer`.
static inline unsigned
command_for_shadow(struct layer *layer, struct backend_command *cmd,
                   const struct win_option *wintype_options,
                   const struct x_monitors *monitors, const struct backend_command *end) {
	auto w = layer->win;
	if (!w->shadow) {
		return 0;
	}
	cmd->op = BACKEND_COMMAND_BLIT;
	cmd->origin = layer->shadow_origin;
	cmd->source = BACKEND_COMMAND_SOURCE_SHADOW;
	// Initialize mask region in the current window's coordinates, we
	// will later move it to the correct coordinates
	pixman_region32_clear(&cmd->mask.region);
	pixman_region32_union_rect(
	    &cmd->mask.region, &cmd->mask.region, layer->shadow_origin.x - layer->origin.x,
	    layer->shadow_origin.y - layer->origin.y, (unsigned)layer->shadow_size.width,
	    (unsigned)layer->shadow_size.height);
	log_trace("Calculate shadow for %#010x (%s)", w->base.id, w->name);
	log_region(TRACE, &cmd->mask.region);
	if (!wintype_options[w->window_type].full_shadow) {
		// We need to not draw under the window
		// From this command up, until the next WINDOW_START
		// should be blits for the current window.
		for (auto j = cmd + 1; j != end; j++) {
			assert(j->op == BACKEND_COMMAND_BLIT);
			assert(j->source == BACKEND_COMMAND_SOURCE_WINDOW);
			assert(j->mask.origin.x == 0 && j->mask.origin.y == 0);
			if (j->blit.corner_radius == 0) {
				pixman_region32_subtract(
				    &cmd->mask.region, &cmd->mask.region, &j->mask.region);
			} else {
				region_t mask_without_corners;
				pixman_region32_init(&mask_without_corners);
				pixman_region32_copy(&mask_without_corners, &j->mask.region);
				win_region_remove_corners(layer->win, &mask_without_corners);
				pixman_region32_subtract(&cmd->mask.region, &cmd->mask.region,
				                         &mask_without_corners);
				pixman_region32_fini(&mask_without_corners);
			}
		}
	}
	log_region(TRACE, &cmd->mask.region);
	// Move mask region to screen coordinates for shadow exclusion
	// calculation
	pixman_region32_translate(&cmd->mask.region, layer->origin.x, layer->origin.y);
	if (monitors && w->randr_monitor >= 0 && w->randr_monitor < monitors->count) {
		pixman_region32_intersect(&cmd->mask.region, &cmd->mask.region,
		                          &monitors->regions[w->randr_monitor]);
	}
	log_region(TRACE, &cmd->mask.region);
	// Finally move mask region to the correct coordinates
	pixman_region32_translate(&cmd->mask.region, -layer->shadow_origin.x,
	                          -layer->shadow_origin.y);
	cmd->mask.corner_radius = w->corner_radius;
	cmd->mask.inverted = true;
	cmd->mask.origin = (struct coord){};
	cmd->need_mask_image = w->corner_radius > 0;
	if (cmd->need_mask_image) {
		// If we use the window's mask image, we need to align the
		// mask region's origin with it.
		cmd->mask.origin =
		    (struct coord){.x = layer->origin.x - layer->shadow_origin.x,
		                   .y = layer->origin.y - layer->shadow_origin.y};
		pixman_region32_translate(&cmd->mask.region, -cmd->mask.origin.x,
		                          -cmd->mask.origin.y);
	}
	log_region(TRACE, &cmd->mask.region);
	cmd->blit = (struct backend_blit_args){
	    .opacity = layer->opacity,
	    .max_brightness = 1,
	    .mask = &cmd->mask,
	    .ewidth = layer->shadow_size.width,
	    .eheight = layer->shadow_size.height,
	};
	pixman_region32_init(&cmd->opaque_region);
	return 1;
}

static inline unsigned
command_for_blur(struct layer *layer, struct backend_command *cmd,
                 const region_t *frame_region, bool force_blend, bool blur_frame) {
	auto w = layer->win;
	auto mode = win_calc_mode(w);
	if (!w->blur_background || layer->blur_opacity == 0) {
		return 0;
	}
	cmd->op = BACKEND_COMMAND_BLUR;
	cmd->origin = (struct coord){};
	cmd->blur.opacity = layer->blur_opacity;
	cmd->blur.mask = &cmd->mask;
	cmd->mask.origin = (struct coord){.x = layer->origin.x, .y = layer->origin.y};
	cmd->need_mask_image = w->corner_radius > 0;
	cmd->mask.corner_radius = w->corner_radius;
	cmd->mask.inverted = false;
	if (force_blend || mode == WMODE_TRANS) {
		pixman_region32_copy(&cmd->mask.region, &w->bounding_shape);
	} else if (blur_frame && mode == WMODE_FRAME_TRANS) {
		pixman_region32_copy(&cmd->mask.region, frame_region);
	} else {
		return 0;
	}
	return 1;
}

static inline void
command_builder_apply_transparent_clipping(struct layout *layout, region_t *scratch_region) {
	// Going from top down, apply transparent-clipping
	if (layout->len == 0) {
		return;
	}

	pixman_region32_clear(scratch_region);
	auto end = &layout->commands[layout->number_of_commands - 1];
	auto begin = &layout->commands[layout->first_layer_start - 1];
	auto layer = &layout->layers[layout->len - 1];
	// `layer_start` is one before the first command for this layer
	auto layer_start = end - layer->number_of_commands;
	for (auto i = end; i != begin; i--) {
		if (i == layer_start) {
			if (layer->win->transparent_clipping) {
				auto win = layer->win;
				auto mode = win_calc_mode(layer->win);
				region_t tmp;
				pixman_region32_init(&tmp);
				if (mode == WMODE_TRANS) {
					pixman_region32_copy(&tmp, &win->bounding_shape);
				} else if (mode == WMODE_FRAME_TRANS) {
					win_get_region_frame_local(win, &tmp);
				}
				pixman_region32_translate(&tmp, layer->origin.x,
				                          layer->origin.y);
				pixman_region32_union(scratch_region, scratch_region, &tmp);
				pixman_region32_fini(&tmp);
			}
			layer -= 1;
			layer_start -= layer->number_of_commands;
		}

		if (i->op == BACKEND_COMMAND_BLUR ||
		    (i->op == BACKEND_COMMAND_BLIT &&
		     i->source != BACKEND_COMMAND_SOURCE_BACKGROUND)) {
			struct coord scratch_origin = {
			    .x = -i->origin.x - i->mask.origin.x,
			    .y = -i->origin.y - i->mask.origin.y,
			};
			region_subtract(&i->mask.region, scratch_origin, scratch_region);
		}
		if (i->op == BACKEND_COMMAND_BLIT &&
		    i->source != BACKEND_COMMAND_SOURCE_BACKGROUND) {
			pixman_region32_subtract(&i->opaque_region, &i->opaque_region,
			                         scratch_region);
		}
	}
}
static inline void
command_builder_apply_shadow_clipping(struct layout *layout, region_t *scratch_region) {
	// Going from bottom up, apply clipping-shadow-above
	pixman_region32_clear(scratch_region);
	auto begin = &layout->commands[layout->first_layer_start];
	auto end = &layout->commands[layout->number_of_commands];
	auto layer = layout->layers - 1;
	// `layer_end` is one after the last command for this layer
	auto layer_end = begin;
	bool clip_shadow_above = false;
	for (auto i = begin; i != end; i++) {
		if (i == layer_end) {
			layer += 1;
			layer_end += layer->number_of_commands;
			clip_shadow_above = layer->win->clip_shadow_above;
		}

		struct coord mask_origin = {
		    .x = i->mask.origin.x + i->origin.x,
		    .y = i->mask.origin.y + i->origin.y,
		};
		if (i->op == BACKEND_COMMAND_BLUR) {
			region_subtract(scratch_region, mask_origin, &i->mask.region);
		} else if (i->op == BACKEND_COMMAND_BLIT) {
			if (i->source == BACKEND_COMMAND_SOURCE_SHADOW) {
				mask_origin.x = -mask_origin.x;
				mask_origin.y = -mask_origin.y;
				region_subtract(&i->mask.region, mask_origin, scratch_region);
			} else if (i->source == BACKEND_COMMAND_SOURCE_WINDOW &&
			           clip_shadow_above) {
				region_union(scratch_region, mask_origin, &i->mask.region);
			}
		}
	}
}

struct command_builder {
	region_t scratch_region;
	struct list_node free_command_lists;
};

struct command_list {
	struct list_node free_list;
	unsigned capacity;
	struct command_builder *super;
	struct backend_command commands[];
};

static struct command_list *
command_builder_command_list_new(struct command_builder *cb, unsigned ncmds) {
	const auto size = sizeof(struct command_list) + sizeof(struct backend_command[ncmds]);
	struct command_list *list = NULL;
	unsigned capacity = 0;
	if (!list_is_empty(&cb->free_command_lists)) {
		list = list_entry(cb->free_command_lists.next, struct command_list, free_list);
		capacity = list->capacity;
		list_remove(&list->free_list);
	}
	if (capacity < ncmds || capacity / 2 > ncmds) {
		for (unsigned i = ncmds; i < capacity; i++) {
			pixman_region32_fini(&list->commands[i].mask.region);
		}

		struct command_list *new_list = realloc(list, size);
		allocchk(new_list);
		list = new_list;
		list_init_head(&list->free_list);
		list->capacity = ncmds;
		list->super = cb;

		for (unsigned i = capacity; i < ncmds; i++) {
			list->commands[i].op = BACKEND_COMMAND_INVALID;
			pixman_region32_init(&list->commands[i].mask.region);
		}
	}
	return list;
}

void command_builder_command_list_free(struct backend_command *cmds) {
	if (!cmds) {
		return;
	}

	auto list = container_of(cmds, struct command_list, commands[0]);
	for (unsigned i = 0; i < list->capacity; i++) {
		auto cmd = &list->commands[i];
		if (cmd->op == BACKEND_COMMAND_BLIT) {
			pixman_region32_fini(&cmd->opaque_region);
		}
		cmd->op = BACKEND_COMMAND_INVALID;
	}
	list_insert_after(&list->super->free_command_lists, &list->free_list);
}

struct command_builder *command_builder_new(void) {
	auto cb = ccalloc(1, struct command_builder);
	pixman_region32_init(&cb->scratch_region);
	list_init_head(&cb->free_command_lists);
	return cb;
}

void command_builder_free(struct command_builder *cb) {
	list_foreach_safe(struct command_list, i, &cb->free_command_lists, free_list) {
		list_remove(&i->free_list);
		for (unsigned j = 0; j < i->capacity; j++) {
			pixman_region32_fini(&i->commands[j].mask.region);
		}
		free(i);
	}

	pixman_region32_fini(&cb->scratch_region);
	free(cb);
}

// TODO(yshui) reduce the number of parameters by storing the final effective parameter
// value in `struct managed_win`.
void command_builder_build(struct command_builder *cb, struct layout *layout, bool force_blend,
                           bool blur_frame, bool inactive_dim_fixed, double max_brightness,
                           double inactive_dim, const struct x_monitors *monitors,
                           const struct win_option *wintype_options) {

	unsigned ncmds = 1;
	for (unsigned i = 0; i < layout->len; i++) {
		auto layer = &layout->layers[i];
		auto mode = win_calc_mode(layer->win);
		if (layer->win->blur_background && layer->blur_opacity > 0 &&
		    (force_blend || mode == WMODE_TRANS ||
		     (blur_frame && mode == WMODE_FRAME_TRANS))) {
			// Needs blur
			ncmds += 1;
		}
		if (layer->win->shadow) {
			ncmds += 1;
		}
		if (layer->win->frame_opacity < 1 && layer->win->frame_opacity > 0) {
			// Needs to draw the frame separately
			ncmds += 1;
		}
		ncmds += 1;        // window body
	}

	auto list = command_builder_command_list_new(cb, ncmds);
	layout->commands = list->commands;

	auto cmd = &layout->commands[ncmds - 1];
	for (int i = to_int_checked(layout->len) - 1; i >= 0; i--) {
		auto layer = &layout->layers[i];
		auto frame_region = win_get_region_frame_local_by_val(layer->win);
		auto last = cmd;

		// Add window body
		cmd -= commands_for_window_body(layer, cmd, &frame_region, inactive_dim_fixed,
		                                inactive_dim, max_brightness);

		// Add shadow
		cmd -= command_for_shadow(layer, cmd, wintype_options, monitors, last + 1);

		// Add blur
		cmd -= command_for_blur(layer, cmd, &frame_region, force_blend, blur_frame);

		layer->number_of_commands = (unsigned)(last - cmd);
		pixman_region32_fini(&frame_region);
	}

	// Command for the desktop background
	cmd->op = BACKEND_COMMAND_COPY_AREA;
	cmd->source = BACKEND_COMMAND_SOURCE_BACKGROUND;
	cmd->origin = (struct coord){};
	pixman_region32_reset(
	    &cmd->mask.region,
	    (rect_t[]){{.x1 = 0, .y1 = 0, .x2 = layout->size.width, .y2 = layout->size.height}});
	cmd->copy_area.region = &cmd->mask.region;
	assert(cmd == list->commands);

	layout->first_layer_start = 1;
	layout->number_of_commands = ncmds;

	command_builder_apply_transparent_clipping(layout, &cb->scratch_region);
	command_builder_apply_shadow_clipping(layout, &cb->scratch_region);
}
