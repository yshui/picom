#include <locale.h>
#include <stdbool.h>

#include "backend/backend_common.h"

#include "gl_common.h"

struct gl_blur_context {
	enum blur_method method;
	struct gl_shader *blur_shader;

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

	enum backend_image_format format;
};

// TODO(yshui) small optimization for kernel blur, if source and target are different,
// single pass blur can paint directly from source to target. Currently a temporary
// texture is always used.

/**
 * Blur contents in a particular region.
 */
static bool gl_kernel_blur(double opacity, struct gl_blur_context *bctx,
                           const struct backend_mask_image *mask, const GLuint vao[2],
                           const int vao_nelems[2], struct gl_texture *source,
                           GLuint blur_sampler, GLuint target_fbo, GLuint default_mask) {
	int curr = 0;
	for (int i = 0; i < bctx->npasses; ++i) {
		auto p = &bctx->blur_shader[i];
		assert(p->prog);

		assert(bctx->blur_textures[curr]);

		// The origin to use when sampling from the source texture
		GLint tex_width, tex_height;
		GLuint src_texture;

		if (i == 0) {
			src_texture = source->texture;
			tex_width = source->width;
			tex_height = source->height;
		} else {
			src_texture = bctx->blur_textures[curr];
			auto src_size = bctx->texture_sizes[curr];
			tex_width = src_size.width;
			tex_height = src_size.height;
		}

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src_texture);
		glBindSampler(0, blur_sampler);
		glUseProgram(p->prog);
		if (p->uniform_bitmask & (1 << UNIFORM_PIXEL_NORM_LOC)) {
			// If the last pass is a trivial blend pass, it will not have
			// pixel_norm.
			glUniform2f(UNIFORM_PIXEL_NORM_LOC, 1.0F / (GLfloat)tex_width,
			            1.0F / (GLfloat)tex_height);
		}

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, default_mask);

		glUniform1i(UNIFORM_MASK_TEX_LOC, 1);
		glUniform2f(UNIFORM_MASK_OFFSET_LOC, 0.0F, 0.0F);
		glUniform1i(UNIFORM_MASK_INVERTED_LOC, 0);
		glUniform1f(UNIFORM_MASK_CORNER_RADIUS_LOC, 0.0F);

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

			glUniform1f(UNIFORM_OPACITY_LOC, 1.0F);
		} else {
			// last pass, draw directly into the back buffer, with origin
			// regions. And apply mask if requested
			if (mask != NULL) {
				auto inner = (struct gl_texture *)mask->image;
				log_trace("Mask texture is %d", inner->texture);
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, inner->texture);
				glUniform1i(UNIFORM_MASK_INVERTED_LOC, mask->inverted);
				glUniform1f(UNIFORM_MASK_CORNER_RADIUS_LOC,
				            (float)mask->corner_radius);
				glUniform2f(UNIFORM_MASK_OFFSET_LOC, (float)(mask->origin.x),
				            (float)(mask->origin.y));
			}
			glBindVertexArray(vao[0]);
			nelems = vao_nelems[0];
			glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);

			glUniform1f(UNIFORM_OPACITY_LOC, (float)opacity);
		}

		glDrawElements(GL_TRIANGLES, nelems, GL_UNSIGNED_INT, NULL);

		// XXX use multiple draw calls is probably going to be slow than
		//     just simply blur the whole area.

		curr = !curr;
	}

	return true;
}

/// Do dual-kawase blur.
///
/// @param vao two vertex array objects.
///            [0]: for sampling from blurred result into the target fbo.
///            [1]: for sampling from the source texture into blurred textures.
bool gl_dual_kawase_blur(double opacity, struct gl_blur_context *bctx,
                         const struct backend_mask_image *mask, const GLuint vao[2],
                         const int vao_nelems[2], struct gl_texture *source,
                         GLuint blur_sampler, GLuint target_fbo, GLuint default_mask) {
	int iterations = bctx->blur_texture_count;
	int scale_factor = 1;

	// Kawase downsample pass
	auto down_pass = &bctx->blur_shader[0];
	assert(down_pass->prog);
	glUseProgram(down_pass->prog);

	glBindVertexArray(vao[1]);
	int nelems = vao_nelems[1];

	for (int i = 0; i < iterations; ++i) {
		// Scale output width / height by half in each iteration
		scale_factor <<= 1;

		GLuint src_texture;
		int tex_width, tex_height;

		if (i == 0) {
			// first pass: copy from back buffer
			src_texture = source->texture;
			tex_width = source->width;
			tex_height = source->height;
		} else {
			// copy from previous pass
			src_texture = bctx->blur_textures[i - 1];
			auto src_size = bctx->texture_sizes[i - 1];
			tex_width = src_size.width;
			tex_height = src_size.height;
		}

		assert(src_texture);
		assert(bctx->blur_fbos[i]);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src_texture);
		glBindSampler(0, blur_sampler);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bctx->blur_fbos[i]);
		glDrawBuffer(GL_COLOR_ATTACHMENT0);

		glUniform1f(UNIFORM_SCALE_LOC, (GLfloat)scale_factor);

		glUniform2f(UNIFORM_PIXEL_NORM_LOC, 1.0F / (GLfloat)tex_width,
		            1.0F / (GLfloat)tex_height);

		glDrawElements(GL_TRIANGLES, nelems, GL_UNSIGNED_INT, NULL);
	}

	// Kawase upsample pass
	auto up_pass = &bctx->blur_shader[1];
	assert(up_pass->prog);
	glUseProgram(up_pass->prog);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, default_mask);

	glUniform1i(UNIFORM_MASK_TEX_LOC, 1);
	glUniform2f(UNIFORM_MASK_OFFSET_LOC, 0.0F, 0.0F);
	glUniform1i(UNIFORM_MASK_INVERTED_LOC, 0);
	glUniform1f(UNIFORM_MASK_CORNER_RADIUS_LOC, 0.0F);
	glUniform1f(UNIFORM_OPACITY_LOC, 1.0F);

	for (int i = iterations - 1; i >= 0; --i) {
		// Scale output width / height back by two in each iteration
		scale_factor >>= 1;

		const GLuint src_texture = bctx->blur_textures[i];
		assert(src_texture);

		// Calculate normalized half-width/-height of a src pixel
		auto src_size = bctx->texture_sizes[i];
		int tex_width = src_size.width;
		int tex_height = src_size.height;

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src_texture);
		glBindSampler(0, blur_sampler);

		if (i > 0) {
			assert(bctx->blur_fbos[i - 1]);

			// not last pass, draw into next framebuffer
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bctx->blur_fbos[i - 1]);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
		} else {
			// last pass, draw directly into the target fbo
			if (mask != NULL) {
				auto inner = (struct gl_texture *)mask->image;
				log_trace("Mask texture is %d", inner->texture);
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, inner->texture);
				glUniform1i(UNIFORM_MASK_INVERTED_LOC, mask->inverted);
				glUniform1f(UNIFORM_MASK_CORNER_RADIUS_LOC,
				            (float)mask->corner_radius);
				glUniform2f(UNIFORM_MASK_OFFSET_LOC, (float)(mask->origin.x),
				            (float)(mask->origin.y));
			}
			glBindVertexArray(vao[0]);
			nelems = vao_nelems[0];
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);

			glUniform1f(UNIFORM_OPACITY_LOC, (GLfloat)opacity);
		}

		glUniform1f(UNIFORM_SCALE_LOC, (GLfloat)scale_factor);
		glUniform2f(UNIFORM_PIXEL_NORM_LOC, 1.0F / (GLfloat)tex_width,
		            1.0F / (GLfloat)tex_height);

		glDrawElements(GL_TRIANGLES, nelems, GL_UNSIGNED_INT, NULL);
	}

	return true;
}

static bool
gl_blur_context_preallocate_textures(struct gl_blur_context *bctx, ivec2 source_size) {
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
			GLint format;
			switch (bctx->format) {
			case BACKEND_IMAGE_FORMAT_PIXMAP_HIGH: format = GL_RGBA16; break;
			case BACKEND_IMAGE_FORMAT_PIXMAP: format = GL_RGBA8; break;
			case BACKEND_IMAGE_FORMAT_MASK: format = GL_R8; break;
			default: unreachable();
			}
			glTexImage2D(GL_TEXTURE_2D, 0, format, tex_size->width,
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
	return true;
}

bool gl_blur(struct backend_base *base, ivec2 origin, image_handle target_,
             const struct backend_blur_args *args) {
	auto gd = (struct gl_data *)base;
	auto target = (struct gl_texture *)target_;
	auto source = (struct gl_texture *)args->source_image;
	auto bctx = (struct gl_blur_context *)args->blur_context;
	log_trace("Blur size: %dx%d, method: %d", source->width, source->height, bctx->method);
	bool ret = false;

	// Remainder: regions are in Xorg coordinates
	auto reg_blur_resized =
	    resize_region(args->target_mask, bctx->resize_width, bctx->resize_height);
	const rect_t *extent = pixman_region32_extents(args->target_mask);
	int width = extent->x2 - extent->x1, height = extent->y2 - extent->y1;
	if (width == 0 || height == 0) {
		return true;
	}

	int nrects, nrects_resized;
	const rect_t *rects = pixman_region32_rectangles(args->target_mask, &nrects),
	             *rects_resized =
	                 pixman_region32_rectangles(&reg_blur_resized, &nrects_resized);
	if (!nrects || !nrects_resized) {
		return true;
	}

	if (!gl_blur_context_preallocate_textures(
	        bctx, (ivec2){source->width, source->height})) {
		return false;
	}

	// Original region for the final compositing step from blur result to target.
	auto coord = ccalloc(nrects * 16, GLfloat);
	auto indices = ccalloc(nrects * 6, GLuint);
	gl_mask_rects_to_coords(origin, nrects, rects, SCALE_IDENTITY, coord, indices);
	if (!target->y_inverted) {
		gl_y_flip_target(nrects, coord, target->height);
	}

	// Resize region for sampling from source texture, and for blur passes
	auto coord_resized = ccalloc(nrects_resized * 16, GLfloat);
	auto indices_resized = ccalloc(nrects_resized * 6, GLuint);
	gl_mask_rects_to_coords(origin, nrects_resized, rects_resized, SCALE_IDENTITY,
	                        coord_resized, indices_resized);
	pixman_region32_fini(&reg_blur_resized);
	// FIXME(yshui) In theory we should handle blurring a non-y-inverted source, but
	// we never actually use that capability anywhere.
	assert(source->y_inverted);

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBindVertexArray(gd->vertex_array_objects[0]);
	glBindBuffer(GL_ARRAY_BUFFER, gd->buffer_objects[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gd->buffer_objects[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * nrects * 16, coord, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * nrects * 6,
	             indices, GL_STREAM_DRAW);
	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_FLOAT, GL_FALSE,
	                      sizeof(GLfloat) * 4, (void *)(sizeof(GLfloat) * 2));

	glBindVertexArray(gd->vertex_array_objects[1]);
	glBindBuffer(GL_ARRAY_BUFFER, gd->buffer_objects[2]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gd->buffer_objects[3]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord_resized) * nrects_resized * 16,
	             coord_resized, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
	             (long)sizeof(*indices_resized) * nrects_resized * 6, indices_resized,
	             GL_STREAM_DRAW);
	glEnableVertexAttribArray(vert_coord_loc);
	glEnableVertexAttribArray(vert_in_texcoord_loc);
	glVertexAttribPointer(vert_coord_loc, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 4, NULL);
	glVertexAttribPointer(vert_in_texcoord_loc, 2, GL_FLOAT, GL_FALSE,
	                      sizeof(GLfloat) * 4, (void *)(sizeof(GLfloat) * 2));

	int vao_nelems[2] = {nrects * 6, nrects_resized * 6};

	auto target_fbo = gl_bind_image_to_fbo(gd, (image_handle)target);
	if (bctx->method == BLUR_METHOD_DUAL_KAWASE) {
		ret = gl_dual_kawase_blur(args->opacity, bctx, args->source_mask,
		                          gd->vertex_array_objects, vao_nelems, source,
		                          gd->samplers[GL_SAMPLER_BLUR], target_fbo,
		                          gd->default_mask_texture);
	} else {
		ret = gl_kernel_blur(args->opacity, bctx, args->source_mask,
		                     gd->vertex_array_objects, vao_nelems, source,
		                     gd->samplers[GL_SAMPLER_BLUR], target_fbo,
		                     gd->default_mask_texture);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);

	// Invalidate buffer data
	glBindBuffer(GL_ARRAY_BUFFER, gd->buffer_objects[0]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gd->buffer_objects[1]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord) * nrects * 16, NULL, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)sizeof(*indices) * nrects * 6, NULL,
	             GL_STREAM_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, gd->buffer_objects[2]);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gd->buffer_objects[3]);
	glBufferData(GL_ARRAY_BUFFER, (long)sizeof(*coord_resized) * nrects_resized * 16,
	             NULL, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
	             (long)sizeof(*indices_resized) * nrects_resized * 6, NULL,
	             GL_STREAM_DRAW);

	// Cleanup vertex array states
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	glUseProgram(0);

	free(indices);
	free(coord);
	free(indices_resized);
	free(coord_resized);

	gl_check_err();
	return ret;
}

static inline void gl_free_blur_shader(struct gl_shader *shader) {
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

	ctx->blur_shader = ccalloc(max2(2, nkernels), struct gl_shader);

	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
	// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
	// Thanks to hiciu for reporting.
	setlocale(LC_NUMERIC, "C");

	// clang-format off
	static const char *FRAG_SHADER_BLUR = GLSL(330,
		%s\n // other extension pragmas
		layout(location = UNIFORM_TEX_SRC_LOC)
		uniform sampler2D tex_src;
		layout(location = UNIFORM_PIXEL_NORM_LOC)
		uniform vec2 pixel_norm;
		layout(location = UNIFORM_OPACITY_LOC)
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
		pass->uniform_bitmask = 1 << UNIFORM_PIXEL_NORM_LOC;
		glBindFragDataLocation(pass->prog, 0, "out_color");

		// Setup projection matrix
		glUseProgram(pass->prog);
		glUniformMatrix4fv(UNIFORM_PROJECTION_LOC, 1, false, projection);
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
		    (const char *[]){blend_with_mask_frag, masking_glsl, NULL});

		// Setup projection matrix
		glUseProgram(pass->prog);
		glUniformMatrix4fv(UNIFORM_PROJECTION_LOC, 1, false, projection);
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
	ctx->blur_shader = ccalloc(ctx->npasses, struct gl_shader);

	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
	// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
	// Thanks to hiciu for reporting.
	setlocale(LC_NUMERIC, "C");

	// Dual-kawase downsample shader / program
	auto down_pass = ctx->blur_shader;
	{
		// clang-format off
		static const char *FRAG_SHADER_DOWN = GLSL(330,
			layout(location = UNIFORM_TEX_SRC_LOC)
			uniform sampler2D tex_src;
			layout(location = UNIFORM_SCALE_LOC)
			uniform float scale = 1.0;
			layout(location = UNIFORM_PIXEL_NORM_LOC)
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

		// Setup projection matrix
		glUseProgram(down_pass->prog);
		glUniformMatrix4fv(UNIFORM_PROJECTION_LOC, 1, false, projection);
		glUseProgram(0);
	}

	// Dual-kawase upsample shader / program
	auto up_pass = ctx->blur_shader + 1;
	{
		// clang-format off
		static const char *FRAG_SHADER_UP = GLSL(330,
			layout(location = UNIFORM_TEX_SRC_LOC)
			uniform sampler2D tex_src;
			layout(location = UNIFORM_SCALE_LOC)
			uniform float scale = 1.0;
			layout(location = UNIFORM_PIXEL_NORM_LOC)
			uniform vec2 pixel_norm;
			layout(location = UNIFORM_OPACITY_LOC)
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

		// Setup projection matrix
		glUseProgram(up_pass->prog);
		glUniformMatrix4fv(UNIFORM_PROJECTION_LOC, 1, false, projection);
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

void *gl_create_blur_context(backend_t *base, enum blur_method method,
                             enum backend_image_format format, void *args) {
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
	ctx->format = format;
	glGenTextures(ctx->blur_texture_count, ctx->blur_textures);

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
