// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2018, Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <stdbool.h>

#include "compiler.h"
#include "config.h"
#include "driver.h"
#include "kernel.h"
#include "region.h"
#include "types.h"
#include "x.h"

typedef struct session session_t;
struct managed_win;

struct backend_shadow_context;

struct ev_loop;
struct backend_operations;

typedef struct backend_base {
	struct backend_operations *ops;
	struct x_connection *c;
	struct ev_loop *loop;

	/// Whether the backend can accept new render request at the moment
	bool busy;
	// ...
} backend_t;

typedef struct geometry {
	int width;
	int height;
} geometry_t;

typedef struct coord {
	int x, y;
} coord_t;

typedef void (*backend_ready_callback_t)(void *);

// This mimics OpenGL's ARB_robustness extension, which enables detection of GPU context
// resets.
// See: https://www.khronos.org/registry/OpenGL/extensions/ARB/ARB_robustness.txt, section
// 2.6 "Graphics Reset Recovery".
enum device_status {
	DEVICE_STATUS_NORMAL,
	DEVICE_STATUS_RESETTING,
};

// When image properties are actually applied to the image, they are applied in a
// particular order:
//
// Corner radius -> Color inversion -> Dimming -> Opacity multiply -> Limit maximum
// brightness
enum image_properties {
	// Whether the color of the image is inverted
	// 1 boolean, default: false
	IMAGE_PROPERTY_INVERTED,
	// How much the image is dimmed
	// 1 double, default: 0
	IMAGE_PROPERTY_DIM_LEVEL,
	// Image opacity, i.e. an alpha value multiplied to the alpha channel
	// 1 double, default: 1
	IMAGE_PROPERTY_OPACITY,
	// The effective size of the image, the image will be tiled to fit.
	// 2 int, default: the actual size of the image
	IMAGE_PROPERTY_EFFECTIVE_SIZE,
	// Limit how bright image can be. The image brightness is estimated by averaging
	// the pixels in the image, and dimming will be applied to scale the average
	// brightness down to the max brightness value.
	// 1 double, default: 1
	IMAGE_PROPERTY_MAX_BRIGHTNESS,
	// Gives the image a rounded corner.
	// 1 double, default: 0
	IMAGE_PROPERTY_CORNER_RADIUS,
	// Border width
	// 1 int, default: 0
	IMAGE_PROPERTY_BORDER_WIDTH,
	// Custom shader for this window.
	// 1 pointer to shader struct, default: NULL
	IMAGE_PROPERTY_CUSTOM_SHADER,
};

enum image_operations {
	// Multiply the alpha channel by the argument
	IMAGE_OP_APPLY_ALPHA,
};

enum shader_attributes {
	// Whether the shader needs to be render regardless of whether the window is
	// updated.
	SHADER_ATTRIBUTE_ANIMATED = 1,
};

struct gaussian_blur_args {
	int size;
	double deviation;
};

struct box_blur_args {
	int size;
};

struct kernel_blur_args {
	struct conv **kernels;
	int kernel_count;
};

struct dual_kawase_blur_args {
	int size;
	int strength;
};

typedef struct {
	// Intentionally left blank
} *image_handle;

struct backend_mask {
	/// Mask image, can be NULL.
	image_handle image;
	/// Clip region, in source image's coordinate.
	region_t region;
	/// Corner radius of the mask image, the corners of the mask image will be
	/// rounded.
	double corner_radius;
	/// Origin of the mask image, in the source image's coordinate.
	struct coord origin;
	/// Whether the mask image should be inverted.
	bool inverted;
};

struct backend_blur_args {
	/// The blur context
	void *blur_context;
	/// The mask for the blur operation
	struct backend_mask *mask;
	/// Source image
	image_handle source_image;
	/// Opacity of the blurred image
	double opacity;
};

struct backend_blit_args {
	/// Source image, can be NULL.
	image_handle source_image;
	/// Mask for the source image.
	struct backend_mask *mask;
	/// Custom shader for this blit operation.
	void *shader;
	/// Opacity of the source image.
	double opacity;
	/// Dim level of the source image.
	double dim;
	/// Brightness limit of the source image. Source image
	/// will be normalized so that the maximum brightness is
	/// this value.
	double max_brightness;
	/// Corner radius of the source image. The corners of
	/// the source image will be rounded.
	double corner_radius;
	/// Effective size of the source image, source image will be
	/// tiled to fit.
	int ewidth, eheight;
	/// Border width of the source image. This is used with
	/// `corner_radius` to create a border for the rounded corners.
	int border_width;
	/// Whether the source image should be inverted.
	bool color_inverted;
};

enum backend_image_format {
	/// A format that can be used for normal rendering, and binding
	/// X pixmaps.
	BACKEND_IMAGE_FORMAT_PIXMAP,
	/// A format that can be used for masks.
	BACKEND_IMAGE_FORMAT_MASK,
};

struct backend_ops_v2 {
	/// Multiply the alpha channel of the target image by a given value.
	///
	/// @param backend_data backend data
	/// @param target       an image handle, cannot be NULL.
	/// @param alpha        the alpha value to multiply
	/// @param region       the region to apply the alpha, in the target image's
	///                     coordinate.
	bool (*apply_alpha)(backend_t *backend_data, image_handle target, double alpha,
	                    const region_t *region) attr_nonnull(1, 2, 4);

	/// Copy pixels from a source image on to the target image
	/// Some effects may be applied.
	///
	/// @param backend_data backend data
	/// @param origin       the origin of the operation, in the target image's
	///                     coordinate.
	/// @param target       an image handle, cannot be NULL.
	/// @param args         arguments for blit
	/// @return             whether the operation is successful
	bool (*blit)(backend_t *backend_data, struct coord origin, image_handle target,
	             struct backend_blit_args *args) attr_nonnull(1, 3, 4);

	/// Blur a given region of a source image and store the result in the target
	/// image.
	///
	/// @param backend_data backend data
	/// @param origin       the origin of the operation, in the target image's
	///                     coordinate.
	/// @param target       an image handle, cannot be NULL.
	/// @param args         argument for blur
	/// @return             whether the operation is successful
	bool (*blur)(backend_t *backend_data, struct coord origin, image_handle target,
	             struct backend_blur_args *args) attr_nonnull(1, 3, 4);

	/// Direct copy of pixels from a source image on to the target image.
	/// This is a simpler version of `blit`, without any effects.
	///
	/// @param backend_data backend data
	/// @param origin       the origin of the operation, in the target image's
	///                     coordinate.
	/// @param target       an image handle, cannot be NULL.
	/// @param source       an image handle, cannot be NULL.
	/// @param region       the region to copy, in the source image's coordinate.
	/// @return             whether the operation is successful
	bool (*copy_area)(backend_t *backend_data, struct coord origin,
	                  image_handle target, image_handle source, const region_t *region)
	    attr_nonnull(1, 3, 4, 5);

	/// Initialize an image with a given color value.
	///
	/// @param backend_data backend data
	/// @param target       an image handle, cannot be NULL.
	/// @param color        the color to fill the image with
	/// @return             whether the operation is successful
	bool (*clear)(backend_t *backend_data, image_handle target, struct color color)
	    attr_nonnull(1, 2);

	/// Create a new, uninitialized image with the given format and size.
	///
	/// @param backend_data backend data
	/// @param format       the format of the image
	/// @param size         the size of the image
	image_handle (*new_image)(backend_t *backend_data, enum backend_image_format format,
	                          geometry_t size) attr_nonnull(1);

	/// Acquire the image handle of the back buffer.
	///
	/// @param backend_data backend data
	image_handle (*back_buffer)(backend_t *backend_data);
};

struct backend_operations {
	// ===========    Initialization    ===========

	/// Initialize the backend, prepare for rendering to the target window.
	backend_t *(*init)(session_t *, xcb_window_t)attr_nonnull(1);
	void (*deinit)(backend_t *backend_data) attr_nonnull(1);

	/// Called when rendering will be stopped for an unknown amount of
	/// time (e.g. when screen is unredirected). Free some resources.
	///
	/// Optional, not yet used
	void (*pause)(backend_t *backend_data, session_t *ps);

	/// Called before rendering is resumed
	///
	/// Optional, not yet used
	void (*resume)(backend_t *backend_data, session_t *ps);

	/// Called when root window size changed. All existing image data ever
	/// returned by this backend should remain valid after this call
	/// returns.
	///
	/// Optional
	void (*root_change)(backend_t *backend_data, session_t *ps);

	// ===========      Rendering      ============

	// NOTE: general idea about reg_paint/reg_op vs reg_visible is that reg_visible is
	// merely a hint. Ignoring reg_visible entirely don't affect the correctness of
	// the operation performed. OTOH reg_paint/reg_op is part of the parameters of the
	// operation, and must be honored in order to complete the operation correctly.

	// NOTE: due to complications introduced by use-damage and blur, the rendering API
	// is a bit weird. The idea is, `compose` and `blur` have to update a temporary
	// buffer, because `blur` requires data from an area slightly larger than the area
	// that will be visible. So the area outside the visible area has to be rendered,
	// but we have to discard the result (because the result of blurring that area
	// will be wrong). That's why we cannot render into the back buffer directly.
	// After rendering is done, `present` is called to update a portion of the actual
	// back buffer, then present it to the target (or update the target directly,
	// if not back buffered).

	/// Called before when a new frame starts.
	///
	/// Optional
	void (*prepare)(backend_t *backend_data, const region_t *reg_damage);

	/**
	 * Paint the content of an image onto the rendering buffer.
	 *
	 * @param backend_data the backend data
	 * @param image        the image to paint, cannot be NULL
	 * @param dst_x, dst_y the top left corner of the image in the target
	 * @param mask         the mask image, the top left of the mask is aligned with
	 *                     the top left of the image. Optional, can be
	 *                     NULL.
	 * @param reg_paint    the clip region, in target coordinates
	 * @param reg_visible  the visible region, in target coordinates
	 */
	void (*compose)(backend_t *backend_data, image_handle image, coord_t image_dst,
	                image_handle mask, coord_t mask_dst, const region_t *reg_paint,
	                const region_t *reg_visible) attr_nonnull(1, 2, 6, 7);

	/// Fill rectangle of the rendering buffer, mostly for debug purposes, optional.
	void (*fill)(backend_t *backend_data, struct color, const region_t *clip);

	/// Blur a given region of the rendering buffer.
	///
	/// The blur can be limited by `mask`. `mask_dst` specifies the top left corner of
	/// the mask. `mask` can be NULL.
	bool (*blur)(backend_t *backend_data, double opacity, void *blur_ctx,
	             image_handle mask, coord_t mask_dst, const region_t *reg_blur,
	             const region_t *reg_visible) attr_nonnull(1, 3, 6, 7);

	/// Update part of the back buffer with the rendering buffer, then present the
	/// back buffer onto the target window (if not back buffered, update part of the
	/// target window directly).
	///
	/// Optional, if NULL, indicates the backend doesn't have render output
	///
	/// @param region part of the target that should be updated
	void (*present)(backend_t *backend_data, const region_t *region) attr_nonnull(1, 2);

	/**
	 * Bind a X pixmap to the backend's internal image data structure.
	 *
	 * @param backend_data backend data
	 * @param pixmap       X pixmap to bind
	 * @param fmt          information of the pixmap's visual
	 * @param owned        whether the ownership of the pixmap is transferred to the
	 *                     backend.
	 * @return             backend specific image handle for the pixmap. May be
	 *                     NULL.
	 */
	image_handle (*bind_pixmap)(backend_t *backend_data, xcb_pixmap_t pixmap,
	                            struct xvisual_info fmt, bool owned);

	/// Create a shadow context for rendering shadows with radius `radius`.
	/// Default implementation: default_create_shadow_context
	struct backend_shadow_context *(*create_shadow_context)(backend_t *backend_data,
	                                                        double radius);
	/// Destroy a shadow context
	/// Default implementation: default_destroy_shadow_context
	void (*destroy_shadow_context)(backend_t *backend_data,
	                               struct backend_shadow_context *ctx);

	/// Create a shadow image based on the parameters. Resulting image should have a
	/// size of `width + radius * 2` x `height + radius * 2`. Radius is set when the
	/// shadow context is created.
	/// Default implementation: default_render_shadow
	///
	/// @return the shadow image, may be NULL.
	///
	/// Required.
	image_handle (*render_shadow)(backend_t *backend_data, int width, int height,
	                              struct backend_shadow_context *ctx, struct color color);

	/// Create a shadow by blurring a mask. `size` is the size of the blur. The
	/// backend can use whichever blur method is the fastest. The shadow produced
	/// shoule be consistent with `render_shadow`.
	///
	/// @param mask the input mask, must not be NULL.
	/// @return the shadow image, may be NULL.
	///
	/// Optional.
	image_handle (*shadow_from_mask)(backend_t *backend_data, image_handle mask,
	                                 struct backend_shadow_context *ctx,
	                                 struct color color);

	/// Create a mask image from region `reg`. This region can be used to create
	/// shadow, or used as a mask for composing. When used as a mask, it should mask
	/// out everything that is not inside the region used to create it.
	///
	/// Image properties might be set on masks too, at least the INVERTED and
	/// CORNER_RADIUS properties must be supported. Inversion should invert the inside
	/// and outside of the mask. Corner radius should exclude the corners from the
	/// mask. Corner radius should be applied before the inversion.
	///
	/// @return the mask image, may be NULL.
	///
	/// Required.
	image_handle (*make_mask)(backend_t *backend_data, geometry_t size,
	                          const region_t *reg);

	// ============ Resource management ===========

	/// Free resources associated with an image data structure
	///
	/// @param image the image to be released, cannot be NULL.
	void (*release_image)(backend_t *backend_data, image_handle image) attr_nonnull(1, 2);

	/// Create a shader object from a shader source.
	///
	/// Optional
	void *(*create_shader)(backend_t *backend_data, const char *source)attr_nonnull(1, 2);

	/// Free a shader object.
	///
	/// Required if create_shader is present.
	void (*destroy_shader)(backend_t *backend_data, void *shader) attr_nonnull(1, 2);

	// ===========        Query         ===========

	/// Get the attributes of a shader.
	///
	/// Optional, Returns a bitmask of attributes, see `shader_attributes`.
	uint64_t (*get_shader_attributes)(backend_t *backend_data, void *shader)
	    attr_nonnull(1, 2);

	/// Return if image is not completely opaque.
	///
	/// This function is needed because some backend might change the content of the
	/// window (e.g. when using a custom shader with the glx backend), so only the
	/// backend knows if an image is transparent.
	///
	/// @param image the image to be checked, must not be NULL.
	bool (*is_image_transparent)(backend_t *backend_data, image_handle image)
	    attr_nonnull(1, 2);

	/// Get the age of the buffer content we are currently rendering on top
	/// of. The buffer that has just been `present`ed has a buffer age of 1.
	/// Every time `present` is called, buffers get older. Return -1 if the
	/// buffer is empty.
	///
	/// Optional
	int (*buffer_age)(backend_t *backend_data);

	/// Get the render time of the last frame. If the render is still in progress,
	/// returns false. The time is returned in `ts`. Frames are delimited by the
	/// present() calls. i.e. after a present() call, last_render_time() should start
	/// reporting the time of the just presented frame.
	///
	/// Optional, if not available, the most conservative estimation will be used.
	bool (*last_render_time)(backend_t *backend_data, struct timespec *ts);

	/// The maximum number buffer_age might return.
	int max_buffer_age;

	// ===========    Post-processing   ============

	/* TODO(yshui) Consider preserving the order of image ops.
	 * Currently in both backends, the image ops are applied lazily when needed.
	 * However neither backends preserve the order of image ops, they just applied all
	 * pending lazy ops in a pre-determined fixed order, regardless in which order
	 * they were originally applied. This might lead to inconsistencies.*/

	/**
	 * Change image properties
	 *
	 * @param backend_data backend data
	 * @param prop         the property to change
	 * @param image        an image handle, cannot be NULL.
	 * @param args         property value
	 * @return             whether the operation is successful
	 */
	bool (*set_image_property)(backend_t *backend_data, enum image_properties prop,
	                           image_handle image, const void *args) attr_nonnull(1, 3);

	/**
	 * Manipulate an image. Image properties are untouched.
	 *
	 * @param backend_data backend data
	 * @param op           the operation to perform
	 * @param image        an image handle, cannot be NULL.
	 * @param reg_op       the clip region, define the part of the image to be
	 *                     operated on.
	 * @param reg_visible  define the part of the image that will eventually
	 *                     be visible on target. this is a hint to the backend
	 *                     for optimization purposes.
	 * @param args         extra arguments, operation specific
	 * @return             whether the operation is successful
	 */
	bool (*image_op)(backend_t *backend_data, enum image_operations op,
	                 image_handle image, const region_t *reg_op,
	                 const region_t *reg_visible, void *args) attr_nonnull(1, 3, 4, 5);

	/// Create another instance of the `image`. The newly created image
	/// inherits its content and all image properties from the image being
	/// cloned. All `image_op` and `set_image_property` calls on the
	/// returned image should not affect the original image.
	///
	/// @param image the image to be cloned, must not be NULL.
	image_handle (*clone_image)(backend_t *base, image_handle image,
	                            const region_t *reg_visible) attr_nonnull_all;

	/// Create a blur context that can be used to call `blur`
	void *(*create_blur_context)(backend_t *base, enum blur_method, void *args);
	/// Destroy a blur context
	void (*destroy_blur_context)(backend_t *base, void *ctx);
	/// Get how many pixels outside of the blur area is needed for blur
	void (*get_blur_size)(void *blur_context, int *width, int *height);

	// ===========         Hooks        ============
	/// Let the backend hook into the event handling queue
	/// Not implemented yet
	void (*set_ready_callback)(backend_t *, backend_ready_callback_t cb);
	/// Called right after the core has handled its events.
	/// Not implemented yet
	void (*handle_events)(backend_t *);
	// ===========         Misc         ============
	/// Return the driver that is been used by the backend
	enum driver (*detect_driver)(backend_t *backend_data);

	void (*diagnostics)(backend_t *backend_data);

	enum device_status (*device_status)(backend_t *backend_data);
};

extern struct backend_operations *backend_list[];

/// paint all windows
///
/// Returns if any render command is issued. IOW if nothing on the screen has changed,
/// this function will return false.
bool paint_all_new(session_t *ps, struct managed_win *t) attr_nonnull(1);
