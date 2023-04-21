// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE-mit for more information.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

#include "backend/gl/gl_common.h"
#include "backend/gl/glx.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "kernel.h"
#include "log.h"
#include "region.h"
#include "string_utils.h"
#include "uthash_extra.h"
#include "utils.h"
#include "win.h"

#include "opengl.h"

#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE 0x84F5
#endif

static inline XVisualInfo *get_visualinfo_from_visual(session_t *ps, xcb_visualid_t visual) {
	XVisualInfo vreq = {.visualid = visual};
	int nitems = 0;

	return XGetVisualInfo(ps->dpy, VisualIDMask, &vreq, &nitems);
}

/**
 * Initialize OpenGL.
 */
bool glx_init(session_t *ps, bool need_render) {
	bool success = false;
	XVisualInfo *pvis = NULL;

	// Check for GLX extension
	if (!ps->glx_exists) {
		if (glXQueryExtension(ps->dpy, &ps->glx_event, &ps->glx_error))
			ps->glx_exists = true;
		else {
			log_error("No GLX extension.");
			goto glx_init_end;
		}
	}

	// Get XVisualInfo
	pvis = get_visualinfo_from_visual(ps, ps->vis);
	if (!pvis) {
		log_error("Failed to acquire XVisualInfo for current visual.");
		goto glx_init_end;
	}

	// Ensure the visual is double-buffered
	if (need_render) {
		int value = 0;
		if (Success != glXGetConfig(ps->dpy, pvis, GLX_USE_GL, &value) || !value) {
			log_error("Root visual is not a GL visual.");
			goto glx_init_end;
		}

		if (Success != glXGetConfig(ps->dpy, pvis, GLX_DOUBLEBUFFER, &value) || !value) {
			log_error("Root visual is not a double buffered GL visual.");
			goto glx_init_end;
		}
	}

	// Ensure GLX_EXT_texture_from_pixmap exists
	if (need_render && !glxext.has_GLX_EXT_texture_from_pixmap)
		goto glx_init_end;

	// Initialize GLX data structure
	if (!ps->psglx) {
		static const glx_session_t CGLX_SESSION_DEF = CGLX_SESSION_INIT;
		ps->psglx = cmalloc(glx_session_t);
		memcpy(ps->psglx, &CGLX_SESSION_DEF, sizeof(glx_session_t));

		// +1 for the zero terminator
		ps->psglx->blur_passes = ccalloc(ps->o.blur_kernel_count, glx_blur_pass_t);

		for (int i = 0; i < ps->o.blur_kernel_count; ++i) {
			glx_blur_pass_t *ppass = &ps->psglx->blur_passes[i];
			ppass->unifm_offset_x = -1;
			ppass->unifm_offset_y = -1;
			ppass->unifm_opacity = -1;
			ppass->unifm_offset = -1;
			ppass->unifm_halfpixel = -1;
			ppass->unifm_fulltex = -1;
		}

		glx_blur_cache_t *pbc = &ps->psglx->blur_cache;
		for (int i = 0; i < MAX_BLUR_PASS; ++i) {
			pbc->fbos[i] = 0;
			pbc->textures[i] = 0;
		}

		ps->psglx->round_passes = ccalloc(2, glx_round_pass_t);
		for (int i = 0; i < 2; ++i) {
			glx_round_pass_t *ppass = &ps->psglx->round_passes[i];
			ppass->unifm_radius = -1;
			ppass->unifm_texcoord = -1;
			ppass->unifm_texsize = -1;
			ppass->unifm_borderw = -1;
			ppass->unifm_borderc = -1;
			ppass->unifm_resolution = -1;
			ppass->unifm_tex_scr = -1;
			ppass->unifm_tex_wnd = -1;
		}

		ps->psglx->round_passes = ccalloc(1, glx_round_pass_t);
		glx_round_pass_t *ppass = ps->psglx->round_passes;
		ppass->unifm_radius = -1;
		ppass->unifm_texcoord = -1;
		ppass->unifm_texsize = -1;
		ppass->unifm_borderw = -1;
		ppass->unifm_borderc = -1;
		ppass->unifm_resolution = -1;
		ppass->unifm_tex_scr = -1;
	}

	glx_session_t *psglx = ps->psglx;

	if (!psglx->context) {
		// Get GLX context
#ifndef DEBUG_GLX_DEBUG_CONTEXT
		psglx->context = glXCreateContext(ps->dpy, pvis, None, GL_TRUE);
#else
		{
			GLXFBConfig fbconfig = get_fbconfig_from_visualinfo(ps, pvis);
			if (!fbconfig) {
				log_error("Failed to get GLXFBConfig for root visual "
				          "%#lx.",
				          pvis->visualid);
				goto glx_init_end;
			}

			f_glXCreateContextAttribsARB p_glXCreateContextAttribsARB =
			    (f_glXCreateContextAttribsARB)glXGetProcAddress(
			        (const GLubyte *)"glXCreateContextAttribsARB");
			if (!p_glXCreateContextAttribsARB) {
				log_error("Failed to get glXCreateContextAttribsARB().");
				goto glx_init_end;
			}

			static const int attrib_list[] = {
			    GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_DEBUG_BIT_ARB, None};
			psglx->context = p_glXCreateContextAttribsARB(
			    ps->dpy, fbconfig, NULL, GL_TRUE, attrib_list);
		}
#endif

		if (!psglx->context) {
			log_error("Failed to get GLX context.");
			goto glx_init_end;
		}

		// Attach GLX context
		if (!glXMakeCurrent(ps->dpy, get_tgt_window(ps), psglx->context)) {
			log_error("Failed to attach GLX context.");
			goto glx_init_end;
		}

#ifdef DEBUG_GLX_DEBUG_CONTEXT
		{
			f_DebugMessageCallback p_DebugMessageCallback =
			    (f_DebugMessageCallback)glXGetProcAddress(
			        (const GLubyte *)"glDebugMessageCallback");
			if (!p_DebugMessageCallback) {
				log_error("Failed to get glDebugMessageCallback(0.");
				goto glx_init_end;
			}
			p_DebugMessageCallback(glx_debug_msg_callback, ps);
		}
#endif
	}

	// Ensure we have a stencil buffer. X Fixes does not guarantee rectangles
	// in regions don't overlap, so we must use stencil buffer to make sure
	// we don't paint a region for more than one time, I think?
	if (need_render && !ps->o.glx_no_stencil) {
		GLint val = 0;
		glGetIntegerv(GL_STENCIL_BITS, &val);
		if (!val) {
			log_error("Target window doesn't have stencil buffer.");
			goto glx_init_end;
		}
	}

	// Check GL_ARB_texture_non_power_of_two, requires a GLX context and
	// must precede FBConfig fetching
	if (need_render)
		psglx->has_texture_non_power_of_two =
		    gl_has_extension("GL_ARB_texture_non_power_of_two");

	// Render preparations
	if (need_render) {
		glx_on_root_change(ps);

		glDisable(GL_DEPTH_TEST);
		glDepthMask(GL_FALSE);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glDisable(GL_BLEND);

		if (!ps->o.glx_no_stencil) {
			// Initialize stencil buffer
			glClear(GL_STENCIL_BUFFER_BIT);
			glDisable(GL_STENCIL_TEST);
			glStencilMask(0x1);
			glStencilFunc(GL_EQUAL, 0x1, 0x1);
		}

		// Clear screen
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		// glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		// glXSwapBuffers(ps->dpy, get_tgt_window(ps));
	}

	success = true;

glx_init_end:
	XFree(pvis);

	if (!success)
		glx_destroy(ps);

	return success;
}

static void glx_free_prog_main(glx_prog_main_t *pprogram) {
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
 * Destroy GLX related resources.
 */
void glx_destroy(session_t *ps) {
	if (!ps->psglx)
		return;

	// Free all GLX resources of windows
	win_stack_foreach_managed(w, &ps->window_stack) {
		free_win_res_glx(ps, w);
	}

	// Free GLSL shaders/programs
	for (int i = 0; i < ps->o.blur_kernel_count; ++i) {
		glx_blur_pass_t *ppass = &ps->psglx->blur_passes[i];
		if (ppass->frag_shader) {
			glDeleteShader(ppass->frag_shader);
		}
		if (ppass->prog) {
			glDeleteProgram(ppass->prog);
		}
	}
	free(ps->psglx->blur_passes);

<<<<<<< HEAD
	glx_round_pass_t *ppass = ps->psglx->round_passes;
	if (ppass->frag_shader) {
		glDeleteShader(ppass->frag_shader);
	}
	if (ppass->prog) {
		glDeleteProgram(ppass->prog);
=======
	glx_blur_cache_t *pbc = &ps->psglx->blur_cache;
	if (pbc) free_glx_bc(ps, pbc);

	for (int i = 0; i < 2; ++i) {
		glx_round_pass_t *ppass = &ps->psglx->round_passes[i];
		if (ppass->frag_shader)
			glDeleteShader(ppass->frag_shader);
		if (ppass->prog)
			glDeleteProgram(ppass->prog);
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
	}
	free(ps->psglx->round_passes);

	glx_free_prog_main(&ps->glx_prog_win);

	gl_check_err();

	// Destroy GLX context
	if (ps->psglx->context) {
		glXDestroyContext(ps->dpy, ps->psglx->context);
		ps->psglx->context = NULL;
	}

	free(ps->psglx);
	ps->psglx = NULL;
}

/**
 * Callback to run on root window size change.
 */
void glx_on_root_change(session_t *ps) {
	glViewport(0, 0, ps->root_width, ps->root_height);

	// Initialize matrix, copied from dcompmgr
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, ps->root_width, 0, ps->root_height, -1000.0, 1000.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}


static inline GLuint glx_gen_texture(GLenum tex_tgt, int width, int height);

/**
 * Initialize GLX blur filter.
 */
bool glx_init_conv_blur(session_t *ps) {
	assert(ps->o.blur_kernel_count > 0);
	assert(ps->o.blur_kerns);
	assert(ps->o.blur_kerns[0]);

	// Allocate PBO if more than one blur kernel is present
	if (ps->o.blur_kernel_count > 1) {
		// Try to generate a framebuffer
		GLuint fbo = 0;
		glGenFramebuffers(1, &fbo);
		if (!fbo) {
			log_error("Failed to generate Framebuffer. Cannot do multi-pass "
			          "blur with GLX"
			          " backend.");
			return false;
		}
		glDeleteFramebuffers(1, &fbo);
	}

	{
		char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
		// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
		// Thanks to hiciu for reporting.
		setlocale(LC_NUMERIC, "C");

		static const char *FRAG_SHADER_BLUR_PREFIX =
		    "#version 110\n"
		    "%s"
		    "uniform float offset_x;\n"
		    "uniform float offset_y;\n"
			"uniform float opacity;\n"
		    "uniform %s tex_scr;\n"
		    "\n"
		    "void main() {\n"
		    "  vec4 sum = vec4(0.0, 0.0, 0.0, 0.0);\n";
		static const char *FRAG_SHADER_BLUR_ADD =
		    "  sum += float(%.7g) * %s(tex_scr, vec2(gl_TexCoord[0].x + offset_x "
		    "* float(%d), gl_TexCoord[0].y + offset_y * float(%d)));\n";
		static const char *FRAG_SHADER_BLUR_SUFFIX =
		    "  sum += %s(tex_scr, vec2(gl_TexCoord[0].x, gl_TexCoord[0].y));\n"
			"  gl_FragColor = sum / (float(%.7g));\n"
			"  gl_FragColor.a = opacity;\n"
		    "}\n";

		const bool use_texture_rect = !ps->psglx->has_texture_non_power_of_two;
		const char *sampler_type = (use_texture_rect ? "sampler2DRect" : "sampler2D");
		const char *texture_func = (use_texture_rect ? "texture2DRect" : "texture2D");
		const char *shader_add = FRAG_SHADER_BLUR_ADD;
		char *extension = NULL;
		if (use_texture_rect) {
			mstrextend(&extension, "#extension GL_ARB_texture_rectangle : "
			                       "require\n");
		}
		if (!extension) {
			extension = strdup("");
		}

		for (int i = 0; i < ps->o.blur_kernel_count; ++i) {
			auto kern = ps->o.blur_kerns[i];
			glx_blur_pass_t *ppass = &ps->psglx->blur_passes[i];

			// Build shader
			int width = kern->w, height = kern->h;
			int nele = width * height - 1;
			assert(nele >= 0);
			auto len =
			    strlen(FRAG_SHADER_BLUR_PREFIX) + strlen(sampler_type) +
			    strlen(extension) +
			    (strlen(shader_add) + strlen(texture_func) + 42) * (uint)nele +
			    strlen(FRAG_SHADER_BLUR_SUFFIX) + strlen(texture_func) + 12 + 1;
			char *shader_str = ccalloc(len, char);
			char *pc = shader_str;
			sprintf(pc, FRAG_SHADER_BLUR_PREFIX, extension, sampler_type);
			pc += strlen(pc);
			assert(strlen(shader_str) < len);

			double sum = 0.0;
			for (int j = 0; j < height; ++j) {
				for (int k = 0; k < width; ++k) {
					if (height / 2 == j && width / 2 == k)
						continue;
					double val = kern->data[j * width + k];
					if (val == 0) {
						continue;
					}
					sum += val;
					sprintf(pc, shader_add, val, texture_func,
					        k - width / 2, j - height / 2);
					pc += strlen(pc);
					assert(strlen(shader_str) < len);
				}
			}

			sprintf(pc, FRAG_SHADER_BLUR_SUFFIX, texture_func, sum);
			assert(strlen(shader_str) < len);
			ppass->frag_shader = gl_create_shader(GL_FRAGMENT_SHADER, shader_str);
			free(shader_str);

			if (!ppass->frag_shader) {
				log_error("Failed to create fragment shader %d.", i);
				free(extension);
				free(lc_numeric_old);
				return false;
			}

			// Build program
			ppass->prog = gl_create_program(&ppass->frag_shader, 1);
			if (!ppass->prog) {
				log_error("Failed to create GLSL program.");
				free(extension);
				free(lc_numeric_old);
				return false;
			}

			// Get uniform addresses
#define P_GET_UNIFM_LOC(name, target)                                                    \
	{                                                                                \
		ppass->target = glGetUniformLocation(ppass->prog, name);                 \
		if (ppass->target < 0) {                                                 \
			log_error("Failed to get location of %d-th uniform '" name       \
			          "'. Might be troublesome.",                            \
			          i);                                                    \
		}                                                                        \
	}

			P_GET_UNIFM_LOC("opacity", unifm_opacity);
			P_GET_UNIFM_LOC("offset_x", unifm_offset_x);
			P_GET_UNIFM_LOC("offset_y", unifm_offset_y);

#undef P_GET_UNIFM_LOC
		}
		free(extension);

		// Restore LC_NUMERIC
		setlocale(LC_NUMERIC, lc_numeric_old);
		free(lc_numeric_old);
	}

	gl_check_err();

	return true;
}

bool glx_init_kawase_blur(session_t *ps) {
	assert(ps->o.blur_kernel_count > 0);

	// Allocate PBO if more than one blur kernel is present
	if (ps->o.blur_kernel_count > 1) {
		// Try to generate a framebuffer
		GLuint fbo = 0;
		glGenFramebuffers(1, &fbo);
		if (!fbo) {
			log_error("Failed to generate Framebuffer. Cannot do multi-pass "
			          "blur with GLX"
			          " backend.");
			return false;
		}
		glDeleteFramebuffers(1, &fbo);
	}

	{
		char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
		// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
		// Thanks to hiciu for reporting.
		setlocale(LC_NUMERIC, "C");

		static const char *FRAG_SHADER_PREFIX =
			"#version 110\n"
			"%s"  // extensions
			"uniform float offset;\n"
			"uniform vec2 halfpixel;\n"
			"uniform vec2 fulltex;\n"
			"uniform %s tex_scr;\n" // sampler2D | sampler2DRect
			"vec4 clamp_tex(vec2 uv)\n"
			"{\n"
			"  return %s(tex_scr, clamp(uv, vec2(0), fulltex));\n" // texture2D | texture2DRect
			"}\n"
			"\n"
			"void main()\n"
			"{\n"
			"  vec2 uv = (gl_TexCoord[0].xy / fulltex);\n"
			"\n";

		// Fragment shader (Dual Kawase Blur) - Downsample
		static const char *FRAG_SHADER_KAWASE_DOWN =
			"  vec4 sum = clamp_tex(uv) * 4.0;\n"
			"  sum += clamp_tex(uv - halfpixel.xy * offset);\n"
			"  sum += clamp_tex(uv + halfpixel.xy * offset);\n"
			"  sum += clamp_tex(uv + vec2(halfpixel.x, -halfpixel.y) * offset);\n"
			"  sum += clamp_tex(uv - vec2(halfpixel.x, -halfpixel.y) * offset);\n"
			"\n"
			"  gl_FragColor = sum / 8.0;\n"
			"}\n";

		// Fragment shader (Dual Kawase Blur) - Upsample
		static const char *FRAG_SHADER_KAWASE_UP =
			"  vec4 sum = clamp_tex(uv + vec2(-halfpixel.x * 2.0, 0.0) * offset);\n"
			"  sum += clamp_tex(uv + vec2(-halfpixel.x, halfpixel.y) * offset) * 2.0;\n"
			"  sum += clamp_tex(uv + vec2(0.0, halfpixel.y * 2.0) * offset);\n"
			"  sum += clamp_tex(uv + vec2(halfpixel.x, halfpixel.y) * offset) * 2.0;\n"
			"  sum += clamp_tex(uv + vec2(halfpixel.x * 2.0, 0.0) * offset);\n"
			"  sum += clamp_tex(uv + vec2(halfpixel.x, -halfpixel.y) * offset) * 2.0;\n"
			"  sum += clamp_tex(uv + vec2(0.0, -halfpixel.y * 2.0) * offset);\n"
			"  sum += clamp_tex(uv + vec2(-halfpixel.x, -halfpixel.y) * offset) * 2.0;\n"
			"\n"
			"  gl_FragColor = sum / 12.0;\n"
			"}\n";

		const bool use_texture_rect = !ps->psglx->has_texture_non_power_of_two;
		const char *sampler_type = (use_texture_rect ? "sampler2DRect": "sampler2D");
		const char *texture_func = (use_texture_rect ? "texture2DRect": "texture2D");
		char *extension = NULL;
		if (use_texture_rect) {
			mstrextend(&extension, "#extension GL_ARB_texture_rectangle : "
			                       "require\n");
		}
		if (!extension) {
			extension = strdup("");
		}

    	// Build kawase downsample shader
		glx_blur_pass_t *down_pass = &ps->psglx->blur_passes[0];
		{
			size_t len = strlen(FRAG_SHADER_PREFIX) + strlen(extension) + strlen(sampler_type) + strlen(texture_func) + strlen(FRAG_SHADER_KAWASE_DOWN) + 1;
			char *shader_str = calloc(len, sizeof(char));
			if (!shader_str) {
				log_error("Failed to allocate %zd bytes for shader string.", len);
				return false;
			}

			char *pc = shader_str;
			sprintf(pc, FRAG_SHADER_PREFIX, extension, sampler_type, texture_func);
			pc += strlen(pc);
			assert(strlen(shader_str) < len);

			sprintf(pc, "%s", FRAG_SHADER_KAWASE_DOWN);
			assert(strlen(shader_str) < len);
#ifdef DEBUG_GLX
			log_debug("Generated kawase downsample shader:\n%s\n", shader_str);
#endif
			down_pass->frag_shader = gl_create_shader(GL_FRAGMENT_SHADER, shader_str);
			free(shader_str);

			if (!down_pass->frag_shader) {
				log_error("Failed to create kawase downsample fragment shader.");
				free(extension);
				free(lc_numeric_old);
				return false;
			}

			// Build program
			down_pass->prog = gl_create_program(&down_pass->frag_shader, 1);
			if (!down_pass->prog) {
				log_error("Failed to create GLSL program.");
				free(extension);
				free(lc_numeric_old);
				return false;
			}

			// Get uniform addresses
#define P_GET_UNIFM_LOC(name, target) \
	{ \
		down_pass->target = glGetUniformLocation(down_pass->prog, name); \
		if (down_pass->target < 0) { \
			log_error("Failed to get location of kawase downsample uniform '" name "'. Might be troublesome."); \
		} \
	}
			P_GET_UNIFM_LOC("offset", unifm_offset);
			P_GET_UNIFM_LOC("halfpixel", unifm_halfpixel);
			P_GET_UNIFM_LOC("fulltex", unifm_fulltex);
#undef P_GET_UNIFM_LOC
		}

		// Build kawase downsample shader
		glx_blur_pass_t *up_pass = &ps->psglx->blur_passes[1];
		{
			size_t len = strlen(FRAG_SHADER_PREFIX) + strlen(extension) + strlen(sampler_type) + strlen(texture_func) + strlen(FRAG_SHADER_KAWASE_UP) + 1;
			char *shader_str = calloc(len, sizeof(char));
			if (!shader_str) {
				log_error("Failed to allocate %zd bytes for shader string.", len);
				return false;
			}

			char *pc = shader_str;
			sprintf(pc, FRAG_SHADER_PREFIX, extension, sampler_type, texture_func);
			pc += strlen(pc);
			assert(strlen(shader_str) < len);

			sprintf(pc, "%s", FRAG_SHADER_KAWASE_UP);
			assert(strlen(shader_str) < len);
#ifdef DEBUG_GLX
			log_debug("Generated kawase upsample shader:\n%s\n", shader_str);
#endif
			up_pass->frag_shader = gl_create_shader(GL_FRAGMENT_SHADER, shader_str);
			free(shader_str);

			if (!up_pass->frag_shader) {
				log_error("Failed to create kawase upsample fragment shader.");
				return false;
			}

			// Build program
			up_pass->prog = gl_create_program(&up_pass->frag_shader, 1);
			if (!up_pass->prog) {
				log_error("Failed to create GLSL program.");
				return false;
			}

			// Get uniform addresses
#define P_GET_UNIFM_LOC(name, target) \
		{ \
			up_pass->target = glGetUniformLocation(up_pass->prog, name); \
			if (up_pass->target < 0) { \
				log_error("Failed to get location of kawase upsample uniform '" name "'. Might be troublesome."); \
			} \
		}
			P_GET_UNIFM_LOC("offset", unifm_offset);
			P_GET_UNIFM_LOC("halfpixel", unifm_halfpixel);
			P_GET_UNIFM_LOC("fulltex", unifm_fulltex);
#undef P_GET_UNIFM_LOC
		}

		free(extension);

		// Restore LC_NUMERIC
		setlocale(LC_NUMERIC, lc_numeric_old);
		free(lc_numeric_old);
	}

	gl_check_err();

	return true;
}

/**
 * Initialize GLX blur filter for the dual-filter kawase blur.
 */

bool
glx_init_dualkawase_blur(session_t *ps) {
	assert(ps->o.blur_strength.iterations);
	int iterations = ps->o.blur_strength.iterations;
  	assert(iterations < MAX_BLUR_PASS);

	// Allocate PBO if more than one blur kernel is present
	/*if (ps->o.blur_kernel_count > 1) {
		// Try to generate a framebuffer
		GLuint fbo = 0;
		glGenFramebuffers(1, &fbo);
		if (!fbo) {
			log_error("Failed to generate Framebuffer. Cannot do multi-pass "
			          "blur with GLX"
			          " backend.");
			return false;
		}
		glDeleteFramebuffers(1, &fbo);
	}*/

	// Allocate required FBOs for dual-filter support
	{
		glx_blur_cache_t *pbc = &ps->psglx->blur_cache;
		glGenFramebuffers(iterations, pbc->fbos);
		if (!pbc->fbos[0]) {
			log_error("Failed to generate Framebuffer. Cannot do "
					"multi-pass blur with GLX backend.");
			return false;
		}
	}

	// Allocate textures if needed and bind to the respective framebuffer
	{
		GLenum tex_tgt = GL_TEXTURE_RECTANGLE;
		if (ps->psglx->has_texture_non_power_of_two)
			tex_tgt = GL_TEXTURE_2D;

		// Allocate scaled texture
		glx_blur_cache_t *pbc = &ps->psglx->blur_cache;

		int tex_width;
		int tex_height;
		for (int i = 0; i <= iterations; ++i) {
			if (!pbc->textures[i]) {
				tex_width = ps->root_width / (1 << (i));
				tex_height = ps->root_height / (1 << (i));
				pbc->textures[i] = glx_gen_texture(tex_tgt, tex_width, tex_height);
				pbc->width[i] = tex_width;
				pbc->height[i] = tex_height;
			}
			if (!pbc->textures[i]) {
				log_error("Failed to allocate texture.");
				return false;
			}

			// Bind texture to framebuffer
			if ((i > 0) && pbc->fbos[i-1]) {
				static const GLenum DRAWBUFS[2] = { GL_COLOR_ATTACHMENT0 };
				glBindFramebuffer(GL_FRAMEBUFFER, pbc->fbos[i-1]);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
										GL_TEXTURE_2D, pbc->textures[i], 0);
				glDrawBuffers(1, DRAWBUFS);
				if (glCheckFramebufferStatus(GL_FRAMEBUFFER)
					!= GL_FRAMEBUFFER_COMPLETE) {
					log_error("Framebuffer attachment failed.");
					glBindFramebuffer(GL_FRAMEBUFFER, 0);
					return false;
				}
			}

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

  // Compile blur shader
  {
	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
    // Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
    // Thanks to hiciu for reporting.
    setlocale(LC_NUMERIC, "C");

    static const char *FRAG_SHADER_PREFIX =
      "#version 110\n"
      "%s"  // extensions
      "uniform float offset;\n"
      "uniform float opacity;\n"
      "uniform vec2 halfpixel;\n"
      "uniform vec2 fulltex;\n"
      "uniform %s tex_scr;\n" // sampler2D | sampler2DRect
      "\n"
      "vec4 clamp_tex(vec2 uv)\n"
      "{\n"
      "  return %s(tex_scr, clamp(uv, vec2(0), fulltex));\n" // texture2D | texture2DRect
      "}\n"
      "\n"
      "void main()\n"
      "{\n"
      "  vec2 uv = (gl_FragCoord.xy / fulltex);\n"
      "\n";

    // Fragment shader (Dual Kawase Blur) - Downsample
    static const char *FRAG_SHADER_KAWASE_DOWN =
      "  vec4 sum = clamp_tex(uv) * 4.0;\n"
      "  sum += clamp_tex(uv - halfpixel.xy * offset);\n"
      "  sum += clamp_tex(uv + halfpixel.xy * offset);\n"
      "  sum += clamp_tex(uv + vec2(halfpixel.x, -halfpixel.y) * offset);\n"
      "  sum += clamp_tex(uv - vec2(halfpixel.x, -halfpixel.y) * offset);\n"
      "\n"
      "  gl_FragColor = sum / 8.0;\n"
      "}\n";

    // Fragment shader (Dual Kawase Blur) - Upsample
    static const char *FRAG_SHADER_KAWASE_UP =
      "  vec4 sum = clamp_tex(uv + vec2(-halfpixel.x * 2.0, 0.0) * offset);\n"
      "  sum += clamp_tex(uv + vec2(-halfpixel.x, halfpixel.y) * offset) * 2.0;\n"
      "  sum += clamp_tex(uv + vec2(0.0, halfpixel.y * 2.0) * offset);\n"
      "  sum += clamp_tex(uv + vec2(halfpixel.x, halfpixel.y) * offset) * 2.0;\n"
      "  sum += clamp_tex(uv + vec2(halfpixel.x * 2.0, 0.0) * offset);\n"
      "  sum += clamp_tex(uv + vec2(halfpixel.x, -halfpixel.y) * offset) * 2.0;\n"
      "  sum += clamp_tex(uv + vec2(0.0, -halfpixel.y * 2.0) * offset);\n"
      "  sum += clamp_tex(uv + vec2(-halfpixel.x, -halfpixel.y) * offset) * 2.0;\n"
      "\n"
      "  gl_FragColor = sum / 12.0;\n"
      "  gl_FragColor.a = opacity;\n"
      "}\n";

    const bool use_texture_rect = !ps->psglx->has_texture_non_power_of_two;
    const char *sampler_type = (use_texture_rect ? "sampler2DRect": "sampler2D");
    const char *texture_func = (use_texture_rect ? "texture2DRect": "texture2D");
    char *extension = NULL;
	if (use_texture_rect) {
		mstrextend(&extension, "#extension GL_ARB_texture_rectangle : "
								"require\n");
	}
	if (!extension) {
		extension = strdup("");
	}

    // Build kawase downsample shader
    glx_blur_pass_t *down_pass = &ps->psglx->blur_passes[0];
    {
      size_t len = strlen(FRAG_SHADER_PREFIX) + strlen(extension) + strlen(sampler_type) + strlen(texture_func) + strlen(FRAG_SHADER_KAWASE_DOWN) + 1;
      char *shader_str = calloc(len, sizeof(char));
      if (!shader_str) {
        log_error("Failed to allocate %zd bytes for shader string.", len);
        return false;
      }

      char *pc = shader_str;
      sprintf(pc, FRAG_SHADER_PREFIX, extension, sampler_type, texture_func);
      pc += strlen(pc);
      assert(strlen(shader_str) < len);

      sprintf(pc, "%s", FRAG_SHADER_KAWASE_DOWN);
      assert(strlen(shader_str) < len);
      down_pass->frag_shader = gl_create_shader(GL_FRAGMENT_SHADER, shader_str);
      free(shader_str);

      if (!down_pass->frag_shader) {
        log_error("Failed to create dual_kawase downsample fragment shader.");
        return false;
      }

      // Build program
      down_pass->prog = gl_create_program(&down_pass->frag_shader, 1);
      if (!down_pass->prog) {
        log_error("Failed to create GLSL program.");
        return false;
      }

      // Get uniform addresses
#define P_GET_UNIFM_LOC(name, target) { \
      down_pass->target = glGetUniformLocation(down_pass->prog, name); \
      if (down_pass->target < 0) { \
        log_error("Failed to get location of dual_kawase downsample uniform '" name "'. Might be troublesome."); \
      } \
    }
      P_GET_UNIFM_LOC("offset", unifm_offset);
      P_GET_UNIFM_LOC("halfpixel", unifm_halfpixel);
      P_GET_UNIFM_LOC("fulltex", unifm_fulltex);
#undef P_GET_UNIFM_LOC
    }

    // Build kawase upsample shader
    glx_blur_pass_t *up_pass = &ps->psglx->blur_passes[1];
    {
      size_t len = strlen(FRAG_SHADER_PREFIX) + strlen(extension) + strlen(sampler_type) + strlen(texture_func) + strlen(FRAG_SHADER_KAWASE_UP) + 1;
      char *shader_str = calloc(len, sizeof(char));
      if (!shader_str) {
        log_error("Failed to allocate %zd bytes for shader string.", len);
        return false;
      }

      char *pc = shader_str;
      sprintf(pc, FRAG_SHADER_PREFIX, extension, sampler_type, texture_func);
      pc += strlen(pc);
      assert(strlen(shader_str) < len);

      sprintf(pc, "%s", FRAG_SHADER_KAWASE_UP);
      assert(strlen(shader_str) < len);
      up_pass->frag_shader = gl_create_shader(GL_FRAGMENT_SHADER, shader_str);
      free(shader_str);

      if (!up_pass->frag_shader) {
        log_error("Failed to create dual_kawase upsample fragment shader.");
        return false;
      }

      // Build program
      up_pass->prog = gl_create_program(&up_pass->frag_shader, 1);
      if (!up_pass->prog) {
        log_error("Failed to create GLSL program.");
        return false;
      }

      // Get uniform addresses
#define P_GET_UNIFM_LOC(name, target) { \
      up_pass->target = glGetUniformLocation(up_pass->prog, name); \
      if (up_pass->target < 0) { \
        log_error("Failed to get location of dual_kawase upsample uniform '" name "'. Might be troublesome."); \
      } \
    }
      P_GET_UNIFM_LOC("offset", unifm_offset);
      P_GET_UNIFM_LOC("opacity", unifm_opacity);
      P_GET_UNIFM_LOC("halfpixel", unifm_halfpixel);
      P_GET_UNIFM_LOC("fulltex", unifm_fulltex);
#undef P_GET_UNIFM_LOC
    }

    free(extension);

    // Restore LC_NUMERIC
    setlocale(LC_NUMERIC, lc_numeric_old);
    free(lc_numeric_old);
  }

  gl_check_err();

  return true;
}

/**
 * Initialize GLX blur filter.
 */
bool glx_init_blur(session_t *ps) {
	
	switch (ps->o.blur_method) {
	case BLUR_METHOD_DUAL_KAWASE:
		return glx_init_kawase_blur(ps);
	case BLUR_METHOD_ALT_KAWASE:
		return glx_init_dualkawase_blur(ps);
	case BLUR_METHOD_KERNEL:
	case BLUR_METHOD_BOX:
	case BLUR_METHOD_GAUSSIAN:
		return glx_init_conv_blur(ps);
	default:
		return false;
  }
}

static inline bool glx_init_frag_shader_corners(glx_round_pass_t *ppass,
			const int shader_idx, const char *PREFIX_STR, const char* SHADER_STR,
			const char *extension, const char *sampler_type, const char *texture_func) {

	// Build rounded corners shader
	{
		auto len = strlen(PREFIX_STR) + strlen(extension)
					+ strlen(sampler_type)*2 + strlen(texture_func)*2
					+ strlen(SHADER_STR) + 1;
		char *shader_str = calloc(len, sizeof(char));
		if (!shader_str) {
			log_error("Failed to allocate %zd bytes for shader string.", len);
			return false;
		}

		char *pc = shader_str;
		sprintf(pc, PREFIX_STR, extension, sampler_type, sampler_type, texture_func, texture_func);
		pc += strlen(pc);
		assert(strlen(shader_str) < len);

		sprintf(pc, "%s", SHADER_STR);
		assert(strlen(shader_str) < len);
#ifdef DEBUG_GLX
		log_debug("Generated rounded corners shader %d:\n%s\n", shader_idx, shader_str);
#endif

		//log_info("Generated rounded corners shader %d:\n%s\n", shader_idx, shader_str);

		ppass->frag_shader = gl_create_shader(GL_FRAGMENT_SHADER, shader_str);
		free(shader_str);

		if (!ppass->frag_shader) {
			log_error("Failed to create rounded corners fragment shader.");
			return false;
		}

		// Build program
		ppass->prog = gl_create_program(&ppass->frag_shader, 1);
		if (!ppass->prog) {
			log_error("Failed to create GLSL program.");
			return false;
		}

		// Get uniform addresses
#define P_GET_UNIFM_LOC(name, target)											\
{																				\
	ppass->target = glGetUniformLocation(ppass->prog, name);					\
	if (ppass->target < 0) {													\
		log_error("Failed to get location of rounded corners uniform '" name	\
					"'. Might be troublesome. (shader_idx: %d)"					\
					, shader_idx);												\
	} 																			\
}
		P_GET_UNIFM_LOC("u_radius", unifm_radius);
		P_GET_UNIFM_LOC("u_texcoord", unifm_texcoord);
		P_GET_UNIFM_LOC("u_texsize", unifm_texsize);
		P_GET_UNIFM_LOC("u_borderw", unifm_borderw);
		P_GET_UNIFM_LOC("u_borderc", unifm_borderc);
		P_GET_UNIFM_LOC("u_resolution", unifm_resolution);
		P_GET_UNIFM_LOC("tex_scr", unifm_tex_scr);
		// We don't need this one anymore since we get
		// the border color using glReadPixel
		// uncomment if you need to use 'tex_wnd' in the shader
		//P_GET_UNIFM_LOC("tex_wnd", unifm_tex_wnd);
#undef P_GET_UNIFM_LOC
	}

	return true;
}

/**
 * Initialize GLX rounded corners filter.
 */
bool glx_init_rounded_corners(session_t *ps) {

	{
		char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
		// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
		// Thanks to hiciu for reporting.
		setlocale(LC_NUMERIC, "C");

		static const char *FRAG_SHADER_PREFIX =
			"#version 110\n"
			"%s"  // extensions
			"uniform float u_radius;\n"
			"uniform float u_borderw;\n"
			"uniform vec4 u_borderc;\n"
			"uniform vec2 u_texcoord;\n"
			"uniform vec2 u_texsize;\n"
			"uniform vec2 u_resolution;\n"
			"uniform %s tex_scr;\n" // sampler2D | sampler2DRect
			"uniform %s tex_wnd;\n" // sampler2D | sampler2DRect
			"\n"
			"// https://www.shadertoy.com/view/ltS3zW\n"
			"float RectSDF(vec2 p, vec2 b, float r) {\n"
			"  vec2 d = abs(p) - b + vec2(r);\n"
			"  return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
			"}\n\n"
			"void main()\n"
			"{\n"
			"  vec2 coord = vec2(u_texcoord.x, u_resolution.y-u_texsize.y-u_texcoord.y);\n"
			"  // Get the window_bg color (so we can \"erase\" corners) from the bg texture\n"
			"  // and the border color from the mid x-axis of the target window (hacky...)\n"
			"  vec4 u_v4WndBgColor = %s(tex_scr, vec2(gl_TexCoord[0].st));\n"
			"  //vec4 u_v4BorderColor = %s(tex_wnd, vec2(0, u_texsize.t/2.));\n"
			"  vec4 u_v4BorderColor = u_borderc;\n"
			"  vec4 u_v4FillColor = vec4(0.0, 0.0, 0.0, 0.0);  // Inside rect, transparent\n"
			"  vec4 v4FromColor = u_v4BorderColor;	//Always the border color. If no border, this still should be set\n"
			"  vec4 v4ToColor = u_v4WndBgColor;		//Outside corners color, we set it to background texture\n"
			"\n";

		// Fragment shader (round corners)
		// dst0 shader
		static const char *FRAG_SHADER_ROUND_CORNERS_0 =
			"  float u_fRadiusPx = u_radius;\n"
			"  float u_fHalfBorderThickness = u_borderw / 2.0;\n"
			"  //v4FromColor = u_v4BorderColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
			"  //u_v4FillColor = vec4(0.0, 0.0, 0.0, 0.0);  // Inside rect color\n"
			"\n"
			"  vec2 u_v2HalfShapeSizePx = u_texsize/2.0 - vec2(u_fHalfBorderThickness);\n"
			"  vec2 v_v2CenteredPos = (gl_FragCoord.xy - u_texsize.xy / 2.0 - coord);\n"
			"\n"
			"  float fDist = RectSDF(v_v2CenteredPos, u_v2HalfShapeSizePx, u_fRadiusPx - u_fHalfBorderThickness);\n"
			"  if (u_fHalfBorderThickness > 0.0) {\n"
			"    if (fDist < 0.0) {\n"
			"      v4ToColor = u_v4FillColor;\n"
			"    }\n"
			"    fDist = abs(fDist) - u_fHalfBorderThickness;\n"
			"  } else {\n"
			"    v4FromColor = u_v4FillColor;\n"
			"  }\n"
			"  float fBlendAmount = smoothstep(-1.0, 1.0, fDist);\n"
			"  vec4 c = mix(v4FromColor, v4ToColor, fBlendAmount);"
			"\n"
			"  // final color\n"
			"  //if ( c == vec4(0.0,0.0,0.0,0.0) ) discard; else\n"
			"  gl_FragColor = c;\n"
			"}\n";
			
		// Fragment shader (round corners)
		// dst1 shader
		static const char *FRAG_SHADER_ROUND_CORNERS_1 =
			"  float u_fRadiusPx = u_radius;\n"
			"  float u_fHalfBorderThickness = u_borderw / 2.0;\n"
			"  //float u_fHalfBorderThickness = 20.0 /2.0;\n"
			"  //u_v4FillColor = vec4(0.0, 1.0, 0.0, 1.0);\n"
			"  //v4FromColor = u_v4BorderColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
			"  //v4ToColor = vec4(0.0, 0.0, 1.0, 1.0); //Outside color\n"
			"\n"
			"  vec2 u_v2HalfShapeSizePx = u_texsize/2.0 - vec2(u_fHalfBorderThickness);\n"
			"  vec2 v_v2CenteredPos = (gl_FragCoord.xy - u_texsize.xy / 2.0 - coord);\n"
			"\n"
			"  float fDist = RectSDF(v_v2CenteredPos, u_v2HalfShapeSizePx, u_fRadiusPx - u_fHalfBorderThickness);\n"
			"  if (u_fHalfBorderThickness > 0.0) {\n"
			"    if (fDist < 0.0) {\n"
			"      v4ToColor = u_v4FillColor;\n"
			"    }\n"
			"    fDist = abs(fDist) - u_fHalfBorderThickness;\n"
			"  } else {\n"
			"    v4FromColor = u_v4FillColor;\n"
			"  }\n"
			"  float fBlendAmount = smoothstep(-1.0, 1.0, fDist);\n"
			"  vec4 c = mix(v4FromColor, v4ToColor, fBlendAmount);"
			"\n"
			"  // final color\n"
			"  //if ( c == vec4(0.0,0.0,0.0,0.0) ) discard; else\n"
			"  gl_FragColor = c;\n"
			"  //gl_FragColor = vec4(vec3(fBlendAmount), 1.0);\n"
			"  //gl_FragColor = vec4(vec3(abs(dist) / (2.0 * corner)), 1.0);\n"
			"}\n";

		const bool use_texture_rect = !ps->psglx->has_texture_non_power_of_two;
		const char *sampler_type = (use_texture_rect ? "sampler2DRect": "sampler2D");
		const char *texture_func = (use_texture_rect ? "texture2DRect": "texture2D");
		char *extension = NULL;
		if (use_texture_rect) {
			mstrextend(&extension, "#extension GL_ARB_texture_rectangle : "
			                       "require\n");
		}
		if (!extension) {
			extension = strdup("");
		}

		if (!glx_init_frag_shader_corners(&ps->psglx->round_passes[0], 0,
									FRAG_SHADER_PREFIX, FRAG_SHADER_ROUND_CORNERS_0,
									extension, sampler_type, texture_func)) {

										log_error("Failed to create rounded corners fragment shader PRE.");
										setlocale(LC_NUMERIC, lc_numeric_old);
										free(lc_numeric_old);
										free(extension);
										return false;
									}

		if (!glx_init_frag_shader_corners(&ps->psglx->round_passes[1], 1,
									FRAG_SHADER_PREFIX, FRAG_SHADER_ROUND_CORNERS_1,
									extension, sampler_type, texture_func)) {

										log_error("Failed to create rounded corners fragment shader POST.");
										setlocale(LC_NUMERIC, lc_numeric_old);
										free(lc_numeric_old);
										free(extension);
										return false;
									}

		free(extension);

		// Restore LC_NUMERIC
		setlocale(LC_NUMERIC, lc_numeric_old);
		free(lc_numeric_old);
	}

	gl_check_err();

	return true;
}

/**
 * Initialize GLX rounded corners filter.
 */
bool glx_init_rounded_corners(session_t *ps) {
	char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
	// Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
	// Thanks to hiciu for reporting.
	setlocale(LC_NUMERIC, "C");

	static const char *FRAG_SHADER =
	    "#version 110\n"
	    "%s"        // extensions
	    "uniform float u_radius;\n"
	    "uniform float u_borderw;\n"
	    "uniform vec4 u_borderc;\n"
	    "uniform vec2 u_texcoord;\n"
	    "uniform vec2 u_texsize;\n"
	    "uniform vec2 u_resolution;\n"
	    "uniform %s tex_scr;\n"        // sampler2D | sampler2DRect
	    "\n"
	    "// https://www.shadertoy.com/view/ltS3zW\n"
	    "float RectSDF(vec2 p, vec2 b, float r) {\n"
	    "  vec2 d = abs(p) - b + vec2(r);\n"
	    "  return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - r;\n"
	    "}\n\n"
	    "void main()\n"
	    "{\n"
	    "  vec2 coord = vec2(u_texcoord.x, "
	    "u_resolution.y-u_texsize.y-u_texcoord.y);\n"
	    "  vec4 u_v4WndBgColor = %s(tex_scr, vec2(gl_TexCoord[0].st));\n"
	    "  float u_fRadiusPx = u_radius;\n"
	    "  float u_fHalfBorderThickness = u_borderw / 2.0;\n"
	    "  vec4 u_v4BorderColor = u_borderc;\n"
	    "  vec4 u_v4FillColor = vec4(0.0, 0.0, 0.0, 0.0);\n"
	    "  vec4 v4FromColor = u_v4BorderColor;	//Always the border "
	    "color. If no border, this still should be set\n"
	    "  vec4 v4ToColor = u_v4WndBgColor;		//Outside color is the "
	    "background texture\n"
	    "\n"
	    "  vec2 u_v2HalfShapeSizePx = u_texsize/2.0 - "
	    "vec2(u_fHalfBorderThickness);\n"
	    "  vec2 v_v2CenteredPos = (gl_FragCoord.xy - u_texsize.xy / 2.0 - "
	    "coord);\n"
	    "\n"
	    "  float fDist = RectSDF(v_v2CenteredPos, u_v2HalfShapeSizePx, "
	    "u_fRadiusPx - u_fHalfBorderThickness);\n"
	    "  if (u_fHalfBorderThickness > 0.0) {\n"
	    "    if (fDist < 0.0) {\n"
	    "      v4ToColor = u_v4FillColor;\n"
	    "    }\n"
	    "    fDist = abs(fDist) - u_fHalfBorderThickness;\n"
	    "  } else {\n"
	    "    v4FromColor = u_v4FillColor;\n"
	    "  }\n"
	    "  float fBlendAmount = smoothstep(-1.0, 1.0, fDist);\n"
	    "  vec4 c = mix(v4FromColor, v4ToColor, fBlendAmount);\n"
	    "\n"
	    "  // final color\n"
	    "  gl_FragColor = c;\n"
	    "\n"
	    "}\n";

	const bool use_texture_rect = !ps->psglx->has_texture_non_power_of_two;
	const char *sampler_type = (use_texture_rect ? "sampler2DRect" : "sampler2D");
	const char *texture_func = (use_texture_rect ? "texture2DRect" : "texture2D");
	char *extension = NULL;
	if (use_texture_rect) {
		mstrextend(&extension, "#extension GL_ARB_texture_rectangle : "
		                       "require\n");
	}
	if (!extension) {
		extension = strdup("");
	}

	bool success = false;
	// Build rounded corners shader
	auto ppass = ps->psglx->round_passes;
	auto len = strlen(FRAG_SHADER) + strlen(extension) + strlen(sampler_type) +
	           strlen(texture_func) + 1;
	char *shader_str = ccalloc(len, char);

	sprintf(shader_str, FRAG_SHADER, extension, sampler_type, texture_func);
	assert(strlen(shader_str) < len);

	log_debug("Generated rounded corners shader:\n%s\n", shader_str);

	ppass->frag_shader = gl_create_shader(GL_FRAGMENT_SHADER, shader_str);
	free(shader_str);

	if (!ppass->frag_shader) {
		log_error("Failed to create rounded corners fragment shader.");
		goto out;
	}

	// Build program
	ppass->prog = gl_create_program(&ppass->frag_shader, 1);
	if (!ppass->prog) {
		log_error("Failed to create GLSL program.");
		goto out;
	}

	// Get uniform addresses
#define P_GET_UNIFM_LOC(name, target)                                                    \
	{                                                                                \
		ppass->target = glGetUniformLocation(ppass->prog, name);                 \
		if (ppass->target < 0) {                                                 \
			log_debug("Failed to get location of rounded corners uniform "   \
			          "'" name "'. Might be troublesome.");                  \
		}                                                                        \
	}
	P_GET_UNIFM_LOC("u_radius", unifm_radius);
	P_GET_UNIFM_LOC("u_texcoord", unifm_texcoord);
	P_GET_UNIFM_LOC("u_texsize", unifm_texsize);
	P_GET_UNIFM_LOC("u_borderw", unifm_borderw);
	P_GET_UNIFM_LOC("u_borderc", unifm_borderc);
	P_GET_UNIFM_LOC("u_resolution", unifm_resolution);
	P_GET_UNIFM_LOC("tex_scr", unifm_tex_scr);
#undef P_GET_UNIFM_LOC

	success = true;

out:
	free(extension);

	// Restore LC_NUMERIC
	setlocale(LC_NUMERIC, lc_numeric_old);
	free(lc_numeric_old);

	gl_check_err();

	return success;
}

/**
 * Load a GLSL main program from shader strings.
 */
bool glx_load_prog_main(const char *vshader_str, const char *fshader_str,
                        glx_prog_main_t *pprogram) {
	assert(pprogram);

	// Build program
	pprogram->prog = gl_create_program_from_str(vshader_str, fshader_str);
	if (!pprogram->prog) {
		log_error("Failed to create GLSL program.");
		return false;
	}

	// Get uniform addresses
#define P_GET_UNIFM_LOC(name, target)                                                    \
	{                                                                                \
		pprogram->target = glGetUniformLocation(pprogram->prog, name);           \
		if (pprogram->target < 0) {                                              \
			log_error("Failed to get location of uniform '" name             \
			          "'. Might be troublesome.");                           \
		}                                                                        \
	}
	P_GET_UNIFM_LOC("opacity", unifm_opacity);
	P_GET_UNIFM_LOC("invert_color", unifm_invert_color);
	P_GET_UNIFM_LOC("tex", unifm_tex);
	P_GET_UNIFM_LOC("time", unifm_time);
#undef P_GET_UNIFM_LOC

	gl_check_err();

	return true;
}

static inline void glx_copy_region_to_tex(session_t *ps, GLenum tex_tgt, int basex,
                                          int basey, int dx, int dy, int width, int height) {
	if (width > 0 && height > 0) {
		glCopyTexSubImage2D(tex_tgt, 0, dx - basex, dy - basey, dx,
		                    ps->root_height - dy - height, width, height);
	}
}

static inline GLuint glx_gen_texture(GLenum tex_tgt, int width, int height) {
	GLuint tex = 0;
	glGenTextures(1, &tex);
	if (!tex) {
		return 0;
	}
	glEnable(tex_tgt);
	glBindTexture(tex_tgt, tex);
	glTexParameteri(tex_tgt, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(tex_tgt, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(tex_tgt, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(tex_tgt, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(tex_tgt, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(tex_tgt, 0);

	return tex;
}

/**
 * Bind an OpenGL texture and fill it with pixel data from back buffer
 */
bool glx_bind_texture(session_t *ps attr_unused, glx_texture_t **pptex, int x, int y,
                      int width, int height) {
	if (ps->o.backend != BKEND_GLX && ps->o.backend != BKEND_XR_GLX_HYBRID) {
		return true;
	}

	glx_texture_t *ptex = *pptex;

	// log_trace("Copying xy(%d %d) wh(%d %d) ptex(%p)", x, y, width, height, ptex);

	// Release texture if parameters are inconsistent
	if (ptex && ptex->texture && (ptex->width != width || ptex->height != height)) {
		free_texture(ps, &ptex);
	}

	// Allocate structure
	if (!ptex) {
		ptex = ccalloc(1, glx_texture_t);
		*pptex = ptex;

		ptex->width = width;
		ptex->height = height;
		ptex->target = GL_TEXTURE_RECTANGLE;
		if (ps->psglx->has_texture_non_power_of_two) {
			ptex->target = GL_TEXTURE_2D;
		}
	}

	// Create texture
	if (!ptex->texture) {
		ptex->texture = glx_gen_texture(ptex->target, width, height);
	}
	if (!ptex->texture) {
		log_error("Failed to allocate texture.");
		return false;
	}

	// Read destination pixels into a texture
	glEnable(ptex->target);
	glBindTexture(ptex->target, ptex->texture);
	if (width > 0 && height > 0) {
		glx_copy_region_to_tex(ps, ptex->target, x, y, x, y, width, height);
	}

	// Cleanup
	glBindTexture(ptex->target, 0);
	glDisable(ptex->target);

	gl_check_err();

	return true;
}

/**
 * @brief Release binding of a texture.
 */
void glx_release_texture(session_t *ps attr_unused, glx_texture_t **pptex) {
	glx_texture_t *ptex = *pptex;
	// Release binding
	if (ptex->texture) {
		//log_info("Deleting texture wh(%d %d)", ptex->width, ptex->height);
		glBindTexture(ptex->target, 0);
		glDeleteTextures(1, &ptex->texture);
	}
	free(ptex);
	*pptex = NULL;

	gl_check_err();
}

/**
 * Bind a X pixmap to an OpenGL texture.
 */
bool glx_bind_texture(session_t *ps attr_unused, glx_texture_t **pptex,
				int x, int y, int width attr_unused, int height attr_unused, bool repeat attr_unused) {
	if (ps->o.backend != BKEND_GLX && ps->o.backend != BKEND_XR_GLX_HYBRID)
		return true;

	glx_texture_t *ptex = *pptex;

	//log_trace("Copying xy(%d %d) wh(%d %d) ptex(%p)", x, y, width, height, ptex);

	// Release texture if parameters are inconsistent
	if (ptex && ptex->texture &&
		(ptex->width != width || ptex->height != height)) {
		//log_info("Windows size changed old_wh(%d %d) new_wh(%d %d)", ptex->width, ptex->height, width, height);
		glx_release_texture(ps, &ptex);
	}

	// Allocate structure
	if (!ptex) {
		static const glx_texture_t GLX_TEX_DEF = {
		    .texture = 0,
		    .glpixmap = 0,
		    .pixmap = 0,
		    .target = 0,
		    .width = 0,
		    .height = 0,
		    .y_inverted = false,
		};

		ptex = cmalloc(glx_texture_t);
		memcpy(ptex, &GLX_TEX_DEF, sizeof(glx_texture_t));
		*pptex = ptex;

		ptex->width = width;
		ptex->height = height;
		ptex->target = GL_TEXTURE_RECTANGLE;
		if (ps->psglx->has_texture_non_power_of_two)
			ptex->target = GL_TEXTURE_2D;
	}

	// Create texture
	if (!ptex->texture) {
		//log_info("Generating texture for xy(%d %d) wh(%d %d)", x, y, width, height);
		GLuint texture = 0;
		glGenTextures(1, &texture);

		if (texture) {
			glEnable(ptex->target);
			glBindTexture(ptex->target, texture);

			glTexParameteri(ptex->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(ptex->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			if (repeat) {
				glTexParameteri(ptex->target, GL_TEXTURE_WRAP_S, GL_REPEAT);
				glTexParameteri(ptex->target, GL_TEXTURE_WRAP_T, GL_REPEAT);
			} else {
				glTexParameteri(ptex->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(ptex->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}

			glTexImage2D(ptex->target, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

			glBindTexture(ptex->target, 0);
			//glDisable(ptex->target);
		}

		ptex->texture = texture;
	}
	if (!ptex->texture) {
		log_error("Failed to allocate texture.");
		return false;
	}

	// Read destination pixels into a texture
	glEnable(ptex->target);
	glBindTexture(ptex->target, ptex->texture);
	if (width > 0 && height > 0)
		glCopyTexSubImage2D(ptex->target, 0, 0, 0, x, ps->root_height - y - height, width, height);

	// Cleanup
	glBindTexture(ptex->target, 0);
	glDisable(ptex->target);

	gl_check_err();

	return true;
}


/**
 * Bind a X pixmap to an OpenGL texture.
 */
bool glx_bind_pixmap(session_t *ps, glx_texture_t **pptex, xcb_pixmap_t pixmap, int width,
                     int height, bool repeat, const struct glx_fbconfig_info *fbcfg) {
	if (ps->o.backend != BKEND_GLX && ps->o.backend != BKEND_XR_GLX_HYBRID)
		return true;

	if (!pixmap) {
		log_error("Binding to an empty pixmap %#010x. This can't work.", pixmap);
		return false;
	}

	assert(fbcfg);
	glx_texture_t *ptex = *pptex;
	bool need_release = true;

	// Release pixmap if parameters are inconsistent
	if (ptex && ptex->texture && ptex->pixmap != pixmap) {
		glx_release_pixmap(ps, ptex);
	}

	// Allocate structure
	if (!ptex) {
		static const glx_texture_t GLX_TEX_DEF = {
		    .texture = 0,
		    .glpixmap = 0,
		    .pixmap = 0,
		    .target = 0,
		    .width = 0,
		    .height = 0,
		    .y_inverted = false,
		};

		ptex = cmalloc(glx_texture_t);
		memcpy(ptex, &GLX_TEX_DEF, sizeof(glx_texture_t));
		*pptex = ptex;
	}

	// Create GLX pixmap
	int depth = 0;
	if (!ptex->glpixmap) {
		need_release = false;

		// Retrieve pixmap parameters, if they aren't provided
		if (!width || !height) {
			auto r = xcb_get_geometry_reply(
			    ps->c, xcb_get_geometry(ps->c, pixmap), NULL);
			if (!r) {
				log_error("Failed to query info of pixmap %#010x.", pixmap);
				return false;
			}
			if (r->depth > OPENGL_MAX_DEPTH) {
				log_error("Requested depth %d higher than %d.", depth,
				          OPENGL_MAX_DEPTH);
				return false;
			}
			depth = r->depth;
			width = r->width;
			height = r->height;
			free(r);
		}

		// Determine texture target, copied from compiz
		// The assumption we made here is the target never changes based on any
		// pixmap-specific parameters, and this may change in the future
		GLenum tex_tgt = 0;
		if (GLX_TEXTURE_2D_BIT_EXT & fbcfg->texture_tgts &&
		    ps->psglx->has_texture_non_power_of_two)
			tex_tgt = GLX_TEXTURE_2D_EXT;
		else if (GLX_TEXTURE_RECTANGLE_BIT_EXT & fbcfg->texture_tgts)
			tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
		else if (!(GLX_TEXTURE_2D_BIT_EXT & fbcfg->texture_tgts))
			tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
		else
			tex_tgt = GLX_TEXTURE_2D_EXT;

		log_debug("depth %d, tgt %#x, rgba %d", depth, tex_tgt,
		          (GLX_TEXTURE_FORMAT_RGBA_EXT == fbcfg->texture_fmt));

		GLint attrs[] = {
		    GLX_TEXTURE_FORMAT_EXT,
		    fbcfg->texture_fmt,
		    GLX_TEXTURE_TARGET_EXT,
		    (GLint)tex_tgt,
		    0,
		};

		ptex->glpixmap = glXCreatePixmap(ps->dpy, fbcfg->cfg, pixmap, attrs);
		ptex->pixmap = pixmap;
		ptex->target =
		    (GLX_TEXTURE_2D_EXT == tex_tgt ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE);
		ptex->width = width;
		ptex->height = height;
		ptex->y_inverted = fbcfg->y_inverted;
	}
	if (!ptex->glpixmap) {
		log_error("Failed to allocate GLX pixmap.");
		return false;
	}

	glEnable(ptex->target);

	// Create texture
	if (!ptex->texture) {
		need_release = false;

		GLuint texture = 0;
		glGenTextures(1, &texture);
		glBindTexture(ptex->target, texture);

		glTexParameteri(ptex->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(ptex->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		if (repeat) {
			glTexParameteri(ptex->target, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(ptex->target, GL_TEXTURE_WRAP_T, GL_REPEAT);
		} else {
			glTexParameteri(ptex->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(ptex->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}

		glBindTexture(ptex->target, 0);

		ptex->texture = texture;
	}
	if (!ptex->texture) {
		log_error("Failed to allocate texture.");
		return false;
	}

	glBindTexture(ptex->target, ptex->texture);

	// The specification requires rebinding whenever the content changes...
	// We can't follow this, too slow.
	if (need_release)
		glXReleaseTexImageEXT(ps->dpy, ptex->glpixmap, GLX_FRONT_LEFT_EXT);

	glXBindTexImageEXT(ps->dpy, ptex->glpixmap, GLX_FRONT_LEFT_EXT, NULL);

	// Cleanup
	glBindTexture(ptex->target, 0);
	glDisable(ptex->target);

	gl_check_err();

	return true;
}

/**
 * @brief Release binding of a texture.
 */
void glx_release_pixmap(session_t *ps, glx_texture_t *ptex) {
	// Release binding
	if (ptex->glpixmap && ptex->texture) {
		glBindTexture(ptex->target, ptex->texture);
		glXReleaseTexImageEXT(ps->dpy, ptex->glpixmap, GLX_FRONT_LEFT_EXT);
		glBindTexture(ptex->target, 0);
	}

	// Free GLX Pixmap
	if (ptex->glpixmap) {
		glXDestroyPixmap(ps->dpy, ptex->glpixmap);
		ptex->glpixmap = 0;
	}

	gl_check_err();
}

/**
 * Set clipping region on the target window.
 */
void glx_set_clip(session_t *ps, const region_t *reg) {
	// Quit if we aren't using stencils
	if (ps->o.glx_no_stencil)
		return;

	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);

	if (!reg)
		return;

	int nrects;
	const rect_t *rects = pixman_region32_rectangles((region_t *)reg, &nrects);

	if (nrects == 1) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(rects[0].x1, ps->root_height - rects[0].y2,
		          rects[0].x2 - rects[0].x1, rects[0].y2 - rects[0].y1);
	}

	gl_check_err();
}

#define P_PAINTREG_START(var)                                                            \
	region_t reg_new;                                                                \
	int nrects;                                                                      \
	const rect_t *rects;                                                             \
	assert(width >= 0 && height >= 0);                                               \
	pixman_region32_init_rect(&reg_new, dx, dy, (uint)width, (uint)height);          \
	pixman_region32_intersect(&reg_new, &reg_new, (region_t *)reg_tgt);              \
	rects = pixman_region32_rectangles(&reg_new, &nrects);                           \
	glBegin(GL_QUADS);                                                               \
                                                                                         \
	for (int ri = 0; ri < nrects; ++ri) {                                            \
		rect_t var = rects[ri];

#define P_PAINTREG_END()                                                                 \
	}                                                                                \
	glEnd();                                                                         \
                                                                                         \
	pixman_region32_fini(&reg_new);

<<<<<<< HEAD
=======
static inline GLuint glx_gen_texture(GLenum tex_tgt, int width, int height) {
	GLuint tex = 0;
	glGenTextures(1, &tex);
	if (!tex)
		return 0;
	glEnable(tex_tgt);
	glBindTexture(tex_tgt, tex);
	glTexParameteri(tex_tgt, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(tex_tgt, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(tex_tgt, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(tex_tgt, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(tex_tgt, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(tex_tgt, 0);

	return tex;
}

static inline void glx_copy_region_to_tex(session_t *ps, GLenum tex_tgt, int basex,
                                          int basey, int dx, int dy, int width, int height) {
	if (width > 0 && height > 0)
		glCopyTexSubImage2D(tex_tgt, 0, dx - basex, dy - basey, dx,
		                    ps->root_height - dy - height, width, height);
}

static inline void
glx_copy_region_to_tex_new(session_t *ps, GLenum tex_tgt, int basex, int basey, int width, int height) {
  if (width > 0 && height > 0) {
    int dx = (basex < 0) ? 0 : basex;
    basey = ps->root_height - (basey + height);
    int dy = (basey < 0) ? 0 : basey;

    width += basex;
    width = (ps->root_width < width) ? ps->root_width - dx : width - dx;
    height += basey;
    height = (ps->root_height < height) ? ps->root_height - dy : height - dy;

    glCopyTexSubImage2D(tex_tgt, 0, (basex < 0) ? 0 : dx, dy, dx, dy, width, height);
  }
}

>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
/**
 * Blur contents in a particular region.
 *
 * XXX seems to be way to complex for what it does
 */
bool glx_conv_blur_dst(session_t *ps, int dx, int dy, int width, int height, float z,
                  double opacity, const region_t *reg_tgt, glx_blur_cache_t *pbc) {
	assert(ps->psglx->blur_passes[0].prog);
	const bool more_passes = ps->o.blur_kernel_count > 1;
	const bool have_scissors = glIsEnabled(GL_SCISSOR_TEST);
	const bool have_stencil = glIsEnabled(GL_STENCIL_TEST);
	bool ret = false;

	// Calculate copy region size
	glx_blur_cache_t ibc = {.width = 0, .height = 0};
	if (!pbc)
		pbc = &ibc;

	int mdx = dx, mdy = dy, mwidth = width, mheight = height;
	// log_trace("%d, %d, %d, %d", mdx, mdy, mwidth, mheight);

	/*
	if (ps->o.resize_damage > 0) {
	  int inc_x = 0, inc_y = 0;
	  for (int i = 0; i < MAX_BLUR_PASS; ++i) {
	    XFixed *kern = ps->o.blur_kerns[i];
	    if (!kern) break;
	    inc_x += XFIXED_TO_DOUBLE(kern[0]) / 2;
	    inc_y += XFIXED_TO_DOUBLE(kern[1]) / 2;
	  }
	  inc_x = min2(ps->o.resize_damage, inc_x);
	  inc_y = min2(ps->o.resize_damage, inc_y);

	  mdx = max2(dx - inc_x, 0);
	  mdy = max2(dy - inc_y, 0);
	  int mdx2 = min2(dx + width + inc_x, ps->root_width),
	      mdy2 = min2(dy + height + inc_y, ps->root_height);
	  mwidth = mdx2 - mdx;
	  mheight = mdy2 - mdy;
	}
	*/

	GLenum tex_tgt = GL_TEXTURE_RECTANGLE;
	if (ps->psglx->has_texture_non_power_of_two)
		tex_tgt = GL_TEXTURE_2D;

	// Free textures if size inconsistency discovered
	if (mwidth != pbc->width[0] || mheight != pbc->height[0])
		free_glx_bc_resize(ps, pbc);

	// Generate FBO and textures if needed
	if (!pbc->textures[0])
		pbc->textures[0] = glx_gen_texture(tex_tgt, mwidth, mheight);
	GLuint tex_scr = pbc->textures[0];
	if (more_passes && !pbc->textures[1])
		pbc->textures[1] = glx_gen_texture(tex_tgt, mwidth, mheight);
	pbc->width[0] = mwidth;
	pbc->height[0] = mheight;
	GLuint tex_scr2 = pbc->textures[1];
	if (more_passes && !pbc->fbos[0])
		glGenFramebuffers(1, &pbc->fbos[0]);
	const GLuint fbo = pbc->fbos[0];

	if (!tex_scr || (more_passes && !tex_scr2)) {
		log_error("Failed to allocate texture.");
		goto glx_conv_blur_dst_end;
	}
	if (more_passes && !fbo) {
		log_error("Failed to allocate framebuffer.");
		goto glx_conv_blur_dst_end;
	}

	// Read destination pixels into a texture
	glEnable(tex_tgt);
	glBindTexture(tex_tgt, tex_scr);
	glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, mdy, mwidth, mheight);
	//glx_copy_region_to_tex_new(ps, tex_tgt, mdx, mdy, mwidth, mheight);
	/*
	if (tex_scr2) {
	  glBindTexture(tex_tgt, tex_scr2);
	  glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, mdy, mwidth, dx - mdx);
	  glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, dy + height,
	      mwidth, mdy + mheight - dy - height);
	  glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, dy, dx - mdx, height);
	  glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, dx + width, dy,
	      mdx + mwidth - dx - width, height);
	} */

	// Texture scaling factor
	GLfloat texfac_x = 1.0f, texfac_y = 1.0f;
	if (tex_tgt == GL_TEXTURE_2D) {
		texfac_x /= (GLfloat)mwidth;
		texfac_y /= (GLfloat)mheight;
	}

	// Paint it back
	if (more_passes) {
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_SCISSOR_TEST);
	}

	bool last_pass = false;
	for (int i = 0; i < ps->o.blur_kernel_count; ++i) {
		last_pass = (i == ps->o.blur_kernel_count - 1);
		const glx_blur_pass_t *ppass = &ps->psglx->blur_passes[i];
		assert(ppass->prog);

		assert(tex_scr);
		glBindTexture(tex_tgt, tex_scr);

		if (!last_pass) {
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			                       GL_TEXTURE_2D, tex_scr2, 0);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				log_error("Framebuffer attachment failed.");
				goto glx_conv_blur_dst_end;
			}
		} else {
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDrawBuffer(GL_BACK);
			if (have_scissors)
				glEnable(GL_SCISSOR_TEST);
			if (have_stencil)
				glEnable(GL_STENCIL_TEST);

			if (opacity < 1.0) { // Blend blur texture to fade in and out with window opacity
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}
		}

		// Color negation for testing...
		// glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		// glTexEnvf(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
		// glTexEnvf(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);

		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glUseProgram(ppass->prog);
		if (ppass->unifm_offset_x >= 0)
			glUniform1f(ppass->unifm_offset_x, texfac_x);
		if (ppass->unifm_offset_y >= 0)
			glUniform1f(ppass->unifm_offset_y, texfac_y);
		if (ppass->unifm_opacity >= 0)
			glUniform1f(ppass->unifm_opacity, (float)opacity);

		P_PAINTREG_START(crect) {
			auto rx = (GLfloat)(crect.x1 - mdx) * texfac_x;
			auto ry = (GLfloat)(mheight - (crect.y1 - mdy)) * texfac_y;
			auto rxe = rx + (GLfloat)(crect.x2 - crect.x1) * texfac_x;
			auto rye = ry - (GLfloat)(crect.y2 - crect.y1) * texfac_y;
			auto rdx = (GLfloat)(crect.x1 - mdx);
			auto rdy = (GLfloat)(mheight - crect.y1 + mdy);
			if (last_pass) {
				rdx = (GLfloat)crect.x1;
				rdy = (GLfloat)(ps->root_height - crect.y1);
			}
			auto rdxe = rdx + (GLfloat)(crect.x2 - crect.x1);
			auto rdye = rdy - (GLfloat)(crect.y2 - crect.y1);

			// log_trace("%f, %f, %f, %f -> %f, %f, %f, %f", rx, ry,
			// rxe, rye, rdx,
			//          rdy, rdxe, rdye);

			glTexCoord2f(rx, ry);
			glVertex3f(rdx, rdy, z);

			glTexCoord2f(rxe, ry);
			glVertex3f(rdxe, rdy, z);

			glTexCoord2f(rxe, rye);
			glVertex3f(rdxe, rdye, z);

			glTexCoord2f(rx, rye);
			glVertex3f(rdx, rdye, z);
		}
		P_PAINTREG_END();

		glUseProgram(0);

		// Swap tex_scr and tex_scr2
		{
			GLuint tmp = tex_scr2;
			tex_scr2 = tex_scr;
			tex_scr = tmp;
		}
	}

	ret = true;

glx_conv_blur_dst_end:
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(tex_tgt, 0);
	glDisable(tex_tgt);
	if (have_scissors)
		glEnable(GL_SCISSOR_TEST);
	if (have_stencil)
		glEnable(GL_STENCIL_TEST);

	glDisable(GL_BLEND);

	if (&ibc == pbc) {
		free_glx_bc(ps, pbc);
	}

	gl_check_err();

	return ret;
}

/**
 * Blur contents in a particular region using the dual-filter kawase blur.
 */
bool
glx_dualkawase_blur_dst(session_t *ps, int dx, int dy, int width, int height, float z attr_unused,
				double opacity attr_unused, const region_t *reg_tgt attr_unused, glx_blur_cache_t *wpbc attr_unused) {
	assert(ps->psglx->blur_passes[0].prog);
	assert(ps->psglx->blur_passes[1].prog);
	const bool have_scissors = glIsEnabled(GL_SCISSOR_TEST);
	const bool have_stencil = glIsEnabled(GL_STENCIL_TEST);
	bool ret = false;
	
	int iterations = ps->o.blur_strength.iterations;
	float offset = ps->o.blur_strength.offset;
	int expand = ps->o.blur_strength.expand;
  
  	// Calculate copy region size
	int mdx = dx - expand, mdy = dy - expand, mwidth = width + 2 * expand, mheight = height + 2 * expand;
#ifdef DEBUG_GLX
	log_debug("%d, %d, %d, %d\n", mdx, mdy, mwidth, mheight);
#endif

	glx_blur_cache_t *psbc = &ps->psglx->blur_cache;
	//glx_blur_cache_t *psbc = wpbc;

	GLenum tex_tgt = GL_TEXTURE_RECTANGLE;
	if (ps->psglx->has_texture_non_power_of_two)
		tex_tgt = GL_TEXTURE_2D;

	// Shrink blur_strength.iterations to still have at least 1px left
	while ((width / (1 << (iterations-1))) < 1 || (height / (1 << (iterations-1))) < 1)
		--iterations;
	assert(iterations < MAX_BLUR_PASS);


	// Allocate textures if needed and bind to the respective framebuffer
	if (!psbc->fbos[0]) {

		log_warn("Allocating blur_cache [iter:%d] for dxy(%d:%d) wh(%d:%d)", iterations, dx, dy, width, height);

		glGenFramebuffers(iterations, psbc->fbos);
		if (!psbc->fbos[0]) {
			log_error("Failed to generate Framebuffer. Cannot do "
						"multi-pass blur with GLX backend.");
			goto glx_dualkawase_blur_dst_end;
		}

		int tex_width;
		int tex_height;
		for (int i = 0; i <= iterations; ++i) {
			if (!psbc->textures[i]) {
				tex_width = ps->root_width / (1 << (i));
				tex_height = ps->root_height / (1 << (i));
				psbc->textures[i] = glx_gen_texture(tex_tgt, tex_width, tex_height);
				psbc->width[i] = tex_width;
				psbc->height[i] = tex_height;
			}
			if (!psbc->textures[i]) {
				log_error("Failed to allocate texture.");
				goto glx_dualkawase_blur_dst_end;
			}

			// Bind texture to framebuffer
			if ((i > 0) && psbc->fbos[i-1]) {
				static const GLenum DRAWBUFS[2] = { GL_COLOR_ATTACHMENT0 };
				glBindFramebuffer(GL_FRAMEBUFFER, psbc->fbos[i-1]);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
									   GL_TEXTURE_2D, psbc->textures[i], 0);
				glDrawBuffers(1, DRAWBUFS);
				if (glCheckFramebufferStatus(GL_FRAMEBUFFER)
					!= GL_FRAMEBUFFER_COMPLETE) {
					log_error("Framebuffer attachment failed.");
					glBindFramebuffer(GL_FRAMEBUFFER, 0);
					goto glx_dualkawase_blur_dst_end;
				}
			}

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// Check for FBO and textures
	GLuint tex_scr = psbc->textures[0];
	if (!tex_scr) {
		log_error("Blur cache texture not allocated.");
		goto glx_dualkawase_blur_dst_end;
	}

	for (int i = 1; i <= iterations; i++) {
		if (!psbc->textures[i]) {
			log_error("Blur cache texture not allocated.");
			goto glx_dualkawase_blur_dst_end;
		}
		if (!psbc->fbos[i - 1]) {
			log_error("Blur cache framebuffer not allocated.");
			goto glx_dualkawase_blur_dst_end;
		}
	}

	// Read destination pixels into a texture
	glEnable(tex_tgt);
	glBindTexture(tex_tgt, tex_scr);
	glx_copy_region_to_tex_new(ps, tex_tgt, mdx, mdy, mwidth, mheight);

	// Paint it back
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);

	// First pass: Kawase Downsample
	const glx_blur_pass_t *down_pass = &ps->psglx->blur_passes[0];
	for (int i = 1; i <= iterations; i++) {
		const int dest_width = psbc->width[i], dest_height = psbc->height[i];
		GLuint tex_src2 = psbc->textures[i - 1];
		GLuint fbo = psbc->fbos[i - 1];

		//assert(tex_src2);
		//assert(fbo);
		glBindTexture(tex_tgt, tex_src2);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);

		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glUseProgram(down_pass->prog);
		if (down_pass->unifm_offset >= 0)
			glUniform1f(down_pass->unifm_offset, offset);
		if (down_pass->unifm_halfpixel >= 0)
			glUniform2f(down_pass->unifm_halfpixel, (GLfloat)0.5 / (GLfloat)dest_width, (GLfloat)0.5 / (GLfloat)dest_height);
		if (down_pass->unifm_fulltex >= 0)
			glUniform2f(down_pass->unifm_fulltex, (GLfloat)dest_width, (GLfloat)dest_height);

		// Start actual rendering
		P_PAINTREG_START(crect) {
			int w = (crect.x2 - crect.x1) + 2 * expand;
			int h = (crect.y2 - crect.y1) + 2 * expand;
			crect.x1 -= expand; crect.y1 -= expand;
			//crect.width += 2 * expand; crect.height += 2 * expand;

			const GLfloat rx = (GLfloat)(crect.x1);
			const GLfloat ry = (GLfloat)ps->root_height - (GLfloat)(crect.y1);
			const GLfloat rxe = rx + (GLfloat)(w);
			const GLfloat rye = ry - (GLfloat)(h);
			GLfloat rdx = rx / (GLfloat)(1 << i);
			GLfloat rdy = ry / (GLfloat)(1 << i);
			GLfloat rdxe = rxe / (GLfloat)(1 << i);
			GLfloat rdye = rye / (GLfloat)(1 << i);


#ifdef DEBUG_GLX
			log_info("Downsample Pass %d xy(%d:%d) wh(%d:%d) dwh(%d:%d):\n\t%.2f, %.2f, %.2f, %.2f -> %.2f, %.2f, %.2f, %.2f\n",
					i, dx, dy, width, height, dest_width, dest_height, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);
#endif

			glTexCoord2f(rx, ry);
			glVertex3f(rdx, rdy, z);

			glTexCoord2f(rxe, ry);
			glVertex3f(rdxe, rdy, z);

			glTexCoord2f(rxe, rye);
			glVertex3f(rdxe, rdye, z);

			glTexCoord2f(rx, rye);
			glVertex3f(rdx, rdye, z);
		}
		P_PAINTREG_END();
	}

	// Second pass: Kawase Upsample
	const glx_blur_pass_t *up_pass = &ps->psglx->blur_passes[1];
	for (int i = iterations; i >= 1; i--) {
		const int dest_width = psbc->width[i - 1], dest_height = psbc->height[i - 1];
		GLuint tex_src2 = psbc->textures[i];
		//assert(tex_src2);
		glBindTexture(tex_tgt, tex_src2);

		if (i != 1) { // is not last pass
			GLuint fbo = psbc->fbos[i - 2];
			//assert(fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		} else { // last pass -> render to screen
			static const GLenum DRAWBUFS[2] = { GL_BACK };
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDrawBuffers(1, DRAWBUFS);
			if (have_scissors)
				glEnable(GL_SCISSOR_TEST);
			if (have_stencil)
				glEnable(GL_STENCIL_TEST);

			if (opacity < 1.0) { // Blend blur texture to fade in and out with window opacity
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}
		}

		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glUseProgram(up_pass->prog);
		if (up_pass->unifm_offset >= 0)
			glUniform1f(up_pass->unifm_offset, offset);
		if (up_pass->unifm_opacity >= 0)
			glUniform1f(up_pass->unifm_opacity, (float)opacity);
		if (up_pass->unifm_halfpixel >= 0)
			glUniform2f(up_pass->unifm_halfpixel, (GLfloat)0.5 / (GLfloat)dest_width, (GLfloat)0.5 / (GLfloat)dest_height);
		if (up_pass->unifm_fulltex >= 0)
			glUniform2f(up_pass->unifm_fulltex, (GLfloat)dest_width, (GLfloat)dest_height);

		// Start actual rendering
		P_PAINTREG_START(crect) {
			int w = (crect.x2 - crect.x1);
			int h = (crect.y2 - crect.y1);
			const GLfloat rx = (GLfloat)(crect.x1 - expand);
			const GLfloat ry = (GLfloat)ps->root_height - (GLfloat)(crect.y2) - (GLfloat)expand;
			//const GLfloat ry = (GLfloat)ps->root_height - (GLfloat)(crect.y1+h) - (GLfloat)expand;
			const GLfloat rxe = rx + (GLfloat)w + (GLfloat)(2 * expand);
			const GLfloat rye = ry + (GLfloat)h + (GLfloat)(2 * expand);
			GLfloat rdx;
			GLfloat rdy;
			GLfloat rdxe;
			GLfloat rdye;

			if (i != 1) { // is not last pass
				rdx = rx / (GLfloat)(1 << (i-1));
				rdy = ry / (GLfloat)(1 << (i-1));
				rdxe = rxe / (GLfloat)(1 << (i-1));
				rdye = rye / (GLfloat)(1 << (i-1));
			} else { // last pass -> render to screen coordinates
				rdx = (GLfloat)crect.x1;
				rdy = (GLfloat)ps->root_height - ((GLfloat)crect.y2);
				//rdy = (GLfloat)ps->root_height - ((GLfloat)crect.y1 + (GLfloat)h);
				rdxe = rdx + (GLfloat)w;
				rdye = rdy + (GLfloat)h;
			}

#ifdef DEBUG_GLX
			log_info("Upsample Pass %d xy(%d:%d) wh(%d:%d):\n\t%.2f, %.2f, %.2f, %.2f -> %.2f, %.2f, %.2f, %.2f\n",
					i, dx, dy, width, height, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);
#endif

			glTexCoord2f(rx, ry);
			glVertex3f(rdx, rdy, z);

			glTexCoord2f(rxe, ry);
			glVertex3f(rdxe, rdy, z);

			glTexCoord2f(rxe, rye);
			glVertex3f(rdxe, rdye, z);

			glTexCoord2f(rx, rye);
			glVertex3f(rdx, rdye, z);
		}
		P_PAINTREG_END();
	}

	glUseProgram(0);
	ret = true;

glx_dualkawase_blur_dst_end:
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(tex_tgt, 0);
	glDisable(tex_tgt);
	if (have_scissors)
		glEnable(GL_SCISSOR_TEST);
	if (have_stencil)
		glEnable(GL_STENCIL_TEST);

	glDisable(GL_BLEND);

	//if (&ibc == pbc) { free_glx_bc(ps, pbc); }

	gl_check_err();

	return ret;
}


bool glx_kawase_blur_dst(session_t *ps, int dx, int dy, int width, int height, float z attr_unused,
                  double opacity attr_unused, const region_t *reg_tgt attr_unused, glx_blur_cache_t *pbc) {
	assert(ps->psglx->blur_passes[0].prog);
	const bool have_scissors = glIsEnabled(GL_SCISSOR_TEST);
	const bool have_stencil = glIsEnabled(GL_STENCIL_TEST);
	bool ret = false;

	int iterations = ps->o.blur_strength.iterations;
	float offset = ps->o.blur_strength.offset;

	// Calculate copy region size
	glx_blur_cache_t ibc = { .width = 0, .height = 0 };
	if (!pbc)
		pbc = &ibc;

	int mdx = dx, mdy = dy, mwidth = width, mheight = height;
#ifdef DEBUG_GLX
	log_debug("%d, %d, %d, %d\n", mdx, mdy, mwidth, mheight);
#endif

	GLenum tex_tgt = GL_TEXTURE_RECTANGLE;
	if (ps->psglx->has_texture_non_power_of_two)
		tex_tgt = GL_TEXTURE_2D;

	// Free textures if size inconsistency discovered
	if (mwidth != pbc->width[0] || mheight != pbc->height[0])
		free_glx_bc_resize(ps, pbc);

	// Generate FBO and textures if needed
	if (!pbc->textures[0])
		pbc->textures[0] = glx_gen_texture(tex_tgt, mwidth, mheight);
	GLuint tex_scr = pbc->textures[0];

	// Check if we can scale down blur_strength.iterations
	while ((mwidth / (1 << (iterations-1))) < 1 || (mheight / (1 << (iterations-1))) < 1)
		--iterations;
	
	assert(iterations < MAX_BLUR_PASS);
	for (int i = 1; i <= iterations; i++) {
		if (!pbc->textures[i])
			pbc->textures[i] = glx_gen_texture(tex_tgt, mwidth / (1 << (i-1)), mheight / (1 << (i-1)));
	}

	pbc->width[0] = mwidth;
	pbc->height[0] = mheight;

	if (!pbc->fbos[0])
		glGenFramebuffers(1, &pbc->fbos[0]);
	const GLuint fbo = pbc->fbos[0];

	if (!tex_scr) {
		log_error("Failed to allocate texture.");
		goto glx_kawase_blur_dst_end;
	}
	for (int i = 1; i <= iterations; i++) {
		if (!pbc->textures[i]) {
			log_error("Failed to allocate additional textures.");
			goto glx_kawase_blur_dst_end;
		}
	}
	if (!fbo) {
		log_error("Failed to allocate framebuffer.");
		goto glx_kawase_blur_dst_end;
	}

	// Read destination pixels into a texture
	glEnable(tex_tgt);
	glBindTexture(tex_tgt, tex_scr);
	glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, mdy, mwidth, mheight);
	//glx_copy_region_to_tex_new(ps, tex_tgt, mdx, mdy, mwidth, mheight);

	// Paint it back
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);

	// First pass(es): Kawase Downsample
	for (int i = 1; i <= iterations; ++i) {
		const glx_blur_pass_t *down_pass = &ps->psglx->blur_passes[0];
		assert(down_pass->prog);

		int tex_width = mwidth / (1 << (i-1)), tex_height = mheight / (1 << (i-1));
		GLuint tex_src2 = pbc->textures[i - 1];
		GLuint tex_dest = pbc->textures[i];

		assert(tex_src2);
		assert(tex_dest);
		glBindTexture(tex_tgt, tex_src2);

		//static const GLenum DRAWBUFS[2] = { GL_COLOR_ATTACHMENT0 };
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
							   GL_TEXTURE_2D, tex_dest, 0);
		glDrawBuffer(GL_COLOR_ATTACHMENT0);
		//glDrawBuffers(1, DRAWBUFS);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			log_error("Framebuffer attachment failed.");
			goto glx_kawase_blur_dst_end;
		}

		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glUseProgram(down_pass->prog);
		if (down_pass->unifm_offset >= 0)
			glUniform1f(down_pass->unifm_offset, offset);
		if (down_pass->unifm_halfpixel >= 0)
			glUniform2f(down_pass->unifm_halfpixel, (float)(0.5 / tex_width), (float)(0.5 / tex_height));
		if (down_pass->unifm_fulltex >= 0)
			glUniform2f(down_pass->unifm_fulltex, (float)tex_width, (float)tex_height);

		// Start actual rendering
		P_PAINTREG_START(crect) {
			auto rx = (GLfloat)(crect.x1 - mdx);
			auto ry = (GLfloat)(mheight - (crect.y1 - mdy));
			auto rxe = rx + (GLfloat)(crect.x2 - crect.x1);
			auto rye = ry - (GLfloat)(crect.y2 - crect.y1);

		#ifdef DEBUG_GLX
			log_debug("Downsample Pass %d: %f, %f, %f, %f -> %f, %f, %f, %f\n", i, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);
		#endif

			glTexCoord2f(rx, ry);
			glVertex3f(rx, ry, z);

			glTexCoord2f(rxe, ry);
			glVertex3f(rxe, ry, z);

			glTexCoord2f(rxe, rye);
			glVertex3f(rxe, rye, z);

			glTexCoord2f(rx, rye);
			glVertex3f(rx, rye, z);
		}
		P_PAINTREG_END();

		glUseProgram(0);
	}


	// Second pass(es): Kawase Upsample
	for (int i = iterations; i >= 1; i--) {
		const glx_blur_pass_t *up_pass = &ps->psglx->blur_passes[1];
		bool is_last = (i == 1);
		assert(up_pass->prog);

		int tex_width = mwidth / (1 << (i-2)), tex_height = mheight / (1 << (i-2));
		if (is_last) {
			tex_width = mwidth, tex_height = mheight;
		}
		GLuint tex_src2 = pbc->textures[i];
		GLuint tex_dest = pbc->textures[i - 1];

		assert(tex_src2);
		assert(tex_dest);
		glBindTexture(tex_tgt, tex_src2);

		if (!is_last) {
			//static const GLenum DRAWBUFS[2] = { GL_COLOR_ATTACHMENT0 };
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
								   GL_TEXTURE_2D, tex_dest, 0);
			//glDrawBuffers(1, DRAWBUFS);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				log_error("Framebuffer attachment failed.");
				goto glx_kawase_blur_dst_end;
			}
		} else {
			//static const GLenum DRAWBUFS[2] = { GL_BACK };
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDrawBuffer(GL_BACK);
			//glDrawBuffers(1, DRAWBUFS);
			if (have_scissors)
				glEnable(GL_SCISSOR_TEST);
			if (have_stencil)
				glEnable(GL_STENCIL_TEST);
		}

		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glUseProgram(up_pass->prog);
		if (up_pass->unifm_offset >= 0)
			glUniform1f(up_pass->unifm_offset, offset);
		if (up_pass->unifm_halfpixel >= 0)
			glUniform2f(up_pass->unifm_halfpixel, (float)(0.5 / tex_width), (float)(0.5 / tex_height));
		if (up_pass->unifm_fulltex >= 0)
			glUniform2f(up_pass->unifm_fulltex, (float)tex_width, (float)tex_height);

		// Start actual rendering
		P_PAINTREG_START(crect) {
			auto rx = (GLfloat)(crect.x1 - mdx);
			auto ry = (GLfloat)(mheight - (crect.y1 - mdy));
			auto rxe = rx + (GLfloat)(crect.x2 - crect.x1);
			auto rye = ry - (GLfloat)(crect.y2 - crect.y1);
			auto rdx = (GLfloat)(crect.x1 - mdx);
			auto rdy = (GLfloat)(mheight - crect.y1 + mdy);
			if (is_last) {
				rdx = (GLfloat)crect.x1;
				rdy = (GLfloat)(ps->root_height - crect.y1);
			}
			auto rdxe = rdx + (GLfloat)(crect.x2 - crect.x1);
			auto rdye = rdy - (GLfloat)(crect.y2 - crect.y1);

		#ifdef DEBUG_GLX
			log_debug("Upsample Pass %d: %f, %f, %f, %f -> %f, %f, %f, %f\n", i, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);
		#endif

			glTexCoord2f(rx, ry);
			glVertex3f(rdx, rdy, z);

			glTexCoord2f(rxe, ry);
			glVertex3f(rdxe, rdy, z);

			glTexCoord2f(rxe, rye);
			glVertex3f(rdxe, rdye, z);

			glTexCoord2f(rx, rye);
			glVertex3f(rdx, rdye, z);
		}
		P_PAINTREG_END();

		glUseProgram(0);
	}

	//glUseProgram(0);

	ret = true;

glx_kawase_blur_dst_end:
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(tex_tgt, 0);
	glDisable(tex_tgt);
	if (have_scissors)
		glEnable(GL_SCISSOR_TEST);
	if (have_stencil)
		glEnable(GL_STENCIL_TEST);

	if (&ibc == pbc) {
		free_glx_bc(ps, pbc);
	}

	gl_check_err();

	return ret;
}

<<<<<<< HEAD
// TODO(bhagwan) this is a mess and needs a more consistent way of getting the border
// pixel I tried looking for a notify event for XCB_CW_BORDER_PIXEL (in
// xcb_create_window()) or a way to get the pixels from xcb_render_picture_t but the
// documentation for the xcb_xrender extension is literaly non existent...
//
// NOTE(yshui) There is no consistent way to get the "border" color of a X window. From
// the WM's perspective there are multiple ways to implement window borders. Using
// glReadPixel is probably the most reliable way.
void glx_read_border_pixel(int root_height, int root_width, int x, int y, int width,
                           int height, float *ppixel) {
	assert(ppixel);

	// Reset the color so the shader doesn't use it
	ppixel[0] = ppixel[1] = ppixel[2] = ppixel[3] = -1.0F;

	// First try bottom left corner past the
	// circle radius (after the rounded corner ends)
	auto screen_x = x;
	auto screen_y = root_height - height - y;

	// X is out of bounds
	// move to the right side
	if (screen_x < 0) {
		screen_x += width;
	}

	// Y is out of bounds
	// move to to top part
	if (screen_y < 0) {
		screen_y += height;
	}

	// All corners are out of bounds, give up
	if (screen_x < 0 || screen_y < 0 || screen_x >= root_width || screen_y >= root_height) {
		return;
	}

	// Invert Y-axis so we can query border color from texture (0,0)
	glReadPixels(screen_x, screen_y, 1, 1, GL_RGBA, GL_FLOAT, (void *)ppixel);

	log_trace("xy(%d, %d), glxy(%d %d) wh(%d %d), border_col(%.2f, %.2f, %.2f, %.2f)",
	          x, y, screen_x, screen_y, width, height, (float)ppixel[0],
	          (float)ppixel[1], (float)ppixel[2], (float)ppixel[3]);

	gl_check_err();
}

bool glx_round_corners_dst(session_t *ps, struct managed_win *w,
                           const glx_texture_t *ptex, int dx, int dy, int width,
                           int height, float z, float cr, const region_t *reg_tgt) {
	assert(ps->psglx->round_passes->prog);
	bool ret = false;

	// log_warn("dxy(%d, %d) wh(%d %d) rwh(%d %d) b(%d), f(%d)",
	//	dx, dy, width, height, ps->root_width, ps->root_height, w->g.border_width,
	// w->focused);

	int mdx = dx, mdy = dy, mwidth = width, mheight = height;
	log_trace("%d, %d, %d, %d", mdx, mdy, mwidth, mheight);

	if (w->g.border_width > 0) {
		glx_read_border_pixel(ps->root_height, ps->root_width, dx, dy, width,
		                      height, &w->border_col[0]);
	}

	{
		const glx_round_pass_t *ppass = ps->psglx->round_passes;
		assert(ppass->prog);

		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

		glUseProgram(ppass->prog);

		// If caller specified a texture use it as source
		log_trace("ptex: %p wh(%d %d) %d %d", ptex, ptex->width, ptex->height,
		          ptex->target, ptex->texture);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(ptex->target, ptex->texture);

		if (ppass->unifm_tex_scr >= 0) {
			glUniform1i(ppass->unifm_tex_scr, (GLint)0);
		}
		if (ppass->unifm_radius >= 0) {
			glUniform1f(ppass->unifm_radius, cr);
		}
		if (ppass->unifm_texcoord >= 0) {
			glUniform2f(ppass->unifm_texcoord, (float)dx, (float)dy);
		}
		if (ppass->unifm_texsize >= 0) {
			glUniform2f(ppass->unifm_texsize, (float)mwidth, (float)mheight);
		}
		if (ppass->unifm_borderw >= 0) {
			// Don't render rounded border if we don't know the border color
			glUniform1f(ppass->unifm_borderw,
			            w->border_col[0] != -1. ? (GLfloat)w->g.border_width : 0);
		}
		if (ppass->unifm_borderc >= 0) {
			glUniform4f(ppass->unifm_borderc, w->border_col[0],
			            w->border_col[1], w->border_col[2], w->border_col[3]);
		}
		if (ppass->unifm_resolution >= 0) {
			glUniform2f(ppass->unifm_resolution, (float)ps->root_width,
			            (float)ps->root_height);
		}
=======
bool glx_blur_dst(session_t *ps, int dx, int dy, int width, int height, float z,
                  double opacity, const region_t *reg_tgt, glx_blur_cache_t *pbc) {
  assert(ps->psglx->blur_passes[0].prog);

  bool ret;
  switch (ps->o.blur_method) {
	case BLUR_METHOD_DUAL_KAWASE:
		ret = glx_kawase_blur_dst(ps, dx, dy, width, height, z,
									opacity, reg_tgt, pbc);
      break;
	case BLUR_METHOD_ALT_KAWASE:
		ret = glx_dualkawase_blur_dst(ps, dx, dy, width, height, z,
									opacity, reg_tgt, pbc);
      break;
    case BLUR_METHOD_KERNEL:
	case BLUR_METHOD_BOX:
	case BLUR_METHOD_GAUSSIAN:
      ret = glx_conv_blur_dst(ps, dx, dy, width, height, z, opacity, reg_tgt, pbc);
      break;
    default:
      ret = false;
      break;
  }

  gl_check_err();

  return ret;
}


// TODO: this is a mess and needs a more consistent way of getting the border pixel
// I tried looking for a notify event for XCB_CW_BORDER_PIXEL (in xcb_create_window())
// or a way to get the pixels from xcb_render_picture_t but the documentation for 
// the xcb_xrender extension is literaly non existent...
bool glx_read_border_pixel(struct managed_win *w, int root_height, int x, int y,
						int width attr_unused, int height, int cr, float *ppixel)
{
	if (!ppixel) return false;

	// First try bottom left corner past the
	// circle radius (after the rounded corner ends)
	auto openglx = x + cr*2;
	auto opengly = root_height-height-y;

	// X is out of bounds
	// move to the right side
	if (openglx < 0)
		openglx = x + width - cr;

	// Y is out of bounds
	// move to to top part
	if (opengly < 0) {
		opengly += height-1;
	}

	// bottom left corner is out of bounds
	// use top border line instead
	if (openglx < 0 || opengly < 0) {

		//log_warn("OUT OF BOUNDS: xy(%d, %d), glxy(%d %d) wh(%d %d), border_col(%.2f, %.2f, %.2f, %.2f)",
		//	x, y, openglx, opengly, width, height,
		//	(float)w->border_col[0], (float)w->border_col[1], (float)w->border_col[2], (float)w->border_col[3]);

		// Reset the color so the shader doesn't use it
		w->border_col[0] = w->border_col[1] = w->border_col[2] = w->border_col[3] = -1.0;
	}

	// Invert Y-axis so we can query border color from texture (0,0)
	glReadPixels((openglx < 0) ? 0 : openglx, (opengly < 0) ? 0 : opengly, 1, 1,
				GL_RGBA, GL_FLOAT, (void*)&w->border_col[0]);

	//log_warn("xy(%d, %d), glxy(%d %d) wh(%d %d), border_col(%.2f, %.2f, %.2f, %.2f)",
	//	x, y, openglx, opengly, width, height,
	//	(float)w->border_col[0], (float)w->border_col[1], (float)w->border_col[2], (float)w->border_col[3]);
	
	gl_check_err();
	
	return true;
}

bool glx_round_corners_dst0(session_t *ps, struct managed_win *w, const glx_texture_t *ptex attr_unused, int shader_idx,
				int dx, int dy, int width, int height, float z, float cr,
				const region_t *reg_tgt attr_unused, glx_blur_cache_t *pbc) {

	assert(shader_idx >= 0 && shader_idx <= 1);
	assert(ps->psglx->round_passes[0].prog);
	assert(ps->psglx->round_passes[1].prog);
	const bool have_scissors = glIsEnabled(GL_SCISSOR_TEST);
	const bool have_stencil = glIsEnabled(GL_STENCIL_TEST);
	bool ret = false;

	//log_warn("dxy(%d, %d) wh(%d %d) rwh(%d %d) bw(%d)",
	//	dx, dy, width, height, ps->root_width, ps->root_height, w->g.border_width);

	if (w->g.border_width >= 1 /*&& w->border_col[0] == -1.0*/) {
		glx_read_border_pixel(w, ps->root_height, dx, dy, width, height, w->corner_radius, &w->border_col[0]);
	}

	// Calculate copy region size
	glx_blur_cache_t ibc = {.width = 0, .height = 0};
	if (!pbc)
		pbc = &ibc;

	int mdx = dx, mdy = dy, mwidth = width, mheight = height;
	// log_trace("%d, %d, %d, %d", mdx, mdy, mwidth, mheight);

	GLenum tex_tgt = GL_TEXTURE_RECTANGLE;
	if (ps->psglx->has_texture_non_power_of_two)
		tex_tgt = GL_TEXTURE_2D;

	// Free textures if size inconsistency discovered
	if (mwidth != pbc->width[0] || mheight != pbc->height[0])
		free_glx_bc_resize(ps, pbc);

	// Generate FBO and textures if needed
	if (!pbc->textures[0])
		pbc->textures[0] = glx_gen_texture(tex_tgt, mwidth, mheight);
	GLuint tex_scr = pbc->textures[0];

	pbc->width[0] = mwidth;
	pbc->height[0] = mheight;

	if (!tex_scr) {
		log_error("Failed to allocate texture.");
		goto glx_round_corners_dst_end;
	}

	// Read destination pixels into a texture
	glEnable(tex_tgt);
	glBindTexture(tex_tgt, tex_scr);
	glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, mdy, mwidth, mheight);

	// Texture scaling factor
	GLfloat texfac_x = 1.0f, texfac_y = 1.0f;
	if (tex_tgt == GL_TEXTURE_2D) {
		texfac_x /= (GLfloat)mwidth;
		texfac_y /= (GLfloat)mheight;
	}

	// Paint it back
	{
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_SCISSOR_TEST);
	}

	{
		const glx_round_pass_t *ppass = &ps->psglx->round_passes[shader_idx];
		assert(ppass->prog);

		assert(tex_scr);

		glActiveTexture(GL_TEXTURE1); 
		glBindTexture(tex_tgt, tex_scr);

		// If caller specified a texture use it as source
		if (ptex) {
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(ptex->target, ptex->texture);
		} else {
			glActiveTexture(GL_TEXTURE0); 
			glBindTexture(tex_tgt, tex_scr);
		}

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDrawBuffer(GL_BACK);
		if (have_scissors)
			glEnable(GL_SCISSOR_TEST);
		if (have_stencil)
			glEnable(GL_STENCIL_TEST);


		// Our shader generates a transparent mid section
		// with opaque corners copied from the background texture
		// We must use blending to get the window pixesl to appear
		//glDisable(GL_BLEND);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

		glUseProgram(ppass->prog);

		if (ppass->unifm_tex_scr >= 0)
			glUniform1i(ppass->unifm_tex_scr, (GLint)0);
		if (ppass->unifm_tex_wnd >= 0)
			glUniform1i(ppass->unifm_tex_wnd, (GLint)1);

		if (ppass->unifm_radius >= 0)
			glUniform1f(ppass->unifm_radius, cr);
		if (ppass->unifm_texcoord >= 0)
			glUniform2f(ppass->unifm_texcoord, (float)dx, (float)dy);
		if (ppass->unifm_texsize >= 0)
			glUniform2f(ppass->unifm_texsize, (float)mwidth, (float)mheight);
		if (ppass->unifm_borderw >= 0)
			glUniform1f(ppass->unifm_borderw, (w->round_borders && w->border_col[0] != -1.) ? w->g.border_width : 0);
		if (ppass->unifm_borderc >= 0)
			glUniform4fv(ppass->unifm_borderc, 1, (GLfloat *)&w->border_col[0]);
		if (ppass->unifm_resolution >= 0)
			glUniform2f(ppass->unifm_resolution, (float)ps->root_width, (float)ps->root_height);
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45

		// Painting
		{
			P_PAINTREG_START(crect) {
<<<<<<< HEAD
				// texture-local coordinates
=======
				// XXX explain these variables
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
				auto rx = (GLfloat)(crect.x1 - dx);
				auto ry = (GLfloat)(crect.y1 - dy);
				auto rxe = rx + (GLfloat)(crect.x2 - crect.x1);
				auto rye = ry + (GLfloat)(crect.y2 - crect.y1);
<<<<<<< HEAD
				if (GL_TEXTURE_2D == ptex->target) {
=======
				// Rectangle textures have [0-w] [0-h] while 2D texture has [0-1]
				// [0-1] Thanks to amonakov for pointing out!
				if (GL_TEXTURE_2D == tex_tgt) {
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
					rx = rx / (GLfloat)width;
					ry = ry / (GLfloat)height;
					rxe = rxe / (GLfloat)width;
					rye = rye / (GLfloat)height;
				}
<<<<<<< HEAD

				// coordinates for the texture in the target
=======
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
				auto rdx = (GLfloat)crect.x1;
				auto rdy = (GLfloat)(ps->root_height - crect.y1);
				auto rdxe = (GLfloat)rdx + (GLfloat)(crect.x2 - crect.x1);
				auto rdye = (GLfloat)rdy - (GLfloat)(crect.y2 - crect.y1);

<<<<<<< HEAD
				// Invert Y if needed, this may not work as expected,
				// though. I don't have such a FBConfig to test with.
				ry = 1.0F - ry;
				rye = 1.0F - rye;

				// log_trace("Rect %d (i:%d): %f, %f, %f, %f -> %f, %f,
				// %f, %f", 	ri ,ptex ? ptex->y_inverted : -1, rx, ry,
				// rxe,
				// rye, rdx, rdy, rdxe, rdye);
=======
				// Invert Y if needed, this may not work as expected, though. I
				// don't have such a FBConfig to test with.
				//if (ptex && !ptex->y_inverted) {
				{
					ry = 1.0f - ry;
					rye = 1.0f - rye;
				}

				//log_trace("Rect %d (i:%d): %f, %f, %f, %f -> %f, %f, %f, %f",
				//	ri ,ptex ? ptex->y_inverted : -1, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45

				glTexCoord2f(rx, ry);
				glVertex3f(rdx, rdy, z);

				glTexCoord2f(rxe, ry);
				glVertex3f(rdxe, rdy, z);

				glTexCoord2f(rxe, rye);
				glVertex3f(rdxe, rdye, z);

				glTexCoord2f(rx, rye);
				glVertex3f(rdx, rdye, z);
<<<<<<< HEAD
=======

>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
			}
			P_PAINTREG_END();
		}

		glUseProgram(0);
		glDisable(GL_BLEND);
	}

	ret = true;

<<<<<<< HEAD
	glBindTexture(ptex->target, 0);
	glDisable(ptex->target);
	glDisable(GL_BLEND);
=======
glx_round_corners_dst_end:
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(tex_tgt, 0);
	glDisable(tex_tgt);
	if (have_scissors)
		glEnable(GL_SCISSOR_TEST);
	if (have_stencil)
		glEnable(GL_STENCIL_TEST);

	if (&ibc == pbc) {
		free_glx_bc(ps, pbc);
	}

	gl_check_err();

	return ret;
}

bool glx_round_corners_dst1(session_t *ps, struct managed_win *w, const glx_texture_t *ptex, int shader_idx,
				int dx, int dy, int width, int height, float z, float cr,
				const region_t *reg_tgt attr_unused, glx_blur_cache_t *pbc attr_unused) {

	assert(shader_idx >= 0 && shader_idx <= 1);
	assert(ps->psglx->round_passes[0].prog);
	assert(ps->psglx->round_passes[1].prog);
	bool ret = false;
	
	if (w->g.border_width >= 1 /*&& w->border_col[0] == -1.0*/) {
		glx_read_border_pixel(w, ps->root_height, dx, dy, width, height, w->corner_radius, &w->border_col[0]);
	}

	{
		const glx_round_pass_t *ppass = &ps->psglx->round_passes[shader_idx];
		assert(ppass->prog);

		// If caller specified a texture use it as source
		if (ptex) {
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(ptex->target, ptex->texture);
		}

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		glUseProgram(ppass->prog);

		if (ppass->unifm_tex_scr >= 0)
			glUniform1i(ppass->unifm_tex_scr, (GLint)0);
		// We have no GL_TEXTURE1 here so just pass the default
		if (ppass->unifm_tex_wnd >= 0)
			glUniform1i(ppass->unifm_tex_wnd, (GLint)0);

		if (ppass->unifm_radius >= 0)
			glUniform1f(ppass->unifm_radius, cr);
		if (ppass->unifm_texcoord >= 0)
			glUniform2f(ppass->unifm_texcoord, (float)dx, (float)dy);
		if (ppass->unifm_texsize >= 0)
			glUniform2f(ppass->unifm_texsize, (float)width, (float)height);
		if (ppass->unifm_borderw >= 0)
			glUniform1f(ppass->unifm_borderw, (w->round_borders && w->border_col[0] != -1.) ? w->g.border_width : 0);
		if (ppass->unifm_borderc >= 0)
			glUniform4fv(ppass->unifm_borderc, 1, (GLfloat *)&w->border_col[0]);
		if (ppass->unifm_resolution >= 0)
			glUniform2f(ppass->unifm_resolution, (float)ps->root_width, (float)ps->root_height);

		// Painting
		{
			P_PAINTREG_START(crect) {
				// XXX explain these variables
				auto rx = (GLfloat)(crect.x1 - dx);
				auto ry = (GLfloat)(crect.y1 - dy);
				auto rxe = rx + (GLfloat)(crect.x2 - crect.x1);
				auto rye = ry + (GLfloat)(crect.y2 - crect.y1);
				// Rectangle textures have [0-w] [0-h] while 2D texture has [0-1]
				// [0-1] Thanks to amonakov for pointing out!
				if (GL_TEXTURE_2D == ptex->target) {
					rx = rx / (GLfloat)width;
					ry = ry / (GLfloat)height;
					rxe = rxe / (GLfloat)width;
					rye = rye / (GLfloat)height;
				}
				auto rdx = (GLfloat)crect.x1;
				auto rdy = (GLfloat)(ps->root_height - crect.y1);
				auto rdxe = (GLfloat)rdx + (GLfloat)(crect.x2 - crect.x1);
				auto rdye = (GLfloat)rdy - (GLfloat)(crect.y2 - crect.y1);

				// Invert Y if needed, this may not work as expected, though. I
				// don't have such a FBConfig to test with.
				//if (ptex && !ptex->y_inverted) {
				{
					ry = 1.0f - ry;
					rye = 1.0f - rye;
				}

				//log_trace("Rect %d (i:%d): %f, %f, %f, %f -> %f, %f, %f, %f",
				//	ri ,ptex ? ptex->y_inverted : -1, rx, ry, rxe, rye, rdx, rdy, rdxe, rdye);

				glTexCoord2f(rx, ry);
				glVertex3f(rdx, rdy, z);

				glTexCoord2f(rxe, ry);
				glVertex3f(rdxe, rdy, z);

				glTexCoord2f(rxe, rye);
				glVertex3f(rdxe, rdye, z);

				glTexCoord2f(rx, rye);
				glVertex3f(rdx, rdye, z);

			}
			P_PAINTREG_END();
		}

		glUseProgram(0);
		glDisable(GL_BLEND);
	}

	ret = true;

	//glBindFramebuffer(GL_FRAMEBUFFER, 0);
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45

	gl_check_err();

	return ret;
}

bool glx_dim_dst(session_t *ps, int dx, int dy, int width, int height, int z,
                 GLfloat factor, const region_t *reg_tgt) {
	// It's possible to dim in glx_render(), but it would be over-complicated
	// considering all those mess in color negation and modulation
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(0.0f, 0.0f, 0.0f, factor);

	P_PAINTREG_START(crect) {
		// XXX what does all of these variables mean?
		GLint rdx = crect.x1;
		GLint rdy = ps->root_height - crect.y1;
		GLint rdxe = rdx + (crect.x2 - crect.x1);
		GLint rdye = rdy - (crect.y2 - crect.y1);

		glVertex3i(rdx, rdy, z);
		glVertex3i(rdxe, rdy, z);
		glVertex3i(rdxe, rdye, z);
		glVertex3i(rdx, rdye, z);
	}
	P_PAINTREG_END();

	glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
	glDisable(GL_BLEND);

	gl_check_err();

	return true;
}

/**
 * @brief Render a region with texture data.
 */
bool glx_render(session_t *ps, struct managed_win *w attr_unused, const glx_texture_t *ptex,
				int x, int y, int dx, int dy, int width, int height, int z, double opacity, bool argb,
				bool neg, int cr attr_unused, const region_t *reg_tgt, const glx_prog_main_t *pprogram) {
	if (!ptex || !ptex->texture) {
		log_error("Missing texture.");
		return false;
	}

	const bool has_prog = pprogram && pprogram->prog;
	bool dual_texture = false;

	// It's required by legacy versions of OpenGL to enable texture target
	// before specifying environment. Thanks to madsy for telling me.
	glEnable(ptex->target);

	// Enable blending if needed
	if (opacity < 1.0 || argb || cr > 0) {

		glEnable(GL_BLEND);

		// Needed for handling opacity of ARGB texture
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

		// This is all weird, but X Render is using premultiplied ARGB format, and
		// we need to use those things to correct it. Thanks to derhass for help.
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glColor4d(opacity, opacity, opacity, opacity);
	}

	if (!has_prog) {
		// The default, fixed-function path
		// Color negation
		if (neg) {
			// Simple color negation
			if (!glIsEnabled(GL_BLEND)) {
				glEnable(GL_COLOR_LOGIC_OP);
				glLogicOp(GL_COPY_INVERTED);
			}
			// ARGB texture color negation
			else if (argb) {
				dual_texture = true;

				// Use two texture stages because the calculation is too
				// complicated, thanks to madsy for providing code Texture
				// stage 0
				glActiveTexture(GL_TEXTURE0);

				// Negation for premultiplied color: color = A - C
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_SUBTRACT);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_ALPHA);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

				// Pass texture alpha through
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);

				// Texture stage 1
				glActiveTexture(GL_TEXTURE1);
				glEnable(ptex->target);
				glBindTexture(ptex->target, ptex->texture);

				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);

				// Modulation with constant factor
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_ALPHA);

				// Modulation with constant factor
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);

				glActiveTexture(GL_TEXTURE0);
			}
			// RGB blend color negation
			else {
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);

				// Modulation with constant factor
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB,
				          GL_ONE_MINUS_SRC_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

				// Modulation with constant factor
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
				glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
			}
		}
	} else {
		// Programmable path
		assert(pprogram->prog);
		glUseProgram(pprogram->prog);
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if (pprogram->unifm_opacity >= 0)
			glUniform1f(pprogram->unifm_opacity, (float)opacity);
		if (pprogram->unifm_invert_color >= 0)
			glUniform1i(pprogram->unifm_invert_color, neg);
		if (pprogram->unifm_tex >= 0)
			glUniform1i(pprogram->unifm_tex, 0);
		if (pprogram->unifm_time >= 0)
			glUniform1f(pprogram->unifm_time, (float)ts.tv_sec * 1000.0f +
			                                      (float)ts.tv_nsec / 1.0e6f);
	}

	// log_trace("Draw: %d, %d, %d, %d -> %d, %d (%d, %d) z %d", x, y, width, height,
	//          dx, dy, ptex->width, ptex->height, z);

	// Bind texture
	glBindTexture(ptex->target, ptex->texture);
	if (dual_texture) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(ptex->target, ptex->texture);
		glActiveTexture(GL_TEXTURE0);
	}

	// Painting
	{
		P_PAINTREG_START(crect) {
			// texture-local coordinates
			auto rx = (GLfloat)(crect.x1 - dx + x);
			auto ry = (GLfloat)(crect.y1 - dy + y);
			auto rxe = rx + (GLfloat)(crect.x2 - crect.x1);
			auto rye = ry + (GLfloat)(crect.y2 - crect.y1);
			// Rectangle textures have [0-w] [0-h] while 2D texture has [0-1]
			// [0-1] Thanks to amonakov for pointing out!
			if (GL_TEXTURE_2D == ptex->target) {
				rx = rx / (GLfloat)ptex->width;
				ry = ry / (GLfloat)ptex->height;
				rxe = rxe / (GLfloat)ptex->width;
				rye = rye / (GLfloat)ptex->height;
			}

			// coordinates for the texture in the target
			GLint rdx = crect.x1;
			GLint rdy = ps->root_height - crect.y1;
			GLint rdxe = rdx + (crect.x2 - crect.x1);
			GLint rdye = rdy - (crect.y2 - crect.y1);

			// Invert Y if needed, this may not work as expected, though. I
			// don't have such a FBConfig to test with.
			if (!ptex->y_inverted) {
				ry = 1.0f - ry;
				rye = 1.0f - rye;
			}

			// log_trace("Rect %d: %f, %f, %f, %f -> %d, %d, %d, %d", ri, rx,
			// ry, rxe, rye,
			//          rdx, rdy, rdxe, rdye);

#define P_TEXCOORD(cx, cy)                                                               \
	{                                                                                \
		if (dual_texture) {                                                      \
			glMultiTexCoord2f(GL_TEXTURE0, cx, cy);                          \
			glMultiTexCoord2f(GL_TEXTURE1, cx, cy);                          \
		} else                                                                   \
			glTexCoord2f(cx, cy);                                            \
	}
			P_TEXCOORD(rx, ry);
			glVertex3i(rdx, rdy, z);

			P_TEXCOORD(rxe, ry);
			glVertex3i(rdxe, rdy, z);

			P_TEXCOORD(rxe, rye);
			glVertex3i(rdxe, rdye, z);

			P_TEXCOORD(rx, rye);
			glVertex3i(rdx, rdye, z);
		}
		P_PAINTREG_END();
	}

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

	if (has_prog)
		glUseProgram(0);

	gl_check_err();

	return true;
}
