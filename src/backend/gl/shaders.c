#include "gl_common.h"

// clang-format off
const char dummy_frag[] = GLSL(330,
	uniform sampler2D tex;
	in vec2 texcoord;
	void main() {
		gl_FragColor = texelFetch(tex, ivec2(texcoord.xy), 0);
	}
);

const char copy_with_mask_frag[] = GLSL(330,
	uniform sampler2D tex;
	in vec2 texcoord;
	float mask_factor();
	void main() {
		gl_FragColor = texelFetch(tex, ivec2(texcoord.xy), 0) * mask_factor();
	}
);

const char fill_frag[] = GLSL(330,
	uniform vec4 color;
	void main() {
		gl_FragColor = color;
	}
);

const char fill_vert[] = GLSL(330,
	layout(location = 0) in vec2 in_coord;
	uniform mat4 projection;
	void main() {
		gl_Position = projection * vec4(in_coord, 0, 1);
	}
);

const char interpolating_frag[] = GLSL(330,
	uniform sampler2D tex;
	in vec2 texcoord;
	void main() {
		gl_FragColor = vec4(texture2D(tex, vec2(texcoord.xy), 0).rgb, 1);
	}
);

const char interpolating_vert[] = GLSL(330,
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
const char masking_glsl[] = GLSL(330,
	uniform sampler2D mask_tex;
	uniform vec2 mask_offset;
	uniform float mask_corner_radius;
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
const char win_shader_glsl[] = GLSL(330,
	uniform float opacity;
	uniform float dim;
	uniform float corner_radius;
	uniform float border_width;
	uniform bool invert_color;
	in vec2 texcoord;
	uniform sampler2D tex;
	uniform sampler2D brightness;
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

		vec2 outer_size = vec2(textureSize(tex, 0));
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

const char win_shader_default[] = GLSL(330,
	in vec2 texcoord;
	uniform sampler2D tex;
	vec4 default_post_processing(vec4 c);
	vec4 window_shader() {
		vec4 c = texelFetch(tex, ivec2(texcoord), 0);
		return default_post_processing(c);
	}
);

const char present_vertex_shader[] = GLSL(330,
	uniform mat4 projection;
	layout(location = 0) in vec2 coord;
	out vec2 texcoord;
	void main() {
		gl_Position = projection * vec4(coord, 0, 1);
		texcoord = coord;
	}
);
const char vertex_shader[] = GLSL(330,
	uniform mat4 projection;
	uniform float scale = 1.0;
	uniform vec2 texorig;
	layout(location = 0) in vec2 coord;
	layout(location = 1) in vec2 in_texcoord;
	out vec2 texcoord;
	void main() {
		gl_Position = projection * vec4(coord, 0, scale);
		texcoord = in_texcoord + texorig;
	}
);
const char shadow_colorization_frag[] = GLSL(330,
	uniform vec4 color;
	uniform sampler2D tex;
	in vec2 texcoord;
	out vec4 out_color;
	void main() {
		vec4 c = texelFetch(tex, ivec2(texcoord), 0);
		out_color = c.r * color;
	}
);
// clang-format on
