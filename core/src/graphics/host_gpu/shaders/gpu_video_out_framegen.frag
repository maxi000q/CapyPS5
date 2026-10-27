// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#version 450

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

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D frame_newest;
layout(binding = 1) uniform sampler2D frame_previous;
layout(push_constant) uniform FrameGenControl {
	float weight;
};

void main() {
	vec4 newest   = texture(frame_newest, uv);
	vec4 previous = texture(frame_previous, uv);
	color = clamp(newest + weight * (newest - previous), vec4(0.0), vec4(1.0));
	color.a = 1.0;
}
