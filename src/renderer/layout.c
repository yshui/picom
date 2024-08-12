// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <stddef.h>
#include <uthash.h>

#include <picom/types.h>

#include "command_builder.h"
#include "common.h"
#include "region.h"
#include "utils/dynarr.h"
#include "utils/list.h"
#include "utils/misc.h"
#include "wm/win.h"
#include "wm/wm.h"

#include "layout.h"
struct layer_index {
	UT_hash_handle hh;
	wm_treeid key;
	unsigned index;
	struct list_node free_list;
};
struct layout_manager {
	unsigned max_buffer_age;
	/// Index of the most recent layout in `layouts`.
	unsigned current;
	/// Mapping from window to its index in the current layout.
	struct layer_index *layer_indices;
	struct list_node free_indices;

	// internal
	/// Scratch region used for calculations, to avoid repeated allocations.
	region_t scratch_region;
	/// Current and past layouts, at most `max_buffer_age` layouts are stored.
	struct layout layouts[];
};

/// Compute layout of a layer from a window. Returns false if the window is not
/// visible / should not be rendered. `out_layer` is modified either way.
static bool layer_from_window(struct layer *out_layer, struct win *w, ivec2 size) {
	bool to_paint = false;
	auto w_opts = win_options(w);
	if (!w->ever_damaged || !w_opts.paint) {
		goto out;
	}
	if (w->win_image == NULL) {
		goto out;
	}

	out_layer->options = w_opts;
	out_layer->scale = (vec2){
	    .x = win_animatable_get(w, WIN_SCRIPT_SCALE_X),
	    .y = win_animatable_get(w, WIN_SCRIPT_SCALE_Y),
	};
	out_layer->window.origin =
	    vec2_as((vec2){.x = w->g.x + win_animatable_get(w, WIN_SCRIPT_OFFSET_X),
	                   .y = w->g.y + win_animatable_get(w, WIN_SCRIPT_OFFSET_Y)});
	out_layer->window.size = vec2_as((vec2){.width = w->widthb, .height = w->heightb});
	out_layer->crop.origin = vec2_as((vec2){
	    .x = win_animatable_get(w, WIN_SCRIPT_CROP_X),
	    .y = win_animatable_get(w, WIN_SCRIPT_CROP_Y),
	});
	out_layer->crop.size = vec2_as((vec2){
	    .x = win_animatable_get(w, WIN_SCRIPT_CROP_WIDTH),
	    .y = win_animatable_get(w, WIN_SCRIPT_CROP_HEIGHT),
	});
	if (w_opts.shadow) {
		out_layer->shadow_scale = (vec2){
		    .x = win_animatable_get(w, WIN_SCRIPT_SHADOW_SCALE_X),
		    .y = win_animatable_get(w, WIN_SCRIPT_SHADOW_SCALE_Y),
		};
		out_layer->shadow.origin =
		    vec2_as((vec2){.x = w->g.x + w->shadow_dx +
		                        win_animatable_get(w, WIN_SCRIPT_SHADOW_OFFSET_X),
		                   .y = w->g.y + w->shadow_dy +
		                        win_animatable_get(w, WIN_SCRIPT_SHADOW_OFFSET_Y)});
		out_layer->shadow.size =
		    vec2_as((vec2){.width = w->shadow_width, .height = w->shadow_height});
	} else {
		out_layer->shadow.origin = (ivec2){};
		out_layer->shadow.size = (ivec2){};
		out_layer->shadow_scale = SCALE_IDENTITY;
	}

	struct ibox window_scaled = {
	    .origin = out_layer->window.origin,
	    .size = ivec2_scale_floor(out_layer->window.size, out_layer->scale),
	};
	struct ibox screen = {.origin = {0, 0}, .size = size};
	if (!ibox_overlap(window_scaled, screen) || !ibox_overlap(out_layer->crop, screen)) {
		goto out;
	}

	out_layer->opacity = (float)win_animatable_get(w, WIN_SCRIPT_OPACITY);
	out_layer->blur_opacity = (float)win_animatable_get(w, WIN_SCRIPT_BLUR_OPACITY);
	out_layer->shadow_opacity = (float)(win_animatable_get(w, WIN_SCRIPT_SHADOW_OPACITY) *
	                                    w->shadow_opacity * w->frame_opacity);
	if (out_layer->opacity == 0 && out_layer->blur_opacity == 0) {
		goto out;
	}

	out_layer->saved_image_blend =
	    (float)win_animatable_get(w, WIN_SCRIPT_SAVED_IMAGE_BLEND);
	if (w->saved_win_image == NULL) {
		out_layer->saved_image_blend = 0;
	}

	pixman_region32_copy(&out_layer->damaged, &w->damaged);
	pixman_region32_translate(&out_layer->damaged, out_layer->window.origin.x,
	                          out_layer->window.origin.y);
	// TODO(yshui) Is there a better way to handle shaped windows? Shaped windows can
	// have a very large number of rectangles in their shape, we don't want to handle
	// that and slow ourselves down. so we treat them as transparent and just use
	// their extent rectangle.
	out_layer->is_opaque =
	    !win_has_alpha(w) && out_layer->opacity == 1.0F && !w->bounding_shaped;
	out_layer->next_rank = -1;
	out_layer->prev_rank = -1;
	out_layer->key = wm_ref_treeid(w->tree_ref);
	out_layer->win = w;
	to_paint = true;

out:
	pixman_region32_clear(&w->damaged);
	return to_paint;
}

static void layer_deinit(struct layer *layer) {
	pixman_region32_fini(&layer->damaged);
}

static void layer_init(struct layer *layer) {
	pixman_region32_init(&layer->damaged);
}

static void layout_deinit(struct layout *layout) {
	dynarr_free(layout->layers, layer_deinit);
	command_builder_command_list_free(layout->commands);
	*layout = (struct layout){};
}

struct layout_manager *layout_manager_new(unsigned max_buffer_age) {
	struct layout_manager *planner = malloc(
	    sizeof(struct layout_manager) + (max_buffer_age + 1) * sizeof(struct layout));
	planner->max_buffer_age = max_buffer_age + 1;
	planner->current = 0;
	planner->layer_indices = NULL;
	list_init_head(&planner->free_indices);
	pixman_region32_init(&planner->scratch_region);
	for (unsigned i = 0; i <= max_buffer_age; i++) {
		planner->layouts[i] = (struct layout){};
		planner->layouts[i].layers = dynarr_new(struct layer, 5);
	}
	return planner;
}

void layout_manager_free(struct layout_manager *lm) {
	for (unsigned i = 0; i < lm->max_buffer_age; i++) {
		layout_deinit(&lm->layouts[i]);
	}
	struct layer_index *index, *tmp;
	HASH_ITER(hh, lm->layer_indices, index, tmp) {
		HASH_DEL(lm->layer_indices, index);
		free(index);
	}
	list_foreach_safe(struct layer_index, i, &lm->free_indices, free_list) {
		list_remove(&i->free_list);
		free(i);
	}
	pixman_region32_fini(&lm->scratch_region);
	free(lm);
}

// ## Layout manager Concepts
//
// - "layer", because windows form a stack, it's easy to think of the final screen as
//   a series of layers stacked on top of each other. Each layer is the same size as
//   the screen, and contains a single window positioned somewhere in the layer. Other
//   parts of the layer are transparent.
//   When talking about "screen at a certain layer", we mean the result you would get
//   if you stack all layers from the bottom up to that certain layer, ignoring any layers
//   above.

void layout_manager_append_layout(struct layout_manager *lm, struct wm *wm,
                                  uint64_t root_pixmap_generation, ivec2 size) {
	auto prev_layout = &lm->layouts[lm->current];
	lm->current = (lm->current + 1) % lm->max_buffer_age;
	auto layout = &lm->layouts[lm->current];
	command_builder_command_list_free(layout->commands);
	layout->root_image_generation = root_pixmap_generation;
	layout->size = size;

	unsigned rank = 0;
	struct layer_index *index, *next_index;
	wm_stack_foreach_rev(wm, cursor) {
		auto w = wm_ref_deref(cursor);
		if (w == NULL) {
			continue;
		}
		dynarr_resize(layout->layers, rank + 1, layer_init, layer_deinit);
		if (!layer_from_window(&layout->layers[rank], (struct win *)w, size)) {
			continue;
		}

		HASH_FIND(hh, lm->layer_indices, &layout->layers[rank].key,
		          sizeof(layout->layers[rank].key), index);
		if (index) {
			prev_layout->layers[index->index].next_rank = (int)rank;
			layout->layers[rank].prev_rank = (int)index->index;
		}
		rank++;
	}
	dynarr_truncate(layout->layers, rank, layer_deinit);

	// Update indices. If a layer exist in both prev_layout and current layout,
	// we could update the index using next_rank; if a layer no longer exist in
	// current layout, we remove it from the indices.
	HASH_ITER(hh, lm->layer_indices, index, next_index) {
		if (prev_layout->layers[index->index].next_rank == -1) {
			HASH_DEL(lm->layer_indices, index);
			list_insert_after(&lm->free_indices, &index->free_list);
		} else {
			index->index = (unsigned)prev_layout->layers[index->index].next_rank;
		}
	}
	// And finally, if a layer in current layout didn't exist in prev_layout, add a
	// new index for it.
	dynarr_foreach(layout->layers, layer) {
		if (layer->prev_rank != -1) {
			continue;
		}
		if (!list_is_empty(&lm->free_indices)) {
			index =
			    list_entry(lm->free_indices.next, struct layer_index, free_list);
			list_remove(&index->free_list);
		} else {
			index = cmalloc(struct layer_index);
		}
		index->key = layer->key;
		index->index = to_u32_checked(layer - layout->layers);
		HASH_ADD(hh, lm->layer_indices, key, sizeof(index->key), index);
	}
}

struct layout *layout_manager_layout(struct layout_manager *lm, unsigned age) {
	if (age >= lm->max_buffer_age) {
		assert(false);
		return NULL;
	}
	return &lm->layouts[(lm->current + lm->max_buffer_age - age) % lm->max_buffer_age];
}

void layout_manager_collect_window_damage(const struct layout_manager *lm, unsigned index,
                                          unsigned buffer_age, region_t *damage) {
	auto curr = lm->current;
	auto layer = &lm->layouts[curr].layers[index];
	for (unsigned i = 0; i < buffer_age; i++) {
		pixman_region32_union(damage, damage, &layer->damaged);
		curr = (curr + lm->max_buffer_age - 1) % lm->max_buffer_age;
		assert(layer->prev_rank >= 0);
		layer = &lm->layouts[curr].layers[layer->prev_rank];
	}
}

unsigned layout_manager_max_buffer_age(const struct layout_manager *lm) {
	return lm->max_buffer_age - 1;
}

int layer_prev_rank(struct layout_manager *lm, unsigned buffer_age, unsigned index_) {
	int index = to_int_checked(index_);
	unsigned layout = lm->current;
	while (buffer_age--) {
		index = lm->layouts[layout].layers[index].prev_rank;
		if (index < 0) {
			break;
		}
		layout = (layout + lm->max_buffer_age - 1) % lm->max_buffer_age;
	}
	return index;
}

int layer_next_rank(struct layout_manager *lm, unsigned buffer_age, unsigned index_) {
	int index = to_int_checked(index_);
	unsigned layout = (lm->current + lm->max_buffer_age - buffer_age) % lm->max_buffer_age;
	while (buffer_age--) {
		index = lm->layouts[layout].layers[index].next_rank;
		if (index < 0) {
			break;
		}
		layout = (layout + 1) % lm->max_buffer_age;
	}
	return index;
}
