uniform float opacity;
uniform bool invert_color;
uniform sampler2D tex;

void main() {
	vec4 c = texture2D(tex, gl_TexCoord[0]);
	{
		// Change vec4(1.0, 1.0, 1.0, 1.0) to your desired color
		vec4 vdiff = abs(vec4(1.0, 1.0, 1.0, 1.0) - c);
		float diff = max(max(max(vdiff.r, vdiff.g), vdiff.b), vdiff.a);
		// Change 0.8 to your desired opacity
		if (diff < 0.001)
			c *= 0.8;
	}
	if (invert_color)
		c = vec4(vec3(c.a, c.a, c.a) - vec3(c), c.a);
	c *= opacity;
	gl_FragColor = c;
}
