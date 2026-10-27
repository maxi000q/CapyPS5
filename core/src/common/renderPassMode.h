// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef COMMON_RENDER_PASS_MODE_H
#define COMMON_RENDER_PASS_MODE_H

#include <cstdint>

// Render-pass clear strategy (v0.0.9proQ4 round 4).
//
// Mode 0  every clear goes through the render pass loadOp (hardware clear):
//         nothing loads the previous target contents first. Cheapest in
//         bandwidth; the natural mode for bandwidth-bound tiled GPUs.
// Mode 1  keep the open pass when only the clears changed (dedup aid).
// Mode 2  clears run in a pass of their own.
// Mode 3  the pass opens with LOAD and clears run inside it (the Adreno
//         depth-write workaround: after a pass that cleared depth with
//         loadOp CLEAR, the next pass that loads that depth could lose its
//         depth writes on some drivers).
// Mode 4  hybrid: colour clears use loadOp CLEAR (the previous colour
//         contents are never loaded - on a multi-megapixel target that is
//         tens of MiB of memory traffic per cleared pass), while depth and
//         stencil clears keep the mode-3 in-pass treatment so the documented
//         depth workaround keeps applying to depth exactly as before. The
//         resulting pixels are identical to mode 3 in both cases; only the
//         redundant colour load disappears.
//
// `debug.capy.rp_mode` selects explicitly. The default: mode 0 on Mali
// (AFBC-friendly hardware clears), mode 4 on Adreno (colour loadOp clears +
// the depth workaround), mode 3 everywhere else. The resolution-keyed policy
// (multi-megapixel class targets observed at runtime) also selects mode 4 on
// every non-Mali vendor: the colour load a clear replaces is
// resolution-linear - tens of MiB per cleared pass at the biggest consumer
// extents - so the hybrid matters most exactly there.
namespace Common::RenderPass {

inline constexpr int64_t kModeHardwareClears = 0;
inline constexpr int64_t kModeKeepOpenPass   = 1;
inline constexpr int64_t kModeOwnClearPass   = 2;
inline constexpr int64_t kModeInPassClears   = 3;
inline constexpr int64_t kModeHybridClears   = 4;
inline constexpr int64_t kModeCount          = 5;

[[nodiscard]] inline int64_t SelectMode(int64_t requested, bool mali_profile,
                                        bool adreno_profile, bool high_res_policy) {
	if (requested >= 0 && requested < kModeCount) {
		return requested;
	}
	if (mali_profile) {
		// AFBC-friendly full hardware clears stay the cheapest option there,
		// at any resolution - the Mali default is not overridden.
		return kModeHardwareClears;
	}
	if (adreno_profile || high_res_policy) {
		// Colour loadOp clears + the depth workaround. The load a clear
		// replaces scales with the target extents, so the resolution policy
		// picks the hybrid on every non-Mali vendor.
		return kModeHybridClears;
	}
	return kModeInPassClears;
}

} // namespace Common::RenderPass

#endif /* COMMON_RENDER_PASS_MODE_H */
