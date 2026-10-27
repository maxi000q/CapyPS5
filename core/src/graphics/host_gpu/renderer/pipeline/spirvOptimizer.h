// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SPIRV_OPTIMIZER_H_
#define GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SPIRV_OPTIMIZER_H_

#include <cstdint>
#include <vector>

#include "graphics/shader/shader.h"

namespace Libs::Graphics::SpirvOptimizer {

/**
 * v0.0.9proQ3: per-vendor spirv-opt pass lists over every translated shader.
 *
 * The recompiler emits straightforward SPIR-V; mobile drivers (Arm's Mali
 * compiler above all) spend visible time chewing through redundancies that
 * spirv-opt removes in microseconds - unreachable branches after constant
 * specialization, dead lanes of never-read vectors, duplicated constants,
 * straight-line block splits. Smaller and simpler modules mean:
 *
 *   - faster pipeline compilation (the first-visit hitch every new material
 *     causes, and shader-storm warm-ups, finish sooner), and
 *   - better driver codegen on compilers that scale poorly with IR size.
 *
 * Only semantics-preserving, interface-preserving passes run: the entry
 * point, descriptor bindings, push constants and IO are untouched, and the
 * optimized module is validated before it replaces the original - on any
 * failure the original module is kept.
 *
 * Vendor lists: Arm (Mali, also PanVK) runs the full list; Qualcomm/Adreno
 * (proprietary or Turnip) runs the same list minus the two most structural
 * passes (block merge and global value numbering), which its compiler does
 * not benefit from. Other vendors compile unoptimized, as before.
 *
 * `debug.capy.spirv_opt`: 0 off, 1 on for every vendor, 2 auto (the
 * default; on for Arm and Qualcomm GPUs). One summary line per session
 * logs how many modules were optimized and how many words disappeared.
 */
void Optimize(ShaderType stage, std::vector<uint32_t>& spirv);

} // namespace Libs::Graphics::SpirvOptimizer

#endif // GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SPIRV_OPTIMIZER_H_
