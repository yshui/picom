// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include "renderer.h"

#include <inttypes.h>

#include "../picom.h"
#include "backend/backend.h"
#include "backend/backend_common.h"
#include "command_builder.h"
#include "damage.h"
#include "layout.h"

struct renderer {
	/// Intermediate image to hold what will be presented to the back buffer.
	image_handle back_image;
	/// 1x1 white image
	image_handle white_image;
	/// 1x1 black image
	image_handle black_image;
	/// 1x1 image with the monitor repaint color
	image_handle monitor_repaint_pixel;
	/// 1x1 shadow colored xrender picture
	xcb_render_picture_t shadow_pixel;
	struct geometry canvas_size;
	/// Format to use for back_image and intermediate images
	enum backend_image_format format;
	struct color shadow_color;
	int shadow_radius;
	void *shadow_blur_context;
	struct backend_shadow_context *common_shadow_context;
};

void renderer_free(struct backend_base *backend, struct renderer *r) {
	if (r->white_image) {
		backend->ops->v2.release_image(backend, r->white_image);
	}
	if (r->black_image) {
		backend->ops->v2.release_image(backend, r->black_image);
	}
	if (r->back_image) {
		backend->ops->v2.release_image(backend, r->back_image);
	}
	if (r->monitor_repaint_pixel) {
		backend->ops->v2.release_image(backend, r->monitor_repaint_pixel);
	}
	if (r->shadow_blur_context) {
		backend->ops->destroy_blur_context(backend, r->shadow_blur_context);
	}
	if (r->common_shadow_context) {
		default_destroy_shadow_context(backend, r->common_shadow_context);
	}
	if (r->shadow_pixel) {
		x_free_picture(backend->c, r->shadow_pixel);
	}
	free(r);
}

static bool
renderer_init(struct renderer *renderer, struct backend_base *backend,
              double shadow_radius, struct color shadow_color, bool dithered_present) {
	auto has_high_precision =
	    backend->ops->v2.is_format_supported(backend, BACKEND_IMAGE_FORMAT_PIXMAP_HIGH);
	renderer->format = has_high_precision && dithered_present
	                       ? BACKEND_IMAGE_FORMAT_PIXMAP_HIGH
	                       : BACKEND_IMAGE_FORMAT_PIXMAP;
	renderer->back_image = NULL;
	renderer->white_image =
	    backend->ops->v2.new_image(backend, renderer->format, (struct geometry){1, 1});
	if (!renderer->white_image || !backend->ops->v2.clear(backend, renderer->white_image,
	                                                      (struct color){1, 1, 1, 1})) {
		return false;
	}
	renderer->black_image =
	    backend->ops->v2.new_image(backend, renderer->format, (struct geometry){1, 1});
	if (!renderer->black_image || !backend->ops->v2.clear(backend, renderer->black_image,
	                                                      (struct color){0, 0, 0, 1})) {
		return false;
	}
	renderer->canvas_size = (struct geometry){0, 0};
	if (shadow_radius > 0) {
		struct gaussian_blur_args args = {
		    .size = (int)shadow_radius,
		    .deviation = gaussian_kernel_std_for_size(shadow_radius, 0.5 / 256.0),
		};
		renderer->shadow_blur_context = backend->ops->create_blur_context(
		    backend, BLUR_METHOD_GAUSSIAN, BACKEND_IMAGE_FORMAT_MASK, &args);
		if (!renderer->shadow_blur_context) {
			log_error("Failed to create shadow blur context");
			return false;
		}
		renderer->shadow_radius = (int)shadow_radius;
		renderer->shadow_color = shadow_color;
		renderer->shadow_pixel =
		    solid_picture(backend->c, true, shadow_color.alpha, shadow_color.red,
		                  shadow_color.green, shadow_color.blue);
		if (renderer->shadow_pixel == XCB_NONE) {
			log_error("Failed to create shadow pixel");
			return false;
		}
		renderer->common_shadow_context =
		    default_create_shadow_context(NULL, renderer->shadow_radius);
		if (!renderer->common_shadow_context) {
			log_error("Failed to create common shadow context");
			return false;
		}
	}
	return true;
}

struct renderer *renderer_new(struct backend_base *backend, double shadow_radius,
                              struct color shadow_color, bool dithered_present) {
	auto renderer = ccalloc(1, struct renderer);
	if (!renderer_init(renderer, backend, shadow_radius, shadow_color, dithered_present)) {
		renderer_free(backend, renderer);
		return NULL;
	}

	return renderer;
}

static inline bool renderer_set_root_size(struct renderer *r, struct backend_base *backend,
                                          struct geometry root_size) {
	if (r->canvas_size.width == root_size.width &&
	    r->canvas_size.height == root_size.height) {
		return true;
	}
	if (r->back_image) {
		backend->ops->v2.release_image(backend, r->back_image);
	}
	r->back_image = backend->ops->v2.new_image(backend, r->format, root_size);
	if (r->back_image != NULL) {
		r->canvas_size = root_size;
		return true;
	}
	r->canvas_size = (struct geometry){0, 0};
	return false;
}

static bool
renderer_bind_mask(struct renderer *r, struct backend_base *backend, struct managed_win *w) {
	struct geometry size = {.width = w->widthb, .height = w->heightb};
	bool succeeded = false;
	auto image = backend->ops->v2.new_image(backend, BACKEND_IMAGE_FORMAT_MASK, size);
	if (!image || !backend->ops->v2.clear(backend, image, (struct color){0, 0, 0, 0})) {
		log_error("Failed to create mask image");
		goto err;
	}

	auto bound_region_local = win_get_bounding_shape_global_by_val(w);
	pixman_region32_translate(&bound_region_local, -w->g.x, -w->g.y);
	succeeded =
	    backend->ops->v2.copy_area(backend, (struct coord){0, 0}, (image_handle)image,
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
		backend->ops->v2.release_image(backend, image);
	}
	return succeeded;
}

image_handle
renderer_shadow_from_mask(struct renderer *r, struct backend_base *backend,
                          image_handle mask, int corner_radius, struct geometry mask_size) {
	image_handle normalized_mask_image = NULL, shadow_image = NULL,
	             shadow_color_pixel = NULL;
	bool succeeded = false;
	int radius = r->shadow_radius;

	log_trace("Generating shadow from mask, mask %p, color (%f, %f, %f, %f)", mask,
	          r->shadow_color.red, r->shadow_color.green, r->shadow_color.blue,
	          r->shadow_color.alpha);

	// Apply the properties on the mask image and blit the result into a larger
	// image, each side larger by `2 * radius` so there is space for blurring.
	normalized_mask_image = backend->ops->v2.new_image(
	    backend, BACKEND_IMAGE_FORMAT_MASK,
	    (struct geometry){mask_size.width + 2 * radius, mask_size.height + 2 * radius});
	if (!normalized_mask_image || !backend->ops->v2.clear(backend, normalized_mask_image,
	                                                      (struct color){0, 0, 0, 0})) {
		log_error("Failed to create mask image");
		goto out;
	}
	{
		struct backend_mask mask_args = {
		    .image = mask,
		    .origin = {0, 0},
		    .corner_radius = corner_radius,
		    .inverted = false,
		};
		pixman_region32_init_rect(&mask_args.region, 0, 0, (unsigned)mask_size.width,
		                          (unsigned)mask_size.height);
		struct backend_blit_args args = {
		    .source_image = r->white_image,
		    .opacity = 1,
		    .mask = &mask_args,
		    .shader = NULL,
		    .color_inverted = false,
		    .ewidth = mask_size.width,
		    .eheight = mask_size.height,
		    .dim = 0,
		    .corner_radius = 0,
		    .border_width = 0,
		    .max_brightness = 1,
		};
		succeeded = backend->ops->v2.blit(backend, (struct coord){radius, radius},
		                                  normalized_mask_image, &args);
		pixman_region32_fini(&mask_args.region);
		if (!succeeded) {
			log_error("Failed to blit for shadow generation");
			goto out;
		}
	}
	// Then we blur the normalized mask image
	if (r->shadow_blur_context != NULL) {
		struct backend_mask mask_args = {
		    .image = NULL,
		    .origin = {0, 0},
		    .corner_radius = 0,
		    .inverted = false,
		};
		pixman_region32_init_rect(&mask_args.region, 0, 0,
		                          (unsigned)(mask_size.width + 2 * radius),
		                          (unsigned)(mask_size.height + 2 * radius));
		struct backend_blur_args args = {
		    .source_image = normalized_mask_image,
		    .opacity = 1,
		    .mask = &mask_args,
		    .blur_context = r->shadow_blur_context,
		};
		succeeded = backend->ops->v2.blur(backend, (struct coord){0, 0},
		                                  normalized_mask_image, &args);
		pixman_region32_fini(&mask_args.region);
		if (!succeeded) {
			log_error("Failed to blur for shadow generation");
			goto out;
		}
	}
	// Finally, we blit with this mask to colorize the shadow
	succeeded = false;
	shadow_image = backend->ops->v2.new_image(
	    backend, BACKEND_IMAGE_FORMAT_PIXMAP,
	    (struct geometry){mask_size.width + 2 * radius, mask_size.height + 2 * radius});
	if (!shadow_image ||
	    !backend->ops->v2.clear(backend, shadow_image, (struct color){0, 0, 0, 0})) {
		log_error("Failed to allocate shadow image");
		goto out;
	}

	shadow_color_pixel = backend->ops->v2.new_image(
	    backend, BACKEND_IMAGE_FORMAT_PIXMAP, (struct geometry){1, 1});
	if (!shadow_color_pixel ||
	    !backend->ops->v2.clear(backend, shadow_color_pixel, r->shadow_color)) {
		log_error("Failed to create shadow color image");
		goto out;
	}

	struct backend_mask mask_args = {
	    .image = (image_handle)normalized_mask_image,
	    .origin = {0, 0},
	    .corner_radius = 0,
	    .inverted = false,
	};
	pixman_region32_init_rect(&mask_args.region, 0, 0,
	                          (unsigned)(mask_size.width + 2 * radius),
	                          (unsigned)(mask_size.height + 2 * radius));
	struct backend_blit_args args = {
	    .source_image = shadow_color_pixel,
	    .opacity = 1,
	    .mask = &mask_args,
	    .shader = NULL,
	    .color_inverted = false,
	    .ewidth = mask_size.width + 2 * radius,
	    .eheight = mask_size.height + 2 * radius,
	    .dim = 0,
	    .corner_radius = 0,
	    .border_width = 0,
	    .max_brightness = 1,
	};
	succeeded = backend->ops->v2.blit(backend, (struct coord){0, 0}, shadow_image, &args);
	pixman_region32_fini(&mask_args.region);

out:
	if (normalized_mask_image) {
		backend->ops->v2.release_image(backend, normalized_mask_image);
	}
	if (shadow_color_pixel) {
		backend->ops->v2.release_image(backend, shadow_color_pixel);
	}
	if (!succeeded && shadow_image) {
		log_error("Failed to draw shadow image");
		backend->ops->v2.release_image(backend, shadow_image);
		shadow_image = NULL;
	}
	return shadow_image;
}

static bool renderer_bind_shadow(struct renderer *r, struct backend_base *backend,
                                 struct managed_win *w) {
	if (backend->ops->v2.quirks(backend) & BACKEND_QUIRK_SLOW_BLUR) {
		xcb_pixmap_t shadow = XCB_NONE;
		xcb_render_picture_t pict = XCB_NONE;

		if (!build_shadow(backend->c, r->shadow_color.alpha, w->widthb,
		                  w->heightb, (void *)r->common_shadow_context,
		                  r->shadow_pixel, &shadow, &pict)) {
			return false;
		}

		auto visual =
		    x_get_visual_for_standard(backend->c, XCB_PICT_STANDARD_ARGB_32);
		w->shadow_image = backend->ops->v2.bind_pixmap(
		    backend, shadow, x_get_visual_info(backend->c, visual));
	} else {
		if (!w->mask_image && !renderer_bind_mask(r, backend, w)) {
			return false;
		}
		w->shadow_image = renderer_shadow_from_mask(
		    r, backend, w->mask_image, w->corner_radius,
		    (struct geometry){.width = w->widthb, .height = w->heightb});
	}
	if (!w->shadow_image) {
		log_error("Failed to create shadow");
		return false;
	}
	return true;
}

/// Go through the list of commands and replace symbolic image references with real
/// images. Allocate images for windows when necessary.
static bool renderer_prepare_commands(struct renderer *r, struct backend_base *backend,
                                      void *blur_context, image_handle root_image,
                                      struct layout *layout) {
	auto end = &layout->commands[layout->number_of_commands];
	auto cmds = layout->commands;
	// These assertions are the limitation of this renderer. If we expand its
	// capabilities, we might remove these.
	assert(cmds[0].op == BACKEND_COMMAND_COPY_AREA &&
	       cmds[0].source == BACKEND_COMMAND_SOURCE_BACKGROUND);
	cmds[0].copy_area.source_image = root_image ?: r->black_image;
	assert(layout->first_layer_start == 1);

	auto layer = layout->layers - 1;
	auto layer_end = &layout->commands[layout->first_layer_start];
	for (auto cmd = &cmds[1]; cmd != end; cmd++) {
		if (cmd == layer_end) {
			layer += 1;
			assert(layer->number_of_commands > 0);
			layer_end = cmd + layer->number_of_commands;
			log_trace("Prepare commands for layer %#010x @ %#010x (%s)",
			          layer->win->base.id, layer->win->client_win,
			          layer->win->name);
		}

		auto w = layer->win;
		if (cmd->need_mask_image && w->mask_image == NULL &&
		    !renderer_bind_mask(r, backend, w)) {
			return false;
		}
		switch (cmd->op) {
		case BACKEND_COMMAND_BLIT:
			assert(cmd->source != BACKEND_COMMAND_SOURCE_BACKGROUND);
			if (cmd->source == BACKEND_COMMAND_SOURCE_SHADOW) {
				if (w->shadow_image == NULL &&
				    !renderer_bind_shadow(r, backend, w)) {
					return false;
				}
				cmd->blit.source_image = w->shadow_image;
			} else if (cmd->source == BACKEND_COMMAND_SOURCE_WINDOW) {
				assert(w->win_image);
				cmd->blit.source_image = w->win_image;
			}
			cmd->blit.mask->image = cmd->need_mask_image ? w->mask_image : NULL;
			break;
		case BACKEND_COMMAND_BLUR:
			cmd->blur.blur_context = blur_context;
			cmd->blur.source_image = r->back_image;
			cmd->blur.mask->image = cmd->need_mask_image ? w->mask_image : NULL;
			break;
		default:
		case BACKEND_COMMAND_COPY_AREA:
		case BACKEND_COMMAND_INVALID: assert(false);
		}
	}
	return true;
}

/// @return true if a frame is rendered, false if this frame is skipped.
bool renderer_render(struct renderer *r, struct backend_base *backend,
                     image_handle root_image, struct layout_manager *lm,
                     struct command_builder *cb, void *blur_context,
                     uint64_t render_start_us, bool use_damage attr_unused,
                     bool monitor_repaint, bool force_blend, bool blur_frame,
                     bool inactive_dim_fixed, double max_brightness, double inactive_dim,
                     const region_t *shadow_exclude, const struct x_monitors *monitors,
                     const struct win_option *wintype_options, uint64_t *after_damage_us) {
	auto layout = layout_manager_layout(lm, 0);
	if (!renderer_set_root_size(
	        r, backend, (struct geometry){layout->size.width, layout->size.height})) {
		log_error("Failed to allocate back image");
		return false;
	}

	if (monitor_repaint && r->monitor_repaint_pixel == NULL) {
		r->monitor_repaint_pixel = backend->ops->v2.new_image(
		    backend, BACKEND_IMAGE_FORMAT_PIXMAP, (struct geometry){1, 1});
		if (r->monitor_repaint_pixel) {
			backend->ops->v2.clear(backend, r->monitor_repaint_pixel,
			                       (struct color){.alpha = 0.5, .red = 0.5});
		}
	}

	command_builder_build(cb, layout, force_blend, blur_frame, inactive_dim_fixed,
	                      max_brightness, inactive_dim, shadow_exclude, monitors,
	                      wintype_options);
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
				          layer->win->base.id, layer->win->client_win,
				          layer->win->name);
			}
			log_backend_command(TRACE, *i);
		}
	}

	auto now = get_time_timespec();
	*after_damage_us = (uint64_t)now.tv_sec * 1000000UL + (uint64_t)now.tv_nsec / 1000;
	log_trace("Getting damage took %" PRIu64 " us", *after_damage_us - render_start_us);

	if (!renderer_prepare_commands(r, backend, blur_context, root_image, layout)) {
		log_error("Failed to prepare render commands");
		return false;
	}

	if (backend->ops->prepare) {
		backend->ops->prepare(backend, &layout->commands[0].mask.region);
	}

	if (!backend_execute(backend, r->back_image, layout->number_of_commands,
	                     layout->commands)) {
		log_error("Failed to complete execution of the render commands");
		return false;
	}

	region_t screen_region;
	pixman_region32_init_rect(&screen_region, 0, 0, (unsigned)r->canvas_size.width,
	                          (unsigned)r->canvas_size.height);
	if (monitor_repaint && r->monitor_repaint_pixel) {
		struct backend_mask mask = {};
		pixman_region32_init(&mask.region);
		pixman_region32_copy(&mask.region, &screen_region);
		auto buffer_age = backend->ops->buffer_age(backend);
		if (buffer_age > 0 &&
		    (unsigned)buffer_age <= layout_manager_max_buffer_age(lm)) {
			struct geometry blur_size = {};
			if (backend->ops->get_blur_size && blur_context) {
				backend->ops->get_blur_size(
				    blur_context, &blur_size.width, &blur_size.height);
			}
			layout_manager_damage(lm, (unsigned)buffer_age, blur_size, &mask.region);
		}
		struct backend_blit_args blit = {
		    .source_image = r->monitor_repaint_pixel,
		    .max_brightness = 1,
		    .opacity = 1,
		    .ewidth = r->canvas_size.width,
		    .eheight = r->canvas_size.height,
		    .mask = &mask,
		};
		log_trace("Blit for monitor repaint");
		backend->ops->v2.blit(backend, (struct coord){}, r->back_image, &blit);
		pixman_region32_fini(&mask.region);
	}

	if (backend->ops->present) {
		backend->ops->v2.copy_area_quantize(backend, (struct coord){},
		                                    backend->ops->v2.back_buffer(backend),
		                                    r->back_image, &screen_region);
		backend->ops->v2.present(backend);
	}
	pixman_region32_fini(&screen_region);
	return true;
}
