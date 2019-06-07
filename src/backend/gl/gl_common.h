// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#pragma once
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdbool.h>
#include <string.h>

#include "backend/backend.h"
#include "config.h"
#include "log.h"
#include "region.h"

#define CASESTRRET(s)                                                                    \
	case s: return #s

// Program and uniforms for window shader
typedef struct {
	GLuint prog;
	GLint unifm_opacity;
	GLint unifm_invert_color;
	GLint unifm_tex;
	GLint unifm_dim;
	GLint in_texcoord;
	GLint coord_loc;
} gl_win_shader_t;

// Program and uniforms for blur shader
typedef struct {
	GLuint prog;
	GLint unifm_offset_x;
	GLint unifm_offset_y;
	GLint unifm_opacity;
	GLint orig_loc;
	GLint coord_loc;
	GLint in_texcoord;
} gl_blur_shader_t;

typedef struct {
	GLuint prog;
	GLint in_coord_loc;
	GLint color_loc;
} gl_fill_shader_t;

struct gl_texture {
	int refcount;
	GLuint texture;
	int width, height;
	bool y_inverted;
	void *user_data;
};

/// @brief Wrapper of a binded GLX texture.
typedef struct gl_image {
	struct gl_texture *inner;
	double opacity;
	double dim;
	int ewidth, eheight;
	bool has_alpha;
	bool color_inverted;
} gl_image_t;

struct gl_data {
	backend_t base;
	// If we are using proprietary NVIDIA driver
	bool is_nvidia;
	// Height and width of the viewport
	int height, width;
	int npasses;
	gl_win_shader_t win_shader;
	gl_fill_shader_t fill_shader;
	gl_blur_shader_t *blur_shader;

	// Temporary textures used for blurring. They are always the same size as the
	// target, so they are always big enough without resizing.
	// Turns out calling glTexImage to resize is expensive, so we avoid that.
	GLuint blur_texture[2];
	// Temporary fbo used for blurring
	GLuint blur_fbo;

	/// Called when an gl_texture is decoupled from the texture it refers. Returns
	/// the decoupled user_data
	void *(*decouple_texture_user_data)(backend_t *base, void *user_data);

	/// Release the user data attached to a gl_texture
	void (*release_user_data)(backend_t *base, struct gl_texture *);

	struct log_target *logger;
};

typedef struct {
	/// Framebuffer used for blurring.
	GLuint fbo;
	/// Textures used for blurring.
	GLuint textures[2];
	/// Width of the textures.
	int width;
	/// Height of the textures.
	int height;
} gl_blur_cache_t;

typedef struct session session_t;

#define GL_PROG_MAIN_INIT                                                                \
	{ .prog = 0, .unifm_opacity = -1, .unifm_invert_color = -1, .unifm_tex = -1, }

GLuint gl_create_shader(GLenum shader_type, const char *shader_str);
GLuint gl_create_program(const GLuint *const shaders, int nshaders);
GLuint gl_create_program_from_str(const char *vert_shader_str, const char *frag_shader_str);

/**
 * @brief Render a region with texture data.
 */
void gl_compose(backend_t *, void *ptex, int dst_x, int dst_y, const region_t *reg_tgt,
                const region_t *reg_visible);

void gl_resize(struct gl_data *, int width, int height);

bool gl_init(struct gl_data *gd, session_t *);
void gl_deinit(struct gl_data *gd);

GLuint gl_new_texture(GLenum target);

bool gl_image_op(backend_t *base, enum image_operations op, void *image_data,
                 const region_t *reg_op, const region_t *reg_visible, void *arg);

void gl_release_image(backend_t *base, void *image_data);

void *gl_copy(backend_t *base, const void *image_data, const region_t *reg_visible);

bool gl_blur(backend_t *base, double opacity, const region_t *reg_blur,
             const region_t *reg_visible);

bool gl_is_image_transparent(backend_t *base, void *image_data);
void gl_fill(backend_t *base, struct color, const region_t *clip);

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
	while (glGetError() != GL_NO_ERROR);
}

#define gl_check_err() gl_check_err_(__func__, __LINE__)

/**
 * Check if a GLX extension exists.
 */
static inline bool gl_has_extension(const char *ext) {
	for (int i = 0; ; i++) {
		const char *exti = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (exti == 0) {
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
