// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2024, Yuxuan Shui <yshuiv7@gmail.com>

#include "compat.h"
#include "backend.h"
#include "backend_common.h"
#include "common.h"
#include "region.h"

// TODO(yshui) `has_alpha` is useless in most cases, we should investigate if we can
// remove it.
static image_handle
backend_compat_new_image(struct backend_base *base, enum backend_image_format format,
                         struct geometry size, bool has_alpha) {
	auto ret = base->ops->v2.new_image(base, format, size);
	if (!ret) {
		log_error("Failed to create new image");
		return ret;
	}
	auto inner = (struct backend_compat_image_base *)ret;
	inner->format = format;
	inner->size = size;
	inner->base.refcount = 1;
	inner->base.has_alpha = has_alpha;
	return ret;
}

// TODO(yshui) make use of reg_visible
void backend_compat_compose(backend_t *base, image_handle image, coord_t image_dst,
                            image_handle mask_, coord_t mask_dst, const region_t *reg_tgt,
                            const region_t *reg_visible attr_unused) {
	auto img = (struct backend_image *)image;
	auto mask = (struct backend_image *)mask_;
	auto compat = (struct backend_compat_base *)base;
	log_trace("Composing, image %p, image_dst (%d, %d), mask %p, mask_dst (%d, %d), "
	          "reg_tgt %p",
	          image, image_dst.x, image_dst.y, mask, mask_dst.x, mask_dst.y, reg_tgt);

	struct coord mask_offset = {.x = mask_dst.x - image_dst.x,
	                            .y = mask_dst.y - image_dst.y};

	struct backend_mask mask_args = {
	    .image = mask ? (image_handle)mask->inner : NULL,
	    .origin = mask_offset,
	    .corner_radius = mask ? mask->corner_radius : 0,
	    .inverted = mask ? mask->color_inverted : false,
	};
	pixman_region32_init(&mask_args.region);
	pixman_region32_copy(&mask_args.region, (region_t *)reg_tgt);
	pixman_region32_translate(&mask_args.region, -mask_dst.x, -mask_dst.y);
	struct backend_blit_args blit_args = {
	    .source_image = (image_handle)img->inner,
	    .mask = &mask_args,
	    .shader = img->shader,
	    .opacity = img->opacity,
	    .color_inverted = img->color_inverted,
	    .ewidth = img->ewidth,
	    .eheight = img->eheight,
	    .dim = img->dim,
	    .corner_radius = img->corner_radius,
	    .border_width = img->border_width,
	    .max_brightness = img->max_brightness,
	};
	if (!base->ops->v2.blit(base, image_dst, compat->back_image, &blit_args)) {
		log_warn("Failed to compose image");
	}
	pixman_region32_fini(&mask_args.region);
}

bool backend_compat_blur(struct backend_base *base, double opacity, void *ctx,
                         image_handle mask_, coord_t mask_dst, const region_t *reg_blur,
                         const region_t *reg_visible attr_unused) {
	log_trace("Blurring, mask %p, mask_dst (%d, %d), reg_blur %p", mask_, mask_dst.x,
	          mask_dst.y, reg_blur);
	auto compat = (struct backend_compat_base *)base;
	auto mask = (struct backend_image *)mask_;
	struct backend_mask mask_args = {
	    .image = mask ? (image_handle)mask->inner : NULL,
	    .origin = mask_dst,
	    .corner_radius = mask ? mask->corner_radius : 0,
	    .inverted = mask ? mask->color_inverted : false,
	};
	pixman_region32_init(&mask_args.region);
	pixman_region32_copy(&mask_args.region, reg_blur);
	pixman_region32_translate(&mask_args.region, -mask_dst.x, -mask_dst.y);
	struct backend_blur_args args = {
	    .source_image = compat->back_image,
	    .opacity = opacity,
	    .mask = &mask_args,
	    .blur_context = ctx,
	};
	auto succeeded =
	    base->ops->v2.blur(base, (struct coord){0, 0}, compat->back_image, &args);
	pixman_region32_fini(&mask_args.region);
	return succeeded;
}

void backend_compat_present(backend_t *base, const region_t *region) {
	auto compat = (struct backend_compat_base *)base;
	if (!base->ops->v2.copy_area_quantize(base, (struct coord){0, 0},
	                                      base->ops->v2.back_buffer(base),
	                                      compat->back_image, region)) {
		log_error("Failed to blit for present");
		return;
	}
	base->ops->v2.present(base);
}

image_handle backend_compat_bind_pixmap(struct backend_base *base, xcb_pixmap_t pixmap,
                                        struct xvisual_info visual) {
	auto g = XCB_AWAIT(xcb_get_geometry, base->c->c, pixmap);
	if (!g) {
		log_error("Failed to get geometry of pixmap");
		return NULL;
	}

	auto image = ccalloc(1, struct backend_image);
	default_init_backend_image(image, g->width, g->height);
	free(g);

	image->inner = (void *)base->ops->v2.bind_pixmap(base, pixmap, visual);
	if (!image->inner) {
		free(image);
		return NULL;
	}
	auto inner = (struct backend_compat_image_base *)image->inner;
	inner->format = BACKEND_IMAGE_FORMAT_PIXMAP;
	inner->size = (struct geometry){image->ewidth, image->eheight};
	inner->base.refcount = 1;
	inner->base.has_alpha = visual.alpha_size > 0;
	return (image_handle)image;
}

xcb_pixmap_t backend_compat_release_image(struct backend_base *base, image_handle image) {
	auto img = (struct backend_image *)image;
	auto inner = (struct backend_compat_image_base *)img->inner;
	assert(img != NULL && inner != NULL);

	auto refcount = --inner->base.refcount;
	free(img);

	if (refcount > 0) {
		return XCB_NONE;
	}
	return base->ops->v2.release_image(base, (image_handle)inner);
}

struct backend_compat_shadow_context {
	double radius;
	void *blur_context;
};

struct backend_shadow_context *
backend_compat_create_shadow_context(struct backend_base *base, double radius) {
	auto ctx = ccalloc(1, struct backend_compat_shadow_context);
	ctx->radius = radius;
	ctx->blur_context = NULL;

	if (radius > 0) {
		struct gaussian_blur_args args = {
		    .size = (int)radius,
		    .deviation = gaussian_kernel_std_for_size(radius, 0.5 / 256.0),
		};
		ctx->blur_context = base->ops->create_blur_context(
		    base, BLUR_METHOD_GAUSSIAN, BACKEND_IMAGE_FORMAT_MASK, &args);
		if (!ctx->blur_context) {
			log_error("Failed to create shadow context");
			free(ctx);
			return NULL;
		}
	}
	return (struct backend_shadow_context *)ctx;
}

void backend_compat_destroy_shadow_context(struct backend_base *base,
                                           struct backend_shadow_context *ctx_) {
	auto ctx = (struct backend_compat_shadow_context *)ctx_;
	if (ctx->blur_context) {
		base->ops->destroy_blur_context(
		    base, (struct backend_blur_context *)ctx->blur_context);
	}
	free(ctx_);
}

image_handle backend_compat_make_mask(struct backend_base *base, struct geometry size,
                                      const region_t *region) {
	auto compat = (struct backend_compat_base *)base;
	auto image = ccalloc(1, struct backend_image);
	default_init_backend_image(image, size.width, size.height);
	image->inner =
	    (void *)backend_compat_new_image(base, BACKEND_IMAGE_FORMAT_MASK, size, false);
	if (!image->inner || !base->ops->v2.clear(base, (image_handle)image->inner,
	                                          (struct color){0, 0, 0, 0})) {
		log_error("Failed to create mask image");
		goto err;
	}

	if (!base->ops->v2.copy_area(base, (struct coord){0, 0}, (image_handle)image->inner,
	                             compat->white_image, region)) {
		log_error("Failed to fill the mask");
		base->ops->v2.release_image(base, (image_handle)image->inner);
		free(image);
		return NULL;
	}
	return (image_handle)image;

err:
	if (image != NULL) {
		if (image->inner != NULL) {
			base->ops->v2.release_image(base, (image_handle)image->inner);
		}
		free(image);
	}
	return NULL;
}

image_handle
backend_compat_shadow_from_mask(struct backend_base *base, image_handle mask,
                                struct backend_shadow_context *ctx, struct color color) {
	auto mask_image = (struct backend_image *)mask;
	auto inner = (struct backend_compat_image_base *)mask_image->inner;
	auto sctx = (struct backend_compat_shadow_context *)ctx;
	auto compat = (struct backend_compat_base *)base;
	image_handle normalized_mask_image = NULL, shadow_image = NULL, shadow_color = NULL;
	bool succeeded = false;
	int radius = (int)sctx->radius;
	if (mask_image->dim != 0 || mask_image->max_brightness != 1 ||
	    mask_image->border_width != 0 || mask_image->opacity != 1 ||
	    mask_image->shader != NULL || inner->format != BACKEND_IMAGE_FORMAT_MASK) {
		log_error("Unsupported mask properties for shadow generation");
		return NULL;
	}
	auto new_image = ccalloc(1, struct backend_image);
	if (!new_image) {
		log_error("Failed to allocate new image");
		return NULL;
	}
	default_init_backend_image(new_image, mask_image->ewidth + 2 * radius,
	                           mask_image->eheight + 2 * radius);

	log_trace("Generating shadow from mask, mask %p, color (%f, %f, %f, %f)", mask,
	          color.red, color.green, color.blue, color.alpha);

	// Apply the properties on the mask image and blit the result into a larger
	// image, each side larger by `2 * radius` so there is space for blurring.
	normalized_mask_image = backend_compat_new_image(
	    base, BACKEND_IMAGE_FORMAT_MASK,
	    (struct geometry){mask_image->ewidth + 2 * radius, mask_image->eheight + 2 * radius},
	    false);
	if (!normalized_mask_image ||
	    !base->ops->v2.clear(base, normalized_mask_image, (struct color){0, 0, 0, 0})) {
		log_error("Failed to create mask image");
		goto out;
	}
	{
		struct backend_mask mask_args = {
		    .image = (image_handle)mask_image->inner,
		    .origin = {0, 0},
		    .corner_radius = mask_image->corner_radius,
		    .inverted = mask_image->color_inverted,
		};
		pixman_region32_init_rect(&mask_args.region, 0, 0, (unsigned)mask_image->ewidth,
		                          (unsigned)mask_image->eheight);
		struct backend_blit_args args = {
		    .source_image = compat->white_image,
		    .opacity = 1,
		    .mask = &mask_args,
		    .shader = NULL,
		    .color_inverted = false,
		    .ewidth = mask_image->ewidth,
		    .eheight = mask_image->eheight,
		    .dim = 0,
		    .corner_radius = 0,
		    .border_width = 0,
		    .max_brightness = 1,
		};
		succeeded = base->ops->v2.blit(base, (struct coord){radius, radius},
		                               normalized_mask_image, &args);
		pixman_region32_fini(&mask_args.region);
		if (!succeeded) {
			log_error("Failed to blit for shadow generation");
			goto out;
		}
	}
	// Then we blur the normalized mask image
	if (sctx->blur_context != NULL) {
		struct backend_mask mask_args = {
		    .image = NULL,
		    .origin = {0, 0},
		    .corner_radius = 0,
		    .inverted = false,
		};
		pixman_region32_init_rect(&mask_args.region, 0, 0,
		                          (unsigned)(mask_image->ewidth + 2 * radius),
		                          (unsigned)(mask_image->eheight + 2 * radius));
		struct backend_blur_args args = {
		    .source_image = normalized_mask_image,
		    .opacity = 1,
		    .mask = &mask_args,
		    .blur_context = sctx->blur_context,
		};
		succeeded = base->ops->v2.blur(base, (struct coord){0, 0},
		                               normalized_mask_image, &args);
		pixman_region32_fini(&mask_args.region);
		if (!succeeded) {
			log_error("Failed to blur for shadow generation");
			goto out;
		}
	}
	// Finally, we blit with this mask to colorize the shadow
	succeeded = false;
	shadow_image = backend_compat_new_image(
	    base, BACKEND_IMAGE_FORMAT_PIXMAP,
	    (struct geometry){mask_image->ewidth + 2 * radius, mask_image->eheight + 2 * radius},
	    true);
	if (!shadow_image ||
	    !base->ops->v2.clear(base, shadow_image, (struct color){0, 0, 0, 0})) {
		log_error("Failed to allocate shadow image");
		goto out;
	}

	shadow_color = backend_compat_new_image(base, BACKEND_IMAGE_FORMAT_PIXMAP,
	                                        (struct geometry){1, 1}, true);
	if (!shadow_color || !base->ops->v2.clear(base, shadow_color, color)) {
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
	                          (unsigned)(mask_image->ewidth + 2 * radius),
	                          (unsigned)(mask_image->eheight + 2 * radius));
	struct backend_blit_args args = {
	    .source_image = shadow_color,
	    .opacity = 1,
	    .mask = &mask_args,
	    .shader = NULL,
	    .color_inverted = false,
	    .ewidth = mask_image->ewidth + 2 * radius,
	    .eheight = mask_image->eheight + 2 * radius,
	    .dim = 0,
	    .corner_radius = 0,
	    .border_width = 0,
	    .max_brightness = 1,
	};
	succeeded = base->ops->v2.blit(base, (struct coord){0, 0}, shadow_image, &args);
	pixman_region32_fini(&mask_args.region);

out:
	if (normalized_mask_image) {
		base->ops->v2.release_image(base, normalized_mask_image);
	}
	if (shadow_color) {
		base->ops->v2.release_image(base, shadow_color);
	}
	if (!succeeded && shadow_image) {
		base->ops->v2.release_image(base, shadow_image);
		shadow_image = NULL;
	}
	if (!shadow_image) {
		free(new_image);
		return NULL;
	}

	new_image->inner = (void *)shadow_image;
	return (image_handle)new_image;
}

static bool
backend_compat_image_decouple(struct backend_base *base, struct backend_image *img) {
	auto inner = (struct backend_compat_image_base *)img->inner;
	if (inner->base.refcount == 1) {
		return true;
	}
	auto new_inner = backend_compat_new_image(base, inner->format, inner->size,
	                                          inner->base.has_alpha);
	if (!new_inner) {
		return false;
	}

	region_t reg;
	pixman_region32_init_rect(&reg, 0, 0, (unsigned)inner->size.width,
	                          (unsigned)inner->size.height);
	bool succeeded = base->ops->v2.copy_area(
	    base, (struct coord){0, 0}, (image_handle)new_inner, (image_handle)inner, &reg);
	pixman_region32_fini(&reg);
	if (!succeeded) {
		base->ops->v2.release_image(base, (image_handle)new_inner);
		return false;
	}
	inner->base.refcount -= 1;
	img->inner = (void *)new_inner;
	return true;
}

bool backend_compat_image_op(struct backend_base *base, enum image_operations op,
                             image_handle image, const region_t *reg_op,
                             const region_t *reg_visible attr_unused, void *args) {
	auto backend_image = (struct backend_image *)image;
	const double *dargs = args;
	switch (op) {
	case IMAGE_OP_APPLY_ALPHA:
		if (!backend_compat_image_decouple(base, backend_image)) {
			return false;
		}
		bool succeeded = base->ops->v2.apply_alpha(
		    base, (image_handle)backend_image->inner, dargs[0], reg_op);
		if (succeeded) {
			backend_image->inner->has_alpha = true;
		}
	}
	return false;
}

void backend_compat_fill(struct backend_base *base, struct color color, const region_t *region) {
	auto compat = (struct backend_compat_base *)base;
	auto compat_back_image = (struct backend_compat_image_base *)compat->back_image;
	auto fill_color = backend_compat_new_image(base, BACKEND_IMAGE_FORMAT_PIXMAP,
	                                           (struct geometry){1, 1}, true);
	if (!fill_color) {
		log_error("Failed to create fill color image");
		return;
	}
	if (!base->ops->v2.clear(base, fill_color, color)) {
		log_error("Failed to clear fill color image");
		base->ops->v2.release_image(base, fill_color);
		return;
	}
	struct backend_mask mask_args = {
	    .image = NULL,
	    .origin = {0, 0},
	    .corner_radius = 0,
	    .inverted = false,
	};
	mask_args.region = *region;
	struct backend_blit_args args = {
	    .source_image = fill_color,
	    .opacity = 1,
	    .mask = &mask_args,
	    .shader = NULL,
	    .color_inverted = false,
	    .ewidth = compat_back_image->size.width,
	    .eheight = compat_back_image->size.height,
	    .dim = 0,
	    .corner_radius = 0,
	    .border_width = 0,
	    .max_brightness = 1,
	};
	base->ops->v2.blit(base, (struct coord){0, 0}, compat->back_image, &args);
	base->ops->v2.release_image(base, fill_color);
}

// ===============     Callbacks     ==============

bool backend_compat_init(struct backend_compat_base *compat, struct session *ps) {
	auto base = &compat->base;
	bool has_high_pixmap =
	    base->ops->v2.is_format_supported(base, BACKEND_IMAGE_FORMAT_PIXMAP_HIGH);
	compat->white_image = backend_compat_new_image(base, BACKEND_IMAGE_FORMAT_PIXMAP,
	                                               (struct geometry){1, 1}, false);
	if (!compat->white_image) {
		log_error("Failed to create white image");
		return false;
	}
	if (!base->ops->v2.clear(base, compat->white_image, (struct color){1, 1, 1, 1})) {
		log_error("Failed to clear white image");
		return false;
	}
	if (ps->o.dithered_present && !has_high_pixmap) {
		log_warn("Dithering is enabled but high bit depth pixmap is not "
		         "supported by the backend. It will be disabled.");
	}
	compat->format = ps->o.dithered_present && has_high_pixmap
	                     ? BACKEND_IMAGE_FORMAT_PIXMAP_HIGH
	                     : BACKEND_IMAGE_FORMAT_PIXMAP;
	return backend_compat_resize(
	    compat, (struct geometry){.width = ps->root_width, .height = ps->root_height});
}

void backend_compat_deinit(struct backend_compat_base *compat) {
	auto base = &compat->base;
	xcb_pixmap_t pixmap;
	if (compat->white_image) {
		pixmap = base->ops->v2.release_image(base, compat->white_image);
		CHECK(pixmap == XCB_NONE);
	}
	if (compat->back_image) {
		pixmap = base->ops->v2.release_image(base, compat->back_image);
		CHECK(pixmap == XCB_NONE);
	}
}

bool backend_compat_resize(struct backend_compat_base *compat, struct geometry new_size) {
	auto base = &compat->base;
	if (compat->back_image) {
		auto pixmap = base->ops->v2.release_image(base, compat->back_image);
		CHECK(pixmap == XCB_NONE);
	}

	compat->back_image = backend_compat_new_image(base, compat->format, new_size, false);
	if (!compat->back_image) {
		log_error("Failed to create back image");
		return false;
	}

	auto compat_inner = (struct backend_compat_image_base *)compat->back_image;
	compat_inner->format = compat->format;
	compat_inner->size = new_size;
	return true;
}
