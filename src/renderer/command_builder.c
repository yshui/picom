// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <picom/backend.h>
#include <picom/types.h>

#include "backend/backend.h"
#include "common.h"
#include "layout.h"
#include "utils/dynarr.h"
#include "wm/win.h"

#include "command_builder.h"

/// Generate commands for rendering the body of the window in `layer`.
///
/// @param[in]  frame_region frame region of the window, in window local coordinates
/// @param[out] cmd          output commands, when multiple commands are generated,
///                          it's stored in `cmd` going backwards, i.e. cmd - 1, -2, ...
/// @return                  number of commands generated
static inline unsigned
commands_for_window_body(struct layer *layer, struct backend_command *cmd_base,
                         const region_t *frame_region, bool inactive_dim_fixed, bool force_blend,
                         double max_brightness, const struct shader_info *shaders) {
	auto w = layer->win;
	auto cmd = cmd_base;
	scoped_region_t crop = region_from_box(layer->crop);
	auto mode = win_calc_mode_raw(layer->win);
	int border_width = w->g.border_width;
	double dim = layer->options.dim;
	if (!inactive_dim_fixed) {
		dim *= layer->opacity;
	}
	if (border_width == 0) {
		// Some WM has borders implemented as WM frames
		border_width = min3(w->frame_extents.left, w->frame_extents.right,
		                    w->frame_extents.bottom);
	}
	pixman_region32_copy(&cmd->target_mask, &w->bounding_shape);
	pixman_region32_translate(&cmd->target_mask, layer->window.origin.x,
	                          layer->window.origin.y);
	if (w->frame_opacity < 1) {
		pixman_region32_subtract(&cmd->target_mask, &cmd->target_mask, frame_region);
	}
	pixman_region32_init(&cmd->opaque_region);
	if ((mode == WMODE_SOLID || mode == WMODE_FRAME_TRANS) && layer->opacity == 1.0 &&
	    !force_blend) {
		pixman_region32_copy(&cmd->opaque_region, &cmd->target_mask);
		if (mode == WMODE_FRAME_TRANS) {
			pixman_region32_subtract(&cmd->opaque_region, &cmd->opaque_region,
			                         frame_region);
		}
	}
	if (layer->options.corner_radius > 0) {
		// Scale is applied below, by region_scale.
		win_remove_region_corners(layer->window.size, SCALE_IDENTITY,
		                          (int)layer->options.corner_radius,
		                          layer->window.origin, &cmd->opaque_region);
	}

	const struct shader_info *shader_info = NULL;
	if (layer->options.shader != NULL) {
		HASH_FIND(hh, shaders, layer->options.shader->data,
		          layer->options.shader->size, shader_info);
	}

	float opacity = layer->opacity * (1 - layer->saved_image_blend);
	if (opacity > (1. - 1. / MAX_ALPHA)) {
		// Avoid division by a very small number
		opacity = 1;
	}
	float opacity_saved = 0;
	if (opacity < 1) {
		opacity_saved = layer->opacity * layer->saved_image_blend / (1 - opacity);
	}
	struct color tint = {.red = 1. - dim, .green = 1. - dim, .blue = 1. - dim, .alpha = 1.};
	struct backend_blit_args args_base = {
	    .border_width = border_width,
	    .corner_radius = layer->options.corner_radius,
	    .tint = color_mult_alpha(tint, opacity),
	    .scale = layer->scale,
	    .effective_size = layer->window.size,
	    .shader = shader_info ? shader_info->backend_shader : NULL,
	    .color_inverted = layer->options.invert_color,
	    .source_mask = NULL,
	    .max_brightness = max_brightness,
	};
	region_scale(&cmd->target_mask, layer->window.origin, layer->scale);
	region_scale(&cmd->opaque_region, layer->window.origin, layer->scale);
	pixman_region32_intersect(&cmd->target_mask, &cmd->target_mask, &crop);
	pixman_region32_intersect(&cmd->opaque_region, &cmd->opaque_region, &crop);
	cmd->op = BACKEND_COMMAND_BLIT;
	cmd->source = BACKEND_COMMAND_SOURCE_WINDOW;
	cmd->origin = layer->window.origin;
	cmd->shader_info = shader_info;
	cmd->blit = args_base;
	cmd->blit.target_mask = &cmd->target_mask;
	cmd -= 1;
	if (layer->saved_image_blend > 0) {
		pixman_region32_copy(&cmd->target_mask, &cmd[1].target_mask);
		cmd->opaque_region = cmd[1].opaque_region;
		pixman_region32_init(&cmd[1].opaque_region);
		cmd->op = BACKEND_COMMAND_BLIT;
		cmd->shader_info = shader_info;
		cmd->source = BACKEND_COMMAND_SOURCE_WINDOW_SAVED;
		cmd->origin = layer->window.origin;
		cmd->blit = args_base;
		cmd->blit.effective_size = (ivec2){
		    .width = (int)(layer->window.size.width / w->saved_win_image_scale.width),
		    .height = (int)(layer->window.size.height / w->saved_win_image_scale.height),
		};
		cmd->blit.tint = color_mult_alpha(tint, opacity_saved);
		cmd->blit.target_mask = &cmd->target_mask;
		cmd->blit.scale = vec2_scale(cmd->blit.scale, w->saved_win_image_scale);
		cmd -= 1;
	}

	if (w->frame_opacity == 1 || w->frame_opacity == 0) {
		return (unsigned)(cmd_base - cmd);
	}

	opacity = (float)(opacity * w->frame_opacity);
	if (opacity > (1. - 1. / MAX_ALPHA)) {
		// Avoid division by a very small number
		opacity = 1;
	}
	opacity_saved = 0;
	if (opacity < 1) {
		opacity_saved = (float)(w->frame_opacity * layer->opacity *
		                        layer->saved_image_blend / (1 - opacity));
	}
	pixman_region32_copy(&cmd->target_mask, frame_region);
	region_scale(&cmd->target_mask, cmd->origin, layer->scale);
	pixman_region32_intersect(&cmd->target_mask, &cmd->target_mask, &crop);
	pixman_region32_init(&cmd->opaque_region);
	cmd->op = BACKEND_COMMAND_BLIT;
	cmd->origin = layer->window.origin;
	cmd->source = BACKEND_COMMAND_SOURCE_WINDOW;
	cmd->shader_info = shader_info;
	cmd->blit = args_base;
	cmd->blit.target_mask = &cmd->target_mask;
	cmd->blit.tint = color_mult_alpha(tint, opacity);
	cmd -= 1;
	if (layer->saved_image_blend > 0) {
		pixman_region32_copy(&cmd->target_mask, &cmd[1].target_mask);
		pixman_region32_init(&cmd->opaque_region);
		cmd->op = BACKEND_COMMAND_BLIT;
		cmd->source = BACKEND_COMMAND_SOURCE_WINDOW_SAVED;
		cmd->origin = layer->window.origin;
		cmd->shader_info = shader_info;
		cmd->blit = args_base;
		cmd->blit.effective_size = (ivec2){
		    .width = (int)(layer->window.size.width / w->saved_win_image_scale.width),
		    .height = (int)(layer->window.size.height / w->saved_win_image_scale.height),
		};
		cmd->blit.tint = color_mult_alpha(tint, opacity_saved);
		cmd->blit.target_mask = &cmd->target_mask;
		cmd->blit.scale = vec2_scale(cmd->blit.scale, w->saved_win_image_scale);
		cmd -= 1;
	}
	return (unsigned)(cmd_base - cmd);
}

/// Generate render command for the shadow in `layer`
///
/// @param[in] end the end of the commands generated for this `layer`.
static inline unsigned
command_for_shadow(struct layer *layer, struct backend_command *cmd,
                   const struct x_monitors *monitors, const struct backend_command *end) {
	auto w = layer->win;
	if (!layer->options.shadow) {
		return 0;
	}

	auto shadow_size_scaled = ivec2_scale_floor(layer->shadow.size, layer->shadow_scale);
	cmd->op = BACKEND_COMMAND_BLIT;
	cmd->origin = layer->shadow.origin;
	cmd->source = BACKEND_COMMAND_SOURCE_SHADOW;
	cmd->shader_info = NULL;
	pixman_region32_clear(&cmd->target_mask);
	pixman_region32_union_rect(&cmd->target_mask, &cmd->target_mask,
	                           layer->shadow.origin.x, layer->shadow.origin.y,
	                           (unsigned)shadow_size_scaled.width,
	                           (unsigned)shadow_size_scaled.height);
	log_trace("Calculate shadow for %#010x (%s)", win_id(w), w->name);
	log_region(TRACE, &cmd->target_mask);
	if (!layer->options.full_shadow) {
		// We need to not draw under the window
		// From this command up, until the next WINDOW_START
		// should be blits for the current window.
		for (auto j = cmd + 1; j != end; j++) {
			assert(j->op == BACKEND_COMMAND_BLIT);
			assert(j->source == BACKEND_COMMAND_SOURCE_WINDOW ||
			       j->source == BACKEND_COMMAND_SOURCE_WINDOW_SAVED);
			if (j->blit.corner_radius == 0) {
				pixman_region32_subtract(
				    &cmd->target_mask, &cmd->target_mask, &j->target_mask);
			} else {
				region_t mask_without_corners;
				pixman_region32_init(&mask_without_corners);
				pixman_region32_copy(&mask_without_corners, &j->target_mask);
				win_remove_region_corners(layer->window.size, layer->scale,
				                          (int)layer->options.corner_radius,
				                          j->origin, &mask_without_corners);
				pixman_region32_subtract(&cmd->target_mask, &cmd->target_mask,
				                         &mask_without_corners);
				pixman_region32_fini(&mask_without_corners);
			}
		}
	}
	log_region(TRACE, &cmd->target_mask);
	if (monitors) {
		auto monitor_index = win_find_monitor(monitors, w);
		if (monitor_index >= 0) {
			pixman_region32_intersect(&cmd->target_mask, &cmd->target_mask,
			                          &monitors->regions[monitor_index]);
		}
	}
	log_region(TRACE, &cmd->target_mask);
	if (layer->options.corner_radius > 0) {
		cmd->source_mask.corner_radius = layer->options.corner_radius;
		cmd->source_mask.inverted = true;
		// The offset between shadow and window is `window.origin -
		// shadow.origin`, in _screen_ coordinates. Since shadow will be scaled,
		// we need to divide by the scale to get the offset in source image
		// (i.e. shadow) coordinates.
		cmd->source_mask.origin = vec2_scale(
		    ivec2_as(ivec2_sub(layer->window.origin, layer->shadow.origin)),
		    vec2_reciprocal(layer->shadow_scale));
	}

	scoped_region_t crop = region_from_box(layer->crop);
	pixman_region32_intersect(&cmd->target_mask, &cmd->target_mask, &crop);

	cmd->blit = (struct backend_blit_args){
	    .tint = layer->shadow_color,
	    .max_brightness = 1,
	    .source_mask = layer->options.corner_radius > 0 ? &cmd->source_mask : NULL,
	    .scale = layer->shadow_scale,
	    .effective_size = layer->shadow.size,
	    .target_mask = &cmd->target_mask,
	};
	pixman_region32_init(&cmd->opaque_region);
	return 1;
}

static inline unsigned
command_for_blur(struct layer *layer, struct backend_command *cmd,
                 const region_t *frame_region, bool force_blend, bool blur_frame) {
	auto w = layer->win;
	auto mode = win_calc_mode_raw(w);
	if (!layer->options.blur_background || layer->blur_opacity == 0) {
		return 0;
	}
	if (force_blend || mode == WMODE_TRANS || layer->opacity < 1.0) {
		pixman_region32_copy(&cmd->target_mask, &w->bounding_shape);
		pixman_region32_translate(&cmd->target_mask, layer->window.origin.x,
		                          layer->window.origin.y);
	} else if (blur_frame && mode == WMODE_FRAME_TRANS) {
		pixman_region32_copy(&cmd->target_mask, frame_region);
	} else {
		return 0;
	}
	region_scale(&cmd->target_mask, layer->window.origin, layer->scale);

	scoped_region_t crop = region_from_box(layer->crop);
	pixman_region32_intersect(&cmd->target_mask, &cmd->target_mask, &crop);

	cmd->op = BACKEND_COMMAND_BLUR;
	cmd->origin = (ivec2){};
	if (layer->options.corner_radius > 0) {
		cmd->source_mask.origin = ivec2_as(layer->window.origin);
		cmd->source_mask.corner_radius = layer->options.corner_radius;
		cmd->source_mask.inverted = false;
	}
	cmd->blur = (struct backend_blur_args){
	    .opacity = layer->blur_opacity,
	    .target_mask = &cmd->target_mask,
	    .source_mask = layer->options.corner_radius > 0 ? &cmd->source_mask : NULL,
	    .source_mask_scale = layer->scale,
	};
	return 1;
}

static inline void
command_builder_apply_transparent_clipping(struct layout *layout, region_t *scratch_region) {
	// Going from top down, apply transparent-clipping
	if (dynarr_is_empty(layout->layers)) {
		return;
	}

	pixman_region32_clear(scratch_region);
	auto end = &layout->commands[layout->number_of_commands - 1];
	auto begin = &layout->commands[layout->first_layer_start - 1];
	auto layer = &dynarr_last(layout->layers);
	// `layer_start` is one before the first command for this layer
	auto layer_start = end - layer->number_of_commands;
	for (auto i = end; i != begin; i--) {
		if (i == layer_start) {
			if (layer->options.transparent_clipping) {
				auto win = layer->win;
				auto mode = win_calc_mode_raw(layer->win);
				region_t tmp;
				pixman_region32_init(&tmp);
				if (mode == WMODE_TRANS || layer->opacity < 1.0) {
					pixman_region32_copy(&tmp, &win->bounding_shape);
				} else if (mode == WMODE_FRAME_TRANS) {
					win_get_region_frame_local(win, &tmp);
				}
				pixman_region32_translate(&tmp, layer->window.origin.x,
				                          layer->window.origin.y);
				pixman_region32_union(scratch_region, scratch_region, &tmp);
				pixman_region32_fini(&tmp);
			}
			layer -= 1;
			layer_start -= layer->number_of_commands;
		}

		if (i->op == BACKEND_COMMAND_BLUR || i->op == BACKEND_COMMAND_BLIT) {
			pixman_region32_subtract(&i->target_mask, &i->target_mask,
			                         scratch_region);
		}
		if (i->op == BACKEND_COMMAND_BLIT) {
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
			clip_shadow_above = layer->options.clip_shadow_above;
		}

		if (i->op == BACKEND_COMMAND_BLUR) {
			pixman_region32_subtract(scratch_region, scratch_region,
			                         &i->target_mask);
		} else if (i->op == BACKEND_COMMAND_BLIT) {
			if (i->source == BACKEND_COMMAND_SOURCE_SHADOW) {
				pixman_region32_subtract(&i->target_mask, &i->target_mask,
				                         scratch_region);
			} else if (i->source == BACKEND_COMMAND_SOURCE_WINDOW &&
			           clip_shadow_above) {
				pixman_region32_union(scratch_region, scratch_region,
				                      &i->target_mask);
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
			pixman_region32_fini(&list->commands[i].target_mask);
		}

		struct command_list *new_list = realloc(list, size);
		allocchk(new_list);
		list = new_list;
		list_init_head(&list->free_list);
		list->capacity = ncmds;
		list->super = cb;

		for (unsigned i = capacity; i < ncmds; i++) {
			list->commands[i].op = BACKEND_COMMAND_INVALID;
			pixman_region32_init(&list->commands[i].target_mask);
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
			pixman_region32_fini(&i->commands[j].target_mask);
		}
		free(i);
	}

	pixman_region32_fini(&cb->scratch_region);
	free(cb);
}

// TODO(yshui) reduce the number of parameters by storing the final effective parameter
// value in `struct managed_win`.
void command_builder_build(struct command_builder *cb, struct layout *layout,
                           bool force_blend, bool blur_frame, bool inactive_dim_fixed,
                           double max_brightness, const struct x_monitors *monitors,
                           image_handle root_image, const rect_t *root_image_extent,
                           const struct shader_info *root_pixmap_shader,
                           const struct shader_info *shaders) {

	unsigned ncmds = root_image != NULL ? 2 : 1;
	dynarr_foreach(layout->layers, layer) {
		auto mode = win_calc_mode_raw(layer->win);
		if (layer->options.blur_background && layer->blur_opacity > 0 &&
		    (force_blend || mode == WMODE_TRANS || layer->opacity < 1.0 ||
		     (blur_frame && mode == WMODE_FRAME_TRANS))) {
			// Needs blur
			ncmds += 1;
		}
		if (layer->options.shadow) {
			ncmds += 1;
		}

		unsigned n_cmds_for_window_body = 1;
		if (layer->win->frame_opacity < 1 && layer->win->frame_opacity > 0) {
			// Needs to draw the frame separately
			n_cmds_for_window_body += 1;
		}
		if (layer->saved_image_blend > 0) {
			n_cmds_for_window_body *= 2;
		}
		ncmds += n_cmds_for_window_body;        // window body
	}

	auto list = command_builder_command_list_new(cb, ncmds);
	layout->commands = list->commands;

	auto cmd = &layout->commands[ncmds - 1];
	dynarr_foreach_rev(layout->layers, layer) {
		auto last = cmd;
		auto frame_region = win_get_region_frame_local_by_val(layer->win);
		pixman_region32_translate(&frame_region, layer->window.origin.x,
		                          layer->window.origin.y);

		// Add window body
		cmd -= commands_for_window_body(layer, cmd, &frame_region, inactive_dim_fixed,
		                                force_blend, max_brightness, shaders);

		// Add shadow
		cmd -= command_for_shadow(layer, cmd, monitors, last + 1);

		// Add blur
		cmd -= command_for_blur(layer, cmd, &frame_region, force_blend, blur_frame);

		layer->number_of_commands = (unsigned)(last - cmd);
		pixman_region32_fini(&frame_region);
	}

	layout->first_layer_start = (unsigned)(cmd + 1 - layout->commands);

	// Command for the desktop background
	const rect_t root_rect = {
	    .x1 = 0, .y1 = 0, .x2 = layout->size.width, .y2 = layout->size.height};

	if (root_image != NULL) {
		cmd->source = BACKEND_COMMAND_SOURCE_IMAGE;
		cmd->origin = (ivec2){};
		cmd->shader_info = root_pixmap_shader;
		pixman_region32_reset(&cmd->target_mask, root_image_extent);

		if (root_pixmap_shader == NULL) {
			// Just a simple copy_area
			cmd->op = BACKEND_COMMAND_COPY_AREA;
			cmd->copy_area.region = &cmd->target_mask;
			cmd->copy_area.source_image = root_image;
		} else {
			// Has shader for desktop background, use blit.
			cmd->op = BACKEND_COMMAND_BLIT;
			pixman_region32_init(&cmd->opaque_region);
			pixman_region32_copy(&cmd->opaque_region, &cmd->target_mask);
			cmd->blit = (struct backend_blit_args){
			    .tint = (struct color){1, 1, 1, 1},
			    .max_brightness = 1,
			    .scale = SCALE_IDENTITY,
			    .effective_size = layout->size,
			    .target_mask = &cmd->target_mask,
			    .shader = root_pixmap_shader->backend_shader,
			    .source_image = root_image,
			};
		}
		cmd--;
	}

	cmd->op = BACKEND_COMMAND_COPY_AREA;
	cmd->source = BACKEND_COMMAND_SOURCE_CLEAR;
	cmd->origin = (ivec2){};
	pixman_region32_reset(&cmd->target_mask, &root_rect);
	cmd->copy_area.region = &cmd->target_mask;
	assert(cmd == list->commands);

	layout->number_of_commands = ncmds;

	command_builder_apply_transparent_clipping(layout, &cb->scratch_region);
	command_builder_apply_shadow_clipping(layout, &cb->scratch_region);
}
