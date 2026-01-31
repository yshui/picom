// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once

#include <pixman-1/pixman.h>
#include <stdbool.h>
#include <xcb/xproto.h>

#include "types.h"

#define PICOM_BACKEND_MAJOR (2UL)
#define PICOM_BACKEND_MINOR (0UL)
#define PICOM_BACKEND_MAKE_VERSION(major, minor) ((major) * 1000 + (minor))

typedef pixman_region32_t region_t;
struct shader_specification;

struct xvisual_info {
	/// Bit depth of the red component
	int red_size;
	/// Bit depth of the green component
	int green_size;
	/// Bit depth of the blue component
	int blue_size;
	/// Bit depth of the alpha component
	int alpha_size;
	/// The depth of X visual
	int visual_depth;

	xcb_visualid_t visual;
};

typedef struct session session_t;
struct win;

struct ev_loop;
struct backend_operations;

typedef struct backend_base backend_t;

// This mimics OpenGL's ARB_robustness extension, which enables detection of GPU context
// resets.
// See: https://www.khronos.org/registry/OpenGL/extensions/ARB/ARB_robustness.txt, section
// 2.6 "Graphics Reset Recovery".
enum device_status {
	DEVICE_STATUS_NORMAL,
	DEVICE_STATUS_RESETTING,
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

typedef struct image_handle {
	// Intentionally left blank
} *image_handle;

typedef struct shader_handle {
	// Intentionally left blank
} *shader_handle;

/// A mask for various backend operations.
///
/// The mask is composed of both a mask region and a mask image. The resulting mask
/// is the intersection of the two. The mask image can be modified by the `corner_radius`
/// and `inverted` properties. Note these properties have no effect on the mask region.
struct backend_mask_image {
	/// Mask image, can be NULL.
	///
	/// Mask image must be an image that was created with the
	/// `BACKEND_IMAGE_FORMAT_MASK` format. Using an image with a wrong format as mask
	/// is undefined behavior.
	image_handle image;
	/// Corner radius of the mask image, the corners of the mask image will be
	/// rounded.
	double corner_radius;
	/// Origin of the mask image, in the source image's coordinate.
	vec2 origin;
	/// Whether the mask image should be inverted.
	bool inverted;
};

struct backend_blur_args {
	/// The blur context
	void *blur_context;
	/// The source mask for the blur operation, may be NULL. Only parts of the source
	/// image covered by the mask should participate in the blur operation.
	const struct backend_mask_image *source_mask;
	/// The scaling factor of the source mask.
	vec2 source_mask_scale;
	/// Region of the target image that will be covered by the blur operation, in the
	/// source image's coordinate.
	const region_t *target_mask;
	/// Source image
	image_handle source_image;
	/// Opacity of the blurred image
	double opacity;
};

struct backend_blit_args {
	/// Source image, can be NULL.
	image_handle source_image;
	/// Mask for the source image. may be NULL. Only contents covered by the mask
	/// should participate in the blit operation. This applies to the source image
	/// before it's scaled.
	const struct backend_mask_image *source_mask;
	/// Mask for the target image. Only regions of the target image covered by this
	/// mask should be modified. This is the target's coordinate system.
	const region_t *target_mask;
	/// Custom shader for this blit operation.
	shader_handle shader;
	/// Tint. Multiply each color channel by a specific factor. i.e.
	/// out.c = in.c * tint.c, where c = r, g, b, or a.
	///
	/// Note since the backends operate in pre-mult alpha mode, applying
	/// a factor to alpha requires applying the same factor to other colors too.
	struct color tint;
	/// Brightness limit of the source image. Source image
	/// will be normalized so that the maximum brightness is
	/// this value.
	double max_brightness;
	/// Scale factor for the horizontal and vertical direction (X for horizontal,
	/// Y for vertical).
	vec2 scale;
	/// Corner radius of the source image BEFORE scaling. The corners of
	/// the source image will be rounded.
	double corner_radius;
	/// Effective size of the source image BEFORE scaling, set where the corners
	/// of the image are.
	ivec2 effective_size;
	/// Border width of the source image BEFORE scaling. This is used with
	/// `corner_radius` to create a border for the rounded corners.
	/// Setting this has no effect if `corner_radius` is 0.
	int border_width;
	/// Whether the source image should be inverted.
	bool color_inverted;
};

enum backend_image_format {
	/// A format that can be used for normal rendering, and binding
	/// X pixmaps.
	/// Images created with `bind_pixmap` have this format automatically.
	BACKEND_IMAGE_FORMAT_PIXMAP,
	/// Like `BACKEND_IMAGE_FORMAT_PIXMAP`, but the image has a higher
	/// precision. Support is optional.
	BACKEND_IMAGE_FORMAT_PIXMAP_HIGH,
	/// A format that can be used for masks.
	BACKEND_IMAGE_FORMAT_MASK,
};

enum backend_image_capability {
	/// Image can be sampled from. This is required for `blit` and `blur` source
	/// images. All images except the back buffer should have this capability.
	/// Note that `copy_area` should work without this capability, this is so that
	/// blurring the back buffer could be done.
	BACKEND_IMAGE_CAP_SRC = 1 << 0,
	/// Image can be rendered to. This is required for target images of any operation.
	/// All images except bound X pixmaps should have this capability.
	BACKEND_IMAGE_CAP_DST = 1 << 1,
};

enum backend_quirk {
	/// Backend cannot do blur quickly. The compositor will avoid using blur to create
	/// shadows on this backend
	BACKEND_QUIRK_SLOW_BLUR = 1 << 0,
};

struct backend_operations {
	// ===========    Initialization    ===========

	/// Initialize the backend, prepare for rendering to the target window.
	backend_t *(*init)(session_t *, xcb_window_t) __attribute__((nonnull(1)));
	void (*deinit)(backend_t *backend_data) __attribute__((nonnull(1)));

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

	/// Called before when a new frame starts.
	///
	/// Optional
	void (*prepare)(backend_t *backend_data, const region_t *reg_damage);

	/// Multiply the alpha channel of the target image by a given value.
	///
	/// @param backend_data backend data
	/// @param target       an image handle, cannot be NULL.
	/// @param alpha        the alpha value to multiply
	/// @param region       the region to apply the alpha, in the target image's
	///                     coordinate.
	bool (*apply_alpha)(struct backend_base *backend_data, image_handle target,
	                    double alpha, const region_t *region)
	    __attribute__((nonnull(1, 2, 4)));

	/// Copy pixels from a source image on to the target image.
	///
	/// Some effects may be applied. If the region specified by the mask
	/// contains parts that are outside the source image, the source image
	/// will be repeated to fit.
	///
	/// Source and target MUST NOT be the same image.
	///
	/// @param backend_data backend data
	/// @param origin       the origin of the operation, in the target image's
	///                     coordinate.
	/// @param target       an image handle, cannot be NULL.
	/// @param args         arguments for blit
	/// @return             whether the operation is successful
	bool (*blit)(struct backend_base *backend_data, ivec2 origin, image_handle target,
	             const struct backend_blit_args *args) __attribute__((nonnull(1, 3, 4)));

	/// Blur a given region of a source image and store the result in the
	/// target image.
	///
	/// The blur operation might access pixels outside the mask region, the
	/// amount of pixels accessed can be queried with `get_blur_size`. If
	/// pixels outside the source image are accessed, the result will be
	/// clamped to the edge of the source image.
	///
	/// Source and target may be the same image.
	///
	/// @param backend_data backend data
	/// @param origin       the origin of the operation, in the target image's
	///                     coordinate.
	/// @param target       an image handle, cannot be NULL.
	/// @param args         argument for blur
	/// @return             whether the operation is successful
	bool (*blur)(struct backend_base *backend_data, ivec2 origin, image_handle target,
	             const struct backend_blur_args *args) __attribute__((nonnull(1, 3, 4)));

	/// Direct copy of pixels from a source image on to the target image.
	/// This is a simpler version of `blit`, without any effects. Note unlike `blit`,
	/// if `region` tries to sample from outside the source image, instead of
	/// repeating, the result will be clamped to the edge of the source image.
	/// Blending should not be applied for the copy.
	///
	/// Source and target MUST NOT be the same image.
	///
	/// @param backend_data backend data
	/// @param origin       the origin of the operation, in the target image's
	///                     coordinate.
	/// @param target       an image handle, cannot be NULL.
	/// @param source       an image handle, cannot be NULL.
	/// @param region       the region to copy, in the target image's coordinate.
	/// @return             whether the operation is successful
	bool (*copy_area)(struct backend_base *backend_data, ivec2 origin,
	                  image_handle target, image_handle source, const region_t *region)
	    __attribute__((nonnull(1, 3, 4, 5)));

	/// Similar to `copy_area`, but is specialized for copying from a higher
	/// precision format to a lower precision format. It has 2 major differences from
	/// `copy_area`:
	///
	///    1. This function _may_ use dithering when copying from a higher precision
	///       format to a lower precision format. But this is not required.
	///    2. This function only needs to support copying from an image with the SRC
	///       capability. Unlike `copy_area`, which supports copying from any image.
	///
	/// It's perfectly legal to have this pointing to the same function as
	/// `copy_area`, if the backend doesn't support dithering.
	///
	/// @param backend_data backend data
	/// @param origin       the origin of the operation, in the target image's
	///                     coordinate.
	/// @param target       an image handle, cannot be NULL.
	/// @param source       an image handle, cannot be NULL.
	/// @param region       the region to copy, in the target image's coordinate.
	/// @return             whether the operation is successful
	bool (*copy_area_quantize)(struct backend_base *backend_data, ivec2 origin,
	                           image_handle target, image_handle source,
	                           const region_t *region)
	    __attribute__((nonnull(1, 3, 4, 5)));

	/// Initialize an image with a given color value. If the image has a mask format,
	/// only the alpha channel of the color is used.
	///
	/// @param backend_data backend data
	/// @param target       an image handle, cannot be NULL.
	/// @param color        the color to fill the image with
	/// @return             whether the operation is successful
	bool (*clear)(struct backend_base *backend_data, image_handle target,
	              struct color color) __attribute__((nonnull(1, 2)));

	/// Present the back buffer to the target window. Ideally the backend should keep
	/// track of the region of the back buffer that has been updated, and use relevant
	/// mechanism (when possible) to present only the updated region.
	bool (*present)(struct backend_base *backend_data) __attribute__((nonnull(1)));

	// ============ Resource management ===========

	/// Create a shader object from a shader source.
	///
	/// Optional
	void *(*create_shader)(backend_t *backend_data, const struct shader_specification *,
	                       const char *source) __attribute__((nonnull(1, 2, 3)));

	/// Free a shader object.
	///
	/// Required if create_shader is present.
	void (*destroy_shader)(backend_t *backend_data, shader_handle shader)
	    __attribute__((nonnull(1, 2)));

	/// Create a new, uninitialized image with the given format and size.
	///
	/// @param backend_data backend data
	/// @param format       format of the image
	/// @param size         size of the image
	image_handle (*new_image)(struct backend_base *backend_data,
	                          enum backend_image_format format, ivec2 size)
	    __attribute__((nonnull(1)));

	/// Create a new image with the given format and size, and initialize it with
	/// data. Optional, only used if backend has quirk: BACKEND_QUIRK_SLOW_BLUR.
	///
	/// @param backend_data backend data
	/// @param format       format of the image
	/// @param size         size of the image
	/// @param pixels       data. for BACKEND_IMAGE_FORMAT_MASK, each byte is a pixel;
	///                     for BACKEND_IMAGE_FORMAT_PIXMAP, it's 4 bytes/pixel, each
	///                     pixel is given in the RGBA order.
	image_handle (*new_image_from_pixels)(struct backend_base *backend_data,
	                                      enum backend_image_format format, ivec2 size,
	                                      int stride, const uint8_t *pixels)
	    __attribute__((nonnull(1)));

	/// Bind a X pixmap to the backend's internal image data structure.
	///
	/// @param backend_data backend data
	/// @param pixmap       X pixmap to bind
	/// @param fmt          information of the pixmap's visual
	/// @return             backend specific image handle for the pixmap. May be
	///                     NULL.
	image_handle (*bind_pixmap)(struct backend_base *backend_data, xcb_pixmap_t pixmap,
	                            struct xvisual_info fmt) __attribute__((nonnull(1)));

	/// Acquire the image handle of the back buffer.
	///
	/// @param backend_data backend data
	image_handle (*back_buffer)(struct backend_base *backend_data);

	/// Free resources associated with an image data structure. Releasing the image
	/// returned by `back_buffer` should be a no-op.
	///
	/// @param image the image to be released, cannot be NULL.
	/// @return if this image is created by `bind_pixmap`, the X pixmap; 0
	///         otherwise.
	xcb_pixmap_t (*release_image)(struct backend_base *backend_data, image_handle image)
	    __attribute__((nonnull(1, 2)));

	// ===========        Query         ===========

	/// Get backend quirks
	/// @return a bitmask of `enum backend_quirk`.
	uint32_t (*quirks)(struct backend_base *backend_data) __attribute__((nonnull(1)));

	/// Get the version of the backend
	void (*version)(struct backend_base *backend_data, uint64_t *major, uint64_t *minor)
	    __attribute__((nonnull(1, 2, 3)));

	/// Check if an optional image format is supported by the backend.
	bool (*is_format_supported)(struct backend_base *backend_data,
	                            enum backend_image_format format)
	    __attribute__((nonnull(1)));

	/// Return the capabilities of an image.
	uint32_t (*image_capabilities)(struct backend_base *backend_data, image_handle image)
	    __attribute__((nonnull(1, 2)));

	/// Get the attributes of a shader.
	///
	/// Optional, Returns a bitmask of attributes, see `shader_attributes`.
	uint64_t (*get_shader_attributes)(backend_t *backend_data, shader_handle shader)
	    __attribute__((nonnull(1, 2)));

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
	int (*max_buffer_age)(backend_t *backend_data);

	// ===========    Post-processing   ============
	/// Create a blur context that can be used to call `blur` for images with a
	/// specific format.
	void *(*create_blur_context)(backend_t *base, enum blur_method,
	                             enum backend_image_format format, void *args);
	/// Destroy a blur context
	void (*destroy_blur_context)(backend_t *base, void *ctx);
	/// Get how many pixels outside of the blur area is needed for blur
	void (*get_blur_size)(void *blur_context, int *width, int *height);

	// ===========         Misc         ============
	/// Return the driver that is been used by the backend
	enum driver (*detect_driver)(backend_t *backend_data);

	void (*diagnostics)(backend_t *backend_data);

	enum device_status (*device_status)(backend_t *backend_data);
};

struct backend_base {
	struct backend_operations ops;
	struct x_connection *c;
	struct ev_loop *loop;

	/// Whether the backend can accept new render request at the moment
	bool busy;
	// ...
};

/// Register a new backend, `major` and `minor` should be the version of the picom backend
/// interface. You should just pass `PICOM_BACKEND_MAJOR` and `PICOM_BACKEND_MINOR` here.
/// `name` is the name of the backend, `init` is the function to initialize the backend,
/// `can_present` should be true if the backend can present the back buffer to the screen,
/// false otherwise (e.g. if the backend does off screen rendering, etc.)
bool backend_register(uint64_t major, uint64_t minor, const char *name,
                      struct backend_base *(*init)(session_t *ps, xcb_window_t target),
                      bool can_present);

/// Define a backend entry point. (Note constructor priority 202 is used here because 1xx
/// is reversed by test.h, and 201 is used for logging initialization.)
#define BACKEND_ENTRYPOINT(func) static void __attribute__((constructor(202))) func(void)
