#include "gl_common.h"
// Don't macro expand bool!
#undef bool
// clang-format off
const char copy_area_frag[] = GLSL(330,
	layout(location = UNIFORM_TEX_LOC)
	uniform sampler2D tex;
	in vec2 texcoord;
	void main() {
		vec2 texsize = textureSize(tex, 0);
		gl_FragColor = texture2D(tex, texcoord / texsize, 0);
	}
);

const char copy_area_with_dither_frag[] = GLSL(330,
	layout(location = UNIFORM_TEX_LOC)
	uniform sampler2D tex;
	in vec2 texcoord;
	vec4 dither(vec4, vec2);
	void main() {
		vec2 texsize = textureSize(tex, 0);
		gl_FragColor = dither(texture2D(tex, texcoord / texsize, 0), gl_FragCoord.xy);
	}
);

const char blend_with_mask_frag[] = GLSL(330,
	layout(location = UNIFORM_TEX_LOC)
	uniform sampler2D tex;
	layout(location = UNIFORM_OPACITY_LOC)
	uniform float opacity;
	in vec2 texcoord;
	float mask_factor();
	void main() {
		gl_FragColor = texelFetch(tex, ivec2(texcoord.xy), 0) * opacity * mask_factor();
	}
);

const char fill_frag[] = GLSL(330,
	layout(location = UNIFORM_COLOR_LOC)
	uniform vec4 color;
	void main() {
		gl_FragColor = color;
	}
);

const char fill_vert[] = GLSL(330,
	layout(location = 0) in vec2 in_coord;
	layout(location = UNIFORM_PROJECTION_LOC)
	uniform mat4 projection;
	void main() {
		gl_Position = projection * vec4(in_coord, 0, 1);
	}
);

const char interpolating_frag[] = GLSL(330,
	layout(location = UNIFORM_TEX_LOC)
	uniform sampler2D tex;
	in vec2 texcoord;
	void main() {
		gl_FragColor = vec4(texture2D(tex, vec2(texcoord.xy), 0).rgb, 1);
	}
);

const char interpolating_vert[] = GLSL(330,
	layout(location = UNIFORM_PROJECTION_LOC)
	uniform mat4 projection;
	layout(location = UNIFORM_TEXSIZE_LOC)
	uniform vec2 texsize;
	layout(location = 0) in vec2 in_coord;
	layout(location = 1) in vec2 in_texcoord;
	out vec2 texcoord;
	void main() {
		gl_Position = projection * vec4(in_coord, 0, 1);
		texcoord = in_texcoord / texsize;
	}
);
const char masking_glsl[] = GLSL(330,
	layout(location = UNIFORM_MASK_TEX_LOC)
	uniform sampler2D mask_tex;
	layout(location = UNIFORM_MASK_OFFSET_LOC)
	uniform vec2 mask_offset;
	layout(location = UNIFORM_MASK_CORNER_RADIUS_LOC)
	uniform float mask_corner_radius;
	layout(location = UNIFORM_MASK_INVERTED_LOC)
	uniform bool mask_inverted;
	in vec2 texcoord;
	float mask_rectangle_sdf(vec2 point, vec2 half_size) {
		vec2 d = abs(point) - half_size;
		return length(max(d, 0.0));
	}
	float mask_factor() {
		vec2 mask_size = textureSize(mask_tex, 0);
		vec2 maskcoord = texcoord - mask_offset;
		vec4 mask = texture2D(mask_tex, maskcoord / mask_size);
		if (mask_corner_radius != 0) {
			vec2 inner_size = mask_size - vec2(mask_corner_radius) * 2.0f;
			float dist = mask_rectangle_sdf(maskcoord - mask_size / 2.0f,
			    inner_size / 2.0f) - mask_corner_radius;
			if (dist > 0.0f) {
				mask.r *= (1.0f - clamp(dist, 0.0f, 1.0f));
			}
		}
		if (mask_inverted) {
			mask.rgb = 1.0 - mask.rgb;
		}
		return mask.r;
	}
);
const char blit_shader_glsl[] = GLSL(330,
	layout(location = UNIFORM_OPACITY_LOC)
	uniform float opacity;
	layout(location = UNIFORM_DIM_LOC)
	uniform float dim;
	layout(location = UNIFORM_CORNER_RADIUS_LOC)
	uniform float corner_radius;
	layout(location = UNIFORM_BORDER_WIDTH_LOC)
	uniform float border_width;
	layout(location = UNIFORM_INVERT_COLOR_LOC)
	uniform bool invert_color;
	in vec2 texcoord;
	layout(location = UNIFORM_TEX_LOC)
	uniform sampler2D tex;
	layout(location = UNIFORM_EFFECTIVE_SIZE_LOC)
	uniform vec2 effective_size;
	layout(location = UNIFORM_BRIGHTNESS_LOC)
	uniform sampler2D brightness;
	layout(location = UNIFORM_MAX_BRIGHTNESS_LOC)
	uniform float max_brightness;
	// Signed distance field for rectangle center at (0, 0), with size of
	// half_size * 2
	float rectangle_sdf(vec2 point, vec2 half_size) {
		vec2 d = abs(point) - half_size;
		return length(max(d, 0.0));
	}

	vec4 default_post_processing(vec4 c) {
		vec4 border_color = texture(tex, vec2(0.0, 0.5));
		if (invert_color) {
			c = vec4(c.aaa - c.rgb, c.a);
			border_color = vec4(border_color.aaa - border_color.rgb, border_color.a);
		}
		c = vec4(c.rgb * (1.0 - dim), c.a) * opacity;
		border_color = vec4(border_color.rgb * (1.0 - dim), border_color.a) * opacity;

		vec3 rgb_brightness = texelFetch(brightness, ivec2(0, 0), 0).rgb;
		// Ref: https://en.wikipedia.org/wiki/Relative_luminance
		float brightness = rgb_brightness.r * 0.21 +
		                   rgb_brightness.g * 0.72 +
		                   rgb_brightness.b * 0.07;
		if (brightness > max_brightness) {
			c.rgb = c.rgb * (max_brightness / brightness);
			border_color.rgb = border_color.rgb * (max_brightness / brightness);
		}

		// Rim color is the color of the outer rim of the window, if there is no
		// border, it's the color of the window itself, otherwise it's the border.
		// Using mix() to avoid a branch here.
		vec4 rim_color = mix(c, border_color, clamp(border_width, 0.0f, 1.0f));

		vec2 outer_size = effective_size;
		vec2 inner_size = outer_size - vec2(corner_radius) * 2.0f;
		float rect_distance = rectangle_sdf(texcoord - outer_size / 2.0f,
		    inner_size / 2.0f) - corner_radius;
		if (rect_distance > 0.0f) {
			c = (1.0f - clamp(rect_distance, 0.0f, 1.0f)) * rim_color;
		} else {
			float factor = clamp(rect_distance + border_width, 0.0f, 1.0f);
			c = (1.0f - factor) * c + factor * border_color;
		}

		return c;
	}

	vec4 window_shader();
	float mask_factor();

	void main() {
		gl_FragColor = window_shader() * mask_factor();
	}
);

const char blit_shader_default[] = GLSL(330,
	in vec2 texcoord;
	uniform sampler2D tex;
	vec4 default_post_processing(vec4 c);
	vec4 window_shader() {
		vec2 texsize = textureSize(tex, 0);
		vec4 c = texture2D(tex, texcoord / texsize, 0);
		return default_post_processing(c);
	}
);

const char vertex_shader[] = GLSL(330,
	layout(location = UNIFORM_PROJECTION_LOC)
	uniform mat4 projection;
	layout(location = UNIFORM_SCALE_LOC)
	uniform float scale = 1.0f;
	layout(location = 0) in vec2 coord;
	layout(location = 1) in vec2 in_texcoord;
	out vec2 texcoord;
	void main() {
		gl_Position = projection * vec4(coord, 0, scale);
		texcoord = in_texcoord;
	}
);
/// Add dithering for downsampling from 16-bit color to 8-bit color.
const char dither_glsl[] = GLSL(330,
	// Stolen from: https://www.shadertoy.com/view/7sfXDn
	float bayer2(vec2 a) {
		a = floor(a);
		return fract(a.x / 2. + a.y * a.y * .75);
	}
	// 16 * 16 is 2^8, so in total we have equivalent of 16-bit
	// color depth, should be enough?
	float bayer(vec2 a16) {
		vec2  a8 = a16 * .5;
		vec2  a4 =  a8 * .5;
		vec2  a2 =  a4 * .5;
		float bayer32 = ((bayer2(a2) * .25 + bayer2( a4))
		                             * .25 + bayer2( a8))
		                             * .25 + bayer2(a16);
		return bayer32;
	}
	vec4 dither(vec4 c, vec2 coord) {
		vec4 residual = mod(c, 1.0 / 255.0);
		residual = min(residual, vec4(1.0 / 255.0) - residual);
		vec4 dithered = vec4(greaterThan(residual, vec4(1.0 / 65535.0)));
		return vec4(c + dithered * bayer(coord) / 255.0);
	}
);
// clang-format on
