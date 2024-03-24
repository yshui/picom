#include <uthash.h>
#include <xcb/xcb.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "types.h"
#include "uthash_extra.h"
#include "utils.h"
#include "x.h"

struct dummy_image {
	xcb_pixmap_t pixmap;
	bool transparent;
	int *refcount;
	bool owned;
	UT_hash_handle hh;
};

struct dummy_data {
	struct backend_base base;
	struct dummy_image *images;

	struct backend_image mask;
};

struct backend_base *dummy_init(session_t *ps attr_unused, xcb_window_t target attr_unused) {
	auto ret = (struct backend_base *)ccalloc(1, struct dummy_data);
	init_backend_base(ret, ps);
	return ret;
}

void dummy_deinit(struct backend_base *data) {
	auto dummy = (struct dummy_data *)data;
	HASH_ITER2(dummy->images, img) {
		log_warn("Backend image for pixmap %#010x is not freed", img->pixmap);
		HASH_DEL(dummy->images, img);
		free(img->refcount);
		if (img->owned) {
			xcb_free_pixmap(data->c->c, img->pixmap);
		}
		free(img);
	}
	free(dummy);
}

static void dummy_check_image(struct backend_base *base, image_handle image) {
	auto dummy = (struct dummy_data *)base;
	auto img = (struct dummy_image *)image;
	if (img == (struct dummy_image *)&dummy->mask) {
		return;
	}
	struct dummy_image *tmp = NULL;
	HASH_FIND_INT(dummy->images, &img->pixmap, tmp);
	if (!tmp) {
		log_warn("Using an invalid (possibly freed) image");
		assert(false);
	}
	assert(*tmp->refcount > 0);
}

void dummy_compose(struct backend_base *base, image_handle image, coord_t dst attr_unused,
                   image_handle mask attr_unused, coord_t mask_dst attr_unused,
                   const region_t *reg_paint attr_unused,
                   const region_t *reg_visible attr_unused) {
	auto dummy attr_unused = (struct dummy_data *)base;
	dummy_check_image(base, image);
	assert(mask == NULL || (struct backend_image *)mask == &dummy->mask);
}

void dummy_fill(struct backend_base *backend_data attr_unused, struct color c attr_unused,
                const region_t *clip attr_unused) {
}

bool dummy_blur(struct backend_base *backend_data attr_unused, double opacity attr_unused,
                void *blur_ctx attr_unused, image_handle mask attr_unused,
                coord_t mask_dst attr_unused, const region_t *reg_blur attr_unused,
                const region_t *reg_visible attr_unused) {
	return true;
}

image_handle dummy_bind_pixmap(struct backend_base *base, xcb_pixmap_t pixmap,
                               struct xvisual_info fmt, bool owned) {
	auto dummy = (struct dummy_data *)base;
	struct dummy_image *img = NULL;
	HASH_FIND_INT(dummy->images, &pixmap, img);
	if (img) {
		(*img->refcount)++;
		return (image_handle)img;
	}

	img = ccalloc(1, struct dummy_image);
	img->pixmap = pixmap;
	img->transparent = fmt.alpha_size != 0;
	img->refcount = ccalloc(1, int);
	*img->refcount = 1;
	img->owned = owned;

	HASH_ADD_INT(dummy->images, pixmap, img);
	return (image_handle)img;
}

void dummy_release_image(backend_t *base, image_handle image) {
	auto dummy = (struct dummy_data *)base;
	if ((struct backend_image *)image == &dummy->mask) {
		return;
	}
	auto img = (struct dummy_image *)image;
	assert(*img->refcount > 0);
	(*img->refcount)--;
	if (*img->refcount == 0) {
		HASH_DEL(dummy->images, img);
		free(img->refcount);
		if (img->owned) {
			xcb_free_pixmap(base->c->c, img->pixmap);
		}
		free(img);
	}
}

bool dummy_is_image_transparent(struct backend_base *base, image_handle image) {
	dummy_check_image(base, image);
	return ((struct dummy_image *)image)->transparent;
}

int dummy_buffer_age(struct backend_base *base attr_unused) {
	return 2;
}

bool dummy_image_op(struct backend_base *base, enum image_operations op attr_unused,
                    image_handle image, const region_t *reg_op attr_unused,
                    const region_t *reg_visible attr_unused, void *args attr_unused) {
	dummy_check_image(base, image);
	return true;
}

image_handle dummy_make_mask(struct backend_base *base, geometry_t size attr_unused,
                             const region_t *reg attr_unused) {
	auto dummy = (struct dummy_data *)base;
	auto mask = &dummy->mask;
	return (image_handle)mask;
}

bool dummy_set_image_property(struct backend_base *base, enum image_properties prop attr_unused,
                              image_handle image, const void *arg attr_unused) {
	dummy_check_image(base, image);
	return true;
}

image_handle dummy_clone_image(struct backend_base *base, image_handle image,
                               const region_t *reg_visible attr_unused) {
	dummy_check_image(base, image);
	auto image_impl = (struct dummy_image *)image;
	(*image_impl->refcount)++;
	return image;
}

void *dummy_create_blur_context(struct backend_base *base attr_unused,
                                enum blur_method method attr_unused, void *args attr_unused) {
	static int dummy_context;
	return &dummy_context;
}

void dummy_destroy_blur_context(struct backend_base *base attr_unused, void *ctx attr_unused) {
}

void dummy_get_blur_size(void *ctx attr_unused, int *width, int *height) {
	// These numbers are arbitrary, to make sure the resize_region code path is
	// covered.
	*width = 5;
	*height = 5;
}

struct backend_operations dummy_ops = {
    .init = dummy_init,
    .deinit = dummy_deinit,
    .compose = dummy_compose,
    .fill = dummy_fill,
    .blur = dummy_blur,
    .bind_pixmap = dummy_bind_pixmap,
    .create_shadow_context = default_create_shadow_context,
    .destroy_shadow_context = default_destroy_shadow_context,
    .render_shadow = default_render_shadow,
    .make_mask = dummy_make_mask,
    .release_image = dummy_release_image,
    .is_image_transparent = dummy_is_image_transparent,
    .buffer_age = dummy_buffer_age,
    .max_buffer_age = 5,

    .image_op = dummy_image_op,
    .clone_image = dummy_clone_image,
    .set_image_property = dummy_set_image_property,
    .create_blur_context = dummy_create_blur_context,
    .destroy_blur_context = dummy_destroy_blur_context,
    .get_blur_size = dummy_get_blur_size,

};
