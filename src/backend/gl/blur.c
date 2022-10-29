#include <locale.h>
#include <stdbool.h>

#include <backend/backend.h>
#include <backend/backend_common.h>

#include "gl_common.h"

struct gl_blur_context {
	enum blur_method method;
	gl_blur_shader_t *blur_shader;

	/// Temporary textures used for blurring
	GLuint *blur_textures;
	int blur_texture_count;
	/// Temporary fbos used for blurring
	GLuint *blur_fbos;
	int blur_fbo_count;

	/// Cached dimensions of each blur_texture. They are the same size as the target,
	/// so they are always big enough without resizing.
	/// Turns out calling glTexImage to resize is expensive, so we avoid that.
	struct texture_size {
		int width;
		int height;
	} *texture_sizes;

	/// Cached dimensions of the offscreen framebuffer. It's the same size as the
	/// target but is expanded in either direction by resize_width / resize_height.
	int fb_width, fb_height;

	/// How much do we need to resize the damaged region for blurring.
	int resize_width, resize_height;

	int npasses;
};

/**
 * Blur contents in a particular region.
 */
bool gl_kernel_blur(double opacity, struct gl_blur_context *bctx, const rect_t *extent,
                    struct backend_image *mask, coord_t mask_dst, const GLuint vao[2],
                    const int vao_nelems[2], GLuint source_texture,
                    geometry_t source_size, GLuint target_fbo, GLuint default_mask) {
	int dst_y_fb_coord = bctx->fb_height - extent->y2;

	int curr = 0;
	for (int i = 0; i < bctx->npasses; ++i) {
		const gl_blur_shader_t *p = &bctx->blur_shader[i];
		assert(p->prog);

		assert(bctx->blur_textures[curr]);

		// The origin to use when sampling from the source texture
		GLint texorig_x = extent->x1, texorig_y = dst_y_fb_coord;
		GLint tex_width, tex_height;
		GLuint src_texture;

		if (i == 0) {
			src_texture = source_texture;
			tex_width = source_size.width;
			tex_height = source_size.height;
		} else {
			src_texture = bctx->blur_textures[curr];
			auto src_size = bctx->texture_sizes[curr];
			tex_width = src_size.width;
			tex_height = src_size.height;
		}

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src_texture);
		glUseProgram(p->prog);
		glUniform2f(p->uniform_pixel_norm, 1.0F / (GLfloat)tex_width,
		            1.0F / (GLfloat)tex_height);

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, default_mask);

		glUniform1i(p->uniform_mask_tex, 1);
		glUniform2f(p->uniform_mask_offset, 0.0F, 0.0F);
		glUniform1i(p->uniform_mask_inverted, 0);
		glUniform1f(p->uniform_mask_corner_radius, 0.0F);

		// The number of indices in the selected vertex array
		GLsizei nelems;

		if (i < bctx->npasses - 1) {
			assert(bctx->blur_fbos[0]);
			assert(bctx->blur_textures[!curr]);

			// not last pass, draw into framebuffer, with resized regions
			glBindVertexArray(vao[1]);
			nelems = vao_nelems[1];
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bctx->blur_fbos[0]);

			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			                       GL_TEXTURE_2D, bctx->blur_textures[!curr], 0);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
			if (!gl_check_fb_complete(GL_FRAMEBUFFER)) {
				return false;
			}

			glUniform1f(p->uniform_opacity, 1.0F);
		} else {
			// last pass, draw directly into the back buffer, with origin
			// regions. And apply mask if requested
			if (mask) {
				auto inner = (struct gl_texture *)mask->inner;
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, inner->texture);
				glUniform1i(p->uniform_mask_inverted, mask->color_inverted);
				glUniform1f(p->uniform_mask_corner_radius,
				            (float)mask->corner_radius);
				glUniform2f(
				    p->uniform_mask_offset, (float)(mask_dst.x),
				    (float)(bctx->fb_height - mask_dst.y - inner->height));
			}
			glBindVertexArray(vao[0]);
			nelems = vao_nelems[0];
			glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);

			glUniform1f(p->uniform_opacity, (float)opacity);
		}

		glUniform2f(p->texorig_loc, (GLfloat)texorig_x, (GLfloat)texorig_y);
		glDrawElements(GL_TRIANGLES, nelems, GL_UNSIGNED_INT, NULL);

		// XXX use multiple draw calls is probably going to be slow than
		//     just simply blur the whole area.

		curr = !curr;
	}

	return true;
}

bool gl_dual_kawase_blur(double opacity, struct gl_blur_context *bctx, const rect_t *extent,
                         struct backend_image *mask, coord_t mask_dst, const GLuint vao[2],
                         const int vao_nelems[2], GLuint source_texture,
                         geometry_t source_size, GLuint target_fbo, GLuint default_mask) {
	int dst_y_fb_coord = bctx->fb_height - extent->y2;

	int iterations = bctx->blur_texture_count;
	int scale_factor = 1;

	// Kawase downsample pass
	const gl_blur_shader_t *down_pass = &bctx->blur_shader[0];
	assert(down_pass->prog);
	glUseProgram(down_pass->prog);

	glUniform2f(down_pass->texorig_loc, (GLfloat)extent->x1, (GLfloat)dst_y_fb_coord);

	for (int i = 0; i < iterations; ++i) {
		// Scale output width / height by half in each iteration
		scale_factor <<= 1;

		GLuint src_texture;
		int tex_width, tex_height;

		if (i == 0) {
			// first pass: copy from back buffer
			src_texture = source_texture;
			tex_width = source_size.width;
			tex_height = source_size.height;
		} else {
			// copy from previous pass
			src_texture = bctx->blur_textures[i - 1];
			auto src_size = bctx->texture_sizes[i - 1];
			tex_width = src_size.width;
			tex_height = src_size.height;
		}

		assert(src_texture);
		assert(bctx->blur_fbos[i]);

		glBindTexture(GL_TEXTURE_2D, src_texture);
		glBindVertexArray(vao[1]);
		auto nelems = vao_nelems[1];
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bctx->blur_fbos[i]);
		glDrawBuffer(GL_COLOR_ATTACHMENT0);

		glUniform1f(down_pass->scale_loc, (GLfloat)scale_factor);

		glUniform2f(down_pass->uniform_pixel_norm, 1.0F / (GLfloat)tex_width,
		            1.0F / (GLfloat)tex_height);

		glDrawElements(GL_TRIANGLES, nelems, GL_UNSIGNED_INT, NULL);
	}

	// Kawase upsample pass
	const gl_blur_shader_t *up_pass = &bctx->blur_shader[1];
	assert(up_pass->prog);
	glUseProgram(up_pass->prog);

	glUniform2f(up_pass->texorig_loc, (GLfloat)extent->x1, (GLfloat)dst_y_fb_coord);

	for (int i = iterations - 1; i >= 0; --i) {
		// Scale output width / height back by two in each iteration
		scale_factor >>= 1;

		const GLuint src_texture = bctx->blur_textures[i];
		assert(src_texture);

		// Calculate normalized half-width/-height of a src pixel
		auto src_size = bctx->texture_sizes[i];
		int tex_width = src_size.width;
		int tex_height = src_size.height;

		// The number of indices in the selected vertex array
		GLsizei nelems;

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src_texture);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, default_mask);

		glUniform1i(up_pass->uniform_mask_tex, 1);
		glUniform2f(up_pass->uniform_mask_offset, 0.0F, 0.0F);
		glUniform1i(up_pass->uniform_mask_inverted, 0);
		glUniform1f(up_pass->uniform_mask_corner_radius, 0.0F);
		if (i > 0) {
			assert(bctx->blur_fbos[i - 1]);

			// not last pass, draw into next framebuffer
			glBindVertexArray(vao[1]);
			nelems = vao_nelems[1];
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bctx->blur_fbos[i - 1]);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);

			glUniform1f(up_pass->uniform_opacity, (GLfloat)1);
		} else {
			// last pass, draw directly into the back buffer
			if (mask) {
				auto inner = (struct gl_texture *)mask->inner;
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, inner->texture);
				glUniform1i(up_pass->uniform_mask_inverted,
				            mask->color_inverted);
				glUniform1f(up_pass->uniform_mask_corner_radius,
				            (float)mask->corner_radius);
				glUniform2f(
				    up_pass->uniform_mask_offset, (float)(mask_dst.x),
				    (float)(bctx->fb_height - mask_dst.y - inner->height));
			}
			glBindVertexArray(vao[0]);
			nelems = vao_nelems[0];
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);

			glUniform1f(up_pass->uniform_opacity, (GLfloat)opacity);
		}

		glUniform1f(up_pass->scale_loc, (GLfloat)scale_factor);
		glUniform2f(up_pass->uniform_pixel_norm, 1.0F / (GLfloat)tex_width,
		            1.0F / (GLfloat)tex_height);

		glDrawElements(GL_TRIANGLES, nelems, GL_UNSIGNED_INT, NULL);
	}

	return true;
}

bool gl_blur_impl(double opacity, struct gl_blur_context *bctx, void *mask,
                  coord_t mask_dst, const region_t *reg_blur,
                  const region_t *reg_visible attr_unused, GLuint source_texture,
                  geometry_t source_size, GLuint target_fbo, GLuint default_mask) {
	bool ret = false;

	if (source_size.width != bctx->fb_width || source_size.height != bctx->fb_height) {
		// Resize the temporary textures used for blur in case the root
		// size changed
		bctx->fb_width = source_size.width;
		bctx->fb_height = source_size.height;

		for (int i = 0; i < bctx->blur_texture_count; ++i) {
			auto tex_size = bctx->texture_sizes + i;
			if (bctx->method == BLUR_METHOD_DUAL_KAWASE) {
				// Use smaller textures for each iteration (quarter of the
				// previous texture)
				tex_size->width = 1 + ((bctx->fb_width - 1) >> (i + 1));
				tex_size->height = 1 + ((bctx->fb_height - 1) >> (i + 1));
			} else {
				tex_size->width = bctx->fb_width;
				tex_size->height = bctx->fb_height;
			}

			glBindTexture(GL_TEXTURE_2D, bctx->blur_textures[i]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_size->width,
			             tex_size->height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

			if (bctx->method == BLUR_METHOD_DUAL_KAWASE) {
				// Attach texture to FBO target
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bctx->blur_fbos[i]);
				glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
				                       GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
				                       bctx->blur_textures[i], 0);
				if (!gl_check_fb_complete(GL_FRAMEBUFFER)) {
					glBindFramebuffer(GL_FRAMEBUFFER, 0);
					return false;
				}
			}
		}
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
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
	auto extent_height = extent_resized->y2 - extent_resized->y1;
	x_rect_to_coords(
	    nrects, rects, (coord_t){.x = extent_resized->x1, .y = extent_resized->y1},
	    extent_height, bctx->fb_height, source_size.height, false, coord, indices);

	auto coord_resized = ccalloc(nrects_resized * 16, GLint);
	auto indices_resized = ccalloc(nrects_resized * 6, GLuint);
	x_rect_to_coords(nrects_resized, rects_resized,
	                 (coord_t){.x = extent_resized->x1, .y = extent_resized->y1},
	                 extent_height, bctx->fb_height, bctx->fb_height, false,
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

	int vao_nelems[2] = {nrects * 6, nrects_resized * 6};

	if (bctx->method == BLUR_METHOD_DUAL_KAWASE) {
		ret = gl_dual_kawase_blur(opacity, bctx, extent_resized, mask, mask_dst,
		                          vao, vao_nelems, source_texture, source_size,
		                          target_fbo, default_mask);
	} else {
		ret = gl_kernel_blur(opacity, bctx, extent_resized, mask, mask_dst, vao,
		                     vao_nelems, source_texture, source_size, target_fbo,
		                     default_mask);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
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

bool gl_blur(backend_t *base, double opacity, void *ctx, void *mask, coord_t mask_dst,
             const region_t *reg_blur, const region_t *reg_visible attr_unused) {
	auto gd = (struct gl_data *)base;
	auto bctx = (struct gl_blur_context *)ctx;
	return gl_blur_impl(opacity, bctx, mask, mask_dst, reg_blur, reg_visible,
	                    gd->back_texture,
	                    (geometry_t){.width = gd->width, .height = gd->height},
	                    gd->back_fbo, gd->default_mask_texture);
}

static inline void gl_free_blur_shader(gl_blur_shader_t *shader) {
	if (shader->prog) {
		glDeleteProgram(shader->prog);
	}

	shader->prog = 0;
}

void gl_destroy_blur_context(backend_t *base attr_unused, void *ctx) {
	auto bctx = (struct gl_blur_context *)ctx;
	// Free GLSL shaders/programs
	for (int i = 0; i < bctx->npasses; ++i) {
		gl_free_blur_shader(&bctx->blur_shader[i]);
	}
	free(bctx->blur_shader);

	if (bctx->blur_texture_count && bctx->blur_textures) {
		glDeleteTextures(bctx->blur_texture_count, bctx->blur_textures);
		free(bctx->blur_textures);
	}
	if (bctx->blur_texture_count && bctx->texture_sizes) {
		free(bctx->texture_sizes);
	}
	if (bctx->blur_fbo_count && bctx->blur_fbos) {
		glDeleteFramebuffers(bctx->blur_fbo_count, bctx->blur_fbos);
		free(bctx->blur_fbos);
	}

	bctx->blur_texture_count = 0;
	bctx->blur_fbo_count = 0;

	free(bctx);

	gl_check_err();
}

/**
 * Initialize GL blur filters.
 */
bool gl_create_kernel_blur_context(void *blur_context, GLfloat *projection,
                                   enum blur_method method, void *args) {
	bool success = false;
	auto ctx = (struct gl_blur_context *)blur_context;

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

	// Specify required textures and FBOs
	ctx->blur_texture_count = 2;
	ctx->blur_fbo_count = 1;

	ctx->blur_shader = ccalloc(max2(2, nkernels), gl_blur_shader_t);

	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
	// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
	// Thanks to hiciu for reporting.
	setlocale(LC_NUMERIC, "C");

	// clang-format off
	static const char *FRAG_SHADER_BLUR = GLSL(330,
		%s\n // other extension pragmas
		uniform sampler2D tex_src;
		uniform vec2 pixel_norm;
		uniform float opacity;
		in vec2 texcoord;
		out vec4 out_color;
		float mask_factor();
		void main() {
			vec2 uv = texcoord * pixel_norm;
			vec4 sum = vec4(0.0, 0.0, 0.0, 0.0);
			%s //body of the convolution
			out_color = sum / float(%.7g) * opacity * mask_factor();
		}
	);
	static const char *FRAG_SHADER_BLUR_ADD = QUOTE(
		sum += float(%.7g) * texture2D(tex_src, uv + pixel_norm * vec2(%.7g, %.7g));
	);
	// clang-format on

	const char *shader_add = FRAG_SHADER_BLUR_ADD;
	char *extension = strdup("");

	for (int i = 0; i < nkernels; i++) {
		auto kern = kernels[i];
		// Build shader
		int width = kern->w, height = kern->h;
		int nele = width * height;
		// '%.7g' is at most 14 characters, inserted 3 times
		size_t body_len = (strlen(shader_add) + 42) * (uint)nele;
		char *shader_body = ccalloc(body_len, char);
		char *pc = shader_body;

		// Make use of the linear interpolation hardware by sampling 2 pixels with
		// one texture access by sampling between both pixels based on their
		// relative weight. Easiest done in a single dimension as 2D bilinear
		// filtering would raise additional constraints on the kernels. Therefore
		// only use interpolation along the larger dimension.
		double sum = 0.0;
		if (width > height) {
			// use interpolation in x dimension (width)
			for (int j = 0; j < height; ++j) {
				for (int k = 0; k < width; k += 2) {
					double val1, val2;
					val1 = kern->data[j * width + k];
					val2 = (k + 1 < width)
					           ? kern->data[j * width + k + 1]
					           : 0;

					double combined_weight = val1 + val2;
					if (combined_weight == 0) {
						continue;
					}
					sum += combined_weight;

					double offset_x =
					    k + (val2 / combined_weight) - (width / 2);
					double offset_y = j - (height / 2);
					pc += snprintf(
					    pc, body_len - (ulong)(pc - shader_body),
					    shader_add, combined_weight, offset_x, offset_y);
					assert(pc < shader_body + body_len);
				}
			}
		} else {
			// use interpolation in y dimension (height)
			for (int j = 0; j < height; j += 2) {
				for (int k = 0; k < width; ++k) {
					double val1, val2;
					val1 = kern->data[j * width + k];
					val2 = (j + 1 < height)
					           ? kern->data[(j + 1) * width + k]
					           : 0;

					double combined_weight = val1 + val2;
					if (combined_weight == 0) {
						continue;
					}
					sum += combined_weight;

					double offset_x = k - (width / 2);
					double offset_y =
					    j + (val2 / combined_weight) - (height / 2);
					pc += snprintf(
					    pc, body_len - (ulong)(pc - shader_body),
					    shader_add, combined_weight, offset_x, offset_y);
					assert(pc < shader_body + body_len);
				}
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
		pass->prog = gl_create_program_from_strv(
		    (const char *[]){vertex_shader, NULL},
		    (const char *[]){shader_str, masking_glsl, NULL});
		free(shader_str);
		if (!pass->prog) {
			log_error("Failed to create GLSL program.");
			success = false;
			goto out;
		}
		glBindFragDataLocation(pass->prog, 0, "out_color");

		// Get uniform addresses
		bind_uniform(pass, pixel_norm);
		bind_uniform(pass, opacity);

		bind_uniform(pass, mask_tex);
		bind_uniform(pass, mask_offset);
		bind_uniform(pass, mask_inverted);
		bind_uniform(pass, mask_corner_radius);
		log_info("Uniform locations: %d %d %d %d %d", pass->uniform_mask_tex,
		         pass->uniform_mask_offset, pass->uniform_mask_inverted,
		         pass->uniform_mask_corner_radius, pass->uniform_opacity);
		pass->texorig_loc = glGetUniformLocationChecked(pass->prog, "texorig");

		// Setup projection matrix
		glUseProgram(pass->prog);
		int pml = glGetUniformLocationChecked(pass->prog, "projection");
		glUniformMatrix4fv(pml, 1, false, projection);
		glUseProgram(0);

		ctx->resize_width += kern->w / 2;
		ctx->resize_height += kern->h / 2;
	}

	if (nkernels == 1) {
		// Generate an extra null pass so we don't need special code path for
		// the single pass case
		auto pass = &ctx->blur_shader[1];
		pass->prog = gl_create_program_from_strv(
		    (const char *[]){vertex_shader, NULL},
		    (const char *[]){copy_with_mask_frag, masking_glsl, NULL});
		pass->uniform_pixel_norm = -1;
		pass->uniform_opacity = -1;
		pass->texorig_loc = glGetUniformLocationChecked(pass->prog, "texorig");
		bind_uniform(pass, mask_tex);
		bind_uniform(pass, mask_offset);
		bind_uniform(pass, mask_inverted);
		bind_uniform(pass, mask_corner_radius);

		// Setup projection matrix
		glUseProgram(pass->prog);
		int pml = glGetUniformLocationChecked(pass->prog, "projection");
		glUniformMatrix4fv(pml, 1, false, projection);
		glUseProgram(0);

		ctx->npasses = 2;
	} else {
		ctx->npasses = nkernels;
	}

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

bool gl_create_dual_kawase_blur_context(void *blur_context, GLfloat *projection,
                                        enum blur_method method, void *args) {
	bool success = false;
	auto ctx = (struct gl_blur_context *)blur_context;

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
			uniform float scale = 1.0;
			uniform vec2 pixel_norm;
			in vec2 texcoord;
			out vec4 out_color;
			void main() {
				vec2 offset = %.7g * pixel_norm;
				vec2 uv = texcoord * pixel_norm * (2.0 / scale);
				vec4 sum = texture2D(tex_src, uv) * 4.0;
				sum += texture2D(tex_src, uv - vec2(0.5, 0.5) * offset);
				sum += texture2D(tex_src, uv + vec2(0.5, 0.5) * offset);
				sum += texture2D(tex_src, uv + vec2(0.5, -0.5) * offset);
				sum += texture2D(tex_src, uv - vec2(0.5, -0.5) * offset);
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
		bind_uniform(down_pass, pixel_norm);
		down_pass->texorig_loc =
		    glGetUniformLocationChecked(down_pass->prog, "texorig");
		down_pass->scale_loc =
		    glGetUniformLocationChecked(down_pass->prog, "scale");

		// Setup projection matrix
		glUseProgram(down_pass->prog);
		int pml = glGetUniformLocationChecked(down_pass->prog, "projection");
		glUniformMatrix4fv(pml, 1, false, projection);
		glUseProgram(0);
	}

	// Dual-kawase upsample shader / program
	auto up_pass = ctx->blur_shader + 1;
	{
		// clang-format off
		static const char *FRAG_SHADER_UP = GLSL(330,
			uniform sampler2D tex_src;
			uniform float scale = 1.0;
			uniform vec2 pixel_norm;
			uniform float opacity;
			in vec2 texcoord;
			out vec4 out_color;
			float mask_factor();
			void main() {
				vec2 offset = %.7g * pixel_norm;
				vec2 uv = texcoord * pixel_norm / (2 * scale);
				vec4 sum = texture2D(tex_src, uv + vec2(-1.0, 0.0) * offset);
				sum += texture2D(tex_src, uv + vec2(-0.5, 0.5) * offset) * 2.0;
				sum += texture2D(tex_src, uv + vec2(0.0, 1.0) * offset);
				sum += texture2D(tex_src, uv + vec2(0.5, 0.5) * offset) * 2.0;
				sum += texture2D(tex_src, uv + vec2(1.0, 0.0) * offset);
				sum += texture2D(tex_src, uv + vec2(0.5, -0.5) * offset) * 2.0;
				sum += texture2D(tex_src, uv + vec2(0.0, -1.0) * offset);
				sum += texture2D(tex_src, uv + vec2(-0.5, -0.5) * offset) * 2.0;
				out_color = sum / 12.0 * opacity * mask_factor();
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
		up_pass->prog = gl_create_program_from_strv(
		    (const char *[]){vertex_shader, NULL},
		    (const char *[]){shader_str, masking_glsl, NULL});
		free(shader_str);
		if (!up_pass->prog) {
			log_error("Failed to create GLSL program.");
			success = false;
			goto out;
		}
		glBindFragDataLocation(up_pass->prog, 0, "out_color");

		// Get uniform addresses
		bind_uniform(up_pass, pixel_norm);
		bind_uniform(up_pass, opacity);

		bind_uniform(up_pass, mask_tex);
		bind_uniform(up_pass, mask_offset);
		bind_uniform(up_pass, mask_inverted);
		bind_uniform(up_pass, mask_corner_radius);

		up_pass->texorig_loc =
		    glGetUniformLocationChecked(up_pass->prog, "texorig");
		up_pass->scale_loc = glGetUniformLocationChecked(up_pass->prog, "scale");

		// Setup projection matrix
		glUseProgram(up_pass->prog);
		int pml = glGetUniformLocationChecked(up_pass->prog, "projection");
		glUniformMatrix4fv(pml, 1, false, projection);
		glUseProgram(0);
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

	// Set projection matrix to gl viewport dimensions so we can use screen
	// coordinates for all vertices
	// Note: OpenGL matrices are column major
	GLint viewport_dimensions[2];
	glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_dimensions);
	GLfloat projection_matrix[4][4] = {{2.0F / (GLfloat)viewport_dimensions[0], 0, 0, 0},
	                                   {0, 2.0F / (GLfloat)viewport_dimensions[1], 0, 0},
	                                   {0, 0, 0, 0},
	                                   {-1, -1, 0, 1}};

	if (method == BLUR_METHOD_DUAL_KAWASE) {
		success = gl_create_dual_kawase_blur_context(ctx, projection_matrix[0],
		                                             method, args);
	} else {
		success =
		    gl_create_kernel_blur_context(ctx, projection_matrix[0], method, args);
	}
	if (!success || ctx->method == BLUR_METHOD_NONE) {
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
			log_error("Failed to generate framebuffer objects for blur");
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
	auto ctx = (struct gl_blur_context *)blur_context;
	*width = ctx->resize_width;
	*height = ctx->resize_height;
}
