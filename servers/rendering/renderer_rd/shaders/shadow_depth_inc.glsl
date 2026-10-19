// Read raw shadow depth for blocker searches and light transmission.
float sample_shadow_depth_linear(texture2D shadow_texture, sampler shadow_depth_sampler, vec2 uv) {
#ifdef WEBGPU
	// WebGPU prohibits sampling depth textures with a filtering sampler, even when
	// that sampler is also shared with color textures. Preserve linear-clamp depth
	// sampling with explicit loads; comparison filtering still uses shadow_sampler.
	ivec2 size = textureSize(sampler2D(shadow_texture, shadow_depth_sampler), 0);
	vec2 texel = clamp(uv, vec2(0.0), vec2(1.0)) * vec2(size) - 0.5;
	ivec2 base = ivec2(floor(texel));
	vec2 weight = fract(texel);
	ivec2 last = size - 1;
	float d00 = texelFetch(sampler2D(shadow_texture, shadow_depth_sampler), clamp(base, ivec2(0), last), 0).r;
	float d10 = texelFetch(sampler2D(shadow_texture, shadow_depth_sampler), clamp(base + ivec2(1, 0), ivec2(0), last), 0).r;
	float d01 = texelFetch(sampler2D(shadow_texture, shadow_depth_sampler), clamp(base + ivec2(0, 1), ivec2(0), last), 0).r;
	float d11 = texelFetch(sampler2D(shadow_texture, shadow_depth_sampler), clamp(base + ivec2(1, 1), ivec2(0), last), 0).r;
	return mix(mix(d00, d10, weight.x), mix(d01, d11, weight.x), weight.y);
#else
	return textureLod(sampler2D(shadow_texture, shadow_depth_sampler), uv, 0.0).r;
#endif
}
