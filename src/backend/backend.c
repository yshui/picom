// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <inttypes.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>

#include "backend/backend.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "renderer/command_builder.h"
#include "renderer/layout.h"
#include "transition.h"
#include "types.h"
#include "win.h"
#include "x.h"

extern struct backend_operations xrender_ops, dummy_ops;
#ifdef CONFIG_OPENGL
extern struct backend_operations glx_ops;
extern struct backend_operations egl_ops;
#endif

struct backend_operations *backend_list[NUM_BKEND] = {
    [BKEND_XRENDER] = &xrender_ops,
    [BKEND_DUMMY] = &dummy_ops,
#ifdef CONFIG_OPENGL
    [BKEND_GLX] = &glx_ops,
    [BKEND_EGL] = &egl_ops,
#endif
};

/**
 * @param all_damage if true ignore damage and repaint the whole screen
 */
region_t get_damage(session_t *ps, bool all_damage) {
	region_t region;
	auto buffer_age_fn = ps->backend_data->ops->buffer_age;
	int buffer_age = buffer_age_fn ? buffer_age_fn(ps->backend_data) : -1;

	if (all_damage) {
		buffer_age = -1;
	}

	pixman_region32_init(&region);
	damage_ring_collect(&ps->damage_ring, &ps->screen_reg, &region, buffer_age);
	return region;
}

void handle_device_reset(session_t *ps) {
	log_error("Device reset detected");
	// Wait for reset to complete
	// Although ideally the backend should return DEVICE_STATUS_NORMAL after a reset
	// is completed, it's not always possible.
	//
	// According to ARB_robustness (emphasis mine):
	//
	//     "If a reset status other than NO_ERROR is returned and subsequent
	//     calls return NO_ERROR, the context reset was encountered and
	//     completed. If a reset status is repeatedly returned, the context **may**
	//     be in the process of resetting."
	//
	//  Which means it may also not be in the process of resetting. For example on
	//  AMDGPU devices, Mesa OpenGL always return CONTEXT_RESET after a reset has
	//  started, completed or not.
	//
	//  So here we blindly wait 5 seconds and hope ourselves best of the luck.
	sleep(5);

	// Reset picom
	log_info("Resetting picom after device reset");
	ev_break(ps->loop, EVBREAK_ALL);
}

/// paint all windows
///
/// Returns if any render command is issued. IOW if nothing on the screen has changed,
/// this function will return false.
bool paint_all_new(session_t *ps, struct managed_win *const t) {
	struct timespec now = get_time_timespec();
	auto paint_all_start_us =
	    (uint64_t)now.tv_sec * 1000000UL + (uint64_t)now.tv_nsec / 1000;
	if (ps->backend_data->ops->device_status &&
	    ps->backend_data->ops->device_status(ps->backend_data) != DEVICE_STATUS_NORMAL) {
		handle_device_reset(ps);
		return false;
	}
	if (ps->o.xrender_sync_fence) {
		if (ps->xsync_exists && !x_fence_sync(&ps->c, ps->sync_fence)) {
			log_error("x_fence_sync failed, xrender-sync-fence will be "
			          "disabled from now on.");
			xcb_sync_destroy_fence(ps->c.c, ps->sync_fence);
			ps->sync_fence = XCB_NONE;
			ps->o.xrender_sync_fence = false;
			ps->xsync_exists = false;
		}
	}

	now = get_time_timespec();
	auto after_sync_fence_us =
	    (uint64_t)now.tv_sec * 1000000UL + (uint64_t)now.tv_nsec / 1000;
	log_trace("Time spent on sync fence: %" PRIu64 " us",
	          after_sync_fence_us - paint_all_start_us);

	layout_manager_append_layout(
	    ps->layout_manager, ps->wm,
	    (struct geometry){.width = ps->root_width, .height = ps->root_height});
	auto layout = layout_manager_layout(ps->layout_manager);
	command_builder_build(
	    ps->command_builder, layout, ps->o.force_win_blend,
	    ps->o.blur_background_frame, ps->o.inactive_dim_fixed, ps->o.max_brightness,
	    ps->o.inactive_dim, &ps->shadow_exclude_reg,
	    ps->o.crop_shadow_to_monitor ? &ps->monitors : NULL, ps->o.wintype_option);
	{
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
	// All painting will be limited to the damage, if _some_ of
	// the paints bleed out of the damage region, it will destroy
	// part of the image we want to reuse
	region_t reg_damage;
	reg_damage = get_damage(ps, ps->o.monitor_repaint || !ps->o.use_damage);
	ps->root_damaged = false;

	if (!pixman_region32_not_empty(&reg_damage)) {
		pixman_region32_fini(&reg_damage);
		return false;
	}

#ifdef DEBUG_REPAINT
	static struct timespec last_paint = {0};
#endif

	// <damage-note>
	// If use_damage is enabled, we MUST make sure only the damaged regions of the
	// screen are ever touched by the compositor. The reason is that at the beginning
	// of each render, we clear the damaged regions with the wallpaper, and nothing
	// else. If later during the render we changed anything outside the damaged
	// region, that won't be cleared by the next render, and will thus accumulate.
	// (e.g. if shadow is drawn outside the damaged region, it will become thicker and
	// thicker over time.)

	/// The adjusted damaged regions
	region_t reg_paint;
	assert(ps->o.blur_method != BLUR_METHOD_INVALID);
	if (ps->o.blur_method != BLUR_METHOD_NONE && ps->backend_data->ops->get_blur_size) {
		int blur_width, blur_height;
		ps->backend_data->ops->get_blur_size(ps->backend_blur_context,
		                                     &blur_width, &blur_height);

		// The region of screen a given window influences will be smeared
		// out by blur. With more windows on top of the given window, the
		// influences region will be smeared out more.
		//
		// Also, blurring requires data slightly outside the area that needs
		// to be blurred. The more semi-transparent windows are stacked on top
		// of each other, the larger the area will be.
		//
		// Instead of accurately calculate how much bigger the damage
		// region will be because of blur, we assume the worst case here.
		// That is, the damaged window is at the bottom of the stack, and
		// all other windows have semi-transparent background
		//
		// TODO(yshui): maybe we don't need to resize reg_damage, only reg_paint?
		int resize_factor = 1;
		if (t) {
			resize_factor = t->stacking_rank + 1;
		}
		resize_region_in_place(&reg_damage, blur_width * resize_factor,
		                       blur_height * resize_factor);
		reg_paint = resize_region(&reg_damage, blur_width * resize_factor,
		                          blur_height * resize_factor);
		pixman_region32_intersect(&reg_paint, &reg_paint, &ps->screen_reg);
		pixman_region32_intersect(&reg_damage, &reg_damage, &ps->screen_reg);
	} else {
		pixman_region32_init(&reg_paint);
		pixman_region32_copy(&reg_paint, &reg_damage);
	}

	// A hint to backend, the region that will be visible on screen
	// backend can optimize based on this info
	region_t reg_visible;
	pixman_region32_init(&reg_visible);
	pixman_region32_copy(&reg_visible, &ps->screen_reg);
	if (t && !ps->o.transparent_clipping) {
		// Calculate the region upon which the root window (wallpaper) is to be
		// painted based on the ignore region of the lowest window, if available
		//
		// NOTE If transparent_clipping is enabled, transparent windows are
		// included in the reg_ignore, but we still want to have the wallpaper
		// beneath them, so we don't use reg_ignore for wallpaper in that case.
		pixman_region32_subtract(&reg_visible, &reg_visible, t->reg_ignore);
	}

	// Region on screen we don't want any shadows on
	region_t reg_shadow_clip;
	pixman_region32_init(&reg_shadow_clip);

	now = get_time_timespec();
	auto after_damage_us = (uint64_t)now.tv_sec * 1000000UL + (uint64_t)now.tv_nsec / 1000;
	log_trace("Getting damage took %" PRIu64 " us", after_damage_us - after_sync_fence_us);
	if (ps->next_render > 0) {
		log_verbose("Render schedule deviation: %ld us (%s) %" PRIu64 " %" PRIu64,
		            labs((long)after_damage_us - (long)ps->next_render),
		            after_damage_us < ps->next_render ? "early" : "late",
		            after_damage_us, ps->next_render);
		ps->last_schedule_delay = 0;
		if (after_damage_us > ps->next_render) {
			ps->last_schedule_delay = after_damage_us - ps->next_render;
		}
	}

	if (ps->backend_data->ops->prepare) {
		ps->backend_data->ops->prepare(ps->backend_data, &reg_paint);
	}

	if (ps->root_image) {
		ps->backend_data->ops->compose(ps->backend_data, ps->root_image,
		                               (coord_t){0}, NULL, (coord_t){0},
		                               &reg_paint, &reg_visible);
	} else {
		ps->backend_data->ops->fill(ps->backend_data, (struct color){0, 0, 0, 1},
		                            &reg_paint);
	}

	// Windows are sorted from bottom to top
	// Each window has a reg_ignore, which is the region obscured by all the windows
	// on top of that window. This is used to reduce the number of pixels painted.
	//
	// Whether this is beneficial is to be determined XXX
	for (struct managed_win *w = t; w; w = w->prev_trans) {
		pixman_region32_subtract(&reg_visible, &ps->screen_reg, w->reg_ignore);
		assert(!(w->flags & WIN_FLAGS_IMAGE_ERROR));
		assert(!(w->flags & WIN_FLAGS_PIXMAP_STALE));
		assert(!(w->flags & WIN_FLAGS_PIXMAP_NONE));

		// The bounding shape of the window, in global/target coordinates
		// reminder: bounding shape contains the WM frame
		auto reg_bound = win_get_bounding_shape_global_by_val(w);
		auto reg_bound_no_corner =
		    win_get_bounding_shape_global_without_corners_by_val(w);

		if (!w->mask_image && (w->bounding_shaped || w->corner_radius != 0)) {
			win_bind_mask(ps->backend_data, w);
		}

		if (ps->o.debug_options.always_rebind_pixmap) {
			auto pixmap = ps->backend_data->ops->release_image(
			    ps->backend_data, w->win_image);
			assert(pixmap != XCB_NONE);
			w->win_image = ps->backend_data->ops->bind_pixmap(
			    ps->backend_data, pixmap, x_get_visual_info(&ps->c, w->a.visual));
		}

		// The clip region for the current window, in global/target coordinates
		// reg_paint_in_bound \in reg_paint
		region_t reg_paint_in_bound;
		pixman_region32_init(&reg_paint_in_bound);
		pixman_region32_intersect(&reg_paint_in_bound, &reg_bound, &reg_paint);
		if (ps->o.transparent_clipping) {
			// <transparent-clipping-note>
			// If transparent_clipping is enabled, we need to be SURE that
			// things are not drawn inside reg_ignore, because otherwise they
			// will appear underneath transparent windows.
			// So here we have make sure reg_paint_in_bound \in reg_visible
			// There are a few other places below where this is needed as
			// well.
			pixman_region32_intersect(&reg_paint_in_bound,
			                          &reg_paint_in_bound, &reg_visible);
		}

		// Blur window background
		/* TODO(yshui) since the backend might change the content of the window
		 * (e.g. with shaders), we should consult the backend whether the window
		 * is transparent or not. for now we will just rely on the force_win_blend
		 * option */
		auto real_win_mode = w->mode;
		coord_t window_coord = {.x = w->g.x, .y = w->g.y};

		const double window_opacity = animatable_get(&w->opacity);
		if (w->blur_background &&
		    (ps->o.force_win_blend || real_win_mode == WMODE_TRANS ||
		     (ps->o.blur_background_frame && real_win_mode == WMODE_FRAME_TRANS))) {
			// Minimize the region we try to blur, if the window
			// itself is not opaque, only the frame is.

			const double blur_opacity = animatable_get(&w->blur_opacity);
			assert(blur_opacity >= 0 && blur_opacity <= 1);

			if (blur_opacity * MAX_ALPHA < 1) {
				// We don't need to blur if it would be completely
				// transparent
			} else if (real_win_mode == WMODE_TRANS || ps->o.force_win_blend) {
				// We need to blur the bounding shape of the window
				// (reg_paint_in_bound = reg_bound \cap reg_paint)
				ps->backend_data->ops->blur(
				    ps->backend_data, blur_opacity,
				    ps->backend_blur_context, w->mask_image, window_coord,
				    &reg_paint_in_bound, &reg_visible);
			} else {
				// Window itself is solid, we only need to blur the frame
				// region

				// Readability assertions
				assert(ps->o.blur_background_frame);
				assert(real_win_mode == WMODE_FRAME_TRANS);

				auto reg_blur = win_get_region_frame_local_by_val(w);
				pixman_region32_translate(&reg_blur, w->g.x, w->g.y);
				// make sure reg_blur \in reg_paint
				pixman_region32_intersect(&reg_blur, &reg_blur, &reg_paint);
				if (ps->o.transparent_clipping) {
					// ref: <transparent-clipping-note>
					pixman_region32_intersect(&reg_blur, &reg_blur,
					                          &reg_visible);
				}
				ps->backend_data->ops->blur(
				    ps->backend_data, blur_opacity, ps->backend_blur_context,
				    w->mask_image, window_coord, &reg_blur, &reg_visible);
				pixman_region32_fini(&reg_blur);
			}
		}

		// The win_bind_shadow function must be called before checking if a window
		// has shadow enabled because it disables shadow for a window on failure.
		if (w->shadow && !w->shadow_image) {
			struct color shadow_color = {.red = ps->o.shadow_red,
			                             .green = ps->o.shadow_green,
			                             .blue = ps->o.shadow_blue,
			                             .alpha = ps->o.shadow_opacity};
			win_bind_shadow(ps->backend_data, w, shadow_color, ps->shadow_context);
		}

		// Draw shadow on target
		if (w->shadow) {
			// Clip region for the shadow
			// reg_shadow \in reg_paint
			auto reg_shadow = win_extents_by_val(w);
			pixman_region32_intersect(&reg_shadow, &reg_shadow, &reg_paint);

			// Mask out the region we don't want shadow on
			if (pixman_region32_not_empty(&ps->shadow_exclude_reg)) {
				pixman_region32_subtract(&reg_shadow, &reg_shadow,
				                         &ps->shadow_exclude_reg);
			}
			if (pixman_region32_not_empty(&reg_shadow_clip)) {
				pixman_region32_subtract(&reg_shadow, &reg_shadow,
				                         &reg_shadow_clip);
			}

			if (ps->o.crop_shadow_to_monitor && w->randr_monitor >= 0 &&
			    w->randr_monitor < ps->monitors.count) {
				// There can be a window where number of monitors is
				// updated, but the monitor number attached to the window
				// have not.
				//
				// Window monitor number will be updated eventually, so
				// here we just check to make sure we don't access out of
				// bounds.
				pixman_region32_intersect(
				    &reg_shadow, &reg_shadow,
				    &ps->monitors.regions[w->randr_monitor]);
			}

			if (ps->o.transparent_clipping) {
				// ref: <transparent-clipping-note>
				pixman_region32_intersect(&reg_shadow, &reg_shadow,
				                          &reg_visible);
			}

			assert(w->shadow_image);
			ps->backend_data->ops->set_image_property(
			    ps->backend_data, IMAGE_PROPERTY_OPACITY, w->shadow_image,
			    &window_opacity);
			coord_t shadow_coord = {.x = w->g.x + w->shadow_dx,
			                        .y = w->g.y + w->shadow_dy};

			auto inverted_mask = NULL;
			if (!ps->o.wintype_option[w->window_type].full_shadow) {
				pixman_region32_subtract(&reg_shadow, &reg_shadow,
				                         &reg_bound_no_corner);
				if (w->mask_image) {
					inverted_mask = w->mask_image;
					ps->backend_data->ops->set_image_property(
					    ps->backend_data, IMAGE_PROPERTY_INVERTED,
					    inverted_mask, (bool[]){true});
				}
			}
			ps->backend_data->ops->compose(
			    ps->backend_data, w->shadow_image, shadow_coord,
			    inverted_mask, window_coord, &reg_shadow, &reg_visible);
			if (inverted_mask) {
				ps->backend_data->ops->set_image_property(
				    ps->backend_data, IMAGE_PROPERTY_INVERTED,
				    inverted_mask, (bool[]){false});
			}
			pixman_region32_fini(&reg_shadow);
		}

		// Update image properties
		{
			double dim_opacity = 0.0;
			if (w->dim) {
				dim_opacity = ps->o.inactive_dim;
				if (!ps->o.inactive_dim_fixed) {
					dim_opacity *= window_opacity;
				}
			}

			ps->backend_data->ops->set_image_property(
			    ps->backend_data, IMAGE_PROPERTY_MAX_BRIGHTNESS, w->win_image,
			    &ps->o.max_brightness);
			ps->backend_data->ops->set_image_property(
			    ps->backend_data, IMAGE_PROPERTY_INVERTED, w->win_image,
			    &w->invert_color);
			ps->backend_data->ops->set_image_property(
			    ps->backend_data, IMAGE_PROPERTY_DIM_LEVEL, w->win_image,
			    &dim_opacity);
			ps->backend_data->ops->set_image_property(
			    ps->backend_data, IMAGE_PROPERTY_OPACITY, w->win_image,
			    &window_opacity);
			ps->backend_data->ops->set_image_property(
			    ps->backend_data, IMAGE_PROPERTY_CORNER_RADIUS, w->win_image,
			    (double[]){w->corner_radius});
			if (w->corner_radius) {
				int border_width = w->g.border_width;
				if (border_width == 0) {
					// Some WM has borders implemented as WM frames
					border_width = min3(w->frame_extents.left,
					                    w->frame_extents.right,
					                    w->frame_extents.bottom);
				}
				ps->backend_data->ops->set_image_property(
				    ps->backend_data, IMAGE_PROPERTY_BORDER_WIDTH,
				    w->win_image, &border_width);
			}

			ps->backend_data->ops->set_image_property(
			    ps->backend_data, IMAGE_PROPERTY_CUSTOM_SHADER, w->win_image,
			    w->fg_shader ? (void *)w->fg_shader->backend_shader : NULL);
		}

		if (window_opacity * MAX_ALPHA < 1) {
			// We don't need to paint the window body itself if it's
			// completely transparent.
			goto skip;
		}

		if (w->clip_shadow_above) {
			// Add window bounds to shadow-clip region
			pixman_region32_union(&reg_shadow_clip, &reg_shadow_clip, &reg_bound);
		} else {
			// Remove overlapping window bounds from shadow-clip region
			pixman_region32_subtract(&reg_shadow_clip, &reg_shadow_clip, &reg_bound);
		}

		if (w->mask_image) {
			// We are now going to compose the main window body, so
			// we need to set the mask image to be non-inverted
			ps->backend_data->ops->set_image_property(
			    ps->backend_data, IMAGE_PROPERTY_INVERTED, w->mask_image,
			    (bool[]){false});
		}

		// Draw window on target
		if (w->frame_opacity == 1) {
			// We use both reg_paint_in_bound and mask image, which is
			// probably no strictly necessary. Even for shaped windows, they
			// should be the same.
			ps->backend_data->ops->compose(
			    ps->backend_data, w->win_image, window_coord, w->mask_image,
			    window_coord, &reg_paint_in_bound, &reg_visible);
		} else {
			// For window image processing, we don't have to limit the process
			// region to damage for correctness. (see <damager-note> for
			// details)

			// The visible region, in window local coordinates Although we
			// don't limit process region to damage, we provide that info in
			// reg_visible as a hint. Since window image data outside of the
			// damage region won't be painted onto target
			region_t reg_visible_local;
			region_t reg_bound_local;
			{
				// The bounding shape, in window local coordinates
				pixman_region32_init(&reg_bound_local);
				pixman_region32_copy(&reg_bound_local, &reg_bound);
				pixman_region32_translate(&reg_bound_local, -w->g.x, -w->g.y);

				pixman_region32_init(&reg_visible_local);
				pixman_region32_intersect(&reg_visible_local,
				                          &reg_visible, &reg_paint);
				pixman_region32_translate(&reg_visible_local, -w->g.x,
				                          -w->g.y);
				// Data outside of the bounding shape won't be visible,
				// but it is not necessary to limit the image operations
				// to the bounding shape yet. So pass that as the visible
				// region, not the clip region.
				pixman_region32_intersect(
				    &reg_visible_local, &reg_visible_local, &reg_bound_local);
			}

			auto new_img = ps->backend_data->ops->clone_image(
			    ps->backend_data, w->win_image, &reg_visible_local);
			auto reg_frame = win_get_region_frame_local_by_val(w);
			ps->backend_data->ops->image_op(
			    ps->backend_data, IMAGE_OP_APPLY_ALPHA, new_img, &reg_frame,
			    &reg_visible_local, (double[]){w->frame_opacity});
			pixman_region32_fini(&reg_frame);
			ps->backend_data->ops->compose(
			    ps->backend_data, new_img, window_coord, w->mask_image,
			    window_coord, &reg_paint_in_bound, &reg_visible);
			auto pixmap =
			    ps->backend_data->ops->release_image(ps->backend_data, new_img);
			CHECK(pixmap == XCB_NONE);
			pixman_region32_fini(&reg_visible_local);
			pixman_region32_fini(&reg_bound_local);
		}
	skip:
		pixman_region32_fini(&reg_bound);
		pixman_region32_fini(&reg_bound_no_corner);
		pixman_region32_fini(&reg_paint_in_bound);
	}
	pixman_region32_fini(&reg_paint);
	pixman_region32_fini(&reg_shadow_clip);

	if (ps->o.monitor_repaint) {
		const struct color DEBUG_COLOR = {0.5, 0, 0, 0.5};
		auto reg_damage_debug = get_damage(ps, false);
		ps->backend_data->ops->fill(ps->backend_data, DEBUG_COLOR, &reg_damage_debug);
		pixman_region32_fini(&reg_damage_debug);
	}

	// Move the head of the damage ring
	damage_ring_advance(&ps->damage_ring);

	if (ps->backend_data->ops->present) {
		// Present the rendered scene
		// Vsync is done here
		ps->backend_data->ops->present(ps->backend_data, &reg_damage);
	}

	pixman_region32_fini(&reg_damage);

#ifdef DEBUG_REPAINT
	struct timespec now = get_time_timespec();
	struct timespec diff = {0};
	timespec_subtract(&diff, &now, &last_paint);
	log_trace("[ %5ld:%09ld ] ", diff.tv_sec, diff.tv_nsec);
	last_paint = now;
	log_trace("paint:");
	for (win *w = t; w; w = w->prev_trans)
		log_trace(" %#010lx", w->id);
#endif
	return true;
}

static inline const char *render_command_source_name(enum backend_command_source source) {
	switch (source) {
	case BACKEND_COMMAND_SOURCE_WINDOW: return "window";
	case BACKEND_COMMAND_SOURCE_SHADOW: return "shadow";
	case BACKEND_COMMAND_SOURCE_BACKGROUND: return "background";
	}
	unreachable();
}

void log_backend_command_(enum log_level level, const char *func,
                          const struct backend_command *cmd) {
	if (level < log_get_level_tls()) {
		return;
	}

	log_printf(tls_logger, level, func, "Render command: %p", cmd);
	switch (cmd->op) {
	case BACKEND_COMMAND_BLIT:
		log_printf(tls_logger, level, func, "blit %s%s",
		           render_command_source_name(cmd->source),
		           cmd->need_mask_image ? ", with mask image" : "");
		log_printf(tls_logger, level, func, "origin: %d,%d", cmd->origin.x,
		           cmd->origin.y);
		log_printf(tls_logger, level, func, "mask region:");
		log_region_(level, func, &cmd->blit.mask->region);
		log_printf(tls_logger, level, func, "opaque region:");
		log_region_(level, func, &cmd->opaque_region);
		break;
	case BACKEND_COMMAND_COPY_AREA:
		log_printf(tls_logger, level, func, "copy area from %s",
		           render_command_source_name(cmd->source));
		log_printf(tls_logger, level, func, "origin: %d,%d", cmd->origin.x,
		           cmd->origin.y);
		log_printf(tls_logger, level, func, "region:");
		log_region_(level, func, cmd->copy_area.region);
		break;
	case BACKEND_COMMAND_BLUR:
		log_printf(tls_logger, level, func, "blur%s",
		           cmd->need_mask_image ? ", with mask image" : "");
		log_printf(tls_logger, level, func, "origin: %d,%d", cmd->origin.x,
		           cmd->origin.y);
		log_printf(tls_logger, level, func, "mask region:");
		log_region_(level, func, &cmd->blur.mask->region);
		break;
	case BACKEND_COMMAND_INVALID:
		log_printf(tls_logger, level, func, "invalid");
		break;
	}
}

// vim: set noet sw=8 ts=8 :
