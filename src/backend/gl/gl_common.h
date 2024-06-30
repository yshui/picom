// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <epoxy/gl.h>
#include <stdbool.h>
#include <string.h>
#include <xcb/xproto.h>

#include "backend/backend.h"

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
#define UNIFORM_FRAME_OPACITY_LOC 22
#define UNIFORM_FRAME_OPACITY_FSC_LOC 23
#define UNIFORM_FRAME_OPACITY_FSCT_LOC 24
#define UNIFORM_FRAME_OPACITY_FSCM_LOC 25
#define NUMBER_OF_UNIFORMS (UNIFORM_FRAME_OPACITY_FSCM_LOC + 1)

struct gl_shader {
	GLuint prog;
	// If the shader is user controlled, we don't know which uniform will be
	// active, so we need to track which one is.
	// This is not used if the shader code is fully controlled by us.
	uint32_t uniform_bitmask;
};

/// @brief Wrapper of a bound GL texture.
struct gl_texture {
	enum backend_image_format format;
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

enum gl_sampler {
	/// A sampler that repeats the texture, with nearest filtering.
	GL_SAMPLER_REPEAT = 0,
	/// A sampler that repeats the texture, with linear filtering.
	GL_SAMPLER_REPEAT_SCALE,
	/// Clamp to edge
	GL_SAMPLER_EDGE,
	/// Clamp to border, border color will be (0, 0, 0, 0)
	GL_SAMPLER_BORDER,
	/// Special sampler for blurring, same as `GL_SAMPLER_CLAMP_TO_EDGE`,
	/// but uses linear filtering.
	GL_SAMPLER_BLUR,
	GL_MAX_SAMPLERS = GL_SAMPLER_BLUR + 1,
};

struct gl_data {
	struct backend_base base;
	// If we are using proprietary NVIDIA driver
	bool is_nvidia;
	// If ARB_robustness extension is present
	bool has_robustness;
	// If EXT_EGL_image_storage extension is present
	bool has_egl_image_storage;
	/// A symbolic image representing the back buffer.
	struct gl_texture back_image;
	struct gl_shader default_shader;
	struct gl_shader brightness_shader;
	struct gl_shader fill_shader;
	GLuint temp_fbo;
	GLuint frame_timing[2];
	int current_frame_timing;
	struct gl_shader copy_area_prog;
	struct gl_shader copy_area_with_dither_prog;
	GLuint samplers[GL_MAX_SAMPLERS];
	GLuint buffer_objects[4];
	GLuint vertex_array_objects[2];

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
void gl_mask_rects_to_coords(ivec2 origin, int nrects, const rect_t *rects, vec2 scale,
                             GLfloat *coord, GLuint *indices);
/// Like `gl_mask_rects_to_coords`, but with `origin` is (0, 0).
static inline void gl_mask_rects_to_coords_simple(int nrects, const rect_t *rects,
                                                  GLfloat *coord, GLuint *indices) {
	return gl_mask_rects_to_coords((ivec2){0, 0}, nrects, rects, SCALE_IDENTITY,
	                               coord, indices);
}

GLuint gl_create_shader(GLenum shader_type, const char *shader_str);
GLuint gl_create_program(const GLuint *shaders, int nshaders);
GLuint gl_create_program_from_str(const char *vert_shader_str, const char *frag_shader_str);
GLuint gl_create_program_from_strv(const char **vert_shaders, const char **frag_shaders);
void *gl_create_window_shader(backend_t *backend_data, const char *source);
void gl_destroy_window_shader(backend_t *backend_data, void *shader);
uint64_t gl_get_shader_attributes(backend_t *backend_data, void *shader);
bool gl_last_render_time(backend_t *backend_data, struct timespec *time);

bool gl_blit(backend_t *base, ivec2 origin, image_handle target,
             const struct backend_blit_args *args);
image_handle gl_new_image(backend_t *backend_data attr_unused,
                          enum backend_image_format format, ivec2 size);
bool gl_clear(backend_t *backend_data, image_handle target, struct color color);

void gl_root_change(backend_t *base, session_t *);

void gl_resize(struct gl_data *, int width, int height);

bool gl_init(struct gl_data *gd, session_t *);
void gl_deinit(struct gl_data *gd);

GLuint gl_new_texture(void);

xcb_pixmap_t gl_release_image(backend_t *base, image_handle image);

image_handle gl_clone(backend_t *base, image_handle image, const region_t *reg_visible);

bool gl_blur(struct backend_base *gd, ivec2 origin, image_handle target,
             const struct backend_blur_args *args);
bool gl_copy_area(backend_t *backend_data, ivec2 origin, image_handle target,
                  image_handle source, const region_t *region);
bool gl_copy_area_quantize(backend_t *backend_data, ivec2 origin, image_handle target_handle,
                           image_handle source_handle, const region_t *region);
bool gl_apply_alpha(backend_t *base, image_handle target, double alpha, const region_t *reg_op);
image_handle gl_back_buffer(struct backend_base *base);
uint32_t gl_image_capabilities(backend_t *base, image_handle img);
bool gl_is_format_supported(backend_t *base, enum backend_image_format format);
void *gl_create_blur_context(backend_t *base, enum blur_method,
                             enum backend_image_format format, void *args);
void gl_destroy_blur_context(backend_t *base, void *ctx);
void gl_get_blur_size(void *blur_context, int *width, int *height);

enum device_status gl_device_status(backend_t *base);

#define gl_check_fb_complete(fb) gl_check_fb_complete_(__func__, __LINE__, (fb))
static inline bool gl_check_fb_complete_(const char *func, int line, GLenum fb);

static inline void gl_finish_render(struct gl_data *gd) {
	glEndQuery(GL_TIME_ELAPSED);
	gd->current_frame_timing ^= 1;
}

/// Return a FBO with `image` bound to the first color attachment. `GL_DRAW_FRAMEBUFFER`
/// will be bound to the returned FBO.
static inline GLuint gl_bind_image_to_fbo(struct gl_data *gd, image_handle image_) {
	auto image = (struct gl_texture *)image_;
	if (image == &gd->back_image) {
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		return 0;
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       image->texture, 0);
	CHECK(gl_check_fb_complete(GL_DRAW_FRAMEBUFFER));
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	return gd->temp_fbo;
}

/// Flip the target coordinates returned by `gl_mask_rects_to_coords` vertically relative
/// to the target. Texture coordinates are unchanged.
///
/// @param[in] nrects        number of rectangles
/// @param[in] coord         OpenGL vertex coordinates
/// @param[in] target_height height of the target image
static inline void gl_y_flip_target(int nrects, GLfloat *coord, GLint target_height) {
	for (ptrdiff_t i = 0; i < nrects; i++) {
		auto current_rect = &coord[i * 16];        // 16 numbers per rectangle
		for (ptrdiff_t j = 0; j < 4; j++) {
			// 4 numbers per vertex, target coordinates are the first two
			auto current_vertex = &current_rect[j * 4];
			current_vertex[1] = (GLfloat)target_height - current_vertex[1];
		}
	}
}

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

static const GLuint vert_coord_loc = 0;
static const GLuint vert_in_texcoord_loc = 1;

// Add one level of indirection so macros in VA_ARGS can be expanded.
#define GLSL_(version, ...)                                                              \
	"#version " #version "\n"                                                        \
	"#extension GL_ARB_explicit_uniform_location : enable\n" #__VA_ARGS__
#define GLSL(version, ...) GLSL_(version, __VA_ARGS__)
#define QUOTE(...) #__VA_ARGS__

extern const char vertex_shader[], blend_with_mask_frag[], masking_glsl[],
    copy_area_frag[], copy_area_with_dither_frag[], fill_frag[], fill_vert[],
    interpolating_frag[], interpolating_vert[], blit_shader_glsl[], blit_shader_default[],
    present_vertex_shader[], dither_glsl[];
