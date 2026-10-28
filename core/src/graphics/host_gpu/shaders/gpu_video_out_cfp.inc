// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// CFP MAX 40 (v0.0.10pro): the shared reconstruction core of every
// present-path shader - the game frame (gpu_video_out_cfp.frag), the frame
// generator (gpu_video_out_framegen.frag) and the guest overlay
// (gpu_video_out_overlay.frag).
//
// The working set runs at 40% (debug.capy.cfp_max): the upload path prefolds
// guest images through an exact-half box step and a mild step down to 40%, so
// what is stored is alias-free. What is left at display time is EDGE ACUTANCE:
// a bilinear magnification of a 40% image looks soft exactly where a native
// picture would not. Two mechanisms, fused into one pass:
//
//   1. Catmull-Rom bicubic magnification (9 bilinear fetches): the cubic
//      kernel's negative lobes rebuild the gradient a bilinear tap washes out,
//      while the kernel stays bounded - none of the Lanczos-class overshoot.
//   2. Contrast-adaptive acutance restoration: a high-pass of the four axis
//      neighbours is added back with a gain that scales with the local range
//      headroom, and the result is HARD-CLAMPED into the neighbourhood's
//      min/max - the output pixel mathematically cannot overshoot into a halo
//      or ring, flat areas stay flat, and edges get their native bite back.
//
// Cost: 13 bilinear fetches per output pixel, one pass, no history, no
// per-pixel divergence - zero added latency by construction.
//
// Every shader that includes this shares one push-control block (bytes must
// match CfpPresentControl in swapchain.cpp):
//
//   float weight;      // frame generator: motion extension weight
//   float sharpness;   // acutance restoration strength (0 disables)
//   uint  src_width;   // source extent, for the texel grid
//   uint  src_height;
//   uint  flags;       // bit0: encode sRGB on store
//                      // bit1: Catmull-Rom sampling enabled
//                      // bit2: input alpha is premultiplied (overlay)

// Catmull-Rom bicubic sample in 9 bilinear fetches: the 4x4 kernel separates
// into two 4-tap passes, and each interior pair of taps folds into one
// bilinear fetch at the weight centroid. Sharper than bilinear, bounded
// unlike Lanczos - the magnification kernel for a 40% picture.
vec4 cfp_catmull_rom(sampler2D tex, vec2 uv, vec2 texel) {
	vec2 sample_pos = uv / texel;
	vec2 pos1 = floor(sample_pos - 0.5) + 0.5;
	vec2 f = sample_pos - pos1;

	// Catmull-Rom weights for the four taps of each axis.
	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 1.5 * f);

	// The interior pair folds into one bilinear fetch at its centroid.
	vec2 w12 = w1 + w2;
	vec2 off12 = w2 / w12;

	vec2 p0 = (pos1 - 1.0) * texel;
	vec2 p3 = (pos1 + 2.0) * texel;
	vec2 p12 = (pos1 + off12) * texel;

	vec4 result = vec4(0.0);
	result += texture(tex, p12) * (w12.x * w12.y);
	result += texture(tex, vec2(p0.x, p12.y)) * (w0.x * w12.y);
	result += texture(tex, vec2(p3.x, p12.y)) * (w3.x * w12.y);
	result += texture(tex, vec2(p12.x, p0.y)) * (w12.x * w0.y);
	result += texture(tex, vec2(p12.x, p3.y)) * (w12.x * w3.y);
	result += texture(tex, vec2(p0.x, p0.y)) * (w0.x * w0.y);
	result += texture(tex, vec2(p3.x, p0.y)) * (w3.x * w0.y);
	result += texture(tex, vec2(p0.x, p3.y)) * (w0.x * w3.y);
	result += texture(tex, vec2(p3.x, p3.y)) * (w3.x * w3.y);
	return result;
}

// Contrast-adaptive acutance restoration. `center` is the already-resampled
// pixel (Catmull-Rom or bilinear); the four axis neighbours calibrate the
// local range. The added high-pass is clamped into [min, max] of the
// neighbourhood, so it can neither ring on a scene edge nor halo next to a
// bright line - it can only move a pixel inside the contrast its own
// neighbourhood already exhibits. strength 0 returns the input untouched.
vec4 cfp_sharpen(sampler2D tex, vec2 uv, vec2 texel, vec4 center, float strength) {
	if (strength <= 0.0) {
		return center;
	}
	vec4 n0 = texture(tex, uv + vec2(0.0, -texel.y));
	vec4 n1 = texture(tex, uv + vec2(-texel.x, 0.0));
	vec4 n2 = texture(tex, uv + vec2(texel.x, 0.0));
	vec4 n3 = texture(tex, uv + vec2(0.0, texel.y));
	vec4 mn = min(center, min(min(n0, n1), min(n2, n3)));
	vec4 mx = max(center, max(max(n0, n1), max(n2, n3)));
	// The local range; guarded so a perfectly flat area divides by a floor,
	// not zero (a flat area adds nothing anyway - the high-pass is zero).
	vec4 range = max(mx - mn, vec4(1.0 / 256.0));
	// Headroom: how far the whole neighbourhood sits from the value-range
	// ends. Saturated neighbourhoods (bright highlights, deep shadows) get
	// their gain faded out - nothing to restore there, and any push would clip.
	vec4 headroom = min(mn, vec4(1.0) - mx);
	vec4 gain = clamp(headroom / range, vec4(0.0), vec4(1.0)) * strength;
	vec4 detail = center * 4.0 - (n0 + n1 + n2 + n3);
	vec4 sharpened = center + detail * (0.25 * gain);
	// Alpha is coverage, not colour: pass it through untouched.
	return vec4(clamp(sharpened.rgb, mn.rgb, mx.rgb), center.a);
}

// Re-encode for a UNORM attachment when the source was sampled through an
// sRGB view (the surface offered no sRGB swapchain format): decode(x) then
// encode(x) is byte-identical for in-range values, so the fallback keeps the
// pass-through the hardware blit used to produce.
vec3 cfp_linear_to_srgb(vec3 c) {
	vec3 low = c * 12.92;
	vec3 high = pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) * 1.055 - vec3(0.055);
	return mix(low, high, step(vec3(0.0031308), c));
}
