// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <pixman.h>
#include <stdint.h>
#include <xcb/xproto.h>

#include <picom/types.h>

#include "config.h"
#include "region.h"
#include "wm/wm.h"

/// A layer to be rendered in a render layout
struct layer {
	/// Window that will be rendered in this layer
	wm_treeid key;
	/// The window, this is only valid for the current layout. Once
	/// a frame has passed, windows could have been freed.
	struct win *win;
	struct window_options options;
	/// Damaged region of this layer, in screen coordinates
	region_t damaged;
	/// Window rectangle in screen coordinates, before it's scaled.
	struct ibox window;
	/// Shadow rectangle in screen coordinates, before it's scaled.
	struct ibox shadow;
	/// Scale of the window. The origin of scaling is the top left corner of the
	/// window.
	vec2 scale;
	/// Scale of the shadow. The origin of scaling is the top left corner of the
	/// shadow.
	vec2 shadow_scale;
	/// Opacity of this window
	float opacity;
	/// Opacity of the background blur of this window
	float blur_opacity;
	/// Opacity of this window's shadow
	float shadow_opacity;
	/// How much the image of this window should be blended with the saved image
	float saved_image_blend;
	/// Crop the content of this layer to this box, in screen coordinates.
	struct ibox crop;

	/// How many commands are needed to render this layer
	unsigned number_of_commands;

	/// Rank of this layer in the previous frame, -1 if this window
	/// appears in this frame for the first time
	int prev_rank;
	/// Rank of this layer in the next frame, -1 if this window is
	/// removed in the next frame
	int next_rank;

	/// Is this window completely opaque?
	bool is_opaque;

	// TODO(yshui) make opaqueness/blur finer grained maybe? to support
	// things like blur-background-frame
	// region_t opaque_region;
	// region_t blur_region;
};

/// Layout of windows at a specific frame
struct layout {
	ivec2 size;
	/// The root image generation, see `struct session::root_image_generation`
	uint64_t root_image_generation;
	/// Layers as a flat array, from bottom to top in stack order. This is a dynarr.
	struct layer *layers;
	/// Number of commands in `commands`
	unsigned number_of_commands;
	/// Where does the commands for the bottom most layer start.
	/// Any commands before that is for the desktop background.
	unsigned first_layer_start;
	/// Commands that are needed to render this layout. Commands
	/// are recorded in the same order as the layers they correspond to. Each layer
	/// can have 0 or more commands associated with it.
	struct backend_command *commands;
};

struct wm;
struct layout_manager;

/// Compute the layout of windows to be rendered in the current frame, and append it to
/// the end of layout manager's ring buffer.  The layout manager has a ring buffer of
/// layouts, with its size chosen at creation time. Calling this will push at new layout
/// at the end of the ring buffer, and remove the oldest layout if the buffer is full.
void layout_manager_append_layout(struct layout_manager *lm, struct wm *wm,
                                  uint64_t root_image_generation, ivec2 size);
/// Get the layout `age` frames into the past. Age `0` is the most recently appended
/// layout.
struct layout *layout_manager_layout(struct layout_manager *lm, unsigned age);
void layout_manager_free(struct layout_manager *lm);
/// Create a new render lm with a ring buffer for `max_buffer_age` layouts.
struct layout_manager *layout_manager_new(unsigned max_buffer_age);
/// Collect damage from the window for the past `buffer_age` frames.
void layout_manager_collect_window_damage(const struct layout_manager *lm, unsigned index,
                                          unsigned buffer_age, region_t *damage);
/// Find where layer at `index` was `buffer_age` frames ago.
int layer_prev_rank(struct layout_manager *lm, unsigned buffer_age, unsigned index_);
/// Find layer that was at `index` `buffer_age` aga in the current layout.
int layer_next_rank(struct layout_manager *lm, unsigned buffer_age, unsigned index_);
unsigned layout_manager_max_buffer_age(const struct layout_manager *lm);
