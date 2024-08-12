// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include "layout.h"
#include "region.h"
#include "utils/dynarr.h"
#include "wm/win.h"

#include "damage.h"

/// Compare two layers that contain the same window, return if they are the "same". Same
/// means these two layers are render in the same way at the same position, with the only
/// possible differences being the contents inside the window.
static bool
layer_compare(const struct layer *past_layer, const struct backend_command *past_layer_cmd,
              const struct layer *curr_layer, const struct backend_command *curr_layer_cmd) {
	if (!ibox_eq(past_layer->window, curr_layer->window)) {
		// Window moved or size changed
		return false;
	}

	// TODO(yshui) consider window body and shadow separately.
	if (!vec2_eq(past_layer->scale, curr_layer->scale) ||
	    !vec2_eq(past_layer->shadow_scale, curr_layer->shadow_scale)) {
		// Window or shadow scale changed
		return false;
	}

	if (!ibox_eq(past_layer->shadow, curr_layer->shadow)) {
		// Shadow moved or size changed
		return false;
	}
	if (past_layer->saved_image_blend != curr_layer->saved_image_blend) {
		// The amount of blending with the saved image changed
		return false;
	}
	if (past_layer->number_of_commands != curr_layer->number_of_commands) {
		// Number of render commands changed. We are being conservative
		// here, because even though the number of commands changed, we can still
		// try to match them up. For example, maybe this window just has shadow
		// disabled, but other commands are still the same. We are not do that
		// here, this could be a TODO
		// TODO(yshui) match render commands here
		return false;
	}

	for (unsigned i = 0; i < past_layer->number_of_commands; i++) {
		auto cmd1 = &past_layer_cmd[i];
		auto cmd2 = &curr_layer_cmd[i];
		if (cmd1->op != cmd2->op || !ivec2_eq(cmd1->origin, cmd2->origin) ||
		    cmd1->source != cmd2->source) {
			return false;
		}
	}
	return true;
}

/// Add all regions of `layer`'s commands to `region`
static inline void region_union_render_layer(region_t *region, const struct layer *layer,
                                             const struct backend_command *cmds) {
	for (auto i = cmds; i < &cmds[layer->number_of_commands]; i++) {
		pixman_region32_union(region, region, &i->target_mask);
	}
}

static inline void
command_blit_damage(region_t *damage, region_t *scratch_region, struct backend_command *cmd1,
                    struct backend_command *cmd2, const struct layout_manager *lm,
                    unsigned layer_index, unsigned buffer_age) {
	// clang-format off
	// First part, if any blit argument that would affect the whole image changed
	if (cmd1->blit.dim     != cmd2->blit.dim                   ||
	    cmd1->blit.shader  != cmd2->blit.shader                ||
	    cmd1->blit.opacity != cmd2->blit.opacity               ||
	    cmd1->blit.corner_radius  != cmd2->blit.corner_radius  ||
	    cmd1->blit.max_brightness != cmd2->blit.max_brightness ||
	    cmd1->blit.color_inverted != cmd2->blit.color_inverted ||

	    // Second part, if round corner is enabled, then border width and effective size
	    // affect the whole image too.
	    (cmd1->blit.corner_radius > 0 &&
	        (cmd1->blit.border_width != cmd2->blit.border_width ||
	         !ivec2_eq(cmd1->blit.effective_size, cmd2->blit.effective_size)))
	   )
	{
		pixman_region32_union(damage, damage, &cmd1->target_mask);
		pixman_region32_union(damage, damage, &cmd2->target_mask);
		return;
	}
	// clang-format on

	if (cmd1->blit.opacity == 0) {
		return;
	}

	// Damage from layers below that is covered up by the current layer, won't be
	// visible. So remove them.
	pixman_region32_subtract(damage, damage, &cmd2->opaque_region);
	region_symmetric_difference_local(damage, scratch_region, &cmd1->target_mask,
	                                  &cmd2->target_mask);
	if (cmd1->source == BACKEND_COMMAND_SOURCE_WINDOW) {
		layout_manager_collect_window_damage(lm, layer_index, buffer_age,
		                                     scratch_region);
		region_scale(scratch_region, cmd2->origin, cmd2->blit.scale);
		pixman_region32_intersect(scratch_region, scratch_region, &cmd1->target_mask);
		pixman_region32_intersect(scratch_region, scratch_region, &cmd2->target_mask);
		pixman_region32_union(damage, damage, scratch_region);
	}
}

static inline void command_blur_damage(region_t *damage, region_t *scratch_region,
                                       struct backend_command *cmd1,
                                       struct backend_command *cmd2, ivec2 blur_size) {
	if (cmd1->blur.opacity != cmd2->blur.opacity) {
		pixman_region32_union(damage, damage, &cmd1->target_mask);
		pixman_region32_union(damage, damage, &cmd2->target_mask);
		return;
	}
	if (cmd1->blur.opacity == 0) {
		return;
	}
	region_symmetric_difference_local(damage, scratch_region, &cmd1->target_mask,
	                                  &cmd2->target_mask);

	// We need to expand the damage region underneath the blur. Because blur
	// "diffuses" the changes from below.
	pixman_region32_copy(scratch_region, damage);
	resize_region_in_place(scratch_region, blur_size.width, blur_size.height);
	pixman_region32_intersect(scratch_region, scratch_region, &cmd2->target_mask);
	pixman_region32_union(damage, damage, scratch_region);
}

/// Do the first step of render planning, collecting damages and calculating which
/// parts of the final screen will be affected by the damages.
void layout_manager_damage(struct layout_manager *lm, unsigned buffer_age,
                           ivec2 blur_size, region_t *damage) {
	log_trace("Damage for buffer age %d", buffer_age);
	unsigned past_layer_rank = 0, curr_layer_rank = 0;
	auto past_layout = layout_manager_layout(lm, buffer_age);
	auto curr_layout = layout_manager_layout(lm, 0);
	auto past_layer = &past_layout->layers[past_layer_rank];
	auto curr_layer = &curr_layout->layers[curr_layer_rank];
	auto past_layer_cmd = &past_layout->commands[past_layout->first_layer_start];
	auto curr_layer_cmd = &curr_layout->commands[curr_layout->first_layer_start];
	region_t scratch_region;
	pixman_region32_init(&scratch_region);
	pixman_region32_clear(damage);
	if (past_layout->size.width != curr_layout->size.width ||
	    past_layout->size.height != curr_layout->size.height ||
	    past_layout->root_image_generation != curr_layout->root_image_generation) {
		pixman_region32_union_rect(damage, damage, 0, 0,
		                           (unsigned)curr_layout->size.width,
		                           (unsigned)curr_layout->size.height);
		return;
	}
	if (log_get_level_tls() <= LOG_LEVEL_TRACE) {
		log_trace("Comparing across %d layouts:", buffer_age);
		for (unsigned l = 0; l <= buffer_age; l++) {
			log_trace("Layout[%d]: ", -l);
			auto layout = layout_manager_layout(lm, l);
			dynarr_foreach(layout->layers, layer) {
				log_trace("\t%#010x %dx%d+%dx%d (prev %d, next %d)",
				          layer->key.x, layer->window.size.width,
				          layer->window.size.height,
				          layer->window.origin.x, layer->window.origin.y,
				          layer->prev_rank, layer->next_rank);
			}
		}
	}

	// Explanation of what's happening here. We want to get damage by comparing
	// `past_layout` and `curr_layout` But windows in them could be different. And
	// comparing different windows doesn't really make sense. So we want to "align"
	// the layouts so we compare matching windows and skip over non-matching ones. For
	// example, say past layout has window "ABCDE"; and in current layout, window C is
	// closed, and F is opened: "ABDFE", we want to align them like this:
	//    ABCD E
	//    AB DFE
	// Note there can be multiple ways of aligning windows, some of them are not
	// optimal. For example, in layout "ABCDEFG", if we move B to after F: "ACDEFBG",
	// we want to align them like this:
	//    ABCDEF G
	//    A CDEFBG
	// not like this:
	//    A    BCDEFG
	//    ACDEFB    G
	//
	// This is the classic Longest Common Sequence (LCS) problem, but we are not doing
	// a full LCS algorithm here. Since damage is calculated every frame, there is
	// likely not a lot of changes between the two layouts. We use a simple linear
	// time greedy approximation that should work well enough in those cases.

	for (;; past_layer_rank += 1, curr_layer_rank += 1,
	        past_layer_cmd += past_layer->number_of_commands,
	        curr_layer_cmd += curr_layer->number_of_commands, past_layer += 1,
	        curr_layer += 1) {
		int past_layer_curr_rank = -1, curr_layer_past_rank = -1;
		unsigned past_layer_rank_target = past_layer_rank,
		         curr_layer_rank_target = curr_layer_rank;
		log_region(TRACE, damage);

		// Skip layers in the past layout doesn't contain a window that has a
		// match in the remaining layers of the current layout; and vice versa.
		while (past_layer_rank_target < dynarr_len(past_layout->layers)) {
			past_layer_curr_rank =
			    layer_next_rank(lm, buffer_age, past_layer_rank_target);
			if (past_layer_curr_rank >= (int)curr_layer_rank) {
				break;
			}
			past_layer_rank_target++;
		};
		while (curr_layer_rank_target < dynarr_len(curr_layout->layers)) {
			curr_layer_past_rank =
			    layer_prev_rank(lm, buffer_age, curr_layer_rank_target);
			if (curr_layer_past_rank >= (int)past_layer_rank) {
				break;
			}
			curr_layer_rank_target++;
		};

		// past_layer_curr_rank/curr_layer_past_rank can be -1
		if (past_layer_curr_rank >= (int)curr_layer_rank ||
		    curr_layer_past_rank >= (int)past_layer_rank) {
			// Now past_layer_current_rank and current_layer_past_rank both
			// have a matching layer in the other layout. We check which side
			// has less layers to skip.
			assert((unsigned)curr_layer_past_rank >= past_layer_rank_target);
			assert((unsigned)past_layer_curr_rank >= curr_layer_rank_target);
			// How many layers will be skipped on either side if we move
			// past_layer_rank to past_layer_rank_target. And vice versa.
			auto skipped_using_past_target =
			    past_layer_rank_target - past_layer_rank +
			    ((unsigned)past_layer_curr_rank - curr_layer_rank);
			auto skipped_using_curr_target =
			    curr_layer_rank_target - curr_layer_rank +
			    ((unsigned)curr_layer_past_rank - past_layer_rank);
			if (skipped_using_curr_target < skipped_using_past_target) {
				past_layer_rank_target = (unsigned)curr_layer_past_rank;
			} else {
				curr_layer_rank_target = (unsigned)past_layer_curr_rank;
			}
		}

		// For the skipped layers, we need to add them to the damage region.
		for (; past_layer_rank < past_layer_rank_target; past_layer_rank++) {
			region_union_render_layer(damage, past_layer, past_layer_cmd);
			past_layer_cmd += past_layer->number_of_commands;
			past_layer += 1;
		}
		for (; curr_layer_rank < curr_layer_rank_target; curr_layer_rank++) {
			region_union_render_layer(damage, curr_layer, curr_layer_cmd);
			curr_layer_cmd += curr_layer->number_of_commands;
			curr_layer += 1;
		}

		if (past_layer_rank >= dynarr_len(past_layout->layers) ||
		    curr_layer_rank >= dynarr_len(curr_layout->layers)) {
			// No more matching layers left.
			assert(past_layer_rank >= dynarr_len(past_layout->layers) &&
			       curr_layer_rank >= dynarr_len(curr_layout->layers));
			break;
		}

		assert(wm_treeid_eq(past_layer->key, curr_layer->key));
		log_trace("%#010x == %#010x %s", past_layer->key.x, curr_layer->key.x,
		          curr_layer->win->name);

		if (!layer_compare(past_layer, past_layer_cmd, curr_layer, curr_layer_cmd)) {
			region_union_render_layer(damage, curr_layer, curr_layer_cmd);
			region_union_render_layer(damage, past_layer, past_layer_cmd);
			continue;
		}

		// Layers are otherwise identical besides the window content. We will
		// process their render command and add appropriate damage.
		log_trace("Adding window damage");
		for (struct backend_command *cmd1 = past_layer_cmd, *cmd2 = curr_layer_cmd;
		     cmd1 < past_layer_cmd + past_layer->number_of_commands; cmd1++, cmd2++) {
			switch (cmd1->op) {
			case BACKEND_COMMAND_BLIT:
				command_blit_damage(damage, &scratch_region, cmd1, cmd2,
				                    lm, curr_layer_rank, buffer_age);
				break;
			case BACKEND_COMMAND_BLUR:
				command_blur_damage(damage, &scratch_region, cmd1, cmd2,
				                    blur_size);
				break;
			default: assert(false);
			}
		}
	}
	pixman_region32_fini(&scratch_region);
}

void commands_cull_with_damage(struct layout *layout, const region_t *damage,
                               ivec2 blur_size, region_t *culled_mask) {
	// This may sound silly, and probably actually is. Why do GPU's job on the CPU?
	// Isn't the GPU supposed to be the one that does culling, depth testing etc.?
	//
	// Well, the things is the compositor is a bit special which makes this a bit
	// hard. First of all, each window is its own texture. If we bundle them in one
	// draw call, we might run into texture unit limits. If we don't bundle them,
	// then because we draw things bottom up, depth testing is pointless. Maybe we
	// can draw consecutive opaque windows top down with depth test, which will work
	// on OpenGL. But xrender won't like it. So that would be backend specific.
	//
	// Which is to say, there might be better way of utilizing the GPU for this, but
	// that will be complicated. And being a compositor makes doing this on CPU
	// easier, we only need to handle a dozen axis aligned rectangles, not hundreds of
	// thousands of triangles. So this is what we are stuck with for now.
	region_t scratch_region, tmp;
	pixman_region32_init(&scratch_region);
	pixman_region32_init(&tmp);
	// scratch_region stores the visible damage region of the screen at the current
	// layer. at the top most layer, all of damage is visible
	pixman_region32_copy(&scratch_region, damage);
	for (int i = to_int_checked(layout->number_of_commands - 1); i >= 0; i--) {
		auto cmd = &layout->commands[i];
		pixman_region32_copy(&culled_mask[i], &cmd->target_mask);
		pixman_region32_intersect(&culled_mask[i], &culled_mask[i], &scratch_region);
		switch (cmd->op) {
		case BACKEND_COMMAND_BLIT:
			pixman_region32_subtract(&scratch_region, &scratch_region,
			                         &cmd->opaque_region);
			cmd->blit.target_mask = &culled_mask[i];
			break;
		case BACKEND_COMMAND_COPY_AREA:
			pixman_region32_subtract(&scratch_region, &scratch_region,
			                         &cmd->target_mask);
			cmd->copy_area.region = &culled_mask[i];
			break;
		case BACKEND_COMMAND_BLUR:
			// To render blur, the layers below must render pixels surrounding
			// the blurred area in this layer.
			pixman_region32_copy(&tmp, &scratch_region);
			pixman_region32_intersect(&tmp, &tmp, &cmd->target_mask);
			resize_region_in_place(&tmp, blur_size.width, blur_size.height);
			pixman_region32_union(&scratch_region, &scratch_region, &tmp);
			cmd->blur.target_mask = &culled_mask[i];
			break;
		case BACKEND_COMMAND_INVALID: assert(false);
		}
	}
	pixman_region32_fini(&tmp);
	pixman_region32_fini(&scratch_region);
}

void commands_uncull(struct layout *layout) {
	for (auto i = layout->commands;
	     i != &layout->commands[layout->number_of_commands]; i++) {
		switch (i->op) {
		case BACKEND_COMMAND_BLIT: i->blit.target_mask = &i->target_mask; break;
		case BACKEND_COMMAND_BLUR: i->blur.target_mask = &i->target_mask; break;
		case BACKEND_COMMAND_COPY_AREA:
			i->copy_area.region = &i->target_mask;
			break;
		case BACKEND_COMMAND_INVALID: assert(false);
		}
	}
}
