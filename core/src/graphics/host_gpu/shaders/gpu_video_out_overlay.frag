// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#version 450
#extension GL_GOOGLE_include_directive : require

// Guest overlay plane (video-out bus 1) and the GL compatibility backend's
// frame fill. CFP MAX 40 (v0.0.10pro): the overlay frame is a working-set
// image too - it runs at 40% like everything else - so its magnification uses
// the same Catmull-Rom kernel the game frame's pass uses (see
// gpu_video_out_cfp.inc); UI text keeps its edges instead of washing out.
// The premultiplied-alpha conversion happens after the resample, in the same
// linear space the kernel runs in.

#include "gpu_video_out_cfp.inc"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D source;
layout(push_constant) uniform AlphaControl {
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
	if ((flags & 4u) == 0u) {
		c.rgb *= c.a;
	}
	if ((flags & 1u) != 0u) {
		c.rgb = cfp_linear_to_srgb(c.rgb);
	}
	color = c;
}
