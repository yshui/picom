// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include "renderer.h"

#include <inttypes.h>
#include <picom/backend.h>
#include <xcb/xcb_aux.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "command_builder.h"
#include "damage.h"
#include "layout.h"
#include "picom.h"
#include "utils/dynarr.h"

/// Cached pre-blurred shadow atlas for one corner radius. The atlas is the
/// shadow mask of a minimal prototype window; any larger shadow is composed
/// exactly from it with nine blits (4 corners at 1:1, 4 edges and the center
/// stretched from translation-invariant bands). See
/// `renderer_compose_shadow_nine_slice`.
struct shadow_atlas_entry {
	unsigned corner_radius;
	image_handle image;
};

#define SHADOW_ATLAS_CACHE_SIZE 8

struct renderer {
	/// Intermediate image to hold what will be presented to the back buffer.
	image_handle back_image;
	/// 1x1 white image
	image_handle white_image;
	/// 1x1 black image
	image_handle black_image;
	/// 1x1 image with the monitor repaint color
	image_handle monitor_repaint_pixel;
	/// Copy of back images before they were tainted by monitor repaint
	image_handle *monitor_repaint_copy;
	/// Regions painted over by monitor repaint
	region_t *monitor_repaint_region;
	/// Copy of the entire back buffer
	image_handle *back_buffer_copy;
	/// Current frame index in ring buffer
	int frame_index;
	int max_buffer_age;
	ivec2 canvas_size;
	/// Format to use for back_image and intermediate images
	enum backend_image_format format;
	int shadow_radius;
	void *shadow_blur_context;
	struct conv *shadow_kernel;
	/// Shadow atlases keyed by corner radius (shadow radius is fixed per
	/// renderer). Tiny LRU-less cache; distinct corner radii per config are
	/// few in practice.
	struct shadow_atlas_entry shadow_atlas[SHADOW_ATLAS_CACHE_SIZE];

	/// A dynarr of region_t for storing culled masks
	region_t *culled_masks;
};

void renderer_free(struct backend_base *backend, struct renderer *r) {
	if (r->white_image) {
		backend->ops.release_image(backend, r->white_image);
	}
	if (r->black_image) {
		backend->ops.release_image(backend, r->black_image);
	}
	if (r->back_image) {
		backend->ops.release_image(backend, r->back_image);
	}
	if (r->monitor_repaint_pixel) {
		backend->ops.release_image(backend, r->monitor_repaint_pixel);
	}
	if (r->shadow_blur_context) {
		backend->ops.destroy_blur_context(backend, r->shadow_blur_context);
	}
	if (r->shadow_kernel) {
		free_conv(r->shadow_kernel);
	}
	for (size_t i = 0; i < SHADOW_ATLAS_CACHE_SIZE; i++) {
		if (r->shadow_atlas[i].image != NULL) {
			backend->ops.release_image(backend, r->shadow_atlas[i].image);
			r->shadow_atlas[i].image = NULL;
		}
	}
	if (r->monitor_repaint_region) {
		for (int i = 0; i < r->max_buffer_age; i++) {
			pixman_region32_fini(&r->monitor_repaint_region[i]);
		}
		free(r->monitor_repaint_region);
	}
	if (r->monitor_repaint_copy) {
		for (int i = 0; i < r->max_buffer_age; i++) {
			backend->ops.release_image(backend, r->monitor_repaint_copy[i]);
		}
		free(r->monitor_repaint_copy);
	}
	dynarr_free(r->culled_masks, pixman_region32_fini);
	free(r);
}

static bool renderer_init(struct renderer *renderer, struct backend_base *backend,
                          double shadow_radius, bool dithered_present) {
	auto has_high_precision =
	    backend->ops.is_format_supported(backend, BACKEND_IMAGE_FORMAT_PIXMAP_HIGH);
	renderer->format = has_high_precision && dithered_present
	                       ? BACKEND_IMAGE_FORMAT_PIXMAP_HIGH
	                       : BACKEND_IMAGE_FORMAT_PIXMAP;
	renderer->back_image = NULL;
	renderer->white_image =
	    backend->ops.new_image(backend, renderer->format, (ivec2){1, 1});
	if (!renderer->white_image || !backend->ops.clear(backend, renderer->white_image,
	                                                  (struct color){1, 1, 1, 1})) {
		return false;
	}
	renderer->black_image =
	    backend->ops.new_image(backend, renderer->format, (ivec2){1, 1});
	if (!renderer->black_image || !backend->ops.clear(backend, renderer->black_image,
	                                                  (struct color){0, 0, 0, 1})) {
		return false;
	}
	renderer->canvas_size = (ivec2){0, 0};
	if (shadow_radius > 0) {
		struct gaussian_blur_args args = {
		    .size = (int)shadow_radius,
		    .deviation = gaussian_kernel_std_for_size(shadow_radius, 0.5 / 256.0),
		};
		renderer->shadow_blur_context = backend->ops.create_blur_context(
		    backend, BLUR_METHOD_GAUSSIAN, BACKEND_IMAGE_FORMAT_MASK,
		    (struct blur_args *)&args);
		if (!renderer->shadow_blur_context) {
			log_error("Failed to create shadow blur context");
			return false;
		}
		renderer->shadow_radius = (int)shadow_radius;
		renderer->shadow_kernel = gaussian_kernel_autodetect_deviation(shadow_radius);
		if (!renderer->shadow_kernel) {
			log_error("Failed to create common shadow context");
			return false;
		}
		sum_kernel_preprocess(renderer->shadow_kernel);
	}
	renderer->max_buffer_age = backend->ops.max_buffer_age(backend) + 1;
	renderer->culled_masks = dynarr_new(region_t, 0);
	return true;
}

struct renderer *
renderer_new(struct backend_base *backend, double shadow_radius, bool dithered_present) {
	auto renderer = ccalloc(1, struct renderer);
	if (!renderer_init(renderer, backend, shadow_radius, dithered_present)) {
		renderer_free(backend, renderer);
		return NULL;
	}

	return renderer;
}

static inline bool
renderer_set_root_size(struct renderer *r, struct backend_base *backend, ivec2 root_size) {
	if (r->canvas_size.width == root_size.width &&
	    r->canvas_size.height == root_size.height) {
		return true;
	}
	if (r->back_image) {
		backend->ops.release_image(backend, r->back_image);
	}
	if (r->back_buffer_copy) {
		for (int i = 0; i < r->max_buffer_age; i++) {
			backend->ops.release_image(backend, r->back_buffer_copy[i]);
		}
		free(r->back_buffer_copy);
		r->back_buffer_copy = NULL;
	}
	if (r->monitor_repaint_copy) {
		for (int i = 0; i < r->max_buffer_age; i++) {
			backend->ops.release_image(backend, r->monitor_repaint_copy[i]);
		}
		free(r->monitor_repaint_copy);
		r->monitor_repaint_copy = NULL;
	}
	r->back_image = backend->ops.new_image(backend, r->format, root_size);
	if (r->back_image != NULL) {
		r->canvas_size = root_size;
		return true;
	}
	r->canvas_size = (ivec2){0, 0};
	return false;
}

/// The size of the window's rendered content box: its geometry, clamped to
/// the bound image while a grow's rebind is in flight. Must match the layer
/// clamp in `layer_from_window` — decoration (shadow, shape mask) is composed
/// at this size, and win_bind_pending_pixmap releases both when the bound
/// size changes.
static inline ivec2 win_effective_content_size(const struct win *w) {
	ivec2 size = {.width = w->widthb, .height = w->heightb};
	if (w->win_image_size.width > 0 && w->win_image_size.height > 0) {
		size.width = min2(size.width, w->win_image_size.width);
		size.height = min2(size.height, w->win_image_size.height);
	}
	return size;
}

static bool
renderer_bind_mask(struct renderer *r, struct backend_base *backend, struct win *w) {
	ivec2 size = win_effective_content_size(w);
	bool succeeded = false;
	auto image = backend->ops.new_image(backend, BACKEND_IMAGE_FORMAT_MASK, size);
	if (!image || !backend->ops.clear(backend, image, (struct color){0, 0, 0, 0})) {
		log_error("Failed to create mask image");
		goto err;
	}

	auto bound_region_local = win_get_bounding_shape_global_by_val(w);
	pixman_region32_translate(&bound_region_local, -w->g.x, -w->g.y);
	pixman_region32_intersect_rect(&bound_region_local, &bound_region_local, 0, 0,
	                               (uint)size.width, (uint)size.height);
	succeeded = backend->ops.copy_area(backend, (ivec2){0, 0}, (image_handle)image,
	                                   r->white_image, &bound_region_local);
	pixman_region32_fini(&bound_region_local);
	if (!succeeded) {
		log_error("Failed to fill the mask");
		goto err;
	}
	w->mask_image = image;
	image = NULL;

err:
	if (image != NULL) {
		backend->ops.release_image(backend, image);
	}
	return succeeded;
}

image_handle
renderer_shadow_mask_from_shape_mask(struct renderer *r, struct backend_base *backend,
                                     image_handle mask, unsigned int corner_radius,
                                     ivec2 mask_size) {
	image_handle normalized_mask_image = NULL;
	bool succeeded = false;
	int radius = r->shadow_radius;

	log_trace("Generating shadow from mask, mask %p", mask);

	// Apply the properties on the mask image and blit the result into a larger
	// image, each side larger by `2 * radius` so there is space for blurring.
	normalized_mask_image = backend->ops.new_image(
	    backend, BACKEND_IMAGE_FORMAT_MASK,
	    (ivec2){mask_size.width + 2 * radius, mask_size.height + 2 * radius});
	if (!normalized_mask_image || !backend->ops.clear(backend, normalized_mask_image,
	                                                  (struct color){0, 0, 0, 0})) {
		log_error("Failed to create mask image");
		goto err;
	}
	{
		region_t target_mask;
		struct backend_mask_image mask_args = {
		    .image = mask,
		    .origin = {0, 0},
		    .corner_radius = corner_radius,
		    .inverted = false,
		};
		struct backend_blit_args args = {
		    .source_image = r->white_image,
		    .source_mask = &mask_args,
		    .target_mask = &target_mask,
		    .shader = NULL,
		    .color_inverted = false,
		    .effective_size = mask_size,
		    .tint = {1, 1, 1, 1},
		    .scale = SCALE_IDENTITY,
		    .corner_radius = 0,
		    .border_width = 0,
		    .max_brightness = 1,
		};
		pixman_region32_init_rect(&target_mask, radius, radius,
		                          (unsigned)mask_size.width,
		                          (unsigned)mask_size.height);
		succeeded = backend->ops.blit(backend, (ivec2){radius, radius},
		                              normalized_mask_image, &args);
		pixman_region32_fini(&target_mask);
		if (!succeeded) {
			log_error("Failed to blit for shadow generation");
			goto err;
		}
	}
	// Then we blur the normalized mask image
	if (r->shadow_blur_context != NULL) {
		region_t target_mask;
		struct backend_blur_args args = {
		    .source_image = normalized_mask_image,
		    .target_mask = &target_mask,
		    .opacity = 1,
		    .blur_context = r->shadow_blur_context,
		};
		pixman_region32_init_rect(&target_mask, 0, 0,
		                          (unsigned)(mask_size.width + 2 * radius),
		                          (unsigned)(mask_size.height + 2 * radius));
		succeeded =
		    backend->ops.blur(backend, (ivec2){0, 0}, normalized_mask_image, &args);
		pixman_region32_fini(&target_mask);
		if (!succeeded) {
			log_error("Failed to blur for shadow generation");
			goto err;
		}
	}

	return normalized_mask_image;

err:
	log_error("Failed to create shadow mask");
	if (normalized_mask_image) {
		backend->ops.release_image(backend, normalized_mask_image);
	}
	return NULL;
}

/// Get (or build) the shadow atlas for `corner_radius`. The atlas is the full
/// shadow mask of a prototype window of side `2 * (corner_radius + shadow
/// radius) + 1`: at that size the four `corner_radius + 2 * shadow_radius`
/// corner blocks are exact, and the 1px-wide bands between them along each
/// axis are translation-invariant (a Gaussian of radius r is unaffected by
/// geometry further than r away), so they can be stretched to any length.
static image_handle renderer_get_shadow_atlas(struct renderer *r, struct backend_base *backend,
                                              unsigned corner_radius) {
	struct shadow_atlas_entry *slot = NULL;
	for (size_t i = 0; i < SHADOW_ATLAS_CACHE_SIZE; i++) {
		if (r->shadow_atlas[i].image != NULL &&
		    r->shadow_atlas[i].corner_radius == corner_radius) {
			return r->shadow_atlas[i].image;
		}
		if (slot == NULL && r->shadow_atlas[i].image == NULL) {
			slot = &r->shadow_atlas[i];
		}
	}
	if (slot == NULL) {
		// Cache full; extremely unlikely (needs >8 distinct corner radii).
		// Caller falls back to full generation.
		return NULL;
	}

	int proto = 2 * ((int)corner_radius + r->shadow_radius) + 1;
	ivec2 proto_size = {.width = proto, .height = proto};

	// Rasterize the prototype's shape mask (rounded rect) then blur it, using
	// the exact same pipeline as full shadow generation so results match.
	auto proto_mask =
	    backend->ops.new_image(backend, BACKEND_IMAGE_FORMAT_MASK, proto_size);
	if (proto_mask == NULL ||
	    !backend->ops.clear(backend, proto_mask, (struct color){0, 0, 0, 0})) {
		if (proto_mask != NULL) {
			backend->ops.release_image(backend, proto_mask);
		}
		return NULL;
	}
	{
		region_t target;
		pixman_region32_init_rect(&target, 0, 0, (unsigned)proto, (unsigned)proto);
		struct backend_blit_args args = {
		    .source_image = r->white_image,
		    .target_mask = &target,
		    .effective_size = proto_size,
		    .tint = {1, 1, 1, 1},
		    .scale = SCALE_IDENTITY,
		    .corner_radius = (double)corner_radius,
		    .max_brightness = 1,
		};
		bool ok = backend->ops.blit(backend, (ivec2){0, 0}, proto_mask, &args);
		pixman_region32_fini(&target);
		if (!ok) {
			backend->ops.release_image(backend, proto_mask);
			return NULL;
		}
	}
	auto atlas =
	    renderer_shadow_mask_from_shape_mask(r, backend, proto_mask, 0, proto_size);
	backend->ops.release_image(backend, proto_mask);
	if (atlas == NULL) {
		return NULL;
	}
	slot->corner_radius = corner_radius;
	slot->image = atlas;
	return atlas;
}

/// Compose a `size`-sized shadow mask from the atlas with nine blits.
/// `size` is the shadow image size (window size + 2 * shadow radius).
static image_handle
renderer_compose_shadow_nine_slice(struct renderer *r, struct backend_base *backend,
                                   image_handle atlas, unsigned corner_radius, ivec2 size) {
	// Fixed block: everything within `fixed` of a prototype corner is copied
	// 1:1; the single-pixel band at offset `fixed` is stretched.
	int fixed = (int)corner_radius + 2 * r->shadow_radius;
	int atlas_side = 2 * ((int)corner_radius + r->shadow_radius) + 1 +
	                 2 * r->shadow_radius;        // proto + 2 * radius
	assert(fixed * 2 + 1 == atlas_side);

	if (size.width < atlas_side || size.height < atlas_side) {
		// Too small for slicing; caller falls back to full generation.
		return NULL;
	}

	auto out = backend->ops.new_image(backend, BACKEND_IMAGE_FORMAT_MASK, size);
	if (out == NULL) {
		return NULL;
	}

	// Nine pieces: source rect in atlas -> target rect in out. The blit API
	// expresses "source rect to target rect" as: target region = target rect,
	// origin = target position of the source image origin, scale = target
	// extent / source extent (applied around the origin).
	struct piece {
		int sx, sy, sw, sh;        // atlas rect
		int tx, ty, tw, th;        // target rect
	} pieces[9];
	int n = 0;
	int mid_t_w = size.width - 2 * fixed;         // stretched middle width
	int mid_t_h = size.height - 2 * fixed;        // stretched middle height
	int right_s = atlas_side - fixed;             // right/bottom fixed block start
	int right_t_x = size.width - fixed;
	int bottom_t_y = size.height - fixed;

	// corners (1:1)
	pieces[n++] = (struct piece){0, 0, fixed, fixed, 0, 0, fixed, fixed};
	pieces[n++] =
	    (struct piece){right_s, 0, fixed, fixed, right_t_x, 0, fixed, fixed};
	pieces[n++] =
	    (struct piece){0, right_s, fixed, fixed, 0, bottom_t_y, fixed, fixed};
	pieces[n++] = (struct piece){right_s,   right_s,    fixed, fixed,
	                             right_t_x, bottom_t_y, fixed, fixed};
	// edges (stretch one axis)
	pieces[n++] = (struct piece){fixed, 0, 1, fixed, fixed, 0, mid_t_w, fixed};
	pieces[n++] =
	    (struct piece){fixed, right_s, 1, fixed, fixed, bottom_t_y, mid_t_w, fixed};
	pieces[n++] = (struct piece){0, fixed, fixed, 1, 0, fixed, fixed, mid_t_h};
	pieces[n++] =
	    (struct piece){right_s, fixed, fixed, 1, right_t_x, fixed, fixed, mid_t_h};
	// center (stretch both)
	pieces[n++] = (struct piece){fixed, fixed, 1, 1, fixed, fixed, mid_t_w, mid_t_h};

	for (int i = 0; i < n; i++) {
		auto p = &pieces[i];
		if (p->tw <= 0 || p->th <= 0) {
			continue;
		}
		region_t target;
		pixman_region32_init_rect(&target, p->tx, p->ty, (unsigned)p->tw,
		                          (unsigned)p->th);
		vec2 scale = {.x = (double)p->tw / p->sw, .y = (double)p->th / p->sh};
		// origin: target coords where the atlas's (0,0) would land, such
		// that atlas pixel (sx, sy) maps to target (tx, ty) under `scale`
		// (the blit derives source coords as (target - origin) / scale).
		ivec2 origin = {
		    .x = p->tx - (int)((double)p->sx * scale.x),
		    .y = p->ty - (int)((double)p->sy * scale.y),
		};
		struct backend_blit_args args = {
		    .source_image = atlas,
		    .target_mask = &target,
		    .effective_size = {.width = (int)(atlas_side * scale.x),
		                       .height = (int)(atlas_side * scale.y)},
		    .tint = {1, 1, 1, 1},
		    .scale = scale,
		    .max_brightness = 1,
		};
		bool ok = backend->ops.blit(backend, origin, out, &args);
		pixman_region32_fini(&target);
		if (!ok) {
			backend->ops.release_image(backend, out);
			return NULL;
		}
	}
	return out;
}

static bool
renderer_bind_shadow(struct renderer *r, struct backend_base *backend, struct win *w) {
	auto content_size = win_effective_content_size(w);
	if (backend->ops.quirks(backend) & BACKEND_QUIRK_SLOW_BLUR) {
		ivec2 shadow_size;
		int shadow_stride;
		uint8_t *shadow_pixels = make_shadow(
		    backend->c, r->shadow_kernel, content_size, &shadow_size, &shadow_stride);
		if (!shadow_pixels) {
			log_error("Couldn't generate shadow");
			return false;
		}

		w->shadow_mask = backend->ops.new_image_from_pixels(
		    backend, BACKEND_IMAGE_FORMAT_MASK, shadow_size, shadow_stride,
		    shadow_pixels);
		free(shadow_pixels);
	} else {
		auto corner_radius = (unsigned)win_options(w).corner_radius;
		// The shadow shape only depends on the window outline. If that is a
		// plain rectangle (note: WMs like i3 stamp rectangular bounding
		// shapes on ordinary windows, so check the region, not the shaped
		// flag), compose the shadow from a cached pre-blurred atlas with
		// nine blits instead of running a full Gaussian over the window
		// area. Exact for any window at least as large as the prototype.
		auto shape_extents = pixman_region32_extents(&w->bounding_shape);
		bool rectangular = pixman_region32_n_rects(&w->bounding_shape) == 1 &&
		                   shape_extents->x1 == 0 && shape_extents->y1 == 0 &&
		                   shape_extents->x2 >= content_size.width &&
		                   shape_extents->y2 >= content_size.height;
		if (rectangular) {
			auto atlas = renderer_get_shadow_atlas(r, backend, corner_radius);
			if (atlas != NULL) {
				// Shadow image spans the content box + margins; use the
				// effective content size so the falloff hugs the drawn
				// decoration during a grow's rebind window.
				ivec2 shadow_size = {
				    .width = w->shadow_width - w->widthb + content_size.width,
				    .height = w->shadow_height - w->heightb + content_size.height,
				};
				w->shadow_mask = renderer_compose_shadow_nine_slice(
				    r, backend, atlas, corner_radius, shadow_size);
			}
		}
		if (w->shadow_mask == NULL) {
			// Shaped window, tiny window, or atlas failure: full path.
			if (!w->mask_image && !renderer_bind_mask(r, backend, w)) {
				return false;
			}
			w->shadow_mask = renderer_shadow_mask_from_shape_mask(
			    r, backend, w->mask_image, win_options(w).corner_radius,
			    content_size);
		}
	}
	if (!w->shadow_mask) {
		log_error("Failed to create shadow");
		return false;
	}

	return true;
}

/// Go through the list of commands and replace symbolic image references with real
/// images. Allocate images for windows when necessary.
static bool renderer_prepare_commands(struct renderer *r, struct backend_base *backend,
                                      void *blur_context, struct layout *layout) {
	auto end = &layout->commands[layout->number_of_commands];
	auto cmds = layout->commands;
	// These assertions are the limitation of this renderer. If we expand its
	// capabilities, we might remove these.
	assert(cmds[0].source == BACKEND_COMMAND_SOURCE_CLEAR);
	assert(cmds[0].op == BACKEND_COMMAND_COPY_AREA);
	cmds[0].copy_area.source_image = r->black_image;

	auto layer = layout->layers - 1;
	auto layer_end = &cmds[layout->first_layer_start];
	for (auto cmd = layer_end; cmd != end; cmd++) {
		if (cmd == layer_end) {
			layer += 1;
			assert(layer->number_of_commands > 0);
			layer_end = cmd + layer->number_of_commands;
			log_trace("Prepare commands for layer %#010x @ %#010x (%s)",
			          win_id(layer->win), win_client_id(layer->win, false),
			          layer->win->name);
		}

		auto w = layer->win;
		switch (cmd->op) {
		case BACKEND_COMMAND_BLIT:
			if (cmd->source == BACKEND_COMMAND_SOURCE_SHADOW) {
				if (w->shadow_mask == NULL &&
				    !renderer_bind_shadow(r, backend, w)) {
					log_error("failed to bind shadow for window %s",
					          w->name);
					return false;
				}
				cmd->blit.source_image = w->shadow_mask;
			} else if (cmd->source == BACKEND_COMMAND_SOURCE_WINDOW) {
				assert(w->win_image);
				cmd->blit.source_image = w->win_image;
			} else if (cmd->source == BACKEND_COMMAND_SOURCE_WINDOW_SAVED) {
				assert(w->saved_win_image);
				cmd->blit.source_image = w->saved_win_image;
			}
			if (cmd->blit.source_mask != NULL) {
				if (w->mask_image == NULL &&
				    !renderer_bind_mask(r, backend, w)) {
					return false;
				}
				cmd->source_mask.image = w->mask_image;
			}
			break;
		case BACKEND_COMMAND_BLUR:
			cmd->blur.blur_context = blur_context;
			cmd->blur.source_image = r->back_image;
			if (cmd->blur.source_mask != NULL) {
				if (w->mask_image == NULL &&
				    !renderer_bind_mask(r, backend, w)) {
					return false;
				}
				cmd->source_mask.image = w->mask_image;
			}
			break;
		default:
		case BACKEND_COMMAND_COPY_AREA:
		case BACKEND_COMMAND_INVALID: assert(false);
		}
	}
	return true;
}

void renderer_ensure_images_ready(struct renderer *r, struct backend_base *backend,
                                  bool monitor_repaint) {
	if (monitor_repaint) {
		if (!r->monitor_repaint_pixel) {
			r->monitor_repaint_pixel = backend->ops.new_image(
			    backend, BACKEND_IMAGE_FORMAT_PIXMAP, (ivec2){1, 1});
			BUG_ON(!r->monitor_repaint_pixel);
			backend->ops.clear(backend, r->monitor_repaint_pixel,
			                   (struct color){.alpha = 0.5, .red = 0.5});
		}
		if (!r->monitor_repaint_copy) {
			r->monitor_repaint_copy = ccalloc(r->max_buffer_age, image_handle);
			for (int i = 0; i < r->max_buffer_age; i++) {
				r->monitor_repaint_copy[i] = backend->ops.new_image(
				    backend, BACKEND_IMAGE_FORMAT_PIXMAP,
				    (ivec2){.width = r->canvas_size.width,
				            .height = r->canvas_size.height});
				BUG_ON(!r->monitor_repaint_copy[i]);
			}
		}
		if (!r->monitor_repaint_region) {
			r->monitor_repaint_region = ccalloc(r->max_buffer_age, region_t);
			for (int i = 0; i < r->max_buffer_age; i++) {
				pixman_region32_init(&r->monitor_repaint_region[i]);
			}
		}
	}
	if (global_debug_options.consistent_buffer_age && !r->back_buffer_copy) {
		r->back_buffer_copy = ccalloc(r->max_buffer_age, image_handle);
		for (int i = 0; i < r->max_buffer_age; i++) {
			r->back_buffer_copy[i] =
			    backend->ops.new_image(backend, BACKEND_IMAGE_FORMAT_PIXMAP,
			                           (ivec2){.width = r->canvas_size.width,
			                                   .height = r->canvas_size.height});
			BUG_ON(!r->back_buffer_copy[i]);
		}
	}
}

/// @return true if a frame is rendered, false if this frame is skipped.
bool renderer_render(struct renderer *r, struct backend_base *backend,
                     image_handle root_image, const rect_t *root_image_extent,
                     struct layout_manager *lm, struct command_builder *cb,
                     void *blur_context, uint64_t render_start_us,
                     xcb_sync_fence_t xsync_fence, bool use_damage, bool monitor_repaint,
                     bool force_blend, bool blur_frame, bool inactive_dim_fixed,
                     double max_brightness, const struct x_monitors *monitors,
                     const struct shader_info *root_pixmap_shader,
                     const struct shader_info *shaders, uint64_t *after_damage_us) {
	if (xsync_fence != XCB_NONE) {
		// Trigger the fence but don't immediately wait on it. Let it run
		// concurrent with our CPU tasks to save time.
		x_set_error_action_abort(
		    backend->c, xcb_sync_trigger_fence(backend->c->c, xsync_fence));
	}
	// TODO(yshui) In some cases we can render directly into the back buffer, and
	// don't need the intermediate back_image. Several conditions need to be met: no
	// dithered present; no blur, with blur we will render areas that's just for blur
	// and can't be presented;
	auto layout = layout_manager_layout(lm, 0);
	if (!renderer_set_root_size(r, backend,
	                            (ivec2){layout->size.width, layout->size.height})) {
		log_error("Failed to allocate back image");
		return false;
	}

	renderer_ensure_images_ready(r, backend, monitor_repaint);

	command_builder_build(cb, layout, force_blend, blur_frame, inactive_dim_fixed,
	                      max_brightness, monitors, root_image, root_image_extent,
	                      root_pixmap_shader, shaders);
	if (log_get_level_tls() <= LOG_LEVEL_TRACE) {
		auto layer = layout->layers - 1;
		auto layer_end = &layout->commands[layout->first_layer_start];
		auto end = &layout->commands[layout->number_of_commands];
		log_trace("Desktop background");
		for (auto i = layout->commands; i != end; i++) {
			if (i == layer_end) {
				layer += 1;
				layer_end += layer->number_of_commands;
				log_trace("Layer for window %#010x @ %#010x (%s)",
				          win_id(layer->win),
				          win_client_id(layer->win, false), layer->win->name);
			}
			log_backend_command(TRACE, *i);
		}
	}
	region_t screen_region, damage_region;
	pixman_region32_init_rect(&screen_region, 0, 0, (unsigned)r->canvas_size.width,
	                          (unsigned)r->canvas_size.height);
	pixman_region32_init(&damage_region);
	pixman_region32_copy(&damage_region, &screen_region);
	ivec2 blur_size = {};
	if (backend->ops.get_blur_size && blur_context) {
		backend->ops.get_blur_size(blur_context, &blur_size.width, &blur_size.height);
	}
	auto buffer_age =
	    (use_damage || monitor_repaint) ? backend->ops.buffer_age(backend) : 0;
	if (buffer_age > 0 && global_debug_options.consistent_buffer_age &&
	    buffer_age < r->max_buffer_age) {
		int past_frame =
		    (r->frame_index + r->max_buffer_age - buffer_age) % r->max_buffer_age;
		region_t region;
		pixman_region32_init_rect(&region, 0, 0, (unsigned)r->canvas_size.width,
		                          (unsigned)r->canvas_size.height);
		backend->ops.copy_area(backend, (ivec2){}, backend->ops.back_buffer(backend),
		                       r->back_buffer_copy[past_frame], &region);
		pixman_region32_fini(&region);
	}
	if (buffer_age > 0 && (unsigned)buffer_age <= layout_manager_max_buffer_age(lm)) {
		layout_manager_damage(lm, (unsigned)buffer_age, blur_size, &damage_region);
	}

	dynarr_resize(r->culled_masks, layout->number_of_commands, pixman_region32_init,
	              pixman_region32_fini);
	commands_cull_with_damage(layout, &damage_region, blur_size, r->culled_masks);

	auto now = get_time_timespec();
	*after_damage_us = (uint64_t)now.tv_sec * 1000000UL + (uint64_t)now.tv_nsec / 1000;
	log_trace("Getting damage took %" PRIu64 " us", *after_damage_us - render_start_us);

	if (!renderer_prepare_commands(r, backend, blur_context, layout)) {
		log_error("Failed to prepare render commands");
		return false;
	}

	if (xsync_fence != XCB_NONE) {
		x_set_error_action_abort(
		    backend->c, xcb_sync_await_fence(backend->c->c, 1, &xsync_fence));
		// Making sure the wait is completed by receiving a response from the X
		// server
		xcb_aux_sync(backend->c->c);
		x_set_error_action_abort(
		    backend->c, xcb_sync_reset_fence(backend->c->c, xsync_fence));
	}

	if (backend->ops.prepare) {
		backend->ops.prepare(backend, &layout->commands[0].target_mask);
	}

	if (monitor_repaint && buffer_age > 0 && buffer_age <= r->max_buffer_age) {
		// Restore the area of back buffer that was tainted by monitor repaint
		int past_frame =
		    (r->frame_index + r->max_buffer_age - buffer_age) % r->max_buffer_age;
		backend->ops.copy_area(backend, (ivec2){}, backend->ops.back_buffer(backend),
		                       r->monitor_repaint_copy[past_frame],
		                       &r->monitor_repaint_region[past_frame]);
	}

	if (!backend_execute(backend, r->back_image, layout->number_of_commands,
	                     layout->commands)) {
		log_error("Failed to complete execution of the render commands");
		return false;
	}

	if (monitor_repaint) {
		// Keep a copy of un-tainted back image
		backend->ops.copy_area(backend, (ivec2){},
		                       r->monitor_repaint_copy[r->frame_index],
		                       r->back_image, &damage_region);
		pixman_region32_copy(&r->monitor_repaint_region[r->frame_index], &damage_region);

		struct backend_blit_args blit = {
		    .source_image = r->monitor_repaint_pixel,
		    .max_brightness = 1,
		    .tint = {1, 1, 1, 1},
		    .effective_size = r->canvas_size,
		    .source_mask = NULL,
		    .target_mask = &damage_region,
		    .scale = SCALE_IDENTITY,
		};
		log_trace("Blit for monitor repaint");
		backend->ops.blit(backend, (ivec2){}, r->back_image, &blit);
	}

	backend->ops.copy_area_quantize(backend, (ivec2){}, backend->ops.back_buffer(backend),
	                                r->back_image, &damage_region);

	if (global_debug_options.consistent_buffer_age) {
		region_t region;
		pixman_region32_init_rect(&region, 0, 0, (unsigned)r->canvas_size.width,
		                          (unsigned)r->canvas_size.height);
		backend->ops.copy_area(backend, (ivec2){}, r->back_buffer_copy[r->frame_index],
		                       backend->ops.back_buffer(backend), &region);
		pixman_region32_fini(&region);
	}

	if (backend->ops.present && !backend->ops.present(backend)) {
		log_warn("Failed to present the frame");
	}

	// "Un-cull" the render commands, so later damage calculation using those commands
	// will not use culled regions.
	commands_uncull(layout);

	pixman_region32_fini(&screen_region);
	pixman_region32_fini(&damage_region);

	r->frame_index = (r->frame_index + 1) % r->max_buffer_age;
	return true;
}
