// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <epoxy/gl.h>
#include <stdbool.h>
#include <string.h>
#include <xcb/xproto.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "log.h"
#include "region.h"

#define CASESTRRET(s)                                                                    \
	case s: return #s
struct gl_blur_context;

// Fragment shader uniforms
#define UNIFORM_OPACITY_LOC 1
#define UNIFORM_INVERT_COLOR_LOC 2
#define UNIFORM_TEX_LOC 3
#define UNIFORM_EFFECTIVE_SIZE_LOC 4
#define UNIFORM_DIM_LOC 5
#define UNIFORM_BRIGHTNESS_LOC 6
#define UNIFORM_MAX_BRIGHTNESS_LOC 7
#define UNIFORM_CORNER_RADIUS_LOC 8
#define UNIFORM_BORDER_WIDTH_LOC 9
#define UNIFORM_TIME_LOC 10
#define UNIFORM_COLOR_LOC 11
#define UNIFORM_PIXEL_NORM_LOC 12
#define UNIFORM_TEX_SRC_LOC 13
#define UNIFORM_MASK_TEX_LOC 14
#define UNIFORM_MASK_OFFSET_LOC 15
#define UNIFORM_MASK_CORNER_RADIUS_LOC 16
#define UNIFORM_MASK_INVERTED_LOC 17

// Vertex shader uniforms
#define UNIFORM_SCALE_LOC 18
#define UNIFORM_PROJECTION_LOC 19
#define UNIFORM_TEXSIZE_LOC 21
#define NUMBER_OF_UNIFORMS (UNIFORM_TEXSIZE_LOC + 1)

struct gl_shader {
	GLuint prog;
	// If the shader is user controlled, we don't know which uniform will be
	// active, so we need to track which one is.
	// This is not used if the shader code is fully controlled by us.
	uint32_t uniform_bitmask;
};

/// @brief Wrapper of a bound GL texture.
struct gl_texture {
	int refcount;
	bool has_alpha;
	GLuint texture;
	int width, height;
	/// Whether the texture is Y-inverted
	/// This is always true for all our internal textures. Textures created from
	/// binding a X pixmap might not be. And our OpenGL back buffer is never
	/// Y-inverted (until we can start using glClipControl).
	bool y_inverted;
	xcb_pixmap_t pixmap;

	// Textures for auxiliary uses.
	GLuint auxiliary_texture[2];
	void *user_data;
};

struct gl_data {
	backend_t base;
	// If we are using proprietary NVIDIA driver
	bool is_nvidia;
	// If ARB_robustness extension is present
	bool has_robustness;
	// If EXT_EGL_image_storage extension is present
	bool has_egl_image_storage;
	// Height and width of the root window
	int height, width;
	struct gl_shader default_shader;
	struct gl_shader brightness_shader;
	struct gl_shader fill_shader;
	struct gl_shader shadow_shader;
	GLuint back_texture, back_fbo;
	GLuint temp_fbo;
	GLint back_format;
	GLuint frame_timing[2];
	int current_frame_timing;
	GLuint present_prog;
	struct gl_shader dummy_prog;

	bool dithered_present;

	GLuint default_mask_texture;

	/// Called when an gl_texture is decoupled from the texture it refers. Returns
	/// the decoupled user_data
	void *(*decouple_texture_user_data)(backend_t *base, void *user_data);

	/// Release the user data attached to a gl_texture
	void (*release_user_data)(backend_t *base, struct gl_texture *);

	struct log_target *logger;
};

typedef struct session session_t;

#define GL_PROG_MAIN_INIT                                                                \
	{ .prog = 0, .unifm_opacity = -1, .unifm_invert_color = -1, .unifm_tex = -1, }

void gl_prepare(backend_t *base, const region_t *reg);
/// Convert a mask formed by a collection of rectangles to OpenGL vertex and texture
/// coordinates.
///
/// @param[in]  origin      origin of the source image in target coordinates
/// @param[in]  mask_origin origin of the mask in source coordinates
/// @param[in]  nrects      number of rectangles
/// @param[in]  rects       mask rectangles, in mask coordinates
/// @param[out] coord       OpenGL vertex coordinates, suitable for creating VAO/VBO
/// @param[out] indices     OpenGL vertex indices, suitable for creating VAO/VBO
void gl_mask_rects_to_coords(struct coord origin, struct coord mask_origin, int nrects,
                             const rect_t *rects, GLint *coord, GLuint *indices);
/// Like `gl_mask_rects_to_coords`, but with `origin` and `mask_origin` set to 0. i.e. all
/// coordinates are in the same space.
static inline void gl_mask_rects_to_coords_simple(int nrects, const rect_t *rects,
                                                  GLint *coord, GLuint *indices) {
	return gl_mask_rects_to_coords((struct coord){0, 0}, (struct coord){0, 0}, nrects,
	                               rects, coord, indices);
}

GLuint gl_create_shader(GLenum shader_type, const char *shader_str);
GLuint gl_create_program(const GLuint *const shaders, int nshaders);
GLuint gl_create_program_from_str(const char *vert_shader_str, const char *frag_shader_str);
GLuint gl_create_program_from_strv(const char **vert_shaders, const char **frag_shaders);
void *gl_create_window_shader(backend_t *backend_data, const char *source);
void gl_destroy_window_shader(backend_t *backend_data, void *shader);
uint64_t gl_get_shader_attributes(backend_t *backend_data, void *shader);
bool gl_last_render_time(backend_t *backend_data, struct timespec *time);

/**
 * @brief Render a region with texture data.
 */
void gl_compose(backend_t *, image_handle image, coord_t image_dst, image_handle mask,
                coord_t mask_dst, const region_t *reg_tgt, const region_t *reg_visible);

void gl_root_change(backend_t *base, session_t *);

void gl_resize(struct gl_data *, int width, int height);

bool gl_init(struct gl_data *gd, session_t *);
void gl_deinit(struct gl_data *gd);

GLuint gl_new_texture(GLenum target);

bool gl_image_op(backend_t *base, enum image_operations op, image_handle image,
                 const region_t *reg_op, const region_t *reg_visible, void *arg);

xcb_pixmap_t gl_release_image(backend_t *base, image_handle image);
image_handle gl_make_mask(backend_t *base, geometry_t size, const region_t *reg);

image_handle gl_clone(backend_t *base, image_handle image, const region_t *reg_visible);

bool gl_blur(backend_t *base, double opacity, void *ctx, image_handle mask,
             coord_t mask_dst, const region_t *reg_blur, const region_t *reg_visible);
bool gl_blur_impl(double opacity, struct gl_blur_context *bctx,
                  struct backend_image *mask, coord_t mask_dst, const region_t *reg_blur,
                  GLuint source_texture, geometry_t source_size, bool source_y_inverted,
                  GLuint target_fbo, GLuint default_mask);
void *gl_create_blur_context(backend_t *base, enum blur_method,
                             enum backend_image_format format, void *args);
void gl_destroy_blur_context(backend_t *base, void *ctx);
struct backend_shadow_context *gl_create_shadow_context(backend_t *base, double radius);
void gl_destroy_shadow_context(backend_t *base attr_unused, struct backend_shadow_context *ctx);
image_handle gl_shadow_from_mask(backend_t *base, image_handle mask,
                                 struct backend_shadow_context *sctx, struct color color);
void gl_get_blur_size(void *blur_context, int *width, int *height);

void gl_fill(backend_t *base, struct color, const region_t *clip);

void gl_present(backend_t *base, const region_t *);
enum device_status gl_device_status(backend_t *base);

/**
 * Get a textual representation of an OpenGL error.
 */
static inline const char *gl_get_err_str(GLenum err) {
	switch (err) {
		CASESTRRET(GL_NO_ERROR);
		CASESTRRET(GL_INVALID_ENUM);
		CASESTRRET(GL_INVALID_VALUE);
		CASESTRRET(GL_INVALID_OPERATION);
		CASESTRRET(GL_INVALID_FRAMEBUFFER_OPERATION);
		CASESTRRET(GL_OUT_OF_MEMORY);
		CASESTRRET(GL_STACK_UNDERFLOW);
		CASESTRRET(GL_STACK_OVERFLOW);
		CASESTRRET(GL_FRAMEBUFFER_UNDEFINED);
		CASESTRRET(GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);
		CASESTRRET(GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT);
		CASESTRRET(GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER);
		CASESTRRET(GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER);
		CASESTRRET(GL_FRAMEBUFFER_UNSUPPORTED);
		CASESTRRET(GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE);
		CASESTRRET(GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS);
	}
	return NULL;
}

/**
 * Check for GL error.
 *
 * http://blog.nobel-joergensen.com/2013/01/29/debugging-opengl-using-glgeterror/
 */
static inline void gl_check_err_(const char *func, int line) {
	GLenum err = GL_NO_ERROR;

	while (GL_NO_ERROR != (err = glGetError())) {
		const char *errtext = gl_get_err_str(err);
		if (errtext) {
			log_printf(tls_logger, LOG_LEVEL_ERROR, func,
			           "GL error at line %d: %s", line, errtext);
		} else {
			log_printf(tls_logger, LOG_LEVEL_ERROR, func,
			           "GL error at line %d: %d", line, err);
		}
	}
}

static inline void gl_clear_err(void) {
	while (glGetError() != GL_NO_ERROR) {
	}
}

#define gl_check_err() gl_check_err_(__func__, __LINE__)

/**
 * Check for GL framebuffer completeness.
 */
static inline bool gl_check_fb_complete_(const char *func, int line, GLenum fb) {
	GLenum status = glCheckFramebufferStatus(fb);

	if (status == GL_FRAMEBUFFER_COMPLETE) {
		return true;
	}

	const char *stattext = gl_get_err_str(status);
	if (stattext) {
		log_printf(tls_logger, LOG_LEVEL_ERROR, func,
		           "Framebuffer attachment failed at line %d: %s", line, stattext);
	} else {
		log_printf(tls_logger, LOG_LEVEL_ERROR, func,
		           "Framebuffer attachment failed at line %d: %d", line, status);
	}

	return false;
}

#define gl_check_fb_complete(fb) gl_check_fb_complete_(__func__, __LINE__, (fb))

static const GLuint vert_coord_loc = 0;
static const GLuint vert_in_texcoord_loc = 1;

// Add one level of indirection so macros in VA_ARGS can be expanded.
#define GLSL_(version, ...)                                                              \
	"#version " #version "\n"                                                        \
	"#extension GL_ARB_explicit_uniform_location : enable\n" #__VA_ARGS__
#define GLSL(version, ...) GLSL_(version, __VA_ARGS__)
#define QUOTE(...) #__VA_ARGS__

extern const char vertex_shader[], blend_with_mask_frag[], masking_glsl[], dummy_frag[],
    present_frag[], fill_frag[], fill_vert[], interpolating_frag[], interpolating_vert[],
    win_shader_glsl[], win_shader_default[], present_vertex_shader[], dither_glsl[],
    shadow_colorization_frag[];
