// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2024, Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

/// Compatibility layer implementing old new backend interface on top of the new new
/// backend interface.

#include "backend.h"
#include "backend_common.h"
#include "region.h"
#include "types.h"

struct backend_compat_base {
	struct backend_base base;
	// Intermediate image to hold what will be presented to the back buffer.
	image_handle back_image;
	// 1x1 white image
	image_handle white_image;
	// Format to use for back_image and intermediate images
	enum backend_image_format format;
};

struct backend_compat_image_base {
	struct backend_image_inner_base base;
	enum backend_image_format format;
	struct geometry size;
};

// =============== Compat functions ===============
//
// These functions implements the old new backend interface on top of the new new
// backend interface. Refer to the members of `struct backend_ops` for what these
// functions should do.
//

void backend_compat_compose(struct backend_base *base, image_handle image,
                            struct coord image_dst, image_handle mask_,
                            struct coord mask_dst, const region_t *reg_tgt,
                            const region_t *reg_visible attr_unused);
bool backend_compat_blur(struct backend_base *base, double opacity, void *ctx,
                         image_handle mask_, coord_t mask_dst, const region_t *reg_blur,
                         const region_t *reg_visible attr_unused);
void backend_compat_present(struct backend_base *base, const region_t *region);
image_handle backend_compat_bind_pixmap(struct backend_base *base, xcb_pixmap_t pixmap,
                                        struct xvisual_info visual);
xcb_pixmap_t backend_compat_release_image(struct backend_base *base, image_handle image);
struct backend_shadow_context *
backend_compat_create_shadow_context(struct backend_base *base, double radius);
void backend_compat_destroy_shadow_context(struct backend_base *base,
                                           struct backend_shadow_context *ctx);
image_handle backend_compat_make_mask(struct backend_base *base, struct geometry size,
                                      const region_t *region);
image_handle
backend_compat_shadow_from_mask(struct backend_base *base, image_handle mask,
                                struct backend_shadow_context *ctx, struct color color);
bool backend_compat_image_op(backend_t *base, enum image_operations op, image_handle image,
                             const region_t *reg_op, const region_t *reg_visible, void *args);
void backend_compat_fill(struct backend_base *base, struct color color, const region_t *region);

// ===============     Callbacks     ==============
/// Call this from your backend's resize function.
bool backend_compat_resize(struct backend_compat_base *compat, struct geometry new_size);
/// Call this from your backend's init function, after you have initialized the backend.
bool backend_compat_init(struct backend_compat_base *compat, struct session *ps);
/// Call this from your backend's deinit function, before you deinitialize the backend.
void backend_compat_deinit(struct backend_compat_base *compat);