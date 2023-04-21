// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once

#include <xcb/render.h>
#include <xcb/xcb_image.h>

#include <stdbool.h>

#include "backend.h"
#include "config.h"
#include "region.h"

typedef struct session session_t;
typedef struct win win;
typedef struct conv conv;
typedef struct backend_base backend_t;
struct backend_operations;

<<<<<<< HEAD
struct dual_kawase_params {
=======
typedef struct dual_kawase_params {
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
	/// Number of downsample passes
	int iterations;
	/// Pixel offset for down- and upsample
	float offset;
	/// Save area around blur target (@ref resize_width, @ref resize_height)
	int expand;
<<<<<<< HEAD
};

struct backend_image_inner_base {
	int refcount;
	bool has_alpha;
};

struct backend_image {
	// Backend dependent inner image data
	struct backend_image_inner_base *inner;
	double opacity;
	double dim;
	double max_brightness;
	double corner_radius;
	// Effective size of the image
	int ewidth, eheight;
	bool color_inverted;
	int border_width;
};
=======
} dual_kawase_params_t;
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45

bool build_shadow(xcb_connection_t *, xcb_drawable_t, double opacity, int width,
                  int height, const conv *kernel, xcb_render_picture_t shadow_pixel,
                  xcb_pixmap_t *pixmap, xcb_render_picture_t *pict);

xcb_render_picture_t solid_picture(xcb_connection_t *, xcb_drawable_t, bool argb,
                                   double a, double r, double g, double b);

xcb_image_t *
make_shadow(xcb_connection_t *c, const conv *kernel, double opacity, int width, int height);

/// The default implementation of `is_win_transparent`, it simply looks at win::mode. So
/// this is not suitable for backends that alter the content of windows
bool default_is_win_transparent(void *, win *, void *);

/// The default implementation of `is_frame_transparent`, it uses win::frame_opacity. Same
/// caveat as `default_is_win_transparent` applies.
bool default_is_frame_transparent(void *, win *, void *);

void *default_backend_render_shadow(backend_t *backend_data, int width, int height,
                                    struct backend_shadow_context *sctx, struct color color);

/// Implement `render_shadow` with `shadow_from_mask`.
void *
backend_render_shadow_from_mask(backend_t *backend_data, int width, int height,
                                struct backend_shadow_context *sctx, struct color color);
struct backend_shadow_context *
default_create_shadow_context(backend_t *backend_data, double radius);

void default_destroy_shadow_context(backend_t *backend_data,
                                    struct backend_shadow_context *sctx);

void init_backend_base(struct backend_base *base, session_t *ps);

struct conv **generate_blur_kernel(enum blur_method method, void *args, int *kernel_count);
<<<<<<< HEAD
struct dual_kawase_params *generate_dual_kawase_params(void *args);

void *default_clone_image(backend_t *base, const void *image_data, const region_t *reg);
bool default_is_image_transparent(backend_t *base attr_unused, void *image_data);
bool default_set_image_property(backend_t *base attr_unused, enum image_properties op,
                                void *image_data, void *arg);
struct backend_image *default_new_backend_image(int w, int h);
=======

struct dual_kawase_params *generate_dual_kawase_params(void *args);
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
