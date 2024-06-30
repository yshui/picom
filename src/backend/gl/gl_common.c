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

#include <picom/types.h>

#include "backend/backend_common.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "utils/misc.h"

#include "gl_common.h"

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

static void gl_destroy_window_shader_inner(struct gl_shader *shader) {
	if (!shader) {
		return;
	}

	if (shader->prog) {
		glDeleteProgram(shader->prog);
		shader->prog = 0;
	}
	gl_check_err();
}

void gl_destroy_window_shader(backend_t *backend_data attr_unused, void *shader) {
	gl_destroy_window_shader_inner(shader);
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
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       destination_texture, 0);
	gl_check_fb_complete(GL_FRAMEBUFFER);

	// Bind source texture as downscaling shader uniform input
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, source_texture);
	glBindSampler(0, 0);

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
	glBindVertexArray(gd->vertex_array_objects[0]);
	glBindBuffer(GL_ARRAY_BUFFER, gd->buffer_objects[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gd->buffer_objects[1]);
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

	// Invalidate buffer data
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * 16, NULL, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * 6, NULL, GL_STREAM_DRAW);

	// Cleanup buffers
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

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
struct gl_texture_unit {
	GLuint texture;
	GLuint sampler;
};

struct gl_uniform_value {
	GLenum type;
	union {
		struct gl_texture_unit tu;
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
    .stride = sizeof(GLfloat) * 4,
    .count = 2,
    .attribs = {{GL_FLOAT, vert_coord_loc, NULL},
                {GL_FLOAT, vert_in_texcoord_loc, ((GLfloat *)NULL) + 2}},
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
static void
gl_blit_inner(struct gl_data *gd, GLuint target_fbo, int nrects, GLfloat *coord,
              GLuint *indices, const struct gl_vertex_attribs_definition *vert_attribs,
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
			if (uniform->tu.texture == 0) {
				glUniform1i(i, 0);
			} else {
				glActiveTexture(texture_unit);
				glBindTexture(GL_TEXTURE_2D, uniform->tu.texture);
				glBindSampler(texture_unit - GL_TEXTURE0, uniform->tu.sampler);
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

	glBindVertexArray(gd->vertex_array_objects[0]);

	glBindBuffer(GL_ARRAY_BUFFER, gd->buffer_objects[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gd->buffer_objects[1]);
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

	// Invalidate buffer data
	glBufferData(GL_ARRAY_BUFFER, vert_attribs->stride * nrects * 4, NULL, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * nrects * 6, NULL,
	             GL_STREAM_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	// Cleanup
	for (GLuint i = GL_TEXTURE1; i < texture_unit; i++) {
		glActiveTexture(i);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	glActiveTexture(GL_TEXTURE0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);

	glUseProgram(0);

	gl_check_err();
}

void gl_mask_rects_to_coords(ivec2 origin, int nrects, const rect_t *rects, vec2 scale,
                             GLfloat *coord, GLuint *indices) {
	for (ptrdiff_t i = 0; i < nrects; i++) {
		// Rectangle in source image coordinates
		rect_t rect_src = region_translate_rect(rects[i], ivec2_neg(origin));
		// Rectangle in target image coordinates
		rect_t rect_dst = rects[i];

		// clang-format off
		memcpy(&coord[i * 16],
		       ((GLfloat[][2]){
		           // Interleaved vertex and texture coordinates, starting with vertex.
		           {(GLfloat)rect_dst.x1, (GLfloat)rect_dst.y1},        // bottom-left
		           {(GLfloat)(rect_src.x1 / scale.x), (GLfloat)(rect_src.y1 / scale.y)},
		           {(GLfloat)rect_dst.x2, (GLfloat)rect_dst.y1},        // bottom-right
		           {(GLfloat)(rect_src.x2 / scale.x), (GLfloat)(rect_src.y1 / scale.y)},
		           {(GLfloat)rect_dst.x2, (GLfloat)rect_dst.y2},        // top-right
		           {(GLfloat)(rect_src.x2 / scale.x), (GLfloat)(rect_src.y2 / scale.y)},
		           {(GLfloat)rect_dst.x1, (GLfloat)rect_dst.y2},        // top-left
		           {(GLfloat)(rect_src.x1 / scale.x), (GLfloat)(rect_src.y2 / scale.y)},
		       }),
		       sizeof(GLint[2]) * 8);
		// clang-format on

		GLuint u = (GLuint)(i * 4);
		memcpy(&indices[i * 6],
		       ((GLuint[]){u + 0, u + 1, u + 2, u + 2, u + 3, u + 0}),
		       sizeof(GLuint) * 6);
	}
}

/// Flip the texture coordinates returned by `gl_mask_rects_to_coords` vertically relative
/// to the texture. Target coordinates are unchanged.
///
/// @param[in] nrects         number of rectangles
/// @param[in] coord          OpenGL vertex coordinates
/// @param[in] texture_height height of the source image
static inline void gl_y_flip_texture(int nrects, GLfloat *coord, GLint texture_height) {
	for (ptrdiff_t i = 0; i < nrects; i++) {
		auto current_rect = &coord[i * 16];        // 16 numbers per rectangle
		for (ptrdiff_t j = 0; j < 4; j++) {
			// 4 numbers per vertex, texture coordinates are the last two
			auto current_vertex = &current_rect[j * 4 + 2];
			current_vertex[1] = (GLfloat)texture_height - current_vertex[1];
		}
	}
}

/// Lower `struct backend_blit_args` into a list of GL coordinates, vertex indices, a
/// shader, and uniforms.
static int
gl_lower_blit_args(struct gl_data *gd, ivec2 origin, const struct backend_blit_args *args,
                   GLfloat **coord, GLuint **indices, struct gl_shader **shader,
                   struct gl_uniform_value *uniforms) {
	auto img = (struct gl_texture *)args->source_image;
	int nrects;
	const rect_t *rects;
	rects = pixman_region32_rectangles(args->target_mask, &nrects);
	if (!nrects) {
		// Nothing to paint
		return 0;
	}
	*coord = ccalloc(nrects * 16, GLfloat);
	*indices = ccalloc(nrects * 6, GLuint);
	gl_mask_rects_to_coords(origin, nrects, rects, args->scale, *coord, *indices);
	if (!img->y_inverted) {
		gl_y_flip_texture(nrects, *coord, img->height);
	}

	auto mask_texture = gd->default_mask_texture;
	auto mask_sampler = gd->samplers[GL_SAMPLER_REPEAT];
	if (args->source_mask != NULL) {
		mask_texture = ((struct gl_texture *)args->source_mask->image)->texture;
		mask_sampler = gd->samplers[GL_SAMPLER_BORDER];
	}
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
	// clang-format off
	auto tex_sampler = vec2_eq(args->scale, SCALE_IDENTITY) ?
	    gd->samplers[GL_SAMPLER_REPEAT] : gd->samplers[GL_SAMPLER_REPEAT_SCALE];
	struct gl_uniform_value from_uniforms[] = {
	    [UNIFORM_OPACITY_LOC]        = {.type = GL_FLOAT, .f = (float)args->opacity},
	    [UNIFORM_INVERT_COLOR_LOC]   = {.type = GL_INT, .i = args->color_inverted},
	    [UNIFORM_TEX_LOC]            = {.type = GL_TEXTURE_2D,
	                                    .tu = {img->texture, tex_sampler}},
	    [UNIFORM_EFFECTIVE_SIZE_LOC] = {.type = GL_FLOAT_VEC2,
	                                    .f2 = {(float)args->effective_size.width,
					           (float)args->effective_size.height}},
	    [UNIFORM_DIM_LOC]            = {.type = GL_FLOAT, .f = (float)args->dim},
	    [UNIFORM_BRIGHTNESS_LOC]     = {.type = GL_TEXTURE_2D,
	                                    .tu = {brightness, gd->samplers[GL_SAMPLER_EDGE]}},
	    [UNIFORM_MAX_BRIGHTNESS_LOC] = {.type = GL_FLOAT, .f = (float)args->max_brightness},
	    [UNIFORM_CORNER_RADIUS_LOC]  = {.type = GL_FLOAT, .f = (float)args->corner_radius},
	    [UNIFORM_BORDER_WIDTH_LOC]   = {.type = GL_FLOAT, .f = (float)border_width},
	    [UNIFORM_MASK_TEX_LOC]       = {.type = GL_TEXTURE_2D,
	                                    .tu = {mask_texture, mask_sampler}},
	    [UNIFORM_MASK_OFFSET_LOC]    = {.type = GL_FLOAT_VEC2, .f2 = {0.0F, 0.0F}},
	    [UNIFORM_MASK_INVERTED_LOC]  = {.type = GL_INT, .i = 0},
	    [UNIFORM_MASK_CORNER_RADIUS_LOC] = {.type = GL_FLOAT, .f = 0.0F},
	    [UNIFORM_FRAME_OPACITY_LOC]  = {.type = GL_FLOAT, .f = (float)args->frame_opacity},
	    [UNIFORM_FRAME_OPACITY_FSC_LOC]  = {.type = GL_INT, .i = args->frame_opacity_for_same_colors},
	    [UNIFORM_FRAME_OPACITY_FSCT_LOC]  = {.type = GL_FLOAT, .f = (float)args->frame_opacity_for_same_colors_tolerance},
	    [UNIFORM_FRAME_OPACITY_FSCM_LOC]  = {.type = GL_INT, .i = (int)args->frame_opacity_for_same_colors_multiplier},
	};
	// clang-format on

	if (args->source_mask != NULL) {
		from_uniforms[UNIFORM_MASK_OFFSET_LOC].f2[0] =
		    (float)args->source_mask->origin.x;
		from_uniforms[UNIFORM_MASK_OFFSET_LOC].f2[1] =
		    (float)args->source_mask->origin.y;
		from_uniforms[UNIFORM_MASK_CORNER_RADIUS_LOC].f =
		    (float)args->source_mask->corner_radius;
		from_uniforms[UNIFORM_MASK_INVERTED_LOC].i = args->source_mask->inverted;
	}
	*shader = args->shader ?: &gd->default_shader;
	if ((*shader)->uniform_bitmask & (1 << UNIFORM_TIME_LOC)) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		from_uniforms[UNIFORM_TIME_LOC] = (struct gl_uniform_value){
		    .type = GL_FLOAT,
		    .f = (float)ts.tv_sec * 1000.0F + (float)ts.tv_nsec / 1.0e6F,
		};
	}
	memcpy(uniforms, from_uniforms, sizeof(from_uniforms));
	return nrects;
}

bool gl_blit(backend_t *base, ivec2 origin, image_handle target_,
             const struct backend_blit_args *args) {
	auto gd = (struct gl_data *)base;
	auto source = (struct gl_texture *)args->source_image;
	auto target = (struct gl_texture *)target_;

	if (source == &gd->back_image) {
		log_error("Trying to blit from the back texture, this is not allowed");
		return false;
	}

	GLfloat *coord;
	GLuint *indices;
	struct gl_shader *shader;
	struct gl_uniform_value uniforms[NUMBER_OF_UNIFORMS] = {};
	int nrects =
	    gl_lower_blit_args(gd, origin, args, &coord, &indices, &shader, uniforms);
	if (nrects == 0) {
		return true;
	}
	if (!target->y_inverted) {
		log_trace("Flipping target texture");
		gl_y_flip_target(nrects, coord, target->height);
	}

	auto fbo = gl_bind_image_to_fbo(gd, target_);
	// X pixmap is in premultiplied alpha, so we might just as well use it too.
	// Thanks to derhass for help.
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	gl_blit_inner(gd, fbo, nrects, coord, indices, &gl_blit_vertex_attribs, shader,
	              NUMBER_OF_UNIFORMS, uniforms);

	free(indices);
	free(coord);
	return true;
}

/// Copy areas by glBlitFramebuffer. This is only used to copy data from the back
/// buffer.
static bool gl_copy_area_blit_fbo(struct gl_data *gd, ivec2 origin, image_handle target,
                                  const region_t *region) {
	gl_bind_image_to_fbo(gd, target);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	int nrects;
	const rect_t *rects = pixman_region32_rectangles(region, &nrects);
	for (ptrdiff_t i = 0; i < nrects; i++) {
		// Remember GL back buffer is Y-up, but the destination image is only
		// allowed to be Y-down
		// clang-format off
		glBlitFramebuffer(
		    rects[i].x1           , gd->back_image.height - rects[i].y1,
		    rects[i].x2           , gd->back_image.height - rects[i].y2,
		    rects[i].x1 + origin.x, rects[i].y1 + origin.y  ,
		    rects[i].x2 + origin.x, rects[i].y2 + origin.y  ,
		    GL_COLOR_BUFFER_BIT, GL_NEAREST);
		// clang-format on
	}
	gl_check_err();
	return true;
}

/// Copy areas by drawing. This is the common path to copy from one texture to another.
static bool gl_copy_area_draw(struct gl_data *gd, ivec2 origin,
                              image_handle target_handle, image_handle source_handle,
                              struct gl_shader *shader, const region_t *region) {
	auto source = (struct gl_texture *)source_handle;
	auto target = (struct gl_texture *)target_handle;
	assert(source->y_inverted);

	int nrects;
	const rect_t *rects = pixman_region32_rectangles(region, &nrects);
	if (nrects == 0) {
		return true;
	}

	auto coord = ccalloc(16 * nrects, GLfloat);
	auto indices = ccalloc(6 * nrects, GLuint);
	gl_mask_rects_to_coords(origin, nrects, rects, SCALE_IDENTITY, coord, indices);
	if (!target->y_inverted) {
		gl_y_flip_target(nrects, coord, target->height);
	}

	struct gl_uniform_value uniforms[] = {
	    [UNIFORM_TEX_LOC] = {.type = GL_TEXTURE_2D,
	                         .tu = {source->texture, gd->samplers[GL_SAMPLER_EDGE]}},
	};
	auto fbo = gl_bind_image_to_fbo(gd, target_handle);
	glBlendFunc(GL_ONE, GL_ZERO);
	gl_blit_inner(gd, fbo, nrects, coord, indices, &gl_blit_vertex_attribs, shader,
	              ARR_SIZE(uniforms), uniforms);
	free(indices);
	free(coord);
	return true;
}

bool gl_copy_area(backend_t *backend_data, ivec2 origin, image_handle target,
                  image_handle source, const region_t *region) {
	auto gd = (struct gl_data *)backend_data;
	if ((struct gl_texture *)source == &gd->back_image) {
		return gl_copy_area_blit_fbo(gd, origin, target, region);
	}
	return gl_copy_area_draw(gd, origin, target, source, &gd->copy_area_prog, region);
}

bool gl_copy_area_quantize(backend_t *backend_data, ivec2 origin, image_handle target_handle,
                           image_handle source_handle, const region_t *region) {
	auto gd = (struct gl_data *)backend_data;
	auto target = (struct gl_texture *)target_handle;
	auto source = (struct gl_texture *)source_handle;
	if (source->format != BACKEND_IMAGE_FORMAT_PIXMAP_HIGH ||
	    source->format == target->format) {
		return gl_copy_area(backend_data, origin, target_handle, source_handle, region);
	}
	return gl_copy_area_draw(gd, origin, target_handle, source_handle,
	                         &gd->copy_area_with_dither_prog, region);
}

uint32_t gl_image_capabilities(backend_t *base, image_handle img) {
	auto gd = (struct gl_data *)base;
	auto inner = (struct gl_texture *)img;
	if (&gd->back_image == inner) {
		return BACKEND_IMAGE_CAP_DST;
	}
	if (inner->user_data) {
		// user_data indicates that the image is a bound X pixmap
		return BACKEND_IMAGE_CAP_SRC;
	}
	return BACKEND_IMAGE_CAP_SRC | BACKEND_IMAGE_CAP_DST;
}

bool gl_is_format_supported(backend_t *base attr_unused,
                            enum backend_image_format format attr_unused) {
	return true;
}

/**
 * Load a GLSL main program from shader strings.
 */
static bool gl_shader_from_stringv(const char **vshader_strv, const char **fshader_strv,
                                   struct gl_shader *ret) {
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

	gd->back_image.height = height;
	gd->back_image.width = width;

	assert(viewport_dimensions[0] >= gd->back_image.width);
	assert(viewport_dimensions[1] >= gd->back_image.height);

	gl_check_err();
}

xcb_pixmap_t gl_release_image(backend_t *base, image_handle image) {
	auto inner = (struct gl_texture *)image;
	auto gd = (struct gl_data *)base;
	if (inner == &gd->back_image) {
		return XCB_NONE;
	}

	xcb_pixmap_t pixmap = inner->user_data ? inner->pixmap : XCB_NONE;
	if (inner->user_data) {
		gd->release_user_data(base, inner);
	}
	assert(inner->user_data == NULL);

	glDeleteTextures(1, &inner->texture);
	glDeleteTextures(2, inner->auxiliary_texture);
	free(inner);
	gl_check_err();
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
		shader->uniform_bitmask |= 1U << loc;
	}
}

static bool gl_create_window_shader_inner(struct gl_shader *out_shader, const char *source) {
	const char *vert[2] = {vertex_shader, NULL};
	const char *frag[] = {blit_shader_glsl, masking_glsl, source, NULL};

	if (!gl_shader_from_stringv(vert, frag, out_shader)) {
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

static const struct {
	GLint filter;
	GLint wrap;
} gl_sampler_params[] = {
    [GL_SAMPLER_REPEAT] = {GL_NEAREST, GL_REPEAT},
    [GL_SAMPLER_REPEAT_SCALE] = {GL_LINEAR, GL_REPEAT},
    [GL_SAMPLER_BLUR] = {GL_LINEAR, GL_CLAMP_TO_EDGE},
    [GL_SAMPLER_EDGE] = {GL_NEAREST, GL_CLAMP_TO_EDGE},
    [GL_SAMPLER_BORDER] = {GL_NEAREST, GL_CLAMP_TO_BORDER},
};

bool gl_init(struct gl_data *gd, session_t *ps) {
	if (!epoxy_has_gl_extension("GL_ARB_explicit_uniform_location")) {
		log_error("GL_ARB_explicit_uniform_location support is required but "
		          "missing.");
		return false;
	}
	glGenQueries(2, gd->frame_timing);
	gd->current_frame_timing = 0;

	glGenBuffers(4, gd->buffer_objects);
	glGenVertexArrays(2, gd->vertex_array_objects);

	// Initialize GL data structure
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);

	glEnable(GL_BLEND);

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

	gd->default_mask_texture = gl_new_texture();
	if (!gd->default_mask_texture) {
		log_error("Failed to generate a default mask texture");
		return false;
	}

	glBindTexture(GL_TEXTURE_2D, gd->default_mask_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE,
	             (GLbyte[]){'\xff'});
	glBindTexture(GL_TEXTURE_2D, 0);

	// Initialize shaders
	if (!gl_create_window_shader_inner(&gd->default_shader, blit_shader_default)) {
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
	gd->copy_area_prog.prog = gl_create_program_from_strv(
	    (const char *[]){vertex_shader, NULL}, (const char *[]){copy_area_frag, NULL});
	if (!gd->copy_area_prog.prog) {
		log_error("Failed to create the copy_area shader");
		return false;
	}
	gd->copy_area_prog.uniform_bitmask = (uint32_t)-1;        // make sure our
	                                                          // uniforms are not
	                                                          // ignored.

	glUseProgram(gd->copy_area_prog.prog);
	glUniform1i(UNIFORM_TEX_LOC, 0);
	glUniformMatrix4fv(UNIFORM_PROJECTION_LOC, 1, false, projection_matrix[0]);

	gd->copy_area_with_dither_prog.prog = gl_create_program_from_strv(
	    (const char *[]){vertex_shader, NULL},
	    (const char *[]){copy_area_with_dither_frag, dither_glsl, NULL});
	if (!gd->copy_area_with_dither_prog.prog) {
		log_error("Failed to create the copy_area with dither shader");
		return false;
	}
	gd->copy_area_with_dither_prog.uniform_bitmask = (uint32_t)-1;

	glUseProgram(gd->copy_area_with_dither_prog.prog);
	glUniform1i(UNIFORM_TEX_LOC, 0);
	glUniformMatrix4fv(UNIFORM_PROJECTION_LOC, 1, false, projection_matrix[0]);

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

	gd->back_image.width = ps->root_width;
	gd->back_image.height = ps->root_height;

	glGenSamplers(GL_MAX_SAMPLERS, gd->samplers);
	for (size_t i = 0; i < ARR_SIZE(gl_sampler_params); i++) {
		glSamplerParameteri(gd->samplers[i], GL_TEXTURE_MIN_FILTER,
		                    gl_sampler_params[i].filter);
		glSamplerParameteri(gd->samplers[i], GL_TEXTURE_MAG_FILTER,
		                    gl_sampler_params[i].filter);
		glSamplerParameteri(gd->samplers[i], GL_TEXTURE_WRAP_S,
		                    gl_sampler_params[i].wrap);
		glSamplerParameteri(gd->samplers[i], GL_TEXTURE_WRAP_T,
		                    gl_sampler_params[i].wrap);
	}

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
	gd->back_image.y_inverted = false;

	gl_check_err();

	return true;
}

void gl_deinit(struct gl_data *gd) {
	if (gd->logger) {
		log_remove_target_tls(gd->logger);
		gd->logger = NULL;
	}

	gl_destroy_window_shader_inner(&gd->default_shader);
	glDeleteProgram(gd->copy_area_prog.prog);
	glDeleteProgram(gd->copy_area_with_dither_prog.prog);
	gd->copy_area_prog.prog = 0;
	gd->copy_area_with_dither_prog.prog = 0;

	glDeleteProgram(gd->fill_shader.prog);
	glDeleteProgram(gd->brightness_shader.prog);
	gd->fill_shader.prog = 0;
	gd->brightness_shader.prog = 0;

	glDeleteTextures(1, &gd->default_mask_texture);

	for (int i = 0; i < GL_MAX_SAMPLERS; i++) {
		glDeleteSamplers(1, &gd->samplers[i]);
	}

	glDeleteFramebuffers(1, &gd->temp_fbo);

	glDeleteBuffers(4, gd->buffer_objects);
	glDeleteVertexArrays(2, gd->vertex_array_objects);

	glDeleteQueries(2, gd->frame_timing);

	gl_check_err();
}

GLuint gl_new_texture(void) {
	GLuint texture;
	glGenTextures(1, &texture);
	if (!texture) {
		log_error("Failed to generate texture");
		return 0;
	}
	return texture;
}

bool gl_clear(backend_t *backend_data, image_handle target, struct color color) {
	auto gd = (struct gl_data *)backend_data;
	auto fbo = gl_bind_image_to_fbo(gd, target);
	auto target_image = (struct gl_texture *)target;
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	if (target_image->format == BACKEND_IMAGE_FORMAT_MASK) {
		glClearColor((GLfloat)color.alpha, 0, 0, 1);
	} else {
		glClearColor((GLfloat)color.red, (GLfloat)color.green,
		             (GLfloat)color.blue, (GLfloat)color.alpha);
	}
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	return true;
}

image_handle gl_back_buffer(struct backend_base *base) {
	auto gd = (struct gl_data *)base;
	return (image_handle)&gd->back_image;
}

image_handle gl_new_image(backend_t *backend_data attr_unused,
                          enum backend_image_format format, ivec2 size) {
	auto tex = ccalloc(1, struct gl_texture);
	log_trace("Creating texture %dx%d", size.width, size.height);
	tex->format = format;
	tex->width = size.width;
	tex->height = size.height;
	tex->texture = gl_new_texture();
	tex->y_inverted = true;
	tex->user_data = NULL;
	tex->pixmap = XCB_NONE;

	GLint gl_format;
	switch (format) {
	case BACKEND_IMAGE_FORMAT_PIXMAP: gl_format = GL_RGBA8; break;
	case BACKEND_IMAGE_FORMAT_PIXMAP_HIGH: gl_format = GL_RGBA16; break;
	case BACKEND_IMAGE_FORMAT_MASK: gl_format = GL_R8; break;
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex->texture);
	glTexImage2D(GL_TEXTURE_2D, 0, gl_format, size.width, size.height, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, NULL);
	if (format == BACKEND_IMAGE_FORMAT_MASK) {
		// Mask images needs a border, so sampling from outside of the texture
		// will correctly return 0
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR,
		                 (GLfloat[]){0, 0, 0, 0});
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	return (image_handle)tex;
}

bool gl_apply_alpha(backend_t *base, image_handle target, double alpha, const region_t *reg_op) {
	auto gd = (struct gl_data *)base;
	static const struct gl_vertex_attribs_definition vertex_attribs = {
	    .stride = sizeof(GLfloat) * 4,
	    .count = 1,
	    .attribs = {{GL_FLOAT, vert_coord_loc, NULL}},
	};
	if (alpha == 1.0 || !pixman_region32_not_empty(reg_op)) {
		return true;
	}
	gl_bind_image_to_fbo(gd, target);
	// Result color = 0 (GL_ZERO) + alpha (GL_CONSTANT_ALPHA) * original color
	glBlendFunc(GL_ZERO, GL_CONSTANT_ALPHA);
	glBlendColor(0, 0, 0, (GLclampf)alpha);

	int nrects;
	const rect_t *rect = pixman_region32_rectangles(reg_op, &nrects);

	auto coord = ccalloc(nrects * 16, GLfloat);
	auto indices = ccalloc(nrects * 6, GLuint);

	struct gl_uniform_value uniforms[] = {
	    [UNIFORM_COLOR_LOC] = {.type = GL_FLOAT_VEC4, .f4 = {0, 0, 0, 0}},
	};
	gl_mask_rects_to_coords_simple(nrects, rect, coord, indices);
	gl_blit_inner(gd, gd->temp_fbo, nrects, coord, indices, &vertex_attribs,
	              &gd->fill_shader, ARR_SIZE(uniforms), uniforms);
	free(indices);
	free(coord);

	gl_check_err();
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	return true;
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
