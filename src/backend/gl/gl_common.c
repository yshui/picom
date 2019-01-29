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

struct gl_data {};

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

void gl_free_prog_main(session_t *ps, gl_win_shader_t *pprogram) {
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
 * @brief Get tightly packed RGB888 data from GL front buffer.
 *
 * Don't expect any sort of decent performance.
 *
 * @returns tightly packed RGB888 data of the size of the screen,
 *          to be freed with `free()`
 */
unsigned char *gl_take_screenshot(session_t *ps, int *out_length) {
	int length = 3 * ps->root_width * ps->root_height;
	GLint unpack_align_old = 0;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_align_old);
	assert(unpack_align_old > 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	unsigned char *buf = ccalloc(length, unsigned char);
	glReadBuffer(GL_FRONT);
	glReadPixels(0, 0, ps->root_width, ps->root_height, GL_RGB, GL_UNSIGNED_BYTE, buf);
	glReadBuffer(GL_BACK);
	glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_align_old);
	if (out_length)
		*out_length = sizeof(unsigned char) * length;
	return buf;
}

/**
 * @brief Render a region with texture data.
 */
bool gl_compose(const gl_texture_t *ptex, int x, int y, int dx, int dy, int width,
                int height, int z, double opacity, bool argb, bool neg,
                const region_t *reg_tgt, const gl_win_shader_t *shader) {
	if (!ptex || !ptex->texture) {
		log_error("Missing texture.");
		return false;
	}

	// argb = argb || (GLX_TEXTURE_FORMAT_RGBA_EXT ==
	//    ps->psglx->fbconfigs[ptex->depth]->texture_fmt);
	bool dual_texture = false;

	// It's required by legacy versions of OpenGL to enable texture target
	// before specifying environment. Thanks to madsy for telling me.
	glEnable(ptex->target);

	// Enable blending if needed
	if (opacity < 1.0 || argb) {

		glEnable(GL_BLEND);

		// Needed for handling opacity of ARGB texture
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

		// This is all weird, but X Render is using premultiplied ARGB format, and
		// we need to use those things to correct it. Thanks to derhass for help.
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glColor4f(opacity, opacity, opacity, opacity);
	}

	// Programmable path
	if (shader->prog) {
		glUseProgram(shader->prog);
		if (shader->unifm_opacity >= 0)
			glUniform1f(shader->unifm_opacity, opacity);
		if (shader->unifm_invert_color >= 0)
			glUniform1i(shader->unifm_invert_color, neg);
		if (shader->unifm_tex >= 0)
			glUniform1i(shader->unifm_tex, 0);
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
	P_PAINTREG_START(reg_tgt, crect) {
		// Calculate texture coordinates
		GLfloat texture_x1 = (double)(crect.x1 - dx + x);
		GLfloat texture_y1 = (double)(crect.y1 - dy + y);
		GLfloat texture_x2 = texture_x1 + (double)(crect.x2 - crect.x1);
		GLfloat texture_y2 = texture_y1 + (double)(crect.y2 - crect.y1);

		if (ptex->target == GL_TEXTURE_2D) {
			// GL_TEXTURE_2D coordinates are 0-1
			texture_x1 /= ptex->width;
			texture_y1 /= ptex->height;
			texture_x2 /= ptex->width;
			texture_y2 /= ptex->height;
		}

		// Vertex coordinates
		GLint vx1 = crect.x1;
		GLint vy1 = crect.y1;
		GLint vx2 = crect.x2;
		GLint vy2 = crect.y2;

		// X pixmaps might be Y inverted, invert the texture coordinates
		if (ptex->y_inverted) {
			texture_y1 = 1.0 - texture_y1;
			texture_y2 = 1.0 - texture_y2;
		}

		// log_trace("Rect %d: %f, %f, %f, %f -> %d, %d, %d, %d",
		//          ri, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);

		GLfloat texture_x[] = {texture_x1, texture_x2, texture_x2, texture_x1};
		GLfloat texture_y[] = {texture_y1, texture_y1, texture_y2, texture_y2};
		GLint vx[] = {vx1, vx2, vx2, vx1};
		GLint vy[] = {vy1, vy1, vy2, vy2};

		for (int i = 0; i < 4; i++) {
			glTexCoord2f(texture_x[i], texture_y[i]);
			glVertex3i(vx[i], vy[i], z);
		}
	}
	P_PAINTREG_END();

	// Cleanup
	glBindTexture(ptex->target, 0);
	glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glDisable(GL_BLEND);
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

	return true;
}

bool gl_dim_reg(session_t *ps, int dx, int dy, int width, int height, float z,
                GLfloat factor, const region_t *reg_tgt) {
	// It's possible to dim in glx_render(), but it would be over-complicated
	// considering all those mess in color negation and modulation
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
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
	glDisable(GL_BLEND);

	gl_check_err();

	return true;
}

static inline int gl_gen_texture(GLenum tex_tgt, int width, int height, GLuint *tex) {
	glGenTextures(1, tex);
	if (!*tex)
		return -1;
	glEnable(tex_tgt);
	glBindTexture(tex_tgt, *tex);
	glTexParameteri(tex_tgt, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(tex_tgt, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(tex_tgt, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(tex_tgt, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(tex_tgt, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(tex_tgt, 0);

	return 0;
}

/**
 * Blur contents in a particular region.
 *
 * XXX seems to be way to complex for what it does
 */

// Blur the area sized width x height starting at dx x dy
bool gl_blur_dst(session_t *ps, const gl_cap_t *cap, int dx, int dy, int width,
                 int height, float z, GLfloat factor_center, const region_t *reg_tgt,
                 gl_blur_cache_t *pbc, const gl_blur_shader_t *pass, int npasses) {
	const bool more_passes = npasses > 1;

	// these should be arguments
	const bool have_scissors = glIsEnabled(GL_SCISSOR_TEST);
	const bool have_stencil = glIsEnabled(GL_STENCIL_TEST);
	bool ret = false;

	// Calculate copy region size
	gl_blur_cache_t ibc = {.width = 0, .height = 0};
	if (!pbc)
		pbc = &ibc;

	// log_trace("(): %d, %d, %d, %d\n", dx, dy, width, height);

	GLenum tex_tgt = GL_TEXTURE_RECTANGLE;
	if (cap->non_power_of_two_texture)
		tex_tgt = GL_TEXTURE_2D;

	// Free textures if size inconsistency discovered
	if (width != pbc->width || height != pbc->height) {
		glDeleteTextures(1, &pbc->textures[0]);
		glDeleteTextures(1, &pbc->textures[1]);
		pbc->width = pbc->height = 0;
		pbc->textures[0] = pbc->textures[1] = 0;
	}

	// Generate FBO and textures if needed
	if (!pbc->textures[0])
		gl_gen_texture(tex_tgt, width, height, &pbc->textures[0]);
	GLuint tex_scr = pbc->textures[0];
	if (npasses > 1 && !pbc->textures[1])
		gl_gen_texture(tex_tgt, width, height, &pbc->textures[1]);
	pbc->width = width;
	pbc->height = height;
	GLuint tex_scr2 = pbc->textures[1];
	if (npasses > 1 && !pbc->fbo)
		glGenFramebuffers(1, &pbc->fbo);
	const GLuint fbo = pbc->fbo;

	if (!tex_scr || (npasses > 1 && !tex_scr2)) {
		log_error("Failed to allocate texture.");
		goto end;
	}
	if (npasses > 1 && !fbo) {
		log_error("Failed to allocate framebuffer.");
		goto end;
	}

	// Read destination pixels into a texture
	glEnable(tex_tgt);
	glBindTexture(tex_tgt, tex_scr);

	// Copy the area to be blurred into tmp buffer
	glCopyTexSubImage2D(tex_tgt, 0, 0, 0, dx, dy, width, height);

	// Texture scaling factor
	GLfloat texfac_x = 1.0f, texfac_y = 1.0f;
	if (tex_tgt == GL_TEXTURE_2D) {
		texfac_x /= width;
		texfac_y /= height;
	}

	// Paint it back
	if (more_passes) {
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_SCISSOR_TEST);
	}

	for (int i = 0; i < npasses; ++i) {
		assert(i < MAX_BLUR_PASS - 1);
		const gl_blur_shader_t *curr = &pass[i];
		assert(curr->prog);

		assert(tex_scr);
		glBindTexture(tex_tgt, tex_scr);

		if (i < npasses - 1) {
			// not last pass, draw into framebuffer
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);

			// XXX not fixing bug during porting
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			                       GL_TEXTURE_2D, tex_scr2,
			                       0);        // XXX wrong, should use tex_tgt
			glDrawBuffers(1, (GLenum[]){GL_COLOR_ATTACHMENT0});
			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				log_error("Framebuffer attachment failed.");
				goto end;
			}
		} else {
			// last pass, draw directly into the back buffer
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDrawBuffers(1, (GLenum[]){GL_BACK});
			if (have_scissors)
				glEnable(GL_SCISSOR_TEST);
			if (have_stencil)
				glEnable(GL_STENCIL_TEST);
		}

		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glUseProgram(curr->prog);
		if (curr->unifm_offset_x >= 0)
			glUniform1f(curr->unifm_offset_x, texfac_x);
		if (curr->unifm_offset_y >= 0)
			glUniform1f(curr->unifm_offset_y, texfac_y);
		if (curr->unifm_factor_center >= 0)
			glUniform1f(curr->unifm_factor_center, factor_center);

		// XXX use multiple draw calls is probably going to be slow than
		//     just simply blur the whole area.

		P_PAINTREG_START(reg_tgt, crect) {
			// Texture coordinates
			const GLfloat texture_x1 = (crect.x1 - dx) * texfac_x;
			const GLfloat texture_y1 = (crect.y1 - dy) * texfac_y;
			const GLfloat texture_x2 = texture_x1 + (crect.x2 - crect.x1) * texfac_x;
			const GLfloat texture_y2 = texture_y1 + (crect.y2 - crect.y1) * texfac_y;

			// Vertex coordinates
			// For passes before the last one, we are drawing into a buffer,
			// so (dx, dy) from source maps to (0, 0)
			GLfloat vx1 = crect.x1 - dx;
			GLfloat vy1 = crect.y1 - dy;
			if (i == npasses - 1) {
				// For last pass, we are drawing back to source, so we
				// don't need to map
				vx1 = crect.x1;
				vy1 = crect.y1;
			}
			GLfloat vx2 = vx1 + (crect.x2 - crect.x1);
			GLfloat vy2 = vy1 + (crect.y2 - crect.y1);

			GLfloat texture_x[] = {texture_x1, texture_x2, texture_x2, texture_x1};
			GLfloat texture_y[] = {texture_y1, texture_y1, texture_y2, texture_y2};
			GLint vx[] = {vx1, vx2, vx2, vx1};
			GLint vy[] = {vy1, vy1, vy2, vy2};

			for (int j = 0; j < 4; j++) {
				glTexCoord2f(texture_x[j], texture_y[j]);
				glVertex3i(vx[j], vy[j], z);
			}
		}
		P_PAINTREG_END();

		glUseProgram(0);

		// Swap tex_scr and tex_scr2
		GLuint tmp = tex_scr2;
		tex_scr2 = tex_scr;
		tex_scr = tmp;
	}

	ret = true;

end:
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(tex_tgt, 0);
	glDisable(tex_tgt);
	if (have_scissors)
		glEnable(GL_SCISSOR_TEST);
	if (have_stencil)
		glEnable(GL_STENCIL_TEST);

	if (&ibc == pbc) {
		glDeleteTextures(1, &pbc->textures[0]);
		glDeleteTextures(1, &pbc->textures[1]);
		glDeleteFramebuffers(1, &pbc->fbo);
	}

	gl_check_err();

	return ret;
}

/**
 * Set clipping region on the target window.
 */
void gl_set_clip(const region_t *reg) {
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);

	if (!reg)
		return;

	int nrects;
	const rect_t *rects = pixman_region32_rectangles((region_t *)reg, &nrects);

	if (nrects == 1) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(rects[0].x1, rects[0].y2, rects[0].x2 - rects[0].x1,
		          rects[0].y2 - rects[0].y1);
	}

	gl_check_err();
}

GLuint glGetUniformLocationChecked(GLuint p, const char *name) {
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
int gl_win_shader_from_string(session_t *ps, const char *vshader_str,
                              const char *fshader_str, gl_win_shader_t *ret) {
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
void gl_resize(int width, int height) {
	glViewport(0, 0, width, height);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, width, 0, height, -1000.0, 1000.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

static void attr_unused gl_destroy_win_shader(session_t *ps, gl_win_shader_t *shader) {
	assert(shader);
	assert(shader->prog);
	glDeleteProgram(shader->prog);
	shader->prog = 0;
	shader->unifm_opacity = -1;
	shader->unifm_invert_color = -1;
	shader->unifm_tex = -1;
}

/**
 * Initialize GL blur filters.
 *
 * Fill `passes` with blur filters, won't create more than MAX_BLUR_FILTER number of
 * filters
 */
bool gl_create_blur_filters(session_t *ps, gl_blur_shader_t *passes, const gl_cap_t *cap) {
	assert(ps->o.blur_kerns[0]);

	// Allocate PBO if more than one blur kernel is present
	if (ps->o.blur_kerns[1]) {
		// Try to generate a framebuffer
		GLuint fbo = 0;
		glGenFramebuffers(1, &fbo);
		if (!fbo) {
			log_error("Failed to generate Framebuffer. Cannot do "
			          "multi-pass blur with GL backends.");
			return false;
		}
		glDeleteFramebuffers(1, &fbo);
	}

	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
	// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
	// Thanks to hiciu for reporting.
	setlocale(LC_NUMERIC, "C");

	static const char *FRAG_SHADER_BLUR_PREFIX = "#version 110\n"
	                                             "%s"
	                                             "uniform float offset_x;\n"
	                                             "uniform float offset_y;\n"
	                                             "uniform float factor_center;\n"
	                                             "uniform %s tex_scr;\n\n"
	                                             "void main() {\n"
	                                             "  vec4 sum = vec4(0.0, 0.0, 0.0, "
	                                             "0.0);\n";
	static const char *FRAG_SHADER_BLUR_ADD =
	    "  sum += float(%.7g) * %s(tex_scr, vec2(gl_TexCoord[0].x + offset_x "
	    "* float(%d), gl_TexCoord[0].y + offset_y * float(%d)));\n";
	static const char *FRAG_SHADER_BLUR_ADD_GPUSHADER4 =
	    "  sum += float(%.7g) * %sOffset(tex_scr, vec2(gl_TexCoord[0].x, "
	    "gl_TexCoord[0].y), ivec2(%d, %d));\n";
	static const char *FRAG_SHADER_BLUR_SUFFIX =
	    "  sum += %s(tex_scr, vec2(gl_TexCoord[0].x, gl_TexCoord[0].y)) * "
	    "factor_center;\n"
	    "  gl_FragColor = sum / (factor_center + float(%.7g));\n"
	    "}\n";

	const bool use_texture_rect = !cap->non_power_of_two_texture;
	const char *sampler_type = (use_texture_rect ? "sampler2DRect" : "sampler2D");
	const char *texture_func = (use_texture_rect ? "texture2DRect" : "texture2D");
	const char *shader_add = FRAG_SHADER_BLUR_ADD;
	char *extension = strdup("");
	if (use_texture_rect)
		mstrextend(&extension, "#extension GL_ARB_texture_rectangle : "
		                       "require\n");
	if (ps->o.glx_use_gpushader4) {
		mstrextend(&extension, "#extension GL_EXT_gpu_shader4 : "
		                       "require\n");
		shader_add = FRAG_SHADER_BLUR_ADD_GPUSHADER4;
	}

	for (int i = 0; i < MAX_BLUR_PASS && ps->o.blur_kerns[i]; ++i) {
		xcb_render_fixed_t *kern = ps->o.blur_kerns[i];
		if (!kern)
			break;

		// Build shader
		int wid = XFIXED_TO_DOUBLE(kern[0]), hei = XFIXED_TO_DOUBLE(kern[1]);
		int nele = wid * hei - 1;
		unsigned int len =
		    strlen(FRAG_SHADER_BLUR_PREFIX) + strlen(sampler_type) +
		    strlen(extension) + (strlen(shader_add) + strlen(texture_func) + 42) * nele +
		    strlen(FRAG_SHADER_BLUR_SUFFIX) + strlen(texture_func) + 12 + 1;
		char *shader_str = ccalloc(len, char);
		char *pc = shader_str;
		sprintf(pc, FRAG_SHADER_BLUR_PREFIX, extension, sampler_type);
		pc += strlen(pc);
		assert(strlen(shader_str) < len);

		double sum = 0.0;
		for (int j = 0; j < hei; ++j) {
			for (int k = 0; k < wid; ++k) {
				if (hei / 2 == j && wid / 2 == k)
					continue;
				double val = XFIXED_TO_DOUBLE(kern[2 + j * wid + k]);
				if (0.0 == val)
					continue;
				sum += val;
				sprintf(pc, shader_add, val, texture_func, k - wid / 2, j - hei / 2);
				pc += strlen(pc);
				assert(strlen(shader_str) < len);
			}
		}

		auto pass = passes + i;
		sprintf(pc, FRAG_SHADER_BLUR_SUFFIX, texture_func, sum);
		assert(strlen(shader_str) < len);
		pass->frag_shader = gl_create_shader(GL_FRAGMENT_SHADER, shader_str);
		free(shader_str);

		if (!pass->frag_shader) {
			log_error("Failed to create fragment shader %d.", i);
			goto err;
		}

		// Build program
		pass->prog = gl_create_program(&pass->frag_shader, 1);
		if (!pass->prog) {
			log_error("Failed to create GLSL program.");
			goto err;
		}

		// Get uniform addresses
		pass->unifm_factor_center =
		    glGetUniformLocationChecked(pass->prog, "factor_center");
		if (!ps->o.glx_use_gpushader4) {
			pass->unifm_offset_x =
			    glGetUniformLocationChecked(pass->prog, "offset_x");
			pass->unifm_offset_y =
			    glGetUniformLocationChecked(pass->prog, "offset_y");
		}
	}
	free(extension);

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
