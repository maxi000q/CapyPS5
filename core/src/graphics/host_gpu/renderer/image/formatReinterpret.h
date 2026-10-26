// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_FORMATREINTERPRET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_FORMATREINTERPRET_H_

#include <cstdint>

namespace Libs::Graphics::FormatReinterpret {

// v0.0.9proQ1 — the framebuffer-compression profile (Mali AFBC / Adreno UBWC) creates
// render targets with a single-format view list: the driver then keeps its lossless
// framebuffer compression alive, which halves the memory traffic of every render pass on
// bandwidth-bound GPUs. A list of only the image's own format forbids views in *other*
// formats, and PS5 games do reinterpret render-target memory (a colour target written as
// RGBA16F and sampled back as R32G32 is the classic post-processing pattern).
//
// This registry remembers every guest-memory range that has ever been viewed in a foreign
// format. Images of a marked range are created fully mutable from the start (no
// single-format list), so foreign views stay valid without a copy; the first foreign view
// of an unmarked image instead recreates that image as mutable with its contents copied
// (Image::ReinterpretAsMutable) and marks the range here. Entries are never removed:
// reinterpreting a range once is a property of the game, not of a single image, and the
// map stays tiny (a handful of ranges per game, bucketed at 64 KB granularity).

/** Marks [address, address + size) as viewed in a format other than the image's own. */
void Mark(uint64_t address, uint64_t size);

/** True when any 64 KB bucket of [address, address + size) has been marked before. */
[[nodiscard]] bool IsMarked(uint64_t address, uint64_t size);

} // namespace Libs::Graphics::FormatReinterpret

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_FORMATREINTERPRET_H_
