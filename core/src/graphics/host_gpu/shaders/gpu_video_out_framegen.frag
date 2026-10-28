// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#version 450
#extension GL_GOOGLE_include_directive : require

// Frame generator (v0.0.10pro phase 5): the presentation-path frame that fills
// an idle display tick between two real game frames. Two history images come
// in - the newest game frame and the one before it - and the shader extends the
// newest frame's own motion by `weight` of the frame-to-frame delta:
//
//   weight = 0   repeats the newest frame (no generation)
//   weight > 0   shows the motion advanced by that fraction of a frame interval
//
// Damped forward extrapolation (not a backward blend) is what keeps the
// generated tick YOUNGER than a plain repeat would be: it softens the judder
// of 24/30/45p cadences without ever waiting for the next real frame, so the
// input path keeps its exact latency. The clamp bounds the failure mode to a
// single soft-ghosted tick on a scene cut instead of overshoot.
//
// CFP MAX 40 (v0.0.10pro): with the working set at 40%, the history frames
// are magnified at present time - and a generated frame must magnify with the
// SAME kernel the real frame's pass uses (Catmull-Rom, see
// gpu_video_out_cfp.inc), or the display would alternate sharp real frames
// with soft generated ones. Each history frame is resampled and acutance
// restored on its own neighbours (so the clamps stay valid), then the motion
// extends the restored pair.

#include "gpu_video_out_cfp.inc"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D frame_newest;
layout(binding = 1) uniform sampler2D frame_previous;
layout(push_constant) uniform FrameGenControl {
	float weight;
	float sharpness;
	uint src_width;
	uint src_height;
	uint flags;
};

void main() {
	vec2 texel = 1.0 / vec2(float(src_width), float(src_height));
	vec4 newest;
	vec4 previous;
	if ((flags & 2u) != 0u) {
		newest = cfp_catmull_rom(frame_newest, uv, texel);
		previous = cfp_catmull_rom(frame_previous, uv, texel);
	} else {
		newest = texture(frame_newest, uv);
		previous = texture(frame_previous, uv);
	}
	// Restore each frame's own acutance before extending its motion; the
	// sharpening clamps to each frame's own neighbourhood, so the motion
	// math sees the same bounded values the real present pass produces.
	newest = cfp_sharpen(frame_newest, uv, texel, newest, sharpness);
	previous = cfp_sharpen(frame_previous, uv, texel, previous, sharpness);
	color = clamp(newest + weight * (newest - previous), vec4(0.0), vec4(1.0));
	if ((flags & 1u) != 0u) {
		color.rgb = cfp_linear_to_srgb(color.rgb);
	}
	color.a = 1.0;
}
