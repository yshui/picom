// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>
#include <GL/gl.h>
#include <GL/glext.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <xcb/render.h>        // for xcb_render_fixed_t, XXX

#include "common.h"
#include "compiler.h"
#include "config.h"
#include "kernel.h"
#include "log.h"
#include "region.h"
#include "string_utils.h"
#include "utils.h"

#include "backend/gl/gl_common.h"

#define P_PAINTREG_START(reg_tgt, var)                                                   \
	do {                                                                             \
		region_t reg_new;                                                        \
		int nrects;                                                              \
		const rect_t *rects;                                                     \
		pixman_region32_init_rect(&reg_new, dx, dy, width, height);              \
		pixman_region32_intersect(&reg_new, &reg_new, (region_t *)reg_tgt);      \
		rects = pixman_region32_rectangles(&reg_new, &nrects);                   \
		glBegin(GL_QUADS);                                                       \
                                                                                         \
		for (int ri = 0; ri < nrects; ++ri) {                                    \
			rect_t var = rects[ri];

#define P_PAINTREG_END()                                                                 \
	}                                                                                \
	glEnd();                                                                         \
	pixman_region32_fini(&reg_new);                                                  \
	}                                                                                \
	while (0)

#define GLSL(version, ...) "#version " #version "\n" #__VA_ARGS__
#define QUOTE(...) #__VA_ARGS__

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
		unsigned int count = 0;
		if (vert_shader)
			shaders[count++] = vert_shader;
		if (frag_shader)
			shaders[count++] = frag_shader;
		assert(count <= sizeof(shaders) / sizeof(shaders[0]));
		if (count)
			prog = gl_create_program(shaders, count);
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
	pprogram->unifm_opacity = -1;
	pprogram->unifm_invert_color = -1;
	pprogram->unifm_tex = -1;
}

/**
 * Render a region with texture data.
 *
 * @param ptex the texture
 * @param dst_x,dst_y the top left corner of region where this texture
 *                    should go. In Xorg coordinate system (important!).
 * @param reg_tgt     the clip region, also in Xorg coordinate system
 * @param reg_visible ignored
 */
void gl_compose(backend_t *base, void *image_data, int dst_x, int dst_y,
                const region_t *reg_tgt, const region_t *reg_visible) {

	gl_texture_t *ptex = image_data;
	struct gl_data *gd = (void *)base;

	// Until we start to use glClipControl, reg_tgt, dst_x and dst_y and
	// in a different coordinate system than the one OpenGL uses.
	// OpenGL window coordinate (or NDC) has the origin at the lower left of the
	// screen, with y axis pointing up; Xorg has the origin at the upper left of the
	// screen, with y axis pointing down. We have to do some coordinate conversion in
	// this function
	if (!ptex || !ptex->texture) {
		log_error("Missing texture.");
		return;
	}

	// dst_y is the top coordinate, in OpenGL, it is the upper bound of the y
	// coordinate.
	dst_y = gd->height - dst_y;
	auto dst_y2 = dst_y - ptex->height;

	bool dual_texture = false;

	// It's required by legacy versions of OpenGL to enable texture target
	// before specifying environment. Thanks to madsy for telling me.
	glEnable(ptex->target);
	if (gd->win_shader.prog) {
		glUseProgram(gd->win_shader.prog);
		if (gd->win_shader.unifm_opacity >= 0)
			glUniform1f(gd->win_shader.unifm_opacity, ptex->opacity);
		if (gd->win_shader.unifm_invert_color >= 0)
			glUniform1i(gd->win_shader.unifm_invert_color, ptex->color_inverted);
		if (gd->win_shader.unifm_tex >= 0)
			glUniform1i(gd->win_shader.unifm_tex, 0);
	}

	// log_trace("Draw: %d, %d, %d, %d -> %d, %d (%d, %d) z %d\n",
	//          x, y, width, height, dx, dy, ptex->width, ptex->height, z);

	// Bind texture
	glBindTexture(ptex->target, ptex->texture);
	if (dual_texture) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(ptex->target, ptex->texture);
		glActiveTexture(GL_TEXTURE0);
	}

	// Painting
	int nrects;

	const rect_t *rects;
	rects = pixman_region32_rectangles((region_t *)reg_tgt, &nrects);

	glBegin(GL_QUADS);
	for (int ri = 0; ri < nrects; ++ri) {
		// Y-flip. Note after this, crect.y1 > crect.y2
		rect_t crect = rects[ri];
		crect.y1 = gd->height - crect.y1;
		crect.y2 = gd->height - crect.y2;

		// Calculate texture coordinates
		// (texture_x1, texture_y1), texture coord for the _bottom left_ corner
		GLfloat texture_x1 = crect.x1 - dst_x;
		GLfloat texture_y1 = crect.y2 - dst_y2;
		GLfloat texture_x2 = texture_x1 + crect.x2 - crect.x1;
		GLfloat texture_y2 = texture_y1 + crect.y1 - crect.y2;

		// X pixmaps might be Y inverted, invert the texture coordinates
		if (ptex->y_inverted) {
			texture_y1 = ptex->height - texture_y1;
			texture_y2 = ptex->height - texture_y2;
		}

		if (ptex->target == GL_TEXTURE_2D) {
			// GL_TEXTURE_2D coordinates are normalized
			texture_x1 /= ptex->width;
			texture_y1 /= ptex->height;
			texture_x2 /= ptex->width;
			texture_y2 /= ptex->height;
		}

		// Vertex coordinates
		GLint vx1 = crect.x1;
		GLint vy1 = crect.y2;
		GLint vx2 = crect.x2;
		GLint vy2 = crect.y1;

		// log_trace("Rect %d: %f, %f, %f, %f -> %d, %d, %d, %d",
		//          ri, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);

		GLfloat texture_x[] = {texture_x1, texture_x2, texture_x2, texture_x1};
		GLfloat texture_y[] = {texture_y1, texture_y1, texture_y2, texture_y2};
		GLint vx[] = {vx1, vx2, vx2, vx1};
		GLint vy[] = {vy1, vy1, vy2, vy2};

		for (int i = 0; i < 4; i++) {
			glTexCoord2f(texture_x[i], texture_y[i]);
			glVertex3i(vx[i], vy[i], 0);
		}
	}

	glEnd();

	// Cleanup
	glBindTexture(ptex->target, 0);
	glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
	glDisable(GL_COLOR_LOGIC_OP);
	glDisable(ptex->target);

	if (dual_texture) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(ptex->target, 0);
		glDisable(ptex->target);
		glActiveTexture(GL_TEXTURE0);
	}

	glUseProgram(0);

	gl_check_err();

	return;
}

bool gl_dim_reg(session_t *ps, int dx, int dy, int width, int height, float z,
                GLfloat factor, const region_t *reg_tgt) {
	glColor4f(0.0f, 0.0f, 0.0f, factor);
	{
		P_PAINTREG_START(reg_tgt, crect) {
			glVertex3i(crect.x1, crect.y1, z);
			glVertex3i(crect.x2, crect.y1, z);
			glVertex3i(crect.x2, crect.y2, z);
			glVertex3i(crect.x1, crect.y2, z);
		}
		P_PAINTREG_END();
	}

	glColor4f(0.0f, 0.0f, 0.0f, 0.0f);

	gl_check_err();

	return true;
}

/**
 * Blur contents in a particular region.
 */
bool gl_blur(backend_t *base, double opacity, const region_t *reg_blur,
             const region_t *reg_visible) {
	// Remainder: regions are in Xorg coordinates
	struct gl_data *gd = (void *)base;
	const rect_t *extent = pixman_region32_extents((region_t *)reg_blur);
	int width = extent->x2 - extent->x1, height = extent->y2 - extent->y1;
	int dst_y = gd->height - extent->y2;
	if (width == 0 || height == 0) {
		return true;
	}

	// these should be arguments
	bool ret = false;

	int curr = 0;
	glReadBuffer(GL_BACK);
	glEnable(gd->blur_texture_target);
	glBindTexture(gd->blur_texture_target, gd->blur_texture[0]);
	// Copy the area to be blurred into tmp buffer
	glCopyTexSubImage2D(gd->blur_texture_target, 0, 0, 0, extent->x1, dst_y, width, height);

	for (int i = 0; i < gd->npasses; ++i) {
		assert(i < MAX_BLUR_PASS - 1);
		const gl_blur_shader_t *p = &gd->blur_shader[i];
		assert(p->prog);

		assert(gd->blur_texture[curr]);
		glBindTexture(gd->blur_texture_target, gd->blur_texture[curr]);

		if (i < gd->npasses - 1) {
			// not last pass, draw into framebuffer
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gd->blur_fbo);

			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			                       gd->blur_texture_target,
			                       gd->blur_texture[!curr], 0);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				log_error("Framebuffer attachment failed.");
				goto end;
			}
		} else {
			// last pass, draw directly into the back buffer
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDrawBuffer(GL_BACK);
		}

		glUseProgram(p->prog);
		if (gd->blur_texture_target == GL_TEXTURE_2D) {
			glUniform1f(p->unifm_offset_x, 1.0 / gd->width);
			glUniform1f(p->unifm_offset_y, 1.0 / gd->height);
		} else {
			glUniform1f(p->unifm_offset_x, 1.0);
			glUniform1f(p->unifm_offset_y, 1.0);
		}

		// XXX use multiple draw calls is probably going to be slow than
		//     just simply blur the whole area.

		int nrects;
		const rect_t *rect =
		    pixman_region32_rectangles((region_t *)reg_blur, &nrects);
		dump_region(reg_blur);
		glBegin(GL_QUADS);
		for (int j = 0; j < nrects; j++) {
			rect_t crect = rect[j];
			// flip y axis, because the regions are in Xorg's coordinates,
			// which is y-flipped from OpenGL's.
			crect.y1 = gd->height - crect.y1;
			crect.y2 = gd->height - crect.y2;

			// Texture coordinates
			GLfloat texture_x1 = (crect.x1 - extent->x1);
			GLfloat texture_y1 = (crect.y2 - dst_y);
			GLfloat texture_x2 = texture_x1 + (crect.x2 - crect.x1);
			GLfloat texture_y2 = texture_y1 + (crect.y1 - crect.y2);

			if (gd->blur_texture_target == GL_TEXTURE_2D) {
				texture_x1 /= gd->width;
				texture_x2 /= gd->width;
				texture_y1 /= gd->height;
				texture_y2 /= gd->height;
			}

			// Vertex coordinates
			// For passes before the last one, we are drawing into a buffer,
			// so (dx, dy) from source maps to (0, 0)
			GLfloat vx1 = crect.x1 - extent->x1;
			GLfloat vy1 = crect.y2 - dst_y;
			if (i == gd->npasses - 1) {
				// For last pass, we are drawing back to source, so we
				// don't need to map
				vx1 = crect.x1;
				vy1 = crect.y2;
			}
			GLfloat vx2 = vx1 + (crect.x2 - crect.x1);
			GLfloat vy2 = vy1 + (crect.y1 - crect.y2);

			GLfloat texture_x[] = {texture_x1, texture_x2, texture_x2, texture_x1};
			GLfloat texture_y[] = {texture_y1, texture_y1, texture_y2, texture_y2};
			GLint vx[] = {vx1, vx2, vx2, vx1};
			GLint vy[] = {vy1, vy1, vy2, vy2};

			for (int k = 0; k < 4; k++) {
				glTexCoord2f(texture_x[k], texture_y[k]);
				glVertex3i(vx[k], vy[k], 0);
			}
		}
		glEnd();

		glUseProgram(0);
		curr = !curr;
	}

	ret = true;

end:
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(gd->blur_texture_target, 0);
	glDisable(gd->blur_texture_target);

	gl_check_err();

	return ret;
}

static GLuint glGetUniformLocationChecked(GLuint p, const char *name) {
	auto ret = glGetUniformLocation(p, name);
	if (ret < 0) {
		log_error("Failed to get location of uniform '%s'. compton might not "
		          "work correctly.",
		          name);
		return 0;
	}
	return ret;
}

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

	gl_check_err();

	return true;
}

/**
 * Callback to run on root window size change.
 */
void gl_resize(struct gl_data *gd, int width, int height) {
	glViewport(0, 0, width, height);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, width, 0, height, -1000.0, 1000.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	gd->height = height;
	gd->width = width;

	// Resize the temporary textures
	glBindTexture(gd->blur_texture_target, gd->blur_texture[0]);
	glTexImage2D(gd->blur_texture_target, 0, GL_RGBA8, gd->width, gd->height, 0,
	             GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	if (gd->npasses > 1) {
		glBindTexture(gd->blur_texture_target, gd->blur_texture[1]);
		glTexImage2D(gd->blur_texture_target, 0, GL_RGBA8, gd->width, gd->height,
		             0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	}
}

void gl_fill(backend_t *base, double r, double g, double b, double a, const region_t *clip) {
	int nrects;
	const rect_t *rect = pixman_region32_rectangles((region_t *)clip, &nrects);
	struct gl_data *gd = (void *)base;
	glColor4f(r, g, b, a);
	glBegin(GL_QUADS);
	for (int i = 0; i < nrects; i++) {
		glVertex2f(rect[i].x1, gd->height - rect[i].y2);
		glVertex2f(rect[i].x2, gd->height - rect[i].y2);
		glVertex2f(rect[i].x2, gd->height - rect[i].y1);
		glVertex2f(rect[i].x1, gd->height - rect[i].y1);
	}
	glEnd();
}

/**
 * Initialize GL blur filters.
 */
static bool gl_init_blur(struct gl_data *gd, conv *const *const kernels) {
	if (!kernels[0]) {
		return true;
	}

	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
	// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
	// Thanks to hiciu for reporting.
	setlocale(LC_NUMERIC, "C");

	// clang-format off
	static const char *FRAG_SHADER_BLUR = GLSL(130,
		%s\n // other extension pragmas
		uniform float offset_x;
		uniform float offset_y;
		uniform %s tex_scr;
		out vec4 out_color;
		void main() {
			vec4 sum = vec4(0.0, 0.0, 0.0, 0.0);
			%s //body of the convolution
			out_color = sum / float(%.7g);
		}
	);
	static const char *FRAG_SHADER_BLUR_ADD = QUOTE(
		sum += float(%.7g) *
		       %s(tex_scr, vec2(gl_TexCoord[0].x + offset_x * float(%d),
		                        gl_TexCoord[0].y + offset_y * float(%d)));
	);
	// clang-format on

	const bool use_texture_rect = !gd->non_power_of_two_texture;
	const char *sampler_type = (use_texture_rect ? "sampler2DRect" : "sampler2D");
	const char *texture_func = (use_texture_rect ? "texture2DRect" : "texture2D");
	const char *shader_add = FRAG_SHADER_BLUR_ADD;
	char *extension = strdup("");
	if (use_texture_rect) {
		mstrextend(&extension, "#extension GL_ARB_texture_rectangle : require\n");
	}

	gl_blur_shader_t *passes = gd->blur_shader;
	for (int i = 0; i < MAX_BLUR_PASS && kernels[i]; gd->npasses = ++i) {
		auto kern = kernels[i];
		// Build shader
		int width = kern->w, height = kern->h;
		int nele = width * height - 1;
		size_t body_len = (strlen(shader_add) + strlen(texture_func) + 42) * nele;
		char *shader_body = ccalloc(body_len, char);
		char *pc = shader_body;

		double sum = 0.0;
		for (int j = 0; j < height; ++j) {
			for (int k = 0; k < width; ++k) {
				double val;
				if (height / 2 == j && width / 2 == k) {
					val = 1;
				} else {
					val = kern->data[j * width + k];
				}
				if (val == 0) {
					continue;
				}
				sum += val;
				pc += snprintf(pc, body_len - (pc - shader_body),
				               FRAG_SHADER_BLUR_ADD, val, texture_func,
				               k - width / 2, j - height / 2);
				assert(pc < shader_body + body_len);
			}
		}

		auto pass = passes + i;
		size_t shader_len = strlen(FRAG_SHADER_BLUR) + strlen(extension) +
		                    strlen(sampler_type) + strlen(shader_body) +
		                    10 /* sum */ + 1 /* null terminator */;
		char *shader_str = ccalloc(shader_len, char);
		size_t real_shader_len = snprintf(shader_str, shader_len, FRAG_SHADER_BLUR,
		                                  extension, sampler_type, shader_body, sum);
		assert(real_shader_len < shader_len);
		free(shader_body);

		// Build program
		pass->prog = gl_create_program_from_str(NULL, shader_str);
		free(shader_str);
		if (!pass->prog) {
			log_error("Failed to create GLSL program.");
			goto err;
		}
		glBindFragDataLocation(pass->prog, 0, "out_color");

		// Get uniform addresses
		pass->unifm_offset_x =
		    glGetUniformLocationChecked(pass->prog, "offset_x");
		pass->unifm_offset_y =
		    glGetUniformLocationChecked(pass->prog, "offset_y");
	}
	free(extension);

	// Generate FBO and textures if needed
	gd->blur_texture_target = GL_TEXTURE_RECTANGLE;
	if (gd->non_power_of_two_texture) {
		gd->blur_texture_target = GL_TEXTURE_2D;
	}

	// Texture size will be defined by gl_resize
	glGenTextures(gd->npasses > 1 ? 2 : 1, gd->blur_texture);
	glBindTexture(gd->blur_texture_target, gd->blur_texture[0]);
	glTexParameteri(gd->blur_texture_target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(gd->blur_texture_target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	if (gd->npasses > 1) {
		glBindTexture(gd->blur_texture_target, gd->blur_texture[1]);
		glTexParameteri(gd->blur_texture_target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(gd->blur_texture_target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glGenFramebuffers(1, &gd->blur_fbo);
		if (!gd->blur_fbo) {
			log_error("Failed to generate framebuffer object for blur");
			return false;
		}
	}

	// Restore LC_NUMERIC
	setlocale(LC_NUMERIC, lc_numeric_old);
	free(lc_numeric_old);

	gl_check_err();

	return true;
err:
	free(extension);
	setlocale(LC_NUMERIC, lc_numeric_old);
	free(lc_numeric_old);
	return false;
}

// clang-format off
const char *win_shader_glsl = GLSL(110,
	uniform float opacity;
	uniform bool invert_color;
	uniform sampler2D tex;

	void main() {
		vec4 c = texture2D(tex, gl_TexCoord[0].xy);
		if (invert_color)
			c = vec4(c.aaa - c.rgb, c.a);
		c *= opacity;
		gl_FragColor = c;
	}
);
// clang-format on

bool gl_init(struct gl_data *gd, session_t *ps) {
	// Initialize GLX data structure
	for (int i = 0; i < MAX_BLUR_PASS; ++i) {
		gd->blur_shader[i] = (gl_blur_shader_t){
		    .prog = 0,
		    .unifm_offset_x = -1,
		    .unifm_offset_y = -1,
		};
	}

	gd->non_power_of_two_texture =
	    gl_has_extension("GL_ARB_texture_non_power_of_two");

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

	gl_win_shader_from_string(NULL, win_shader_glsl, &gd->win_shader);
	if (!gl_init_blur(gd, ps->o.blur_kerns)) {
		return false;
	}

	// Set up the size of the viewport. We do this last because it expects the blur
	// textures are already set up.
	gl_resize(gd, ps->root_width, ps->root_height);

	return true;
}

static inline void gl_free_blur_shader(gl_blur_shader_t *shader) {
	if (shader->prog) {
		glDeleteProgram(shader->prog);
	}

	shader->prog = 0;
}

void gl_deinit(struct gl_data *gd) {
	// Free GLSL shaders/programs
	for (int i = 0; i < MAX_BLUR_PASS; ++i) {
		gl_free_blur_shader(&gd->blur_shader[i]);
	}

	gl_free_prog_main(&gd->win_shader);

	glDeleteTextures(gd->npasses > 1 ? 2 : 1, gd->blur_texture);
	if (gd->npasses > 1) {
		glDeleteFramebuffers(1, &gd->blur_fbo);
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

	glEnable(target);
	glBindTexture(target, texture);
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(target, 0);

	return texture;
}

/// stub for backend_operations::image_op
bool gl_image_op(backend_t *base, enum image_operations op, void *image_data,
                 const region_t *reg_op, const region_t *reg_visible, void *arg) {
	struct gl_texture *tex = image_data;
	int *iargs = arg;
	switch (op) {
	case IMAGE_OP_INVERT_COLOR_ALL: tex->color_inverted = true; break;
	case IMAGE_OP_DIM_ALL: log_warn("IMAGE_OP_DIM_ALL not implemented yet"); break;
	case IMAGE_OP_APPLY_ALPHA_ALL: tex->opacity *= *(double *)arg; break;
	case IMAGE_OP_APPLY_ALPHA:
		log_warn("IMAGE_OP_APPLY_ALPHA not implemented yet");
		break;
	case IMAGE_OP_RESIZE_TILE:
		// texture is already set to repeat, so nothing else we need to do
		tex->ewidth = iargs[0];
		tex->eheight = iargs[1];
		break;
	}

	return true;
}

bool gl_is_image_transparent(backend_t *base, void *image_data) {
	gl_texture_t *img = image_data;
	return img->has_alpha;
}
