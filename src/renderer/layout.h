// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <pixman.h>
#include <stdint.h>
#include <xcb/xproto.h>
#include "backend/backend.h"
#include "region.h"
#include "types.h"

struct layer_key {
	/// Window generation, (see `struct wm::generation` for explanation of what a
	/// generation is)
	uint64_t generation;
	/// Window ID
	xcb_window_t window;
	uint32_t pad;        // explicit padding because this will be used as hash table
	                     // key
};

static_assert(sizeof(struct layer_key) == 16, "layer_key has implicit padding");

/// A layer to be rendered in a render layout
struct layer {
	/// Window that will be rendered in this layer
	struct layer_key key;
	/// The window, this is only valid for the current layout. Once
	/// a frame has passed, windows could have been freed.
	struct managed_win *win;
	/// Damaged region of this layer, in screen coordinates
	region_t damaged;
	/// Origin (the top left outmost corner) of the window in screen coordinates
	struct coord origin;
	/// Size of the window
	struct geometry size;
	/// Origin of the shadow in screen coordinates
	struct coord shadow_origin;
	/// Size of the shadow
	struct geometry shadow_size;
	/// Opacity of this window
	float opacity;
	/// Opacity of the background blur of this window
	float blur_opacity;

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
	/// Is this window clipping the windows beneath it?
	bool is_clipping;

	// TODO(yshui) make opaqueness/blur finer grained maybe? to support
	// things like blur-background-frame
	// region_t opaque_region;
	// region_t blur_region;

	// TODO(yshui) support cropping
	/// x and y offset for cropping. Anything to the top or
	/// left of the crop point will be cropped out.
	// uint32_t crop_x, crop_y;
};

/// Layout of windows at a specific frame
struct layout {
	struct geometry size;
	/// The root image generation, see `struct session::root_image_generation`
	uint64_t root_image_generation;
	/// Number of layers in `layers`
	unsigned len;
	/// Capacity of `layers`
	unsigned capacity;
	/// Layers as a flat array, from bottom to top in stack order.
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
                                  uint64_t root_image_generation, struct geometry size);
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
