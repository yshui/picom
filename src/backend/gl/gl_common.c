// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <epoxy/gl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX

#include "backend/backend.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "kernel.h"
#include "log.h"
#include "region.h"
#include "types.h"
#include "utils.h"

#include "backend/backend_common.h"
#include "backend/gl/gl_common.h"

void gl_prepare(backend_t *base, const region_t *reg attr_unused) {
	auto gd = (struct gl_data *)base;
	glBeginQuery(GL_TIME_ELAPSED, gd->frame_timing[gd->current_frame_timing]);
}

GLuint gl_create_shader(GLenum shader_type, const char *shader_str) {
	log_trace("===\n%s\n===", shader_str);

	bool success = false;
	GLuint shader = glCreateShader(shader_type);
	if (!shader) {
		log_error("Failed to create shader with type %#x.", shader_type);
		goto end;
	}
	glShaderSource(shader, 1, &shader_str, NULL);
	glCompileShader(shader);

	// Get shader status
	{
		GLint status = GL_FALSE;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
		if (status == GL_FALSE) {
			GLint log_len = 0;
			glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
			if (log_len) {
				char log[log_len + 1];
				glGetShaderInfoLog(shader, log_len, NULL, log);
				log_error("Failed to compile shader with type %d: %s",
				          shader_type, log);
			}
			goto end;
		}
	}

	success = true;

end:
	if (shader && !success) {
		glDeleteShader(shader);
		shader = 0;
	}
	gl_check_err();

	return shader;
}

GLuint gl_create_program(const GLuint *const shaders, int nshaders) {
	bool success = false;
	GLuint program = glCreateProgram();
	if (!program) {
		log_error("Failed to create program.");
		goto end;
	}

	for (int i = 0; i < nshaders; ++i) {
		glAttachShader(program, shaders[i]);
	}
	glLinkProgram(program);

	// Get program status
	{
		GLint status = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (status == GL_FALSE) {
			GLint log_len = 0;
			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
			if (log_len) {
				char log[log_len + 1];
				glGetProgramInfoLog(program, log_len, NULL, log);
				log_error("Failed to link program: %s", log);
			}
			goto end;
		}
	}
	success = true;

end:
	if (program) {
		for (int i = 0; i < nshaders; ++i) {
			glDetachShader(program, shaders[i]);
		}
	}
	if (program && !success) {
		glDeleteProgram(program);
		program = 0;
	}
	gl_check_err();

	return program;
}

/**
 * @brief Create a program from NULL-terminated arrays of vertex and fragment shader
 * strings.
 */
GLuint gl_create_program_from_strv(const char **vert_shaders, const char **frag_shaders) {
	int vert_count, frag_count;
	for (vert_count = 0; vert_shaders && vert_shaders[vert_count]; ++vert_count) {
	}
	for (frag_count = 0; frag_shaders && frag_shaders[frag_count]; ++frag_count) {
	}

	GLuint prog = 0;
	auto shaders = (GLuint *)ccalloc(vert_count + frag_count, GLuint);
	for (int i = 0; i < vert_count; ++i) {
		shaders[i] = gl_create_shader(GL_VERTEX_SHADER, vert_shaders[i]);
		if (shaders[i] == 0) {
			goto out;
		}
	}
	for (int i = 0; i < frag_count; ++i) {
		shaders[vert_count + i] =
		    gl_create_shader(GL_FRAGMENT_SHADER, frag_shaders[i]);
		if (shaders[vert_count + i] == 0) {
			goto out;
		}
	}

	prog = gl_create_program(shaders, vert_count + frag_count);

out:
	for (int i = 0; i < vert_count + frag_count; ++i) {
		if (shaders[i] != 0) {
			glDeleteShader(shaders[i]);
		}
	}
	free(shaders);
	gl_check_err();

	return prog;
}

/**
 * @brief Create a program from vertex and fragment shader strings.
 */
GLuint gl_create_program_from_str(const char *vert_shader_str, const char *frag_shader_str) {
	const char *vert_shaders[2] = {vert_shader_str, NULL};
	const char *frag_shaders[2] = {frag_shader_str, NULL};

	return gl_create_program_from_strv(vert_shaders, frag_shaders);
}

void gl_destroy_window_shader(backend_t *backend_data attr_unused, void *shader) {
	if (!shader) {
		return;
	}

	auto pprogram = (struct gl_shader *)shader;
	if (pprogram->prog) {
		glDeleteProgram(pprogram->prog);
		pprogram->prog = 0;
	}
	gl_check_err();

	free(shader);
}

/*
 * @brief Implements recursive part of gl_average_texture_color.
 *
 * @note In order to reduce number of textures which needs to be
 * allocated and deleted during this recursive render
 * we reuse the same two textures for render source and
 * destination simply by alternating between them.
 * Unfortunately on first iteration source_texture might
 * be read-only. In this case we will select auxiliary_texture as
 * destination_texture in order not to touch that read-only source
 * texture in following render iteration.
 * Otherwise we simply will switch source and destination textures
 * between each other on each render iteration.
 */
static GLuint
_gl_average_texture_color(GLuint source_texture, GLuint destination_texture,
                          GLuint auxiliary_texture, GLuint fbo, int width, int height) {
	const int max_width = 1;
	const int max_height = 1;
	const int from_width = next_power_of_two(width);
	const int from_height = next_power_of_two(height);
	const int to_width = from_width > max_width ? from_width / 2 : from_width;
	const int to_height = from_height > max_height ? from_height / 2 : from_height;

	// Prepare coordinates
	GLint coord[] = {
	    // top left
	    0, 0,        // vertex coord
	    0, 0,        // texture coord

	    // top right
	    to_width, 0,        // vertex coord
	    width, 0,           // texture coord

	    // bottom right
	    to_width, to_height,        // vertex coord
	    width, height,              // texture coord

	    // bottom left
	    0, to_height,        // vertex coord
	    0, height,           // texture coord
	};
	glBufferSubData(GL_ARRAY_BUFFER, 0, (long)sizeof(*coord) * 16, coord);

	// Prepare framebuffer for new render iteration
	glBindTexture(GL_TEXTURE_2D, destination_texture);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       destination_texture, 0);
	gl_check_fb_complete(GL_FRAMEBUFFER);

	// Bind source texture as downscaling shader uniform input
	glBindTexture(GL_TEXTURE_2D, source_texture);

	// Render into framebuffer
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);

	// Have we downscaled enough?
	GLuint result;
	if (to_width > max_width || to_height > max_height) {
		GLuint new_source_texture = destination_texture;
		GLuint new_destination_texture =
		    auxiliary_texture != 0 ? auxiliary_texture : source_texture;
		result = _gl_average_texture_color(
		    new_source_texture, new_destination_texture, 0, fbo, to_width, to_height);
	} else {
		result = destination_texture;
	}

	return result;
}

/*
 * @brief Builds a 1x1 texture which has color corresponding to the average of all
 * pixels of img by recursively rendering into texture of quarter the size (half
 * width and half height).
 * Returned texture must not be deleted, since it's owned by the gl_image. It will be
 * deleted when the gl_image is released.
 */
static GLuint gl_average_texture_color(struct gl_data *gd, struct gl_texture *img) {
	// Prepare textures which will be used for destination and source of rendering
	// during downscaling.
	const int texture_count = ARR_SIZE(img->auxiliary_texture);
	if (!img->auxiliary_texture[0]) {
		assert(!img->auxiliary_texture[1]);
		glGenTextures(texture_count, img->auxiliary_texture);
		glActiveTexture(GL_TEXTURE0);
		for (int i = 0; i < texture_count; i++) {
			glBindTexture(GL_TEXTURE_2D, img->auxiliary_texture[i]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
			glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR,
			                 (GLint[]){0, 0, 0, 0});
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, img->width, img->height,
			             0, GL_BGR, GL_UNSIGNED_BYTE, NULL);
		}
	}

	// Prepare framebuffer used for rendering and bind it
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	// Enable shaders
	glUseProgram(gd->brightness_shader.prog);
	glUniform2f(UNIFORM_TEXSIZE_LOC, (GLfloat)img->width, (GLfloat)img->height);

	// Prepare vertex attributes
	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	GLuint bo[2];
	glGenBuffers(2, bo);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_INT, GL_FALSE,
	                      sizeof(GLint) * 4, (void *)(sizeof(GLint) * 2));

	// Allocate buffers for render input
	GLint coord[16] = {0};
	GLuint indices[] = {0, 1, 2, 2, 3, 0};
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * 16, coord, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * 6, indices,
	             GL_STREAM_DRAW);

	// Do actual recursive render to 1x1 texture
	GLuint result_texture = _gl_average_texture_color(
	    img->texture, img->auxiliary_texture[0], img->auxiliary_texture[1],
	    gd->temp_fbo, img->width, img->height);

	// Cleanup vertex attributes
	glDisableVertexAttribArray(vert_coord_loc);
	glDisableVertexAttribArray(vert_in_texcoord_loc);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDeleteBuffers(2, bo);
	glBindVertexArray(0);
	glDeleteVertexArrays(1, &vao);

	// Cleanup shaders
	glUseProgram(0);

	// Cleanup framebuffers
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);

	// Cleanup render textures
	glBindTexture(GL_TEXTURE_2D, 0);

	gl_check_err();

	return result_texture;
}

struct gl_uniform_value {
	GLenum type;
	union {
		GLuint texture;
		GLint i;
		GLfloat f;
		GLint i2[2];
		GLfloat f2[2];
		GLfloat f4[4];
	};
};

struct gl_vertex_attrib {
	GLenum type;
	GLuint loc;
	void *offset;
};

struct gl_vertex_attribs_definition {
	GLsizeiptr stride;
	unsigned count;
	struct gl_vertex_attrib attribs[];
};

static const struct gl_vertex_attribs_definition gl_blit_vertex_attribs = {
    .stride = sizeof(GLint) * 4,
    .count = 2,
    .attribs = {{GL_INT, vert_coord_loc, NULL},
                {GL_INT, vert_in_texcoord_loc, ((GLint *)NULL) + 2}},
};

// For when texture coordinates are the same as vertex coordinates
static const struct gl_vertex_attribs_definition gl_simple_vertex_attribs = {
    .stride = sizeof(GLint) * 2,
    .count = 2,
    .attribs = {{GL_INT, vert_coord_loc, NULL}, {GL_INT, vert_in_texcoord_loc, NULL}},
};

/**
 * Render a region with texture data.
 *
 * @param target_fbo   the FBO to render into
 * @param nrects       number of rectangles to render
 * @param coord        GL vertices
 * @param indices      GL indices
 * @param vert_attribs vertex attributes layout in `coord`
 * @param shader       shader to use
 * @param nuniforms    number of uniforms for `shader`
 * @param uniforms     uniforms for `shader`
 */
static void gl_blit_inner(GLuint target_fbo, int nrects, GLint *coord, GLuint *indices,
                          const struct gl_vertex_attribs_definition *vert_attribs,
                          const struct gl_shader *shader, int nuniforms,
                          struct gl_uniform_value *uniforms) {
	// FIXME(yshui) breaks when `mask` and `img` doesn't have the same y_inverted
	//              value. but we don't ever hit this problem because all of our
	//              images and masks are y_inverted.
	log_trace("Blitting %d rectangles", nrects);
	assert(shader);
	assert(shader->prog);
	glUseProgram(shader->prog);
	// TEXTURE0 reserved for the default texture
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	GLuint texture_unit = GL_TEXTURE1;
	for (int i = 0; i < nuniforms; i++) {
		if (!(shader->uniform_bitmask & (1 << i))) {
			continue;
		}

		auto uniform = &uniforms[i];
		switch (uniform->type) {
		case 0: break;
		case GL_TEXTURE_2D:
			if (uniform->texture == 0) {
				glUniform1i(i, 0);
			} else {
				glActiveTexture(texture_unit);
				glBindTexture(GL_TEXTURE_2D, uniform->texture);
				glUniform1i(i, (GLint)(texture_unit - GL_TEXTURE0));
				texture_unit += 1;
			}
			break;
		case GL_INT: glUniform1i(i, uniform->i); break;
		case GL_FLOAT: glUniform1f(i, uniform->f); break;
		case GL_INT_VEC2: glUniform2iv(i, 1, uniform->i2); break;
		case GL_FLOAT_VEC2: glUniform2fv(i, 1, uniform->f2); break;
		case GL_FLOAT_VEC4: glUniform4fv(i, 1, uniform->f4); break;
		default: assert(false);
		}
	}
	gl_check_err();

	// log_trace("Draw: %d, %d, %d, %d -> %d, %d (%d, %d) z %d\n",
	//          x, y, width, height, dx, dy, ptex->width, ptex->height, z);

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glBufferData(GL_ARRAY_BUFFER, vert_attribs->stride * nrects * 4, coord, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * nrects * 6,
	             indices, GL_STREAM_DRAW);
	for (ptrdiff_t i = 0; i < vert_attribs->count; i++) {
		auto attrib = &vert_attribs->attribs[i];
		glEnableVertexAttribArray(attrib->loc);
		glVertexAttribPointer(attrib->loc, 2, attrib->type, GL_FALSE,
		                      (GLsizei)vert_attribs->stride, attrib->offset);
	}

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);
	glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);

	glDisableVertexAttribArray(vert_coord_loc);
	glDisableVertexAttribArray(vert_in_texcoord_loc);
	glBindVertexArray(0);
	glDeleteVertexArrays(1, &vao);

	// Cleanup
	for (GLuint i = GL_TEXTURE1; i < texture_unit; i++) {
		glActiveTexture(i);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	glActiveTexture(GL_TEXTURE0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDeleteBuffers(2, bo);

	glUseProgram(0);

	gl_check_err();
}

void gl_mask_rects_to_coords(struct coord origin, struct coord mask_origin, int nrects,
                             const rect_t *rects, GLint *coord, GLuint *indices) {
	for (ptrdiff_t i = 0; i < nrects; i++) {
		// Rectangle in source image coordinates
		rect_t rect_src = region_translate_rect(rects[i], mask_origin);
		// Rectangle in target image coordinates
		rect_t rect_dst = region_translate_rect(rect_src, origin);

		memcpy(&coord[i * 16],
		       ((GLint[][2]){
		           {rect_dst.x1, rect_dst.y1},        // Vertex, bottom-left
		           {rect_src.x1, rect_src.y1},        // Texture
		           {rect_dst.x2, rect_dst.y1},        // Vertex, bottom-right
		           {rect_src.x2, rect_src.y1},        // Texture
		           {rect_dst.x2, rect_dst.y2},        // Vertex, top-right
		           {rect_src.x2, rect_src.y2},        // Texture
		           {rect_dst.x1, rect_dst.y2},        // Vertex, top-left
		           {rect_src.x1, rect_src.y2},        // Texture
		       }),
		       sizeof(GLint[2]) * 8);

		GLuint u = (GLuint)(i * 4);
		memcpy(&indices[i * 6],
		       ((GLuint[]){u + 0, u + 1, u + 2, u + 2, u + 3, u + 0}),
		       sizeof(GLuint) * 6);
	}
}

/// Flip the target coordinates returned by `gl_mask_rects_to_coords` vertically relative
/// to the target. Texture coordinates are unchanged.
///
/// @param[in] nrects        number of rectangles
/// @param[in] coord         OpenGL vertex coordinates
/// @param[in] target_height height of the target image
static void gl_y_flip_target(int nrects, GLint *coord, GLint target_height) {
	for (ptrdiff_t i = 0; i < nrects; i++) {
		auto current_rect = &coord[i * 16];        // 16 numbers per rectangle
		for (ptrdiff_t j = 0; j < 4; j++) {
			// 4 numbers per vertex, target coordinates are the first two
			auto current_vertex = &current_rect[j * 4];
			current_vertex[1] = target_height - current_vertex[1];
		}
	}
}

/// Flip the texture coordinates returned by `gl_mask_rects_to_coords` vertically relative
/// to the texture. Target coordinates are unchanged.
///
/// @param[in] nrects         number of rectangles
/// @param[in] coord          OpenGL vertex coordinates
/// @param[in] texture_height height of the source image
static inline void gl_y_flip_texture(int nrects, GLint *coord, GLint texture_height) {
	for (ptrdiff_t i = 0; i < nrects; i++) {
		auto current_rect = &coord[i * 16];        // 16 numbers per rectangle
		for (ptrdiff_t j = 0; j < 4; j++) {
			// 4 numbers per vertex, texture coordinates are the last two
			auto current_vertex = &current_rect[j * 4 + 2];
			current_vertex[1] = texture_height - current_vertex[1];
		}
	}
}

/// Lower `struct backend_blit_args` into a list of GL coordinates, vertex indices, a
/// shader, and uniforms.
static int gl_lower_blit_args(struct gl_data *gd, struct coord origin,
                              struct backend_blit_args *args, GLint **coord, GLuint **indices,
                              struct gl_shader **shader, struct gl_uniform_value *uniforms) {
	auto img = (struct gl_texture *)args->source_image;
	int nrects;
	const rect_t *rects;
	rect_t source_extent;
	if (args->mask != NULL) {
		rects = pixman_region32_rectangles(&args->mask->region, &nrects);
	} else {
		nrects = 1;
		source_extent = (rect_t){0, 0, args->ewidth, args->eheight};
		rects = &source_extent;
	}
	if (!nrects) {
		// Nothing to paint
		return 0;
	}
	struct coord mask_origin = {};
	if (args->mask != NULL) {
		mask_origin = args->mask->origin;
	}
	*coord = ccalloc(nrects * 16, GLint);
	*indices = ccalloc(nrects * 6, GLuint);
	gl_mask_rects_to_coords(origin, mask_origin, nrects, rects, *coord, *indices);
	if (!img->y_inverted) {
		gl_y_flip_texture(nrects, *coord, img->height);
	}

	auto mask_image = args->mask ? (struct gl_texture *)args->mask->image : NULL;
	auto mask_texture = mask_image ? mask_image->texture : gd->default_mask_texture;
	GLuint brightness = 0;        // 0 means the default texture, which will be
	                              // incomplete, and sampling from it will return (0,
	                              // 0, 0, 1), which should be fine.
	if (args->max_brightness < 1.0) {
		brightness = gl_average_texture_color(gd, img);
	}
	auto border_width = args->border_width;
	if (border_width > args->corner_radius) {
		border_width = 0;
	}
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	float time_ms = (float)ts.tv_sec * 1000.0F + (float)ts.tv_nsec / 1.0e6F;
	// clang-format off
	struct gl_uniform_value from_uniforms[] = {
	    [UNIFORM_OPACITY_LOC] = {.type = GL_FLOAT, .f = (float)args->opacity},
	    [UNIFORM_INVERT_COLOR_LOC] = {.type = GL_INT, .i = args->color_inverted},
	    [UNIFORM_TEX_LOC] = {.type = GL_TEXTURE_2D, .texture = img->texture},
	    [UNIFORM_EFFECTIVE_SIZE_LOC] = {.type = GL_FLOAT_VEC2,
	                                    .f2 = {(float)args->ewidth, (float)args->eheight}},
	    [UNIFORM_DIM_LOC] = {.type = GL_FLOAT, .f = (float)args->dim},
	    [UNIFORM_BRIGHTNESS_LOC] = {.type = GL_TEXTURE_2D, .texture = brightness},
	    [UNIFORM_MAX_BRIGHTNESS_LOC] = {.type = GL_FLOAT, .f = (float)args->max_brightness},
	    [UNIFORM_CORNER_RADIUS_LOC] = {.type = GL_FLOAT, .f = (float)args->corner_radius},
	    [UNIFORM_BORDER_WIDTH_LOC] = {.type = GL_FLOAT, .f = (float)args->border_width},
	    [UNIFORM_TIME_LOC] = {.type = GL_FLOAT, .f = time_ms},
	    [UNIFORM_MASK_TEX_LOC] = {.type = GL_TEXTURE_2D, .texture = mask_texture},
	    [UNIFORM_MASK_OFFSET_LOC] = {.type = GL_FLOAT_VEC2, .f2 = {0.0F, 0.0F}},
	    [UNIFORM_MASK_CORNER_RADIUS_LOC] = {.type = GL_FLOAT, .f = 0.0F},
	    [UNIFORM_MASK_INVERTED_LOC] = {.type = GL_INT, .i = 0},
	};
	// clang-format on

	if (args->mask != NULL) {
		from_uniforms[UNIFORM_MASK_OFFSET_LOC].f2[0] = (float)args->mask->origin.x;
		from_uniforms[UNIFORM_MASK_OFFSET_LOC].f2[1] = (float)args->mask->origin.y;
		from_uniforms[UNIFORM_MASK_INVERTED_LOC].i = args->mask->inverted;
		from_uniforms[UNIFORM_MASK_CORNER_RADIUS_LOC].f =
		    (float)args->mask->corner_radius;
	}
	memcpy(uniforms, from_uniforms, sizeof(from_uniforms));
	*shader = args->shader ?: &gd->default_shader;
	return nrects;
}

static const struct gl_uniform_value default_blit_uniforms[] = {
    [UNIFORM_OPACITY_LOC] = {.type = GL_FLOAT, .f = 1.0F},
    [UNIFORM_INVERT_COLOR_LOC] = {.type = GL_INT, .i = 0},
    [UNIFORM_TEX_LOC] = {.type = GL_TEXTURE_2D, .texture = 0},
    [UNIFORM_EFFECTIVE_SIZE_LOC] = {.type = GL_FLOAT_VEC2, .f2 = {0.0F, 0.0F}},        // Must be set
    [UNIFORM_DIM_LOC] = {.type = GL_FLOAT, .f = 0.0F},
    [UNIFORM_BRIGHTNESS_LOC] = {.type = GL_TEXTURE_2D, .texture = 0},
    [UNIFORM_MAX_BRIGHTNESS_LOC] = {.type = GL_FLOAT, .f = 1.0F},
    [UNIFORM_CORNER_RADIUS_LOC] = {.type = GL_FLOAT, .f = 0.0F},
    [UNIFORM_BORDER_WIDTH_LOC] = {.type = GL_FLOAT, .f = 0.0F},
    [UNIFORM_TIME_LOC] = {.type = GL_FLOAT, .f = 0.0F},
    [UNIFORM_MASK_TEX_LOC] = {.type = GL_TEXTURE_2D, .texture = 0},        // Must be set
    [UNIFORM_MASK_OFFSET_LOC] = {.type = GL_FLOAT_VEC2, .f2 = {0.0F, 0.0F}},
    [UNIFORM_MASK_CORNER_RADIUS_LOC] = {.type = GL_FLOAT, .f = 0.0F},
    [UNIFORM_MASK_INVERTED_LOC] = {.type = GL_INT, .i = 0},
};

// TODO(yshui) make use of reg_visible
void gl_compose(backend_t *base, image_handle image, coord_t image_dst,
                image_handle mask_, coord_t mask_dst, const region_t *reg_tgt,
                const region_t *reg_visible attr_unused) {
	auto gd = (struct gl_data *)base;
	auto img = (struct backend_image *)image;
	auto mask = (struct backend_image *)mask_;

	struct coord mask_offset = {.x = mask_dst.x - image_dst.x,
	                            .y = mask_dst.y - image_dst.y};

	log_trace("Mask is %p, texture %d", mask ? mask->inner : NULL,
	          mask ? ((struct gl_texture *)mask->inner)->texture : 0);
	struct backend_mask mask_args = {
	    .image = mask ? (image_handle)mask->inner : NULL,
	    .origin = mask_offset,
	    .corner_radius = mask ? mask->corner_radius : 0,
	    .inverted = mask ? mask->color_inverted : false,
	};
	pixman_region32_init(&mask_args.region);
	pixman_region32_copy(&mask_args.region, (region_t *)reg_tgt);
	pixman_region32_translate(&mask_args.region, -mask_dst.x, -mask_dst.y);
	struct backend_blit_args blit_args = {
	    .source_image = (image_handle)img->inner,
	    .mask = &mask_args,
	    .shader = (void *)img->shader ?: &gd->default_shader,
	    .opacity = img->opacity,
	    .color_inverted = img->color_inverted,
	    .ewidth = img->ewidth,
	    .eheight = img->eheight,
	    .dim = img->dim,
	    .corner_radius = img->corner_radius,
	    .border_width = img->border_width,
	    .max_brightness = img->max_brightness,
	};
	struct gl_shader *shader;
	struct gl_uniform_value uniforms[NUMBER_OF_UNIFORMS] = {};
	GLint *coord = NULL;
	GLuint *indices = NULL;
	int nrects = gl_lower_blit_args(gd, image_dst, &blit_args, &coord, &indices,
	                                &shader, uniforms);
	if (nrects > 0) {
		gl_blit_inner(gd->back_fbo, nrects, coord, indices, &gl_blit_vertex_attribs,
		              shader, NUMBER_OF_UNIFORMS, uniforms);
	}

	pixman_region32_fini(&mask_args.region);
	free(indices);
	free(coord);
}

/**
 * Load a GLSL main program from shader strings.
 */
static bool gl_win_shader_from_stringv(const char **vshader_strv,
                                       const char **fshader_strv, struct gl_shader *ret) {
	// Build program
	ret->prog = gl_create_program_from_strv(vshader_strv, fshader_strv);
	if (!ret->prog) {
		log_error("Failed to create GLSL program.");
		gl_check_err();
		return false;
	}

	gl_check_err();

	return true;
}

void gl_root_change(backend_t *base, session_t *ps) {
	auto gd = (struct gl_data *)base;
	gl_resize(gd, ps->root_width, ps->root_height);
}

/**
 * Callback to run on root window size change.
 */
void gl_resize(struct gl_data *gd, int width, int height) {
	GLint viewport_dimensions[2];
	glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_dimensions);

	gd->height = height;
	gd->width = width;

	assert(viewport_dimensions[0] >= gd->width);
	assert(viewport_dimensions[1] >= gd->height);

	glBindTexture(GL_TEXTURE_2D, gd->back_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, gd->back_format, width, height, 0, GL_BGR,
	             GL_UNSIGNED_BYTE, NULL);

	gl_check_err();
}

/// Fill a given region in bound framebuffer.
/// @param[in] y_inverted whether the y coordinates in `clip` should be inverted
static void
gl_fill_inner(backend_t *base, struct color c, const region_t *clip, GLuint target) {
	static const struct gl_vertex_attribs_definition vertex_attribs = {
	    .stride = sizeof(GLint) * 4,
	    .count = 1,
	    .attribs = {{GL_INT, vert_coord_loc, NULL}},
	};

	int nrects;
	const rect_t *rect = pixman_region32_rectangles((region_t *)clip, &nrects);
	auto gd = (struct gl_data *)base;

	auto coord = ccalloc(nrects * 16, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);

	struct gl_uniform_value uniforms[] = {
	    [UNIFORM_COLOR_LOC] = {.type = GL_FLOAT_VEC4,
	                           .f4 = {(float)c.red, (float)c.green, (float)c.blue,
	                                  (float)c.alpha}},
	};
	gl_mask_rects_to_coords_simple(nrects, rect, coord, indices);
	gl_blit_inner(target, nrects, coord, indices, &vertex_attribs, &gd->fill_shader,
	              ARR_SIZE(uniforms), uniforms);
	free(indices);
	free(coord);

	gl_check_err();
}

void gl_fill(backend_t *base, struct color c, const region_t *clip) {
	auto gd = (struct gl_data *)base;
	return gl_fill_inner(base, c, clip, gd->back_fbo);
}

image_handle gl_make_mask(backend_t *base, geometry_t size, const region_t *reg) {
	auto tex = ccalloc(1, struct gl_texture);
	auto gd = (struct gl_data *)base;
	auto img = ccalloc(1, struct backend_image);
	default_init_backend_image(img, size.width, size.height);
	log_trace("Creating mask texture %dx%d", size.width, size.height);
	tex->width = size.width;
	tex->height = size.height;
	tex->texture = gl_new_texture(GL_TEXTURE_2D);
	tex->has_alpha = false;
	tex->y_inverted = true;
	img->inner = (struct backend_image_inner_base *)tex;
	img->inner->refcount = 1;

	glBindTexture(GL_TEXTURE_2D, tex->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, size.width, size.height, 0, GL_RED,
	             GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBlendFunc(GL_ONE, GL_ZERO);
	glBindFramebuffer(GL_FRAMEBUFFER, gd->temp_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       tex->texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	gl_fill_inner(base, (struct color){1, 1, 1, 1}, reg, gd->temp_fbo);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	return (image_handle)img;
}

static void gl_release_image_inner(backend_t *base, struct gl_texture *inner) {
	auto gd = (struct gl_data *)base;
	if (inner->user_data) {
		gd->release_user_data(base, inner);
	}
	assert(inner->user_data == NULL);

	glDeleteTextures(1, &inner->texture);
	glDeleteTextures(2, inner->auxiliary_texture);
	free(inner);
	gl_check_err();
}

xcb_pixmap_t gl_release_image(backend_t *base, image_handle image) {
	auto wd = (struct backend_image *)image;
	auto inner = (struct gl_texture *)wd->inner;
	inner->refcount--;
	assert(inner->refcount >= 0);

	xcb_pixmap_t pixmap = XCB_NONE;
	if (inner->refcount == 0) {
		pixmap = inner->pixmap;
		gl_release_image_inner(base, inner);
	}
	free(wd);
	return pixmap;
}

static inline void gl_init_uniform_bitmask(struct gl_shader *shader) {
	GLint number_of_uniforms = 0;
	glGetProgramiv(shader->prog, GL_ACTIVE_UNIFORMS, &number_of_uniforms);
	for (int i = 0; i < number_of_uniforms; i++) {
		char name[32];
		glGetActiveUniformName(shader->prog, (GLuint)i, sizeof(name), NULL, name);
		GLint loc = glGetUniformLocation(shader->prog, name);
		assert(loc >= 0 && loc <= UNIFORM_TEXSIZE_LOC);
		shader->uniform_bitmask |= 1 << loc;
	}
}

static bool gl_create_window_shader_inner(struct gl_shader *out_shader, const char *source) {
	const char *vert_shaders[2] = {vertex_shader, NULL};
	const char *frag_shaders[4] = {win_shader_glsl, masking_glsl, source, NULL};

	if (!gl_win_shader_from_stringv(vert_shaders, frag_shaders, out_shader)) {
		return false;
	}

	GLint viewport_dimensions[2];
	glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_dimensions);

	// Set projection matrix to gl viewport dimensions so we can use screen
	// coordinates for all vertices
	// Note: OpenGL matrices are column major
	GLfloat projection_matrix[4][4] = {{2.0F / (GLfloat)viewport_dimensions[0], 0, 0, 0},
	                                   {0, 2.0F / (GLfloat)viewport_dimensions[1], 0, 0},
	                                   {0, 0, 0, 0},
	                                   {-1, -1, 0, 1}};

	glUseProgram(out_shader->prog);
	glUniformMatrix4fv(UNIFORM_PROJECTION_LOC, 1, false, projection_matrix[0]);
	glUseProgram(0);

	gl_init_uniform_bitmask(out_shader);
	gl_check_err();

	return true;
}

void *gl_create_window_shader(backend_t *backend_data attr_unused, const char *source) {
	auto ret = ccalloc(1, struct gl_shader);
	if (!gl_create_window_shader_inner(ret, source)) {
		free(ret);
		return NULL;
	}
	return ret;
}

uint64_t gl_get_shader_attributes(backend_t *backend_data attr_unused, void *shader) {
	auto win_shader = (struct gl_shader *)shader;
	uint64_t ret = 0;
	if (glGetUniformLocation(win_shader->prog, "time") >= 0) {
		ret |= SHADER_ATTRIBUTE_ANIMATED;
	}
	return ret;
}

bool gl_init(struct gl_data *gd, session_t *ps) {
	if (!epoxy_has_gl_extension("GL_ARB_explicit_uniform_location")) {
		log_error("GL_ARB_explicit_uniform_location support is required but "
		          "missing.");
		return false;
	}
	glGenQueries(2, gd->frame_timing);
	gd->current_frame_timing = 0;

	// Initialize GL data structure
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);

	glEnable(GL_BLEND);
	// X pixmap is in premultiplied alpha, so we might just as well use it too.
	// Thanks to derhass for help.
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	// Initialize stencil buffer
	glDisable(GL_STENCIL_TEST);
	glStencilMask(0x1);
	glStencilFunc(GL_EQUAL, 0x1, 0x1);

	// Set gl viewport to the maximum supported size so we won't have to worry about
	// it later on when the screen is resized. The corresponding projection matrix can
	// be set now and won't have to be updated. Since fragments outside the target
	// buffer are skipped anyways, this should have no impact on performance.
	GLint viewport_dimensions[2];
	glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_dimensions);
	glViewport(0, 0, viewport_dimensions[0], viewport_dimensions[1]);

	// Clear screen
	glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	glGenFramebuffers(1, &gd->temp_fbo);
	glGenFramebuffers(1, &gd->back_fbo);
	glGenTextures(1, &gd->back_texture);
	if (!gd->back_fbo || !gd->back_texture) {
		log_error("Failed to generate a framebuffer object");
		return false;
	}

	glBindTexture(GL_TEXTURE_2D, gd->back_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	gd->default_mask_texture = gl_new_texture(GL_TEXTURE_2D);
	if (!gd->default_mask_texture) {
		log_error("Failed to generate a default mask texture");
		return false;
	}

	glBindTexture(GL_TEXTURE_2D, gd->default_mask_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE,
	             (GLbyte[]){'\xff'});
	glBindTexture(GL_TEXTURE_2D, 0);

	// Initialize shaders
	if (!gl_create_window_shader_inner(&gd->default_shader, win_shader_default)) {
		log_error("Failed to create window shaders");
		return false;
	}

	// Set projection matrix to gl viewport dimensions so we can use screen
	// coordinates for all vertices
	// Note: OpenGL matrices are column major
	GLfloat projection_matrix[4][4] = {{2.0F / (GLfloat)viewport_dimensions[0], 0, 0, 0},
	                                   {0, 2.0F / (GLfloat)viewport_dimensions[1], 0, 0},
	                                   {0, 0, 0, 0},
	                                   {-1, -1, 0, 1}};

	gd->fill_shader.prog = gl_create_program_from_str(fill_vert, fill_frag);
	gd->fill_shader.uniform_bitmask = (uint32_t)-1;        // make sure our uniforms
	                                                       // are not ignored.
	glUseProgram(gd->fill_shader.prog);
	glUniformMatrix4fv(UNIFORM_PROJECTION_LOC, 1, false, projection_matrix[0]);
	glUseProgram(0);

	gd->dithered_present = ps->o.dithered_present;
	gd->dummy_prog.prog = gl_create_program_from_strv(
	    (const char *[]){vertex_shader, NULL}, (const char *[]){dummy_frag, NULL});
	if (!gd->dummy_prog.prog) {
		log_error("Failed to create the dummy shader");
		return false;
	}
	gd->dummy_prog.uniform_bitmask = (uint32_t)-1;        // make sure our uniforms
	                                                      // are not ignored.
	if (gd->dithered_present) {
		gd->present_prog = gl_create_program_from_strv(
		    (const char *[]){vertex_shader, NULL},
		    (const char *[]){present_frag, dither_glsl, NULL});
	} else {
		gd->present_prog = gd->dummy_prog.prog;
	}
	if (!gd->present_prog) {
		log_error("Failed to create the present shader");
		return false;
	}

	glUseProgram(gd->dummy_prog.prog);
	glUniform1i(UNIFORM_TEX_LOC, 0);
	glUniformMatrix4fv(UNIFORM_PROJECTION_LOC, 1, false, projection_matrix[0]);
	if (gd->present_prog != gd->dummy_prog.prog) {
		glUseProgram(gd->present_prog);
		glUniform1i(UNIFORM_TEX_LOC, 0);
		glUniformMatrix4fv(UNIFORM_PROJECTION_LOC, 1, false, projection_matrix[0]);
	}

	gd->shadow_shader.prog =
	    gl_create_program_from_str(vertex_shader, shadow_colorization_frag);
	gd->shadow_shader.uniform_bitmask = (uint32_t)-1;        // make sure our uniforms
	                                                         // are not ignored.
	glUseProgram(gd->shadow_shader.prog);
	glUniform1i(UNIFORM_TEX_LOC, 0);
	glUniformMatrix4fv(UNIFORM_PROJECTION_LOC, 1, false, projection_matrix[0]);
	glBindFragDataLocation(gd->shadow_shader.prog, 0, "out_color");

	gd->brightness_shader.prog =
	    gl_create_program_from_str(interpolating_vert, interpolating_frag);
	if (!gd->brightness_shader.prog) {
		log_error("Failed to create the brightness shader");
		return false;
	}
	glUseProgram(gd->brightness_shader.prog);
	glUniform1i(UNIFORM_TEX_LOC, 0);
	glUniformMatrix4fv(UNIFORM_PROJECTION_LOC, 1, false, projection_matrix[0]);

	glUseProgram(0);

	// Set up the size and format of the back texture
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->back_fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	const GLint *format = gd->dithered_present ? (const GLint[]){GL_RGB16, GL_RGBA16}
	                                           : (const GLint[]){GL_RGB8, GL_RGBA8};
	for (int i = 0; i < 2; i++) {
		gd->back_format = format[i];
		gl_resize(gd, ps->root_width, ps->root_height);

		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, gd->back_texture, 0);
		if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
			log_info("Using back buffer format %#x", gd->back_format);
			break;
		}
	}
	if (!gl_check_fb_complete(GL_DRAW_FRAMEBUFFER)) {
		return false;
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	gd->logger = gl_string_marker_logger_new();
	if (gd->logger) {
		log_add_target_tls(gd->logger);
	}

	const char *vendor = (const char *)glGetString(GL_VENDOR);
	log_debug("GL_VENDOR = %s", vendor);
	if (strcmp(vendor, "NVIDIA Corporation") == 0) {
		log_info("GL vendor is NVIDIA, enable xrender sync fence.");
		gd->is_nvidia = true;
	} else {
		gd->is_nvidia = false;
	}
	gd->has_robustness = epoxy_has_gl_extension("GL_ARB_robustness");
	gd->has_egl_image_storage = epoxy_has_gl_extension("GL_EXT_EGL_image_storage");
	gl_check_err();

	return true;
}

void gl_deinit(struct gl_data *gd) {
	if (gd->logger) {
		log_remove_target_tls(gd->logger);
		gd->logger = NULL;
	}

	if (gd->default_shader.prog) {
		glDeleteProgram(gd->default_shader.prog);
		gd->default_shader.prog = 0;
	}
	glDeleteProgram(gd->dummy_prog.prog);
	if (gd->present_prog != gd->dummy_prog.prog) {
		glDeleteProgram(gd->present_prog);
	}
	gd->dummy_prog.prog = 0;
	gd->present_prog = 0;

	glDeleteProgram(gd->fill_shader.prog);
	glDeleteProgram(gd->brightness_shader.prog);
	glDeleteProgram(gd->shadow_shader.prog);
	gd->fill_shader.prog = 0;
	gd->brightness_shader.prog = 0;
	gd->shadow_shader.prog = 0;

	glDeleteTextures(1, &gd->default_mask_texture);
	glDeleteTextures(1, &gd->back_texture);

	glDeleteFramebuffers(1, &gd->temp_fbo);
	glDeleteFramebuffers(1, &gd->back_fbo);

	glDeleteQueries(2, gd->frame_timing);

	gl_check_err();
}

GLuint gl_new_texture(GLenum target) {
	GLuint texture;
	glGenTextures(1, &texture);
	if (!texture) {
		log_error("Failed to generate texture");
		return 0;
	}

	glBindTexture(target, texture);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(target, 0);

	return texture;
}

/// Actually duplicate a texture into a new one, if this texture is shared
static inline void gl_image_decouple(backend_t *base, struct backend_image *img) {
	if (img->inner->refcount == 1) {
		return;
	}
	log_trace("Decoupling image %p", img);
	auto gd = (struct gl_data *)base;
	auto inner = (struct gl_texture *)img->inner;
	auto new_tex = ccalloc(1, struct gl_texture);

	*new_tex = *inner;
	new_tex->pixmap = XCB_NONE;
	new_tex->texture = gl_new_texture(GL_TEXTURE_2D);
	new_tex->refcount = 1;
	new_tex->user_data = gd->decouple_texture_user_data(base, new_tex->user_data);

	// Prepare the new texture
	glBindTexture(GL_TEXTURE_2D, new_tex->texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, new_tex->width, new_tex->height, 0,
	             GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       new_tex->texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	gl_check_fb_complete(GL_DRAW_FRAMEBUFFER);

	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(gd->dummy_prog.prog);
	glBindTexture(GL_TEXTURE_2D, inner->texture);

	// clang-format off
	GLint coord[] = {
	    0, 0,                            // top left
	    new_tex->width, 0,               // top right
	    new_tex->width, new_tex->height, // bottom right
	    0, new_tex->height,              // bottom left
	};
	GLuint indices[] = {0, 1, 2, 2, 3, 0};
	// clang-format on

	struct gl_uniform_value uniforms[] = {
	    [UNIFORM_TEX_LOC] = {.type = GL_TEXTURE_2D, .texture = inner->texture},
	};
	gl_blit_inner(gd->temp_fbo, 1, coord, indices, &gl_simple_vertex_attribs,
	              &gd->dummy_prog, ARR_SIZE(uniforms), uniforms);

	img->inner = (struct backend_image_inner_base *)new_tex;
	inner->refcount--;
}

static void gl_image_apply_alpha(backend_t *base, struct backend_image *img,
                                 const region_t *reg_op, double alpha) {
	// Result color = 0 (GL_ZERO) + alpha (GL_CONSTANT_ALPHA) * original color
	auto inner = (struct gl_texture *)img->inner;
	auto gd = (struct gl_data *)base;
	glBlendFunc(GL_ZERO, GL_CONSTANT_ALPHA);
	glBlendColor(0, 0, 0, (GLclampf)alpha);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       inner->texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	gl_fill_inner(base, (struct color){0, 0, 0, 0}, reg_op, gd->temp_fbo);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

void gl_present(backend_t *base, const region_t *region) {
	auto gd = (struct gl_data *)base;

	int nrects;
	const rect_t *rect = pixman_region32_rectangles((region_t *)region, &nrects);
	auto coord = ccalloc(nrects * 16, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);
	gl_mask_rects_to_coords_simple(nrects, rect, coord, indices);
	// Our back_texture is in Xorg coordinate system, but the GL back buffer is in
	// GL clip space, which has the Y axis flipped.
	gl_y_flip_target(nrects, coord, gd->height);

	glUseProgram(gd->present_prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gd->back_texture);

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(GLint) * nrects * 16, coord, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(GLuint) * nrects * 6, indices,
	             GL_STREAM_DRAW);

	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_INT, GL_FALSE,
	                      sizeof(GLint) * 4, &((GLint *)NULL)[2]);
	glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	glDeleteBuffers(2, bo);
	glDeleteVertexArrays(1, &vao);

	glEndQuery(GL_TIME_ELAPSED);
	gd->current_frame_timing ^= 1;

	gl_check_err();

	free(coord);
	free(indices);
}

bool gl_last_render_time(backend_t *base, struct timespec *ts) {
	auto gd = (struct gl_data *)base;
	GLint available = 0;
	glGetQueryObjectiv(gd->frame_timing[gd->current_frame_timing ^ 1],
	                   GL_QUERY_RESULT_AVAILABLE, &available);
	if (!available) {
		return false;
	}

	GLuint64 time;
	glGetQueryObjectui64v(gd->frame_timing[gd->current_frame_timing ^ 1],
	                      GL_QUERY_RESULT, &time);
	ts->tv_sec = (long)(time / 1000000000);
	ts->tv_nsec = (long)(time % 1000000000);
	gl_check_err();
	return true;
}

bool gl_image_op(backend_t *base, enum image_operations op, image_handle image,
                 const region_t *reg_op, const region_t *reg_visible attr_unused, void *arg) {
	auto tex = (struct backend_image *)image;
	switch (op) {
	case IMAGE_OP_APPLY_ALPHA:
		gl_image_decouple(base, tex);
		assert(tex->inner->refcount == 1);
		gl_image_apply_alpha(base, tex, reg_op, *(double *)arg);
		break;
	}

	return true;
}

struct gl_shadow_context {
	double radius;
	void *blur_context;
};

struct backend_shadow_context *gl_create_shadow_context(backend_t *base, double radius) {
	auto ctx = ccalloc(1, struct gl_shadow_context);
	ctx->radius = radius;
	ctx->blur_context = NULL;

	if (radius > 0) {
		struct gaussian_blur_args args = {
		    .size = (int)radius,
		    .deviation = gaussian_kernel_std_for_size(radius, 0.5 / 256.0),
		};
		ctx->blur_context = gl_create_blur_context(
		    base, BLUR_METHOD_GAUSSIAN, BACKEND_IMAGE_FORMAT_MASK, &args);
		if (!ctx->blur_context) {
			log_error("Failed to create shadow context");
			free(ctx);
			return NULL;
		}
	}
	return (struct backend_shadow_context *)ctx;
}

void gl_destroy_shadow_context(backend_t *base attr_unused, struct backend_shadow_context *ctx) {
	auto ctx_ = (struct gl_shadow_context *)ctx;
	if (ctx_->blur_context) {
		gl_destroy_blur_context(base, (struct backend_blur_context *)ctx_->blur_context);
	}
	free(ctx_);
}

image_handle gl_shadow_from_mask(backend_t *base, image_handle mask_,
                                 struct backend_shadow_context *sctx, struct color color) {
	log_debug("Create shadow from mask");
	auto gd = (struct gl_data *)base;
	auto mask = (struct backend_image *)mask_;
	auto inner = (struct gl_texture *)mask->inner;
	auto gsctx = (struct gl_shadow_context *)sctx;
	int radius = (int)gsctx->radius;
	if (mask->eheight != inner->height || mask->ewidth != inner->width ||
	    mask->dim != 0 || mask->max_brightness != 1 || mask->border_width != 0 ||
	    mask->opacity != 1 || mask->shader != NULL) {
		log_error("Unsupported mask properties for shadow generation");
		return NULL;
	}

	auto new_inner = ccalloc(1, struct gl_texture);
	new_inner->width = inner->width + radius * 2;
	new_inner->height = inner->height + radius * 2;
	new_inner->texture = gl_new_texture(GL_TEXTURE_2D);
	new_inner->has_alpha = inner->has_alpha;
	new_inner->y_inverted = true;
	auto new_img = ccalloc(1, struct backend_image);
	default_init_backend_image(new_img, new_inner->width, new_inner->height);
	new_img->inner = (struct backend_image_inner_base *)new_inner;
	new_img->inner->refcount = 1;

	// We apply the mask properties by blitting a pure white texture with the mask
	// image as mask.
	auto white_texture = gl_new_texture(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, white_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE,
	             (GLbyte[]){'\xff'});
	glBindTexture(GL_TEXTURE_2D, 0);

	auto source_texture = gl_new_texture(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, source_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, new_inner->width, new_inner->height, 0,
	             GL_RED, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       source_texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	if (mask->color_inverted) {
		// If the mask is inverted, clear the source_texture to white, so the
		// "outside" of the mask would be correct.
		// FIXME(yshui): we should make masks clamp to border, and set border
		// color to 0.
		glClearColor(1, 1, 1, 1);
	} else {
		glClearColor(0, 0, 0, 1);
	}
	glClear(GL_COLOR_BUFFER_BIT);
	{
		// clang-format off
		//               |             vertex coordinates              |     texture coordinates
		GLint coords[] = {radius               , radius                , 0           ,             0,
				  radius + inner->width, radius                , inner->width,             0,
				  radius + inner->width, radius + inner->height, inner->width, inner->height,
				  radius               , radius + inner->height, 0           , inner->height,};
		// clang-format on
		GLuint indices[] = {0, 1, 2, 2, 3, 0};
		auto shader = &gd->default_shader;
		struct gl_uniform_value uniforms[ARR_SIZE(default_blit_uniforms)];
		memcpy(uniforms, default_blit_uniforms, sizeof(default_blit_uniforms));
		uniforms[UNIFORM_EFFECTIVE_SIZE_LOC].f2[0] = (float)inner->width;
		uniforms[UNIFORM_EFFECTIVE_SIZE_LOC].f2[1] = (float)inner->height;
		uniforms[UNIFORM_TEX_LOC].i = (GLint)white_texture;
		uniforms[UNIFORM_MASK_TEX_LOC].i = (GLint)inner->texture;
		uniforms[UNIFORM_MASK_INVERTED_LOC].i = mask->color_inverted;
		uniforms[UNIFORM_MASK_CORNER_RADIUS_LOC].f = (float)mask->corner_radius;
		gl_blit_inner(gd->temp_fbo, 1, coords, indices, &gl_blit_vertex_attribs,
		              shader, ARR_SIZE(default_blit_uniforms), uniforms);
		glDeleteTextures(1, &white_texture);
	}

	gl_check_err();

	auto tmp_texture = source_texture;
	if (gsctx->blur_context != NULL) {
		glActiveTexture(GL_TEXTURE0);
		tmp_texture = gl_new_texture(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, tmp_texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, new_inner->width,
		             new_inner->height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, tmp_texture, 0);

		region_t reg_blur;
		pixman_region32_init_rect(&reg_blur, 0, 0, (unsigned int)new_inner->width,
		                          (unsigned int)new_inner->height);
		// gl_blur expects reg_blur to be in X coordinate system (i.e. y flipped),
		// but we are covering the whole texture so we don't need to worry about
		// that.
		gl_blur_impl(
		    1.0, gsctx->blur_context, NULL, (coord_t){0}, &reg_blur, source_texture,
		    (geometry_t){.width = new_inner->width, .height = new_inner->height},
		    true, gd->temp_fbo, gd->default_mask_texture);
		pixman_region32_fini(&reg_blur);
	}

	// Colorize the shadow with color.
	log_debug("Colorize shadow");
	// First prepare the new texture
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, new_inner->texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, new_inner->width, new_inner->height, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->temp_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       new_inner->texture, 0);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);

	struct gl_uniform_value uniforms[] = {
	    [UNIFORM_TEX_LOC] = {.type = GL_TEXTURE_2D, .i = (GLint)tmp_texture},
	    // The shadow color is converted to the premultiplied format to respect the
	    // globally set glBlendFunc and thus get the correct and expected result.
	    [UNIFORM_COLOR_LOC] = {.type = GL_FLOAT_VEC4,
	                           .f4 = {(float)(color.red * color.alpha),
	                                  (float)(color.green * color.alpha),
	                                  (float)(color.blue * color.alpha), (float)color.alpha}},
	};

	// clang-format off
	GLuint indices[] = {0, 1, 2, 2, 3, 0};
	GLint coord[] = {0                , 0                ,
	                 new_inner->width , 0                ,
	                 new_inner->width , new_inner->height,
	                 0                , new_inner->height,};
	// clang-format on
	gl_blit_inner(gd->temp_fbo, 1, coord, indices, &gl_simple_vertex_attribs,
	              &gd->shadow_shader, ARR_SIZE(uniforms), uniforms);

	glDeleteTextures(1, (GLuint[]){source_texture});
	if (tmp_texture != source_texture) {
		glDeleteTextures(1, (GLuint[]){tmp_texture});
	}
	gl_check_err();
	return (image_handle)new_img;
}

enum device_status gl_device_status(backend_t *base) {
	auto gd = (struct gl_data *)base;
	if (!gd->has_robustness) {
		return DEVICE_STATUS_NORMAL;
	}
	if (glGetGraphicsResetStatusARB() == GL_NO_ERROR) {
		return DEVICE_STATUS_NORMAL;
	}
	return DEVICE_STATUS_RESETTING;
}
