#include <uthash.h>
#include <xcb/xcb.h>

#include <picom/types.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "utils/misc.h"
#include "utils/uthash_extra.h"
#include "x.h"

struct dummy_image {
	enum backend_image_format format;
	xcb_pixmap_t pixmap;
	struct list_node siblings;
	UT_hash_handle hh;
};

struct dummy_data {
	struct backend_base base;
	struct dummy_image *pixmap_images;
	struct list_node non_pixmap_images;

	struct dummy_image back_buffer;
};

const struct backend_operations dummy_ops;

struct backend_base *dummy_init(session_t *ps attr_unused, xcb_window_t target attr_unused) {
	auto ret = ccalloc(1, struct dummy_data);
	init_backend_base(&ret->base, ps);
	ret->base.ops = dummy_ops;
	list_init_head(&ret->non_pixmap_images);
	return &ret->base;
}

void dummy_deinit(struct backend_base *data) {
	auto dummy = (struct dummy_data *)data;
	HASH_ITER2(dummy->pixmap_images, img) {
		log_warn("Backend image %p for pixmap %#010x is not freed", img, img->pixmap);
		HASH_DEL(dummy->pixmap_images, img);
		xcb_free_pixmap(data->c->c, img->pixmap);
		free(img);
	}
	list_foreach_safe(struct dummy_image, img, &dummy->non_pixmap_images, siblings) {
		log_warn("Backend image %p for non-pixmap is not freed", img);
		list_remove(&img->siblings);
		free(img);
	}
	free(dummy);
}

static void dummy_check_image(struct backend_base *base, image_handle image) {
	auto dummy = (struct dummy_data *)base;
	auto img = (struct dummy_image *)image;
	if (img == (struct dummy_image *)&dummy->back_buffer) {
		return;
	}
	if (!img->pixmap) {
		return;
	}
	struct dummy_image *tmp = NULL;
	HASH_FIND_INT(dummy->pixmap_images, &img->pixmap, tmp);
	if (!tmp) {
		log_warn("Using an invalid (possibly freed) image");
		assert(false);
	}
}

bool dummy_blit(struct backend_base *base, ivec2 origin attr_unused, image_handle target,
                const struct backend_blit_args *args) {
	dummy_check_image(base, target);
	dummy_check_image(base, args->source_image);
	if (args->source_mask) {
		auto mask = (struct dummy_image *)args->source_mask->image;
		if (mask->format != BACKEND_IMAGE_FORMAT_MASK) {
			log_error("Invalid mask image format");
			assert(false);
			return false;
		}
		dummy_check_image(base, args->source_mask->image);
	}
	return true;
}

bool dummy_blur(struct backend_base *base, ivec2 origin attr_unused, image_handle target,
                const struct backend_blur_args *args) {
	dummy_check_image(base, target);
	dummy_check_image(base, args->source_image);
	if (args->source_mask) {
		auto mask = (struct dummy_image *)args->source_mask->image;
		if (mask->format != BACKEND_IMAGE_FORMAT_MASK) {
			log_error("Invalid mask image format");
			assert(false);
			return false;
		}
		dummy_check_image(base, args->source_mask->image);
	}
	return true;
}

image_handle dummy_bind_pixmap(struct backend_base *base, xcb_pixmap_t pixmap,
                               struct xvisual_info fmt attr_unused) {
	auto dummy = (struct dummy_data *)base;
	struct dummy_image *img = NULL;
	HASH_FIND_INT(dummy->pixmap_images, &pixmap, img);
	if (img) {
		log_error("Pixmap %#010x is already bound to an image", pixmap);
		return NULL;
	}

	img = ccalloc(1, struct dummy_image);
	img->format = BACKEND_IMAGE_FORMAT_PIXMAP;
	img->pixmap = pixmap;

	HASH_ADD_INT(dummy->pixmap_images, pixmap, img);
	return (image_handle)img;
}

xcb_pixmap_t dummy_release_image(backend_t *base, image_handle image) {
	auto dummy = (struct dummy_data *)base;
	if ((struct dummy_image *)image == &dummy->back_buffer) {
		return XCB_NONE;
	}
	auto img = (struct dummy_image *)image;
	xcb_pixmap_t pixmap = XCB_NONE;
	if (img->pixmap) {
		HASH_DEL(dummy->pixmap_images, img);
		pixmap = img->pixmap;
	} else {
		list_remove(&img->siblings);
	}
	free(img);
	return pixmap;
}

int dummy_buffer_age(struct backend_base *base attr_unused) {
	return 2;
}

bool dummy_apply_alpha(struct backend_base *base, image_handle target,
                       double alpha attr_unused, const region_t *reg attr_unused) {
	dummy_check_image(base, target);
	return true;
}

bool dummy_copy_area(struct backend_base *base, ivec2 origin attr_unused, image_handle target,
                     image_handle source, const region_t *reg attr_unused) {
	dummy_check_image(base, target);
	dummy_check_image(base, source);
	return true;
}

bool dummy_clear(struct backend_base *base, image_handle target,
                 struct color color attr_unused) {
	dummy_check_image(base, target);
	return true;
}

image_handle dummy_new_image(struct backend_base *base, enum backend_image_format format,
                             ivec2 size attr_unused) {
	auto new_img = ccalloc(1, struct dummy_image);
	auto dummy = (struct dummy_data *)base;
	list_insert_after(&dummy->non_pixmap_images, &new_img->siblings);
	new_img->format = format;
	return (image_handle)new_img;
}

image_handle dummy_back_buffer(struct backend_base *base) {
	auto dummy = (struct dummy_data *)base;
	return (image_handle)&dummy->back_buffer;
}

void *dummy_create_blur_context(struct backend_base *base attr_unused,
                                enum blur_method method attr_unused,
                                enum backend_image_format format attr_unused,
                                void *args attr_unused) {
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

uint32_t dummy_image_capabilities(struct backend_base *base attr_unused,
                                  image_handle image attr_unused) {
	return BACKEND_IMAGE_CAP_SRC | BACKEND_IMAGE_CAP_DST;
}

bool dummy_is_format_supported(struct backend_base *base attr_unused,
                               enum backend_image_format format attr_unused) {
	return true;
}

static int dummy_max_buffer_age(struct backend_base *base attr_unused) {
	return 5;
}

#define PICOM_BACKEND_DUMMY_MAJOR (0UL)
#define PICOM_BACKEND_DUMMY_MINOR (1UL)

static void dummy_version(struct backend_base * /*base*/, uint64_t *major, uint64_t *minor) {
	*major = PICOM_BACKEND_DUMMY_MAJOR;
	*minor = PICOM_BACKEND_DUMMY_MINOR;
}

const struct backend_operations dummy_ops = {
    .apply_alpha = dummy_apply_alpha,
    .back_buffer = dummy_back_buffer,
    .blit = dummy_blit,
    .blur = dummy_blur,
    .clear = dummy_clear,
    .copy_area = dummy_copy_area,
    .copy_area_quantize = dummy_copy_area,
    .image_capabilities = dummy_image_capabilities,
    .is_format_supported = dummy_is_format_supported,
    .new_image = dummy_new_image,
    .bind_pixmap = dummy_bind_pixmap,
    .quirks = backend_no_quirks,
    .version = dummy_version,
    .release_image = dummy_release_image,

    .init = dummy_init,
    .deinit = dummy_deinit,
    .buffer_age = dummy_buffer_age,
    .max_buffer_age = dummy_max_buffer_age,

    .create_blur_context = dummy_create_blur_context,
    .destroy_blur_context = dummy_destroy_blur_context,
    .get_blur_size = dummy_get_blur_size,
};

BACKEND_ENTRYPOINT(dummy_register) {
	if (!backend_register(PICOM_BACKEND_MAJOR, PICOM_BACKEND_MINOR, "dummy",
	                      dummy_ops.init, false)) {
		log_error("Failed to register dummy backend");
	}
}
