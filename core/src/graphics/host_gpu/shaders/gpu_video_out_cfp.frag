// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#version 450
#extension GL_GOOGLE_include_directive : require

// CFP MAX 40 (v0.0.10pro): the present-path reconstruction of a scaled game
// frame. The whole working set (textures, render targets, the video-out
// image) runs at 40% when CFP MAX is on - or at any res_scale below native -
// and this pass magnifies it back to the display with the shared CFP core
// (see gpu_video_out_cfp.inc): a Catmull-Rom bicubic resample rebuilds the
// edge gradients a bilinear blit washes out, and a contrast-adaptive,
// range-clamped high-pass restores the acutance so the picture reads at
// native sharpness. One pass, no history, no added latency: the frame is
// presented the moment it is ready, exactly as before.
//
// Replaces the scaled blitImage in RecordPresentCommands whenever the source
// is smaller than the swapchain (a magnification) and its format is one of
// the 8-bit RGBA families; every other case keeps the hardware blit.

#include "gpu_video_out_cfp.inc"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D source;
layout(push_constant) uniform CfpControl {
	float weight;
	float sharpness;
	uint src_width;
	uint src_height;
	uint flags;
};

void main() {
	vec2 texel = 1.0 / vec2(float(src_width), float(src_height));
	vec4 c;
	if ((flags & 2u) != 0u) {
		c = cfp_catmull_rom(source, uv, texel);
	} else {
		c = texture(source, uv);
	}
	c = cfp_sharpen(source, uv, texel, c, sharpness);
	if ((flags & 1u) != 0u) {
		c.rgb = cfp_linear_to_srgb(c.rgb);
	}
	color = vec4(c.rgb, 1.0);
}
