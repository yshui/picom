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
	UT_hash_handle hh;
};

struct dummy_data {
	struct backend_base base;
	struct dummy_image *images;

	struct backend_image mask;
};

struct backend_base *dummy_init(struct session *ps attr_unused) {
	auto ret = (struct backend_base *)ccalloc(1, struct dummy_data);
	ret->c = ps->c;
	ret->loop = ps->loop;
	ret->root = ps->root;
	ret->busy = false;
	return ret;
}

void dummy_deinit(struct backend_base *data) {
	auto dummy = (struct dummy_data *)data;
	HASH_ITER2(dummy->images, img) {
		log_warn("Backend image for pixmap %#010x is not freed", img->pixmap);
		HASH_DEL(dummy->images, img);
		free(img->refcount);
		free(img);
	}
	free(dummy);
}

static void dummy_check_image(struct backend_base *base, const struct dummy_image *img) {
	auto dummy = (struct dummy_data *)base;
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

<<<<<<< HEAD
void dummy_compose(struct backend_base *base, void *image, coord_t dst attr_unused,
                   void *mask attr_unused, coord_t mask_dst attr_unused,
                   const region_t *reg_paint attr_unused,
=======
void dummy_compose(struct backend_base *base, struct managed_win *w attr_unused, void *image, int dst_x attr_unused,
                   int dst_y attr_unused, const region_t *reg_paint attr_unused,
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
                   const region_t *reg_visible attr_unused) {
	auto dummy attr_unused = (struct dummy_data *)base;
	dummy_check_image(base, image);
	assert(mask == NULL || mask == &dummy->mask);
}

void dummy_fill(struct backend_base *backend_data attr_unused, struct color c attr_unused,
                const region_t *clip attr_unused) {
}

bool dummy_blur(struct backend_base *backend_data attr_unused, double opacity attr_unused,
                void *blur_ctx attr_unused, void *mask attr_unused,
                coord_t mask_dst attr_unused, const region_t *reg_blur attr_unused,
                const region_t *reg_visible attr_unused) {
	return true;
}

bool dummy_round(struct backend_base *backend_data attr_unused, struct managed_win *w attr_unused,
                void *ctx_ attr_unused, void *image_data attr_unused, const region_t *reg_round attr_unused,
                const region_t *reg_visible attr_unused) {
	return true;
}

void *dummy_bind_pixmap(struct backend_base *base, xcb_pixmap_t pixmap,
                        struct xvisual_info fmt, bool owned attr_unused) {
	auto dummy = (struct dummy_data *)base;
	struct dummy_image *img = NULL;
	HASH_FIND_INT(dummy->images, &pixmap, img);
	if (img) {
		(*img->refcount)++;
		return img;
	}

	img = ccalloc(1, struct dummy_image);
	img->pixmap = pixmap;
	img->transparent = fmt.alpha_size != 0;
	img->refcount = ccalloc(1, int);
	*img->refcount = 1;

	HASH_ADD_INT(dummy->images, pixmap, img);
	return (void *)img;
}

void dummy_release_image(backend_t *base, void *image) {
	auto dummy = (struct dummy_data *)base;
	if (image == &dummy->mask) {
		return;
	}
	auto img = (struct dummy_image *)image;
	assert(*img->refcount > 0);
	(*img->refcount)--;
	if (*img->refcount == 0) {
		HASH_DEL(dummy->images, img);
		free(img->refcount);
		free(img);
	}
}

bool dummy_is_image_transparent(struct backend_base *base, void *image) {
	auto img = (struct dummy_image *)image;
	dummy_check_image(base, img);
	return img->transparent;
}

int dummy_buffer_age(struct backend_base *base attr_unused) {
	return 2;
}

bool dummy_image_op(struct backend_base *base, enum image_operations op attr_unused,
                    void *image, const region_t *reg_op attr_unused,
                    const region_t *reg_visible attr_unused, void *args attr_unused) {
	dummy_check_image(base, image);
	return true;
}

void *dummy_make_mask(struct backend_base *base, geometry_t size attr_unused,
                      const region_t *reg attr_unused) {
	return &(((struct dummy_data *)base)->mask);
}

bool dummy_set_image_property(struct backend_base *base, enum image_properties prop attr_unused,
                              void *image, void *arg attr_unused) {
	dummy_check_image(base, image);
	return true;
}

void *dummy_clone_image(struct backend_base *base, const void *image,
                        const region_t *reg_visible attr_unused) {
	auto img = (const struct dummy_image *)image;
	dummy_check_image(base, img);
	(*img->refcount)++;
	return (void *)img;
}

void *dummy_create_blur_context(struct backend_base *base attr_unused,
                                enum blur_method method attr_unused, void *args attr_unused) {
	static int dummy_context;
	return &dummy_context;
}

void dummy_destroy_blur_context(struct backend_base *base attr_unused, void *ctx attr_unused) {
}

void *dummy_create_round_context(struct backend_base *base attr_unused, void *args attr_unused) {
	static int dummy_context;
	return &dummy_context;
}

void dummy_destroy_round_context(struct backend_base *base attr_unused, void *ctx attr_unused) {
}

void dummy_get_blur_size(void *ctx attr_unused, int *width, int *height) {
	// These numbers are arbitrary, to make sure the reisze_region code path is
	// covered.
	*width = 5;
	*height = 5;
}

bool dummy_store_back_texture(backend_t *backend_data attr_unused, struct managed_win *w attr_unused,  void *ctx_ attr_unused,
		const region_t *reg_tgt attr_unused, int x attr_unused, int y attr_unused, int width attr_unused, int height attr_unused) {
	return true;
}

struct backend_operations dummy_ops = {
    .init = dummy_init,
    .deinit = dummy_deinit,
    .compose = dummy_compose,
    .fill = dummy_fill,
    .blur = dummy_blur,
    .round = dummy_round,
    .bind_pixmap = dummy_bind_pixmap,
    .create_shadow_context = default_create_shadow_context,
    .destroy_shadow_context = default_destroy_shadow_context,
    .render_shadow = default_backend_render_shadow,
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
    .create_round_context = dummy_create_round_context,
    .destroy_round_context = dummy_destroy_round_context,
    .get_blur_size = dummy_get_blur_size,
    .store_back_texture = dummy_store_back_texture

};
