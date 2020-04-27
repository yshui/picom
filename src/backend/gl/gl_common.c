// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <GL/gl.h>
#include <GL/glext.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX

#include "backend/backend.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "kernel.h"
#include "log.h"
#include "region.h"
#include "string_utils.h"
#include "types.h"
#include "utils.h"
#include "win.h"

#include "backend/backend_common.h"
#include "backend/gl/gl_common.h"

#define GLSL(version, ...) "#version " #version "\n" #__VA_ARGS__
#define QUOTE(...) #__VA_ARGS__

static const GLuint vert_coord_loc = 0;
static const GLuint vert_in_texcoord_loc = 1;

struct gl_blur_context {
	enum blur_method method;
	gl_blur_shader_t *blur_shader;

	/// Temporary textures used for blurring. They are always the same size as the
	/// target, so they are always big enough without resizing.
	/// Turns out calling glTexImage to resize is expensive, so we avoid that.
	GLuint *blur_textures;
	/// Temporary fbos used for blurring
	GLuint *blur_fbos;
	/// Cached size of each blur_texture
	struct texture_size {
		int width;
		int height;
	} * texture_sizes;

	int blur_texture_count;
	int blur_fbo_count;

	int fb_width, fb_height;

	/// How much do we need to resize the damaged region for blurring.
	int resize_width, resize_height;

	int npasses;
};

struct gl_round_context {
	gl_round_shader_t *round_shader;
	GLuint *bg_fbo;
	GLuint *bg_tex;
	/// Cached size of each blur_texture
	struct tex_size {
		int width;
		int height;
	} * tex_sizes;
	int tex_count;
	int fbo_count;
	bool round_borders;
};

static GLint glGetUniformLocationChecked(GLuint p, const char *name) {
	auto ret = glGetUniformLocation(p, name);
	if (ret < 0) {
		log_info("Failed to get location of uniform '%s'. This is normal when "
		         "using custom shaders.",
		         name);
	}
	return ret;
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
		if (GL_FALSE == status) {
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

	return shader;
}

GLuint gl_create_program(const GLuint *const shaders, int nshaders) {
	bool success = false;
	GLuint program = glCreateProgram();
	if (!program) {
		log_error("Failed to create program.");
		goto end;
	}

	for (int i = 0; i < nshaders; ++i)
		glAttachShader(program, shaders[i]);
	glLinkProgram(program);

	// Get program status
	{
		GLint status = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (GL_FALSE == status) {
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
		for (int i = 0; i < nshaders; ++i)
			glDetachShader(program, shaders[i]);
	}
	if (program && !success) {
		glDeleteProgram(program);
		program = 0;
	}

	return program;
}

/**
 * @brief Create a program from vertex and fragment shader strings.
 */
GLuint gl_create_program_from_str(const char *vert_shader_str, const char *frag_shader_str) {
	GLuint vert_shader = 0;
	GLuint frag_shader = 0;
	GLuint prog = 0;

	if (vert_shader_str)
		vert_shader = gl_create_shader(GL_VERTEX_SHADER, vert_shader_str);
	if (frag_shader_str)
		frag_shader = gl_create_shader(GL_FRAGMENT_SHADER, frag_shader_str);

	{
		GLuint shaders[2];
		int count = 0;
		if (vert_shader) {
			shaders[count++] = vert_shader;
		}
		if (frag_shader) {
			shaders[count++] = frag_shader;
		}
		if (count) {
			prog = gl_create_program(shaders, count);
		}
	}

	if (vert_shader)
		glDeleteShader(vert_shader);
	if (frag_shader)
		glDeleteShader(frag_shader);

	return prog;
}

static void gl_free_prog_main(gl_win_shader_t *pprogram) {
	if (!pprogram)
		return;
	if (pprogram->prog) {
		glDeleteProgram(pprogram->prog);
		pprogram->prog = 0;
	}
}

/*
 * @brief Implements recursive part of gl_average_texture_color.
 *
 * @note In order to reduce number of textures which needs to be
 * allocated and deleted during this recursive render
 * we reuse the same two textures for render source and
 * destination simply by alterating between them.
 * Unfortunately on first iteration source_texture might
 * be read-only. In this case we will select auxiliary_texture as
 * destination_texture in order not to touch that read-only source
 * texture in following render iteration.
 * Otherwise we simply will switch source and destination textures
 * between each other on each render iteration.
 */
static GLuint
_gl_average_texture_color(backend_t *base, GLuint source_texture, GLuint destination_texture,
                          GLuint auxiliary_texture, GLuint fbo, int width, int height) {
	const int max_width = 1;
	const int max_height = 1;
	const int from_width = next_power_of_two(width);
	const int from_height = next_power_of_two(height);
	const int to_width = from_width > max_width ? from_width / 2 : from_width;
	const int to_height = from_height > max_height ? from_height / 2 : from_height;

	glViewport(0, 0, width, height);

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
		result = _gl_average_texture_color(base, new_source_texture,
		                                   new_destination_texture, 0, fbo,
		                                   to_width, to_height);
	} else {
		result = destination_texture;
	}

	return result;
}

/*
 * @brief Builds a 1x1 texture which has color corresponding to the average of all
 * pixels of img by recursively rendering into texture of quorter the size (half
 * width and half height).
 * Returned texture must not be deleted, since it's owned by the gl_image. It will be
 * deleted when the gl_image is released.
 */
static GLuint gl_average_texture_color(backend_t *base, struct gl_image *img) {
	auto gd = (struct gl_data *)base;

	// Prepare textures which will be used for destination and source of rendering
	// during downscaling.
	const int texture_count = ARR_SIZE(img->inner->auxiliary_texture);
	if (!img->inner->auxiliary_texture[0]) {
		assert(!img->inner->auxiliary_texture[1]);
		glGenTextures(texture_count, img->inner->auxiliary_texture);
		glActiveTexture(GL_TEXTURE0);
		for (int i = 0; i < texture_count; i++) {
			glBindTexture(GL_TEXTURE_2D, img->inner->auxiliary_texture[i]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
			glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR,
			                 (GLint[]){0, 0, 0, 0});
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, img->inner->width,
			             img->inner->height, 0, GL_BGR, GL_UNSIGNED_BYTE, NULL);
		}
	}

	// Prepare framebuffer used for rendering and bind it
	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	// Enable shaders
	glUseProgram(gd->brightness_shader.prog);
	glUniform2f(glGetUniformLocationChecked(gd->brightness_shader.prog, "texsize"),
	            (GLfloat)img->inner->width, (GLfloat)img->inner->height);

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
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * 16, coord, GL_DYNAMIC_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * 6, indices,
	             GL_STATIC_DRAW);

	// Do actual recursive render to 1x1 texture
	GLuint result_texture = _gl_average_texture_color(
	    base, img->inner->texture, img->inner->auxiliary_texture[0],
	    img->inner->auxiliary_texture[1], fbo, img->inner->width, img->inner->height);

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
	glDeleteFramebuffers(1, &fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);

	// Cleanup render textures
	glBindTexture(GL_TEXTURE_2D, 0);

	gl_check_err();

	return result_texture;
}

/**
 * Render a region with texture data.
 *
 * @param ptex the texture
 * @param target the framebuffer to render into
 * @param dst_x,dst_y the top left corner of region where this texture
 *                    should go. In OpenGL coordinate system (important!).
 * @param reg_tgt     the clip region, in Xorg coordinate system
 * @param reg_visible ignored
 */
static void _gl_compose(backend_t *base, struct gl_image *img, GLuint target,
                        GLint *coord, GLuint *indices, int nrects) {
	auto gd = (struct gl_data *)base;
	if (!img || !img->inner->texture) {
		log_error("Missing texture.");
		return;
	}

	GLuint brightness = 0;
	if (img->max_brightness < 1.0) {
		brightness = gl_average_texture_color(base, img);
	}

	assert(gd->win_shader.prog);
	glUseProgram(gd->win_shader.prog);
	if (gd->win_shader.unifm_opacity >= 0) {
		glUniform1f(gd->win_shader.unifm_opacity, (float)img->opacity);
	}
	if (gd->win_shader.unifm_invert_color >= 0) {
		glUniform1i(gd->win_shader.unifm_invert_color, img->color_inverted);
	}
	if (gd->win_shader.unifm_tex >= 0) {
		glUniform1i(gd->win_shader.unifm_tex, 0);
	}
	if (gd->win_shader.unifm_dim >= 0) {
		glUniform1f(gd->win_shader.unifm_dim, (float)img->dim);
	}
	if (gd->win_shader.unifm_brightness >= 0) {
		glUniform1i(gd->win_shader.unifm_brightness, 1);
	}
	if (gd->win_shader.unifm_max_brightness >= 0) {
		glUniform1f(gd->win_shader.unifm_max_brightness, (float)img->max_brightness);
	}

	// log_trace("Draw: %d, %d, %d, %d -> %d, %d (%d, %d) z %d\n",
	//          x, y, width, height, dx, dy, ptex->width, ptex->height, z);

	// Bind texture
	glViewport(0, 0, gd->width, gd->height);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, brightness);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, img->inner->texture);

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * nrects * 16, coord, GL_STATIC_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * nrects * 6,
	             indices, GL_STATIC_DRAW);

	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_INT, GL_FALSE,
	                      sizeof(GLint) * 4, (void *)(sizeof(GLint) * 2));
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);
	glDisableVertexAttribArray(vert_coord_loc);
	glDisableVertexAttribArray(vert_in_texcoord_loc);
	glBindVertexArray(0);
	glDeleteVertexArrays(1, &vao);

	// Cleanup
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDeleteBuffers(2, bo);

	glUseProgram(0);

	gl_check_err();

	return;
}

/// Convert rectangles in X coordinates to OpenGL vertex and texture coordinates
/// @param[in] nrects, rects   rectangles
/// @param[in] dst_x, dst_y    origin of the OpenGL texture, affect the calculated texture
///                            coordinates
/// @param[in] texture_height  height of the OpenGL texture
/// @param[in] root_height     height of the back buffer
/// @param[in] y_inverted      whether the texture is y inverted
/// @param[out] coord, indices output
static void
x_rect_to_coords(int nrects, const rect_t *rects, int dst_x, int dst_y, int texture_height,
                 int root_height, bool y_inverted, GLint *coord, GLuint *indices) {
	dst_y = root_height - dst_y;
	if (y_inverted) {
		dst_y -= texture_height;
	}

	for (int i = 0; i < nrects; i++) {
		// Y-flip. Note after this, crect.y1 > crect.y2
		rect_t crect = rects[i];
		crect.y1 = root_height - crect.y1;
		crect.y2 = root_height - crect.y2;

		// Calculate texture coordinates
		// (texture_x1, texture_y1), texture coord for the _bottom left_ corner
		GLint texture_x1 = crect.x1 - dst_x, texture_y1 = crect.y2 - dst_y,
		      texture_x2 = texture_x1 + (crect.x2 - crect.x1),
		      texture_y2 = texture_y1 + (crect.y1 - crect.y2);

		// X pixmaps might be Y inverted, invert the texture coordinates
		if (y_inverted) {
			texture_y1 = texture_height - texture_y1;
			texture_y2 = texture_height - texture_y2;
		}

		// Vertex coordinates
		auto vx1 = crect.x1;
		auto vy1 = crect.y2;
		auto vx2 = crect.x2;
		auto vy2 = crect.y1;

		// log_trace("Rect %d: %f, %f, %f, %f -> %d, %d, %d, %d",
		//          ri, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);

		memcpy(&coord[i * 16],
		       (GLint[][2]){
		           {vx1, vy1},
		           {texture_x1, texture_y1},
		           {vx2, vy1},
		           {texture_x2, texture_y1},
		           {vx2, vy2},
		           {texture_x2, texture_y2},
		           {vx1, vy2},
		           {texture_x1, texture_y2},
		       },
		       sizeof(GLint[2]) * 8);

		GLuint u = (GLuint)(i * 4);
		memcpy(&indices[i * 6], (GLuint[]){u + 0, u + 1, u + 2, u + 2, u + 3, u + 0},
		       sizeof(GLuint) * 6);
	}
}

// TODO: make use of reg_visible
void gl_compose(backend_t *base, struct managed_win *w attr_unused, void *image_data, int dst_x, int dst_y,
                const region_t *reg_tgt, const region_t *reg_visible attr_unused) {
	auto gd = (struct gl_data *)base;
	struct gl_image *img = image_data;

	// Painting
	int nrects;
	const rect_t *rects;
	rects = pixman_region32_rectangles((region_t *)reg_tgt, &nrects);
	if (!nrects) {
		// Nothing to paint
		return;
	}

	// Until we start to use glClipControl, reg_tgt, dst_x and dst_y and
	// in a different coordinate system than the one OpenGL uses.
	// OpenGL window coordinate (or NDC) has the origin at the lower left of the
	// screen, with y axis pointing up; Xorg has the origin at the upper left of the
	// screen, with y axis pointing down. We have to do some coordinate conversion in
	// this function

	auto coord = ccalloc(nrects * 16, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);
	x_rect_to_coords(nrects, rects, dst_x, dst_y, img->inner->height, gd->height,
	                 img->inner->y_inverted, coord, indices);
	_gl_compose(base, img, gd->back_fbo, coord, indices, nrects);

	free(indices);
	free(coord);
}

/**
 * Blur contents in a particular region.
 */
bool gl_kernel_blur(backend_t *base, double opacity, void *ctx, const rect_t *extent,
                    const int width attr_unused, const int height attr_unused,
                    const int nrects, const GLuint vao[2]) {
	struct gl_blur_context *bctx = ctx;
	struct gl_data *gd = (void *)base;

	int dst_y_screen_coord = gd->height - extent->y2,
	    dst_y_fb_coord = bctx->fb_height - extent->y2;

	int curr = 0;
	for (int i = 0; i < bctx->npasses; ++i) {
		const gl_blur_shader_t *p = &bctx->blur_shader[i];
		assert(p->prog);

		assert(bctx->blur_textures[curr]);

		// The origin to use when sampling from the source texture
		GLint texorig_x, texorig_y;
		GLuint src_texture;

		if (i == 0) {
			texorig_x = extent->x1;
			texorig_y = dst_y_screen_coord;
			src_texture = gd->back_texture;
		} else {
			texorig_x = extent->x1 + bctx->resize_width;
			texorig_y = dst_y_fb_coord - bctx->resize_height;
			src_texture = bctx->blur_textures[curr];
		}

		glBindTexture(GL_TEXTURE_2D, src_texture);
		glUseProgram(p->prog);
		if (i < bctx->npasses - 1) {
			// not last pass, draw into framebuffer, with resized regions
			glBindVertexArray(vao[1]);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bctx->blur_fbos[0]);

			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			                       GL_TEXTURE_2D, bctx->blur_textures[!curr], 0);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				log_error("Framebuffer attachment failed.");
				return false;
			}

			glUniform1f(p->unifm_opacity, 1.0);
			// For other than last pass, we are drawing to a texture, we
			// translate the render origin so we don't need a big texture
			glUniform2f(p->orig_loc, (GLfloat)bctx->resize_width,
			            -(GLfloat)bctx->resize_height);
			glViewport(0, 0, bctx->fb_width, bctx->fb_height);
		} else {
			// last pass, draw directly into the back buffer, with origin
			// regions
			glBindVertexArray(vao[0]);
			glBindFramebuffer(GL_FRAMEBUFFER, gd->back_fbo);

			glUniform1f(p->unifm_opacity, (float)opacity);

			glUniform2f(p->orig_loc, 0, 0);
			glViewport(0, 0, gd->width, gd->height);
		}

		glUniform2f(p->texorig_loc, (GLfloat)texorig_x, (GLfloat)texorig_y);
		glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);

		// XXX use multiple draw calls is probably going to be slow than
		//     just simply blur the whole area.

		curr = !curr;
	}

	return true;
}

bool gl_dual_kawase_blur(backend_t *base, double opacity, void *ctx, const rect_t *extent,
                         const int width, const int height, const int nrects,
                         const GLuint vao[2]) {
	struct gl_blur_context *bctx = ctx;
	struct gl_data *gd = (void *)base;

	int dst_y_screen_coord = gd->height - extent->y2,
	    dst_y_fb_coord = bctx->fb_height - extent->y2;

	// Reduce number of iterations until the last one renders at least 1px in both
	// dimensions
	int iterations = bctx->blur_texture_count;
	while (((width / (1 << iterations)) < 1 || (height / (1 << iterations)) < 1) &&
	       iterations > 0) {
		--iterations;
	}

	// Note: OpenGL matrices are column major
	GLfloat projection_matrix[4][4] = {{2.0f / (GLfloat)bctx->fb_width, 0, 0, 0},
	                                   {0, 2.0f / (GLfloat)bctx->fb_height, 0, 0},
	                                   {0, 0, 0, 0},
	                                   {-1, -1, 0, 1}};

	// Kawase downsample pass
	const gl_blur_shader_t *down_pass = &bctx->blur_shader[0];
	assert(down_pass->prog);
	glUseProgram(down_pass->prog);

	// Update projection matrices in the blur shaders
	glUniformMatrix4fv(down_pass->projection_loc, 1, false, projection_matrix[0]);
	glUniform2f(down_pass->orig_loc, (GLfloat)bctx->resize_width,
	            -(GLfloat)bctx->resize_height);

	for (int i = 0; i < iterations; ++i) {
		GLuint src_texture;
		int tex_width, tex_height;
		int texorig_x, texorig_y;

		float halfpixel_x, halfpixel_y;

		if (i == 0) {
			// first pass: copy from back buffer
			src_texture = gd->back_texture;
			tex_width = gd->width;
			tex_height = gd->height;

			texorig_x = extent->x1;
			texorig_y = dst_y_screen_coord;

			halfpixel_x = 0.5f / (float)gd->width;
			halfpixel_y = 0.5f / (float)gd->height;
		} else {
			// copy from previous pass
			src_texture = bctx->blur_textures[i - 1];
			tex_width = bctx->fb_width;
			tex_height = bctx->fb_height;

			texorig_x = extent->x1 + bctx->resize_width;
			texorig_y = dst_y_fb_coord - bctx->resize_height;

			auto src_size = bctx->texture_sizes[i - 1];
			halfpixel_x = 0.5f / (float)src_size.width;
			halfpixel_y = 0.5f / (float)src_size.height;
		}

		glBindTexture(GL_TEXTURE_2D, src_texture);
		glBindVertexArray(vao[1]);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bctx->blur_fbos[i]);
		glDrawBuffer(GL_COLOR_ATTACHMENT0);

		glUniform2f(down_pass->texorig_loc, (GLfloat)texorig_x, (GLfloat)texorig_y);
		glUniform2f(down_pass->unifm_texture_size, (GLfloat)tex_width,
		            (GLfloat)tex_height);
		glUniform2f(down_pass->unifm_halfpixel, halfpixel_x, halfpixel_y);

		auto tgt_size = bctx->texture_sizes[i];
		glViewport(0, 0, tgt_size.width, tgt_size.height);
		glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);
	}

	// Kawase upsample pass
	const gl_blur_shader_t *up_pass = &bctx->blur_shader[1];
	assert(up_pass->prog);
	glUseProgram(up_pass->prog);

	// Update projection matrices in the blur shaders
	glUniformMatrix4fv(up_pass->projection_loc, 1, false, projection_matrix[0]);

	glUniform2f(up_pass->texorig_loc, (GLfloat)(extent->x1 + bctx->resize_width),
	            (GLfloat)(dst_y_fb_coord - bctx->resize_height));
	glUniform2f(up_pass->unifm_texture_size, (GLfloat)bctx->fb_width,
	            (GLfloat)bctx->fb_height);

	for (int i = iterations - 1; i >= 0; --i) {
		const GLuint src_texture = bctx->blur_textures[i];
		int orig_x, orig_y;

		auto src_size = bctx->texture_sizes[i];
		float halfpixel_x = 0.5f / (float)src_size.width,
		      halfpixel_y = 0.5f / (float)src_size.height;

		int vp_width, vp_height;

		glBindTexture(GL_TEXTURE_2D, src_texture);
		if (i > 0) {
			// not last pass, draw into next framebuffer
			glBindVertexArray(vao[1]);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bctx->blur_fbos[i - 1]);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
			glClearBufferiv(GL_COLOR, 0, (GLint[4]){255, 0, 0, 255});

			orig_x = bctx->resize_width;
			orig_y = -bctx->resize_height;

			auto tgt_size = bctx->texture_sizes[i - 1];
			vp_width = tgt_size.width;
			vp_height = tgt_size.height;

			glUniform1f(up_pass->unifm_opacity, (GLfloat)1);

			// For other than last pass, we are drawing to a texture, we
			// translate the render origin so we don't need a big texture
			//glUniform2f(up_pass->orig_loc, (GLfloat)bctx->resize_width,
			//            -(GLfloat)bctx->resize_height);
			//glViewport(0, 0, vp_width, vp_height);

		} else {
			// last pass, draw directly into the back buffer
			glBindVertexArray(vao[0]);
			glBindFramebuffer(GL_FRAMEBUFFER, gd->back_fbo);

			// Update projection matrix
			projection_matrix[0][0] = 2.0f / (GLfloat)gd->width;
			projection_matrix[1][1] = 2.0f / (GLfloat)gd->height;
			glUniformMatrix4fv(up_pass->projection_loc, 1, false,
			                   projection_matrix[0]);

			orig_x = 0;
			orig_y = 0;

			vp_width = gd->width;
			vp_height = gd->height;

			glUniform1f(up_pass->unifm_opacity, (GLfloat)opacity);

			//glUniform2f(up_pass->orig_loc, 0, 0);
			//glViewport(0, 0, vp_width, vp_height);
		}

		glUniform2f(up_pass->orig_loc, (GLfloat)orig_x, (GLfloat)orig_y);
		glUniform2f(up_pass->unifm_halfpixel, halfpixel_x, halfpixel_y);

		glViewport(0, 0, vp_width, vp_height);
		glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);
	}

	return true;
}

bool gl_blur(backend_t *base, double opacity, void *ctx, const region_t *reg_blur,
             const region_t *reg_visible attr_unused) {
	struct gl_blur_context *bctx = ctx;
	auto gd = (struct gl_data *)base;

	bool ret = false;

	if (gd->width + bctx->resize_width * 2 != bctx->fb_width ||
	    gd->height + bctx->resize_height * 2 != bctx->fb_height) {
		// Resize the temporary textures used for blur in case the root
		// size changed
		bctx->fb_width = gd->width + bctx->resize_width * 2;
		bctx->fb_height = gd->height + bctx->resize_height * 2;

		if (bctx->method == BLUR_METHOD_DUAL_KAWASE ||
			bctx->method == BLUR_METHOD_ALT_KAWASE) {
			// Use smaller textures for each iteration
			for (int i = 0; i < bctx->blur_texture_count; ++i) {
				auto tex_size = bctx->texture_sizes + i;
				tex_size->width = 1 + (bctx->fb_width - 1) / (1 << (i + 1));
				tex_size->height = 1 + (bctx->fb_height - 1) / (1 << (i + 1));

				glBindTexture(GL_TEXTURE_2D, bctx->blur_textures[i]);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_size->width,
				             tex_size->height, 0, GL_BGRA,
				             GL_UNSIGNED_BYTE, NULL);

				// Attach texture to FBO target
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bctx->blur_fbos[i]);
				glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
				                       GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
				                       bctx->blur_textures[i], 0);
				if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
				    GL_FRAMEBUFFER_COMPLETE) {
					log_error("Framebuffer attachment failed.");
					glBindFramebuffer(GL_FRAMEBUFFER, 0);
					return false;
				}
			}
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		} else {
			glBindTexture(GL_TEXTURE_2D, bctx->blur_textures[0]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bctx->fb_width,
			             bctx->fb_height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
			glBindTexture(GL_TEXTURE_2D, bctx->blur_textures[1]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bctx->fb_width,
			             bctx->fb_height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

			// XXX: do we need projection matrix for blur at all?
			// Note: OpenGL matrices are column major
			GLfloat projection_matrix[4][4] = {
			    {2.0f / (GLfloat)bctx->fb_width, 0, 0, 0},
			    {0, 2.0f / (GLfloat)bctx->fb_height, 0, 0},
			    {0, 0, 0, 0},
			    {-1, -1, 0, 1}};

			// Update projection matrices in the blur shaders
			for (int i = 0; i < bctx->npasses - 1; i++) {
				assert(bctx->blur_shader[i].prog);
				glUseProgram(bctx->blur_shader[i].prog);
				glUniformMatrix4fv(bctx->blur_shader[i].projection_loc, 1,
				                   false, projection_matrix[0]);
			}

			GLfloat projection_matrix2[4][4] = {
			    {2.0f / (GLfloat)gd->width, 0, 0, 0},
			    {0, 2.0f / (GLfloat)gd->height, 0, 0},
			    {0, 0, 0, 0},
			    {-1, -1, 0, 1}};
			assert(bctx->blur_shader[bctx->npasses - 1].prog);
			glUseProgram(bctx->blur_shader[bctx->npasses - 1].prog);
			glUniformMatrix4fv(bctx->blur_shader[bctx->npasses - 1].projection_loc,
			                   1, false, projection_matrix2[0]);
		}
	}

	// Remainder: regions are in Xorg coordinates
	auto reg_blur_resized =
	    resize_region(reg_blur, bctx->resize_width, bctx->resize_height);
	const rect_t *extent = pixman_region32_extents((region_t *)reg_blur),
	             *extent_resized = pixman_region32_extents(&reg_blur_resized);
	int width = extent->x2 - extent->x1, height = extent->y2 - extent->y1;
	if (width == 0 || height == 0) {
		return true;
	}

	int nrects, nrects_resized;
	const rect_t *rects = pixman_region32_rectangles((region_t *)reg_blur, &nrects),
	             *rects_resized =
	                 pixman_region32_rectangles(&reg_blur_resized, &nrects_resized);
	if (!nrects || !nrects_resized) {
		return true;
	}

	auto coord = ccalloc(nrects * 16, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);
	x_rect_to_coords(nrects, rects, extent_resized->x1, extent_resized->y2,
	                 bctx->fb_height, gd->height, false, coord, indices);

	auto coord_resized = ccalloc(nrects_resized * 16, GLint);
	auto indices_resized = ccalloc(nrects_resized * 6, GLuint);
	x_rect_to_coords(nrects_resized, rects_resized, extent_resized->x1,
	                 extent_resized->y2, bctx->fb_height, bctx->fb_height, false,
	                 coord_resized, indices_resized);
	pixman_region32_fini(&reg_blur_resized);

	GLuint vao[2];
	glGenVertexArrays(2, vao);
	GLuint bo[4];
	glGenBuffers(4, bo);

	glBindVertexArray(vao[0]);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * nrects * 16, coord, GL_STATIC_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * nrects * 6,
	             indices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_INT, GL_FALSE,
	                      sizeof(GLint) * 4, (void *)(sizeof(GLint) * 2));

	glBindVertexArray(vao[1]);
	glBindBuffer(GL_ARRAY_BUFFER, bo[2]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[3]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord_resized) * nrects_resized * 16,
	             coord_resized, GL_STATIC_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
	             (long)sizeof(*indices_resized) * nrects_resized * 6, indices_resized,
	             GL_STATIC_DRAW);
	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_INT, GL_FALSE,
	                      sizeof(GLint) * 4, (void *)(sizeof(GLint) * 2));

	if (bctx->method == BLUR_METHOD_DUAL_KAWASE ||
		bctx->method == BLUR_METHOD_ALT_KAWASE) {
		ret = gl_dual_kawase_blur(base, opacity, ctx, extent_resized, width,
		                          height, nrects, vao);
	} else {
		ret = gl_kernel_blur(base, opacity, ctx, extent_resized, width, height,
		                     nrects, vao);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDeleteBuffers(4, bo);
	glBindVertexArray(0);
	glDeleteVertexArrays(2, vao);
	glUseProgram(0);

	free(indices);
	free(coord);
	free(indices_resized);
	free(coord_resized);

	gl_check_err();
	return ret;
}

bool gl_round(backend_t *backend_data attr_unused, struct managed_win *w, void *ctx_, void *image_data,
                 const region_t *reg_round attr_unused, const region_t *reg_visible attr_unused) {

	struct gl_round_context *cctx = ctx_;
	auto gd = (struct gl_data *)backend_data;
	auto img = (struct gl_image*)image_data;

	//log_warn("r(%d) b(%d), wxy(%d %d) wwh(%d %d) img(%d %d)",
	//	w->corner_radius, w->g.border_width, w->g.x, w->g.y,
	//	w->widthb, w->heightb, img->inner->width, img->inner->height);

	int nrects;
	const rect_t *rects;
	rects = pixman_region32_rectangles((region_t *)reg_round, &nrects);
	if (!nrects) {
		// Nothing to paint
		return false;
	}

	GLuint target = gd->back_fbo;
	int dst_x = w->g.x;
	int dst_y = w->g.y;

	auto coord = ccalloc(nrects * 16, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);
	x_rect_to_coords(nrects, rects, dst_x, dst_y,
					img ? img->inner->height : w->heightb, gd->height,
					img ? img->inner->y_inverted : true, coord, indices);

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * nrects * 16, coord, GL_STATIC_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * nrects * 6,
	             indices, GL_STATIC_DRAW);

	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_INT, GL_FALSE,
	                      sizeof(GLint) * 4, (void *)(sizeof(GLint) * 2));

	// XXX: do we need projection matrix at all?
	// Note: OpenGL matrices are column major
	GLfloat projection_matrix[4][4] = {{2.0f / (GLfloat)gd->width, 0, 0, 0},
									   {0, 2.0f / (GLfloat)gd->height, 0, 0},
									   {0, 0, 0, 0},
									   {-1, -1, 0, 1}};

	//glDisable(GL_BLEND);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	// Bind texture
	glViewport(0, 0, gd->width, gd->height);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, cctx->bg_tex[0]);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, img ? img->inner->texture : gd->back_texture);

	const gl_round_shader_t *ppass = &cctx->round_shader[0];
	glUseProgram(ppass->prog);

	if (ppass->projection_loc >= 0)
		glUniformMatrix4fv(ppass->projection_loc, 1, false, projection_matrix[0]);
	if (ppass->unifm_tex_bg >= 0)
			glUniform1i(ppass->unifm_tex_bg, (GLint)1);
	if (ppass->unifm_radius)
		glUniform1f(ppass->unifm_radius, (float)w->corner_radius);
	if (ppass->unifm_texcoord)
		glUniform2f(ppass->unifm_texcoord, (float)w->g.x, (float)w->g.y);
	if (ppass->unifm_texsize)
		glUniform2f(ppass->unifm_texsize, (float)w->widthb, (float)w->heightb);
	if (ppass->unifm_borderw)
		glUniform1f(ppass->unifm_borderw, (w->round_borders) ? w->g.border_width : 0);
	if (ppass->unifm_resolution)
		glUniform2f(ppass->unifm_resolution, (float)gd->width, (float)gd->height);

	// Draw
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);
	glDisableVertexAttribArray(vert_coord_loc);
	glDisableVertexAttribArray(vert_in_texcoord_loc);
	glBindVertexArray(0);
	glDeleteVertexArrays(1, &vao);

	// Cleanup
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);
	glEnable(GL_BLEND);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDeleteBuffers(2, bo);

	glUseProgram(0);
	gl_check_err();

	free(indices);
	free(coord);

	return true;
}

// Assumes the two textures are the same dimensions
static bool copyFrameBufferTexture(int width, int height, GLuint fboIn, GLuint textureIn, GLuint fboOut, GLuint textureOut)
{
	bool ret = false;

    // Bind input FBO + texture to a color attachment
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fboIn);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureIn, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
		GL_FRAMEBUFFER_COMPLETE) {
		log_error("Source framebuffer attachment failed.");
		goto out;
	}
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    // Bind destination FBO + texture to another color attachment
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboOut);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, textureOut, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
		GL_FRAMEBUFFER_COMPLETE) {
		log_error("Destination framebuffer attachment failed.");
		goto out;
	}
    glDrawBuffer(GL_COLOR_ATTACHMENT1);

    // specify source, destination drawing (sub)rectangles.
    glBlitFramebuffer(0, 0, width, height,
                        0, 0, width, height,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);

	ret = true;

out:
    // unbind the color attachments
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	gl_check_err();

	return ret;
}

bool gl_store_back_texture(backend_t *backend_data attr_unused,
			struct managed_win *w attr_unused, void *ctx_ attr_unused,
			const region_t *reg_tgt attr_unused, int x attr_unused, int y attr_unused,
			int width attr_unused, int height attr_unused) {

	struct gl_round_context *cctx = ctx_;
	auto gd = (struct gl_data *)backend_data;

	//log_info("Copying xy(%d %d) wh(%d %d)", x, y, width, height);

	{
		// Prepare our backup texture
		glBindTexture(GL_TEXTURE_2D, cctx->bg_tex[0]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gd->width,
				gd->height, 0, GL_BGR, GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);

		copyFrameBufferTexture(gd->width, gd->height, gd->back_fbo, gd->back_texture, cctx->bg_fbo[0], cctx->bg_tex[0]);
	}
	
	return true;
}

// clang-format off
const char *vertex_shader = GLSL(330,
	uniform mat4 projection;
	uniform vec2 orig;
	uniform vec2 texorig;
	layout(location = 0) in vec2 coord;
	layout(location = 1) in vec2 in_texcoord;
	out vec2 texcoord;
	void main() {
		gl_Position = projection * vec4(coord + orig, 0, 1);
		texcoord = in_texcoord + texorig;
	}
);
// clang-format on

/**
 * Load a GLSL main program from shader strings.
 */
static int gl_win_shader_from_string(const char *vshader_str, const char *fshader_str,
                                     gl_win_shader_t *ret) {
	// Build program
	ret->prog = gl_create_program_from_str(vshader_str, fshader_str);
	if (!ret->prog) {
		log_error("Failed to create GLSL program.");
		return -1;
	}

	// Get uniform addresses
	ret->unifm_opacity = glGetUniformLocationChecked(ret->prog, "opacity");
	ret->unifm_invert_color = glGetUniformLocationChecked(ret->prog, "invert_color");
	ret->unifm_tex = glGetUniformLocationChecked(ret->prog, "tex");
	ret->unifm_dim = glGetUniformLocationChecked(ret->prog, "dim");
	ret->unifm_brightness = glGetUniformLocationChecked(ret->prog, "brightness");
	ret->unifm_max_brightness =
	    glGetUniformLocationChecked(ret->prog, "max_brightness");

	glUseProgram(ret->prog);
	int orig_loc = glGetUniformLocation(ret->prog, "orig");
	glUniform2f(orig_loc, 0, 0);

	gl_check_err();

	return true;
}

/**
 * Callback to run on root window size change.
 */
void gl_resize(struct gl_data *gd, int width, int height) {
	gd->height = height;
	gd->width = width;

	// XXX: do we need projection matrix at all?
	// Note: OpenGL matrices are column major
	GLfloat projection_matrix[4][4] = {{2.0f / (GLfloat)width, 0, 0, 0},
	                                   {0, 2.0f / (GLfloat)height, 0, 0},
	                                   {0, 0, 0, 0},
	                                   {-1, -1, 0, 1}};

	// Update projection matrix in the win shader
	glUseProgram(gd->win_shader.prog);
	int pml = glGetUniformLocationChecked(gd->win_shader.prog, "projection");
	glUniformMatrix4fv(pml, 1, false, projection_matrix[0]);

	glUseProgram(gd->fill_shader.prog);
	pml = glGetUniformLocationChecked(gd->fill_shader.prog, "projection");
	glUniformMatrix4fv(pml, 1, false, projection_matrix[0]);

	glUseProgram(gd->present_prog);
	pml = glGetUniformLocationChecked(gd->present_prog, "projection");
	glUniformMatrix4fv(pml, 1, false, projection_matrix[0]);

	glUseProgram(gd->brightness_shader.prog);
	pml = glGetUniformLocationChecked(gd->brightness_shader.prog, "projection");
	glUniformMatrix4fv(pml, 1, false, projection_matrix[0]);

	glBindTexture(GL_TEXTURE_2D, gd->back_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_BGR,
	             GL_UNSIGNED_BYTE, NULL);

	gl_check_err();
}

// clang-format off
static const char dummy_frag[] = GLSL(330,
	uniform sampler2D tex;
	in vec2 texcoord;
	void main() {
		gl_FragColor = texelFetch(tex, ivec2(texcoord.xy), 0);
	}
);

static const char fill_frag[] = GLSL(330,
	uniform vec4 color;
	void main() {
		gl_FragColor = color;
	}
);

static const char fill_vert[] = GLSL(330,
	layout(location = 0) in vec2 in_coord;
	uniform mat4 projection;
	void main() {
		gl_Position = projection * vec4(in_coord, 0, 1);
	}
);

static const char interpolating_frag[] = GLSL(330,
	uniform sampler2D tex;
	in vec2 texcoord;
	void main() {
		gl_FragColor = vec4(texture2D(tex, vec2(texcoord.xy), 0).rgb, 1);
	}
);

static const char interpolating_vert[] = GLSL(330,
	uniform mat4 projection;
	uniform vec2 texsize;
	layout(location = 0) in vec2 in_coord;
	layout(location = 1) in vec2 in_texcoord;
	out vec2 texcoord;
	void main() {
		gl_Position = projection * vec4(in_coord, 0, 1);
		texcoord = in_texcoord / texsize;
	}
);
// clang-format on

/// Fill a given region in bound framebuffer.
/// @param[in] y_inverted whether the y coordinates in `clip` should be inverted
static void _gl_fill(backend_t *base, struct color c, const region_t *clip, GLuint target,
                     int height, bool y_inverted) {
	static const GLuint fill_vert_in_coord_loc = 0;
	int nrects;
	const rect_t *rect = pixman_region32_rectangles((region_t *)clip, &nrects);
	auto gd = (struct gl_data *)base;

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glUseProgram(gd->fill_shader.prog);
	glUniform4f(gd->fill_shader.color_loc, (GLfloat)c.red, (GLfloat)c.green,
	            (GLfloat)c.blue, (GLfloat)c.alpha);
	glEnableVertexAttribArray(fill_vert_in_coord_loc);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);

	auto coord = ccalloc(nrects * 8, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);
	for (int i = 0; i < nrects; i++) {
		GLint y1 = y_inverted ? height - rect[i].y2 : rect[i].y1,
		      y2 = y_inverted ? height - rect[i].y1 : rect[i].y2;
		memcpy(&coord[i * 8],
		       (GLint[][2]){
		           {rect[i].x1, y1}, {rect[i].x2, y1}, {rect[i].x2, y2}, {rect[i].x1, y2}},
		       sizeof(GLint[2]) * 4);
		indices[i * 6 + 0] = (GLuint)i * 4 + 0;
		indices[i * 6 + 1] = (GLuint)i * 4 + 1;
		indices[i * 6 + 2] = (GLuint)i * 4 + 2;
		indices[i * 6 + 3] = (GLuint)i * 4 + 2;
		indices[i * 6 + 4] = (GLuint)i * 4 + 3;
		indices[i * 6 + 5] = (GLuint)i * 4 + 0;
	}
	glBufferData(GL_ARRAY_BUFFER, nrects * 8 * (long)sizeof(*coord), coord, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, nrects * 6 * (long)sizeof(*indices),
	             indices, GL_STREAM_DRAW);

	glVertexAttribPointer(fill_vert_in_coord_loc, 2, GL_INT, GL_FALSE,
	                      sizeof(*coord) * 2, (void *)0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDisableVertexAttribArray(fill_vert_in_coord_loc);
	glBindVertexArray(0);
	glDeleteVertexArrays(1, &vao);

	glDeleteBuffers(2, bo);
	free(indices);
	free(coord);
}

void gl_fill(backend_t *base, struct color c, const region_t *clip) {
	auto gd = (struct gl_data *)base;
	return _gl_fill(base, c, clip, gd->back_fbo, gd->height, true);
}

void gl_release_image(backend_t *base, void *image_data) {
	struct gl_image *wd = image_data;
	auto gd = (struct gl_data *)base;
	wd->inner->refcount--;
	assert(wd->inner->refcount >= 0);
	if (wd->inner->refcount > 0) {
		free(wd);
		return;
	}

	gd->release_user_data(base, wd->inner);
	assert(wd->inner->user_data == NULL);

	glDeleteTextures(1, &wd->inner->texture);
	glDeleteTextures(2, wd->inner->auxiliary_texture);
	free(wd->inner);
	free(wd);
	gl_check_err();
}

void *gl_copy(backend_t *base attr_unused, const void *image_data,
              const region_t *reg_visible attr_unused) {
	const struct gl_image *img = image_data;
	auto new_img = ccalloc(1, struct gl_image);
	*new_img = *img;
	new_img->inner->refcount++;
	return new_img;
}

static inline void gl_free_blur_shader(gl_blur_shader_t *shader) {
	if (shader->prog) {
		glDeleteProgram(shader->prog);
	}

	shader->prog = 0;
}

void gl_destroy_blur_context(backend_t *base attr_unused, void *ctx) {
	struct gl_blur_context *bctx = ctx;
	// Free GLSL shaders/programs
	for (int i = 0; i < bctx->npasses; ++i) {
		gl_free_blur_shader(&bctx->blur_shader[i]);
	}
	free(bctx->blur_shader);

	if (bctx->blur_textures) {
		glDeleteTextures(bctx->blur_texture_count, bctx->blur_textures);
		free(bctx->blur_textures);
	}
	if (bctx->blur_fbos) {
		glDeleteFramebuffers(bctx->blur_fbo_count, bctx->blur_fbos);
		free(bctx->blur_fbos);
	}
	if (bctx->texture_sizes) {
		free(bctx->texture_sizes);
	}

	bctx->blur_texture_count = 0;
	bctx->blur_fbo_count = 0;

	free(bctx);

	gl_check_err();
}

void gl_destroy_round_context(struct backend_base *base attr_unused, void *ctx attr_unused) {

	struct gl_round_context *cctx = ctx;

	if (cctx->round_shader && cctx->round_shader->prog) {
		glDeleteProgram(cctx->round_shader->prog);
		cctx->round_shader->prog = 0;
		free(cctx->round_shader);
	}

	if (cctx->bg_tex) {
		glDeleteTextures(cctx->tex_count, cctx->bg_tex);
		free(cctx->bg_tex);
	}
	if (cctx->bg_fbo) {
		glDeleteFramebuffers(cctx->fbo_count, cctx->bg_fbo);
		free(cctx->bg_fbo);
	}
	if (cctx->tex_sizes) {
		free(cctx->tex_sizes);
	}

	cctx->tex_count = 0;
	cctx->fbo_count = 0;

	free(cctx);

	gl_check_err();
}

/**
 * Initialize GL blur filters.
 */
bool gl_create_kernel_blur_context(void *blur_context, enum blur_method method, void *args) {
	bool success;

	struct gl_blur_context *ctx = blur_context;
	struct conv **kernels;

	int nkernels;
	ctx->method = BLUR_METHOD_KERNEL;
	if (method == BLUR_METHOD_KERNEL) {
		nkernels = ((struct kernel_blur_args *)args)->kernel_count;
		kernels = ((struct kernel_blur_args *)args)->kernels;
	} else {
		kernels = generate_blur_kernel(method, args, &nkernels);
	}

	if (!nkernels) {
		ctx->method = BLUR_METHOD_NONE;
		return true;
	}

	ctx->blur_shader = ccalloc(max2(2, nkernels), gl_blur_shader_t);

	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
	// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
	// Thanks to hiciu for reporting.
	setlocale(LC_NUMERIC, "C");

	// clang-format off
	static const char *FRAG_SHADER_BLUR = GLSL(330,
		%s\n // other extension pragmas
		uniform sampler2D tex_scr;
		uniform float opacity;
		in vec2 texcoord;
		out vec4 out_color;
		void main() {
			vec4 sum = vec4(0.0, 0.0, 0.0, 0.0);
			%s //body of the convolution
			out_color = sum / float(%.7g) * opacity;
		}
	);
	static const char *FRAG_SHADER_BLUR_ADD = QUOTE(
		sum += float(%.7g) *
		       texelFetch(tex_scr, ivec2(texcoord + vec2(%d, %d)), 0);
	);
	// clang-format on

	const char *shader_add = FRAG_SHADER_BLUR_ADD;
	char *extension = strdup("");

	for (int i = 0; i < nkernels; i++) {
		auto kern = kernels[i];
		// Build shader
		int width = kern->w, height = kern->h;
		int nele = width * height;
		size_t body_len = (strlen(shader_add) + 42) * (uint)nele;
		char *shader_body = ccalloc(body_len, char);
		char *pc = shader_body;

		double sum = 0.0;
		for (int j = 0; j < height; ++j) {
			for (int k = 0; k < width; ++k) {
				double val;
				val = kern->data[j * width + k];
				if (val == 0) {
					continue;
				}
				sum += val;
				pc += snprintf(pc, body_len - (ulong)(pc - shader_body),
				               FRAG_SHADER_BLUR_ADD, val, k - width / 2,
				               j - height / 2);
				assert(pc < shader_body + body_len);
			}
		}

		auto pass = ctx->blur_shader + i;
		size_t shader_len = strlen(FRAG_SHADER_BLUR) + strlen(extension) +
		                    strlen(shader_body) + 10 /* sum */ +
		                    1 /* null terminator */;
		char *shader_str = ccalloc(shader_len, char);
		auto real_shader_len = snprintf(shader_str, shader_len, FRAG_SHADER_BLUR,
		                                extension, shader_body, sum);
		CHECK(real_shader_len >= 0);
		CHECK((size_t)real_shader_len < shader_len);
		free(shader_body);

		// Build program
		pass->prog = gl_create_program_from_str(vertex_shader, shader_str);
		free(shader_str);
		if (!pass->prog) {
			log_error("Failed to create GLSL program.");
			success = false;
			goto out;
		}
		glBindFragDataLocation(pass->prog, 0, "out_color");

		// Get uniform addresses
		pass->unifm_opacity = glGetUniformLocationChecked(pass->prog, "opacity");
		pass->orig_loc = glGetUniformLocationChecked(pass->prog, "orig");
		pass->texorig_loc = glGetUniformLocationChecked(pass->prog, "texorig");
		pass->projection_loc =
		    glGetUniformLocationChecked(pass->prog, "projection");
		ctx->resize_width += kern->w / 2;
		ctx->resize_height += kern->h / 2;
	}

	if (nkernels == 1) {
		// Generate an extra null pass so we don't need special code path for
		// the single pass case
		auto pass = &ctx->blur_shader[1];
		pass->prog = gl_create_program_from_str(vertex_shader, dummy_frag);
		pass->unifm_opacity = -1;
		pass->orig_loc = glGetUniformLocationChecked(pass->prog, "orig");
		pass->texorig_loc = glGetUniformLocationChecked(pass->prog, "texorig");
		pass->projection_loc =
		    glGetUniformLocationChecked(pass->prog, "projection");
		ctx->npasses = 2;
	} else {
		ctx->npasses = nkernels;
	}

	// Specify required textures and FBOs
	ctx->blur_texture_count = 2;
	ctx->blur_fbo_count = 1;

	success = true;
out:
	if (method != BLUR_METHOD_KERNEL) {
		// We generated the blur kernels, so we need to free them
		for (int i = 0; i < nkernels; i++) {
			free(kernels[i]);
		}
		free(kernels);
	}

	free(extension);
	// Restore LC_NUMERIC
	setlocale(LC_NUMERIC, lc_numeric_old);
	free(lc_numeric_old);

	return success;
}

bool gl_create_dual_kawase_blur_context(void *blur_context, enum blur_method method,
                                        void *args) {
	bool success;
	struct gl_blur_context *ctx = blur_context;

	ctx->method = method;

	auto blur_params = generate_dual_kawase_params(args);

	// Specify required textures and FBOs
	ctx->blur_texture_count = blur_params->iterations;
	ctx->blur_fbo_count = blur_params->iterations;

	ctx->resize_width += blur_params->expand;
	ctx->resize_height += blur_params->expand;

	ctx->npasses = 2;
	ctx->blur_shader = ccalloc(ctx->npasses, gl_blur_shader_t);

	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
	// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
	// Thanks to hiciu for reporting.
	setlocale(LC_NUMERIC, "C");

	// Dual-kawase downsample shader / program
	auto down_pass = ctx->blur_shader;
	{
		// clang-format off
		static const char *FRAG_SHADER_DOWN = GLSL(330,
			uniform sampler2D tex_src;
			uniform vec2 texture_size;
			uniform vec2 halfpixel;
			in vec2 texcoord;
			out vec4 out_color;
			void main() {
				float offset = %.7g;
				vec2 uv = texcoord / texture_size;
				vec4 sum = texture2D(tex_src, uv) * 4.0;
				sum += texture2D(tex_src, uv - halfpixel.xy * offset);
				sum += texture2D(tex_src, uv + halfpixel.xy * offset);
				sum += texture2D(tex_src, uv + vec2(halfpixel.x, -halfpixel.y) * offset);
				sum += texture2D(tex_src, uv - vec2(halfpixel.x, -halfpixel.y) * offset);
				out_color = sum / 8.0;
			}
		);
		// clang-format on

		// Build shader
		size_t shader_len =
		    strlen(FRAG_SHADER_DOWN) + 10 /* offset */ + 1 /* null terminator */;
		char *shader_str = ccalloc(shader_len, char);
		auto real_shader_len =
		    snprintf(shader_str, shader_len, FRAG_SHADER_DOWN, blur_params->offset);
		CHECK(real_shader_len >= 0);
		CHECK((size_t)real_shader_len < shader_len);

		// Build program
		down_pass->prog = gl_create_program_from_str(vertex_shader, shader_str);
		free(shader_str);
		if (!down_pass->prog) {
			log_error("Failed to create GLSL program.");
			success = false;
			goto out;
		}
		glBindFragDataLocation(down_pass->prog, 0, "out_color");

		// Get uniform addresses
		down_pass->unifm_texture_size =
		    glGetUniformLocationChecked(down_pass->prog, "texture_size");
		down_pass->unifm_halfpixel =
		    glGetUniformLocationChecked(down_pass->prog, "halfpixel");
		down_pass->orig_loc =
		    glGetUniformLocationChecked(down_pass->prog, "orig");
		down_pass->texorig_loc =
		    glGetUniformLocationChecked(down_pass->prog, "texorig");
		down_pass->projection_loc =
		    glGetUniformLocationChecked(down_pass->prog, "projection");
	}

	// Dual-kawase upsample shader / program
	auto up_pass = ctx->blur_shader + 1;
	{
		// clang-format off
		static const char *FRAG_SHADER_UP = GLSL(330,
			uniform sampler2D tex_src;
			uniform vec2 texture_size;
			uniform vec2 halfpixel;
			uniform float offset;
			uniform float opacity;
			in vec2 texcoord;
			out vec4 out_color;
			void main() {
				float offset = %.7g;
				vec2 uv = texcoord / texture_size;
				vec4 sum = texture2D(tex_src, uv + vec2(-halfpixel.x * 2.0, 0.0) * offset);
				sum += texture2D(tex_src, uv + vec2(-halfpixel.x, halfpixel.y) * offset) * 2.0;
				sum += texture2D(tex_src, uv + vec2(0.0, halfpixel.y * 2.0) * offset);
				sum += texture2D(tex_src, uv + vec2(halfpixel.x, halfpixel.y) * offset) * 2.0;
				sum += texture2D(tex_src, uv + vec2(halfpixel.x * 2.0, 0.0) * offset);
				sum += texture2D(tex_src, uv + vec2(halfpixel.x, -halfpixel.y) * offset) * 2.0;
				sum += texture2D(tex_src, uv + vec2(0.0, -halfpixel.y * 2.0) * offset);
				sum += texture2D(tex_src, uv + vec2(-halfpixel.x, -halfpixel.y) * offset) * 2.0;
				out_color = sum / 12.0;// * opacity;
			}
		);
		// clang-format on

		// Build shader
		size_t shader_len =
		    strlen(FRAG_SHADER_UP) + 10 /* offset */ + 1 /* null terminator */;
		char *shader_str = ccalloc(shader_len, char);
		auto real_shader_len =
		    snprintf(shader_str, shader_len, FRAG_SHADER_UP, blur_params->offset);
		CHECK(real_shader_len >= 0);
		CHECK((size_t)real_shader_len < shader_len);

		// Build program
		up_pass->prog = gl_create_program_from_str(vertex_shader, shader_str);
		free(shader_str);
		if (!up_pass->prog) {
			log_error("Failed to create GLSL program.");
			success = false;
			goto out;
		}
		glBindFragDataLocation(up_pass->prog, 0, "out_color");

		// Get uniform addresses
		up_pass->unifm_opacity =
		    glGetUniformLocationChecked(up_pass->prog, "opacity");
		up_pass->unifm_texture_size =
		    glGetUniformLocationChecked(up_pass->prog, "texture_size");
		up_pass->unifm_halfpixel =
		    glGetUniformLocationChecked(up_pass->prog, "halfpixel");
		up_pass->orig_loc = glGetUniformLocationChecked(up_pass->prog, "orig");
		up_pass->texorig_loc =
		    glGetUniformLocationChecked(up_pass->prog, "texorig");
		up_pass->projection_loc =
		    glGetUniformLocationChecked(up_pass->prog, "projection");
	}

	success = true;
out:
	free(blur_params);

	if (!success) {
		ctx = NULL;
	}

	// Restore LC_NUMERIC
	setlocale(LC_NUMERIC, lc_numeric_old);
	free(lc_numeric_old);

	return success;
}

void *gl_create_blur_context(backend_t *base, enum blur_method method, void *args) {
	bool success;
	auto gd = (struct gl_data *)base;

	auto ctx = ccalloc(1, struct gl_blur_context);

	if (!method || method >= BLUR_METHOD_INVALID) {
		ctx->method = BLUR_METHOD_NONE;
		return ctx;
	}

	if (method == BLUR_METHOD_DUAL_KAWASE || method == BLUR_METHOD_ALT_KAWASE) {
		success = gl_create_dual_kawase_blur_context(ctx, method, args);
	} else {
		success = gl_create_kernel_blur_context(ctx, method, args);
	}
	if (!success) {
		goto out;
	}

	// Texture size will be defined by gl_blur
	ctx->blur_textures = ccalloc(ctx->blur_texture_count, GLuint);
	ctx->texture_sizes = ccalloc(ctx->blur_texture_count, struct texture_size);
	glGenTextures(ctx->blur_texture_count, ctx->blur_textures);
	for (int i = 0; i < ctx->blur_texture_count; ++i) {
		glBindTexture(GL_TEXTURE_2D, ctx->blur_textures[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

	// Generate FBO and textures when needed
	ctx->blur_fbos = ccalloc(ctx->blur_fbo_count, GLuint);
	glGenFramebuffers(ctx->blur_fbo_count, ctx->blur_fbos);
	for (int i = 0; i < ctx->blur_fbo_count; ++i) {
		if (!ctx->blur_fbos[i]) {
			log_error("Failed to generate framebuffer object for blur");
			success = false;
			goto out;
		}
	}

out:
	if (!success) {
		gl_destroy_blur_context(&gd->base, ctx);
		ctx = NULL;
	}

	gl_check_err();
	return ctx;
}

void gl_get_blur_size(void *blur_context, int *width, int *height) {
	struct gl_blur_context *ctx = blur_context;
	*width = ctx->resize_width;
	*height = ctx->resize_height;
}

void *gl_create_round_context(struct backend_base *base attr_unused, void *args attr_unused) {
	bool success;
	auto gd = (struct gl_data *)base;
	auto ctx = ccalloc(1, struct gl_round_context);

	struct round_corners_args *round_params = (struct round_corners_args *)args;

	ctx->round_borders = round_params->round_borders;
	ctx->round_shader = ccalloc(1, gl_round_shader_t);

	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
	// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
	// Thanks to hiciu for reporting.
	setlocale(LC_NUMERIC, "C");

	// Dual-kawase downsample shader / program
	auto pass = ctx->round_shader;
	{
		// TEST passthrough shader
		/*static const char frag_passthrough[] = GLSL(330,
			uniform sampler2D tex;
			in vec2 texcoord;
			void main() {
				//gl_FragColor = texture2D(tex, texcoord);
				//gl_FragColor = vec4(texture2D(tex, vec2(texcoord.xy), 0).rgb, 1);
				gl_FragColor = texelFetch(tex, ivec2(texcoord.xy), 0);
				//gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
			}
		);*/

		// dst0 shader from opengl.c
		// clang-format off
		static const char *FRAG_SHADER_ROUND_CORNERS = GLSL(330,
			uniform sampler2D tex;
			uniform sampler2D tex_bg;
			uniform float u_radius;
			uniform float u_borderw;
			uniform vec2 u_texcoord;
			uniform vec2 u_texsize;
			uniform vec2 u_resolution;
			in vec2 texcoord;
			out vec4 out_color;
			// https://www.shadertoy.com/view/ltS3zW
			float RectSDF(vec2 p, vec2 b, float r) {
			  vec2 d = abs(p) - b + vec2(r);
			  return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;
			}
			void main() {
				vec2 coord = vec2(u_texcoord.x, u_resolution.y-u_texsize.y-u_texcoord.y);
				vec4 u_v4WndBgColor = texelFetch(tex_bg, ivec2(gl_FragCoord.xy), 0);
				vec4 u_v4BorderColor = texelFetch(tex, ivec2(0, 0), 0);
				vec4 u_v4FillColor = vec4(0.0, 0.0, 0.0, 0.0);  // Inside rect, transparent
				vec4 v4FromColor = u_v4BorderColor;				// Always the border color. If no border, this still should be set
				vec4 v4ToColor = u_v4WndBgColor;				// Outside corners color = background texture
				float u_fRadiusPx = u_radius;
				float u_fHalfBorderThickness = u_borderw / 2.0;

				// misc tests, uncomment for diff rect colors
				//u_v4FillColor = texture2D(tex, texcoord/u_texsize);
				//u_v4FillColor = texelFetch(tex, ivec2(texcoord.xy), 0);
				//u_v4FillColor = vec4(0.0, 1.0, 0.0, 0.0);  // Inside rect color
				//v4FromColor = u_v4BorderColor = vec4(1.0, 1.0, 0.0, 1.0);
				//v4ToColor = vec4(0.0, 0.0, 1.0, 1.0); //Outside color

				vec2 u_v2HalfShapeSizePx = u_texsize/2.0 - vec2(u_fHalfBorderThickness);
				vec2 v_v2CenteredPos = (gl_FragCoord.xy - u_texsize.xy / 2.0 - coord);

				float fDist = RectSDF(v_v2CenteredPos, u_v2HalfShapeSizePx, u_fRadiusPx - u_fHalfBorderThickness);
				if (u_fHalfBorderThickness > 0.0) {
					if (fDist < 0.0) {
						v4ToColor = u_v4FillColor;
					}
					fDist = abs(fDist) - u_fHalfBorderThickness;
				} else {
					v4FromColor = u_v4FillColor;
				}
				float fBlendAmount = smoothstep(-1.0, 1.0, fDist);

				// final color
				vec4 c = mix(v4FromColor, v4ToColor, fBlendAmount);
				// we don't use discard due to alleged worse perf
				// instead we can use alpha blending
				//if ( c == vec4(0.0,0.0,0.0,0.0) ) discard; else
				out_color = c;
			}
		);
		// clang-format on

		// Build shader
		const char* SHADER_STR = FRAG_SHADER_ROUND_CORNERS;
		//const char* SHADER_STR = frag_passthrough;
		size_t shader_len = strlen(SHADER_STR) + 1 /* null terminator */;
		char *shader_str = ccalloc(shader_len, char);
		auto real_shader_len = snprintf(shader_str, shader_len, "%s", SHADER_STR);
		CHECK(real_shader_len >= 0);
		CHECK((size_t)real_shader_len < shader_len);

		// Build program
		pass->prog = gl_create_program_from_str(vertex_shader, shader_str);
		free(shader_str);
		if (!pass->prog) {
			log_error("Failed to create GLSL program.");
			success = false;
			goto out;
		}
		glBindFragDataLocation(pass->prog, 0, "out_color");

		// Get uniform addresses
		pass->projection_loc = glGetUniformLocationChecked(pass->prog, "projection");
		pass->unifm_tex_bg = glGetUniformLocationChecked(pass->prog, "tex_bg");
		pass->unifm_radius = glGetUniformLocationChecked(pass->prog, "u_radius");
		pass->unifm_texcoord = glGetUniformLocationChecked(pass->prog, "u_texcoord");
		pass->unifm_texsize = glGetUniformLocationChecked(pass->prog, "u_texsize");
		pass->unifm_borderw = glGetUniformLocationChecked(pass->prog, "u_borderw");
		pass->unifm_resolution = glGetUniformLocationChecked(pass->prog, "u_resolution");
	}

	// Texture size will be defined by gl_round
	ctx->tex_count = 1;
	ctx->bg_tex = ccalloc(ctx->tex_count, GLuint);
	ctx->tex_sizes = ccalloc(ctx->tex_count, struct tex_size);
	glGenTextures(ctx->tex_count, ctx->bg_tex);
	for (int i = 0; i < ctx->tex_count; ++i) {
		glBindTexture(GL_TEXTURE_2D, ctx->bg_tex[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

	// Generate FBO and textures when needed
	ctx->fbo_count = 1;
	ctx->bg_fbo = ccalloc(ctx->fbo_count, GLuint);
	glGenFramebuffers(ctx->fbo_count, ctx->bg_fbo);
	for (int i = 0; i < ctx->fbo_count; ++i) {
		if (!ctx->bg_fbo[i]) {
			log_error("Failed to generate framebuffer object for blur");
			success = false;
			goto out;
		}
	}

	success = true;

out:

	// Restore LC_NUMERIC
	setlocale(LC_NUMERIC, lc_numeric_old);
	free(lc_numeric_old);

	if (!success) {
		gl_destroy_round_context(&gd->base, ctx);
		ctx = NULL;
	}

	gl_check_err();
	return ctx;
}

// clang-format off
const char *win_shader_glsl = GLSL(330,
	uniform float opacity;
	uniform float dim;
	uniform bool invert_color;
	in vec2 texcoord;
	uniform sampler2D tex;
	uniform sampler2D brightness;
	uniform float max_brightness;

	void main() {
		vec4 c = texelFetch(tex, ivec2(texcoord), 0);
		if (invert_color) {
			c = vec4(c.aaa - c.rgb, c.a);
		}
		c = vec4(c.rgb * (1.0 - dim), c.a) * opacity;

		vec3 rgb_brightness = texelFetch(brightness, ivec2(0, 0), 0).rgb;
		// Ref: https://en.wikipedia.org/wiki/Relative_luminance
		float brightness = rgb_brightness.r * 0.21 +
				   rgb_brightness.g * 0.72 +
				   rgb_brightness.b * 0.07;
		if (brightness > max_brightness)
			c.rgb = c.rgb * (max_brightness / brightness);

		gl_FragColor = c;
	}
);

const char *present_vertex_shader = GLSL(330,
	uniform mat4 projection;
	layout(location = 0) in vec2 coord;
	out vec2 texcoord;
	void main() {
		gl_Position = projection * vec4(coord, 0, 1);
		texcoord = coord;
	}
);
// clang-format on

bool gl_init(struct gl_data *gd, session_t *ps) {
	// Initialize GLX data structure
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

	// Clear screen
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

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

	gl_win_shader_from_string(vertex_shader, win_shader_glsl, &gd->win_shader);
	gd->fill_shader.prog = gl_create_program_from_str(fill_vert, fill_frag);
	gd->fill_shader.color_loc = glGetUniformLocation(gd->fill_shader.prog, "color");

	gd->present_prog = gl_create_program_from_str(present_vertex_shader, dummy_frag);
	if (!gd->present_prog) {
		log_error("Failed to create the present shader");
		return false;
	}
	glUseProgram(gd->present_prog);
	glUniform1i(glGetUniformLocationChecked(gd->present_prog, "tex"), 0);
	glUseProgram(0);

	gd->brightness_shader.prog =
	    gl_create_program_from_str(interpolating_vert, interpolating_frag);
	if (!gd->brightness_shader.prog) {
		log_error("Failed to create the brightness shader");
		return false;
	}
	glUseProgram(gd->brightness_shader.prog);
	glUniform1i(glGetUniformLocationChecked(gd->brightness_shader.prog, "tex"), 0);
	glUseProgram(0);

	// Set up the size of the viewport. We do this last because it expects the blur
	// textures are already set up.
	gl_resize(gd, ps->root_width, ps->root_height);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->back_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       gd->back_texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	gd->logger = gl_string_marker_logger_new();
	if (gd->logger) {
		log_add_target_tls(gd->logger);
	}

	const char *vendor = (const char *)glGetString(GL_VENDOR);
	log_debug("GL_VENDOR = %s", vendor);
	if (strcmp(vendor, "NVIDIA Corporation") == 0) {
		log_info("GL vendor is NVIDIA, don't use glFinish");
		gd->is_nvidia = true;
	} else {
		gd->is_nvidia = false;
	}

	return true;
}

void gl_deinit(struct gl_data *gd) {
	gl_free_prog_main(&gd->win_shader);

	if (gd->logger) {
		log_remove_target_tls(gd->logger);
		gd->logger = NULL;
	}

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

/// Decouple `img` from the image it references, also applies all the lazy operations
static inline void gl_image_decouple(backend_t *base, struct gl_image *img) {
	if (img->inner->refcount == 1) {
		return;
	}

	auto gd = (struct gl_data *)base;
	auto new_tex = ccalloc(1, struct gl_texture);

	new_tex->texture = gl_new_texture(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, new_tex->texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img->inner->width, img->inner->height, 0,
	             GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	new_tex->y_inverted = true;
	new_tex->height = img->inner->height;
	new_tex->width = img->inner->width;
	new_tex->refcount = 1;
	new_tex->user_data = gd->decouple_texture_user_data(base, img->inner->user_data);

	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       new_tex->texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);

	// clang-format off
	GLint coord[] = {
		// top left
		0, 0,                 // vertex coord
		0, 0,                 // texture coord

		// top right
		img->inner->width, 0, // vertex coord
		img->inner->width, 0, // texture coord

		// bottom right
		img->inner->width, img->inner->height,
		img->inner->width, img->inner->height,

		// bottom left
		0, img->inner->height,
		0, img->inner->height,
	};
	// clang-format on

	_gl_compose(base, img, fbo, coord, (GLuint[]){0, 1, 2, 2, 3, 0}, 1);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);

	img->inner->refcount--;
	img->inner = new_tex;

	// Clear lazy operation flags
	img->color_inverted = false;
	img->dim = 0;
	img->opacity = 1;
}

static void gl_image_apply_alpha(backend_t *base, struct gl_image *img,
                                 const region_t *reg_op, double alpha) {
	// Result color = 0 (GL_ZERO) + alpha (GL_CONSTANT_ALPHA) * original color
	glBlendFunc(GL_ZERO, GL_CONSTANT_ALPHA);
	glBlendColor(0, 0, 0, (GLclampf)alpha);
	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       img->inner->texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	_gl_fill(base, (struct color){0, 0, 0, 0}, reg_op, fbo, 0, false);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
}

void gl_present(backend_t *base, const region_t *region) {
	auto gd = (struct gl_data *)base;

	int nrects;
	const rect_t *rect = pixman_region32_rectangles((region_t *)region, &nrects);
	auto coord = ccalloc(nrects * 8, GLint);
	auto indices = ccalloc(nrects * 6, GLuint);
	for (int i = 0; i < nrects; i++) {
		// clang-format off
		memcpy(&coord[i * 8],
		       (GLint[]){rect[i].x1, gd->height - rect[i].y2,
				 rect[i].x2, gd->height - rect[i].y2,
				 rect[i].x2, gd->height - rect[i].y1,
				 rect[i].x1, gd->height - rect[i].y1},
		       sizeof(GLint) * 8);
		// clang-format on

		GLuint u = (GLuint)(i * 4);
		memcpy(&indices[i * 6], (GLuint[]){u + 0, u + 1, u + 2, u + 2, u + 3, u + 0},
		       sizeof(GLuint) * 6);
	}

	glViewport(0, 0, gd->width, gd->height);
	glUseProgram(gd->present_prog);
	glBindTexture(GL_TEXTURE_2D, gd->back_texture);

	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint bo[2];
	glGenBuffers(2, bo);
	glEnableVertexAttribArray(vert_coord_loc);
	glBindBuffer(GL_ARRAY_BUFFER, bo[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bo[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(GLint) * nrects * 8, coord, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(GLuint) * nrects * 6, indices,
	             GL_STREAM_DRAW);

	glVertexAttribPointer(vert_coord_loc, 2, GL_INT, GL_FALSE, sizeof(GLint) * 2, NULL);
	glDrawElements(GL_TRIANGLES, nrects * 6, GL_UNSIGNED_INT, NULL);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	glDeleteBuffers(2, bo);
	glDeleteVertexArrays(1, &vao);

	free(coord);
	free(indices);
}

/// stub for backend_operations::image_op
bool gl_image_op(backend_t *base, enum image_operations op, void *image_data,
                 const region_t *reg_op, const region_t *reg_visible attr_unused, void *arg) {
	struct gl_image *tex = image_data;
	int *iargs = arg;
	switch (op) {
	case IMAGE_OP_INVERT_COLOR_ALL: tex->color_inverted = true; break;
	case IMAGE_OP_DIM_ALL:
		tex->dim = 1.0 - (1.0 - tex->dim) * (1.0 - *(double *)arg);
		break;
	case IMAGE_OP_APPLY_ALPHA_ALL: tex->opacity *= *(double *)arg; break;
	case IMAGE_OP_APPLY_ALPHA:
		gl_image_decouple(base, tex);
		assert(tex->inner->refcount == 1);
		gl_image_apply_alpha(base, tex, reg_op, *(double *)arg);
		break;
	case IMAGE_OP_RESIZE_TILE:
		// texture is already set to repeat, so nothing else we need to do
		tex->ewidth = iargs[0];
		tex->eheight = iargs[1];
		break;
	case IMAGE_OP_MAX_BRIGHTNESS: tex->max_brightness = *(double *)arg; break;
	}

	return true;
}

bool gl_is_image_transparent(backend_t *base attr_unused, void *image_data) {
	struct gl_image *img = image_data;
	return img->has_alpha;
}
