// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdbool.h>
#include <string.h>

#include "backend/backend.h"
#include "log.h"
#include "region.h"

#define CASESTRRET(s)                                                                    \
	case s: return #s
struct gl_blur_context;

static inline GLint glGetUniformLocationChecked(GLuint p, const char *name) {
	auto ret = glGetUniformLocation(p, name);
	if (ret < 0) {
		log_info("Failed to get location of uniform '%s'. This is normal when "
		         "using custom shaders.",
		         name);
	}
	return ret;
}

#define bind_uniform(shader, uniform)                                                    \
	(shader)->uniform_##uniform = glGetUniformLocationChecked((shader)->prog, #uniform)

// Program and uniforms for window shader
typedef struct {
	UT_hash_handle hh;
	uint32_t id;
	GLuint prog;
	GLint uniform_opacity;
	GLint uniform_invert_color;
	GLint uniform_tex;
	GLint uniform_dim;
	GLint uniform_brightness;
	GLint uniform_max_brightness;
	GLint uniform_corner_radius;
	GLint uniform_border_width;
	GLint uniform_time;

	GLint uniform_mask_tex;
	GLint uniform_mask_offset;
	GLint uniform_mask_corner_radius;
	GLint uniform_mask_inverted;
} gl_win_shader_t;

// Program and uniforms for brightness shader
typedef struct {
	GLuint prog;
} gl_brightness_shader_t;

typedef struct {
	GLuint prog;
	GLint uniform_color;
} gl_shadow_shader_t;

// Program and uniforms for blur shader
typedef struct {
	GLuint prog;
	GLint uniform_pixel_norm;
	GLint uniform_opacity;
	GLint texorig_loc;
	GLint scale_loc;

	GLint uniform_mask_tex;
	GLint uniform_mask_offset;
	GLint uniform_mask_corner_radius;
	GLint uniform_mask_inverted;
} gl_blur_shader_t;

typedef struct {
	GLuint prog;
	GLint color_loc;
} gl_fill_shader_t;

/// @brief Wrapper of a binded GLX texture.
struct gl_texture {
	int refcount;
	bool has_alpha;
	GLuint texture;
	int width, height;
	bool y_inverted;

	// Textures for auxiliary uses.
	GLuint auxiliary_texture[2];
	gl_win_shader_t *shader;
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
	// Hash-table of window shaders
	gl_win_shader_t *default_shader;
	gl_brightness_shader_t brightness_shader;
	gl_fill_shader_t fill_shader;
	gl_shadow_shader_t shadow_shader;
	GLuint back_texture, back_fbo;
	GLint back_format;
	GLuint present_prog;

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

void x_rect_to_coords(int nrects, const rect_t *rects, coord_t image_dst,
                      int extent_height, int texture_height, int root_height,
                      bool y_inverted, GLint *coord, GLuint *indices);

GLuint gl_create_shader(GLenum shader_type, const char *shader_str);
GLuint gl_create_program(const GLuint *const shaders, int nshaders);
GLuint gl_create_program_from_str(const char *vert_shader_str, const char *frag_shader_str);
GLuint gl_create_program_from_strv(const char **vert_shaders, const char **frag_shaders);
void *gl_create_window_shader(backend_t *backend_data, const char *source);
void gl_destroy_window_shader(backend_t *backend_data, void *shader);
uint64_t gl_get_shader_attributes(backend_t *backend_data, void *shader);
bool gl_set_image_property(backend_t *backend_data, enum image_properties prop,
                           void *image_data, void *args);

/**
 * @brief Render a region with texture data.
 */
void gl_compose(backend_t *, void *image_data, coord_t image_dst, void *mask,
                coord_t mask_dst, const region_t *reg_tgt, const region_t *reg_visible);

void gl_resize(struct gl_data *, int width, int height);

bool gl_init(struct gl_data *gd, session_t *);
void gl_deinit(struct gl_data *gd);

GLuint gl_new_texture(GLenum target);

bool gl_image_op(backend_t *base, enum image_operations op, void *image_data,
                 const region_t *reg_op, const region_t *reg_visible, void *arg);

void gl_release_image(backend_t *base, void *image_data);
void *gl_make_mask(backend_t *base, geometry_t size, const region_t *reg);

void *gl_clone(backend_t *base, const void *image_data, const region_t *reg_visible);

bool gl_blur(backend_t *base, double opacity, void *ctx, void *mask, coord_t mask_dst,
             const region_t *reg_blur, const region_t *reg_visible);
bool gl_blur_impl(double opacity, struct gl_blur_context *bctx, void *mask, coord_t mask_dst,
                  const region_t *reg_blur, const region_t *reg_visible attr_unused,
                  GLuint source_texture, geometry_t source_size, GLuint target_fbo,
                  GLuint default_mask, bool high_precision);
void *gl_create_blur_context(backend_t *base, enum blur_method, void *args);
void gl_destroy_blur_context(backend_t *base, void *ctx);
struct backend_shadow_context *gl_create_shadow_context(backend_t *base, double radius);
void gl_destroy_shadow_context(backend_t *base attr_unused, struct backend_shadow_context *ctx);
void *gl_shadow_from_mask(backend_t *base, void *mask,
                          struct backend_shadow_context *sctx, struct color color);
void gl_get_blur_size(void *blur_context, int *width, int *height);

void gl_fill(backend_t *base, struct color, const region_t *clip);

void gl_present(backend_t *base, const region_t *);
bool gl_read_pixel(backend_t *base, void *image_data, int x, int y, struct color *output);
enum device_status gl_device_status(backend_t *base);

static inline void gl_delete_texture(GLuint texture) {
	glDeleteTextures(1, &texture);
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
 * Check for GLX error.
 *
 * http://blog.nobel-joergensen.com/2013/01/29/debugging-opengl-using-glgeterror/
 */
static inline void gl_check_err_(const char *func, int line) {
	GLenum err = GL_NO_ERROR;

	while (GL_NO_ERROR != (err = glGetError())) {
		const char *errtext = gl_get_err_str(err);
		if (errtext) {
			log_printf(tls_logger, LOG_LEVEL_ERROR, func,
			           "GLX error at line %d: %s", line, errtext);
		} else {
			log_printf(tls_logger, LOG_LEVEL_ERROR, func,
			           "GLX error at line %d: %d", line, err);
		}
	}
}

static inline void gl_clear_err(void) {
	while (glGetError() != GL_NO_ERROR)
		;
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

/**
 * Check if a GLX extension exists.
 */
static inline bool gl_has_extension(const char *ext) {
	int nexts = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &nexts);
	for (int i = 0; i < nexts || !nexts; i++) {
		const char *exti = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (exti == NULL) {
			break;
		}
		if (strcmp(ext, exti) == 0) {
			return true;
		}
	}
	gl_clear_err();
	log_info("Missing GL extension %s.", ext);
	return false;
}

static const GLuint vert_coord_loc = 0;
static const GLuint vert_in_texcoord_loc = 1;

#define GLSL(version, ...) "#version " #version "\n" #__VA_ARGS__
#define QUOTE(...) #__VA_ARGS__

extern const char vertex_shader[], copy_with_mask_frag[], masking_glsl[], dummy_frag[],
    present_frag[], fill_frag[], fill_vert[], interpolating_frag[], interpolating_vert[],
    win_shader_glsl[], win_shader_default[], present_vertex_shader[], dither_glsl[],
    shadow_colorization_frag[];
