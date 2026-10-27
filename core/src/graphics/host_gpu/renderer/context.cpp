// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "common/assert.h"
#include "common/common.h"
#include "common/debugFlags.h"
#include "common/contentProfile.h"
#include "common/common.h"
#include "common/contentProfile.h"
#include "common/gpuProfile.h"
#include "common/renderPassMode.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/gpuProfiler.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <bit>
#include <cstring>
namespace Libs::Graphics {

CommandBuffer::CommandBuffer(CommandScheduler& scheduler)
    : m_context(scheduler.Context()), m_graphics(scheduler.Graphics()) {}

bool CommandBuffer::IsInvalid() const {
	return m_buffer == nullptr;
}

vk::CommandBuffer CommandBuffer::Handle() const {
	EXIT_IF(IsInvalid());
	return m_buffer;
}

void CommandBuffer::Begin() {
	EXIT_IF(m_rendering || IsInvalid());
	auto buffer = Handle();

	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

	auto result = buffer.begin(&begin_info);

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::End() const {
	EndRendering();
	auto buffer = Handle();

	auto result = buffer.end();

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1,
                                 uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	m_debug_op        = op;
	m_debug_submit_id = submit_id;
	m_debug_arg0      = arg0;
	m_debug_arg1      = arg1;
	m_debug_arg2      = arg2;
	m_debug_arg3      = arg3;
	m_debug_arg4      = arg4;
}

static RenderState WithoutClears(RenderState state) {
	for (auto& attachment: state.color_attachments) {
		attachment.is_clear    = false;
		attachment.clear_value = {};
	}
	state.depth_stencil_attachment.depth_clear   = false;
	state.depth_stencil_attachment.stencil_clear = false;
	state.depth_stencil_attachment.clear_value   = {};
	return state;
}

void CommandBuffer::BeginRendering(const RenderState& state) const {
	if (m_rendering && m_render_state == state) {
		return;
	}
	// Adreno: after a pass that clears depth with loadOp CLEAR, the next pass that loads that
	// depth loses its depth writes (Dead Cells wall layers vanished from the depth buffer and lit
	// sprites showed through in 8x8 blocks). Default (3): open with LOAD and clear inside the
	// pass. `debug.capy.rp_mode`: 0 loadOp CLEAR, 1 keep the open pass when only the clears
	// change, 2 clear in a pass of its own.
	// v0.0.9pro: the default now depends on the GPU. Mode 3 (clear inside a LOAD pass)
	// works around an Adreno depth bug; on Mali it makes every pass load and decompress
	// the previous contents only to overwrite them, which costs bandwidth on a GPU that
	// is bandwidth-bound. The Mali profile defaults to 0 instead: hardware loadOp clears
	// are free and keep AFBC (Arm Frame Buffer Compression) alive - same pixels, less
	// memory traffic. An explicit `setprop debug.capy.rp_mode` still wins.
	static Common::DebugFlags::CachedValue rp_mode("debug.capy.rp_mode", -1);
	const auto mode = [&] {
		// v0.0.9proQ4 round 4: mode 4 (hybrid) joins the set - colour clears
		// go through loadOp (the previous colour contents are never loaded:
		// on a multi-megapixel target that is tens of MiB of memory traffic
		// per cleared pass), while depth and stencil clears keep the in-pass
		// treatment of mode 3 so the documented Adreno depth-write
		// workaround keeps applying to depth exactly as before. Default: 0
		// on Mali, 4 on Adreno and under the resolution policy, 3 elsewhere
		// (common/renderPassMode.h).
		return Common::RenderPass::SelectMode(rp_mode.Get(),
		                                      Common::GpuProfile::MaliProfileEnabled(),
		                                      Common::GpuProfile::AdrenoProfileEnabled(),
		                                      Common::ContentProfile::HighResolutionPolicyActive());
	}();
	if (mode == 1 && m_rendering && WithoutClears(m_render_state) == state) {
		return;
	}
	if (mode == 3 && !(WithoutClears(state) == state)) {
		// Open with LOAD and clear inside the pass instead of using loadOp CLEAR.
		BeginRendering(WithoutClears(state));
		std::array<vk::ClearAttachment, RENDER_COLOR_ATTACHMENTS_MAX + 1> clears {};
		uint32_t count = 0;
		for (uint32_t i = 0; i < state.num_color_attachments; i++) {
			const auto& attachment = state.color_attachments[i];
			if (attachment.is_clear) {
				auto& clear                  = clears[count++];
				clear.aspectMask             = vk::ImageAspectFlagBits::eColor;
				clear.colorAttachment        = i;
				clear.clearValue.color.uint32 = attachment.clear_value;
			}
		}
		const auto& ds = state.depth_stencil_attachment;
		vk::ImageAspectFlags ds_aspects {};
		if (ds.has_depth && ds.depth_clear) {
			ds_aspects |= vk::ImageAspectFlagBits::eDepth;
		}
		if (ds.has_stencil && ds.stencil_clear) {
			ds_aspects |= vk::ImageAspectFlagBits::eStencil;
		}
		if (ds_aspects) {
			auto& clear                         = clears[count++];
			clear.aspectMask                    = ds_aspects;
			clear.clearValue.depthStencil.depth = std::bit_cast<float>(ds.clear_value[0]);
			clear.clearValue.depthStencil.stencil = ds.clear_value[1];
		}
		vk::ClearRect rect {};
		rect.rect.extent = vk::Extent2D {state.width, state.height};
		rect.layerCount  = state.num_layers;
		if (count != 0) {
			Handle().clearAttachments(count, clears.data(), 1, &rect);
		}
		return;
	}
	if (mode == 4 && (state.depth_stencil_attachment.depth_clear ||
		               state.depth_stencil_attachment.stencil_clear)) {
		// Hybrid: strip only the depth/stencil clears from the pass (colour
		// clears keep their loadOp CLEAR in the main path below), then clear
		// depth in-pass - the depth path behaves exactly as in mode 3, the
		// colour path stops paying a load it immediately overwrites.
		const auto& ds = state.depth_stencil_attachment;
		RenderState stripped = state;
		stripped.depth_stencil_attachment.depth_clear   = false;
		stripped.depth_stencil_attachment.stencil_clear = false;
		BeginRendering(stripped);
		vk::ClearAttachment clear {};
		if (ds.has_depth && ds.depth_clear) {
			clear.aspectMask |= vk::ImageAspectFlagBits::eDepth;
		}
		if (ds.has_stencil && ds.stencil_clear) {
			clear.aspectMask |= vk::ImageAspectFlagBits::eStencil;
		}
		clear.clearValue.depthStencil.depth   = std::bit_cast<float>(ds.clear_value[0]);
		clear.clearValue.depthStencil.stencil = ds.clear_value[1];
		vk::ClearRect rect {};
		rect.rect.extent = vk::Extent2D {state.width, state.height};
		rect.layerCount  = state.num_layers;
		Handle().clearAttachments(1, &clear, 1, &rect);
		return;
	}
	static thread_local bool splitting = false;
	if (mode == 2 && !splitting && !(WithoutClears(state) == state)) {
		splitting = true;
		BeginRendering(state);
		EndRendering();
		BeginRendering(WithoutClears(state));
		splitting = false;
		return;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.num_color_attachments > RENDER_COLOR_ATTACHMENTS_MAX);
	EndRendering();

	// `debug.capy.full_barrier 1`: a full memory barrier before every render pass (diagnoses
	// missing synchronization; expensive).
	static Common::DebugFlags::CachedValue full_barrier("debug.capy.full_barrier", 0);
	if (full_barrier.Get() != 0) {
		vk::MemoryBarrier2 barrier {};
		barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		vk::DependencyInfo dependency {};
		dependency.memoryBarrierCount = 1;
		dependency.pMemoryBarriers    = &barrier;
		Handle().pipelineBarrier2(dependency);
	}

	std::array<vk::RenderingAttachmentInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
	for (uint32_t i = 0; i < state.num_color_attachments; i++) {
		const auto& attachment = state.color_attachments[i];
		colors[i].imageView    = attachment.image_view;
		colors[i].imageLayout  = attachment.image_layout;
		colors[i].loadOp =
		    attachment.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		colors[i].storeOp                 = vk::AttachmentStoreOp::eStore;
		colors[i].clearValue.color.uint32 = attachment.clear_value;
	}

	const auto&                 depth_stencil = state.depth_stencil_attachment;
	vk::RenderingAttachmentInfo depth {};
	depth.imageView   = depth_stencil.image_view;
	depth.imageLayout = depth_stencil.image_layout;
	depth.loadOp =
	    depth_stencil.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depth.storeOp                       = vk::AttachmentStoreOp::eStore;
	depth.clearValue.depthStencil.depth = std::bit_cast<float>(depth_stencil.clear_value[0]);

	vk::RenderingAttachmentInfo stencil {};
	stencil.imageView   = depth_stencil.image_view;
	stencil.imageLayout = depth_stencil.image_layout;
	stencil.loadOp =
	    depth_stencil.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	stencil.storeOp                         = vk::AttachmentStoreOp::eStore;
	stencil.clearValue.depthStencil.stencil = depth_stencil.clear_value[1];

	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = {state.width, state.height};
	rendering.layerCount           = state.num_layers;
	rendering.colorAttachmentCount = state.num_color_attachments;
	rendering.pColorAttachments    = colors.data();
	rendering.pDepthAttachment     = depth_stencil.has_depth ? &depth : nullptr;
	rendering.pStencilAttachment   = depth_stencil.has_stencil ? &stencil : nullptr;
	if (Common::DebugFlags::FrameTracing()) {
		Common::DebugFlags::FrameTrace("  rp begin %ux%u colors=%u depth=%d dclear=%d", state.width,
		                               state.height, state.num_color_attachments,
		                               state.depth_stencil_attachment.has_depth ? 1 : 0,
		                               state.depth_stencil_attachment.depth_clear ? 1 : 0);
	}
	if (GpuProfiler::Enabled()) {
		GpuProfiler::BeginPass(m_graphics, Handle(), state.width, state.height,
		                       state.num_color_attachments, state.depth_stencil_attachment.has_depth);
	}
	Handle().beginRendering(rendering);
	m_render_state = state;
	m_rendering    = true;
	g_pass_serial++;
}

void CommandBuffer::EndRendering() const {
	if (!m_rendering) {
		return;
	}
	if (Common::DebugFlags::FrameTracing()) {
		Common::DebugFlags::FrameTrace("  rp end (caller %p)", __builtin_return_address(0));
	}
	Handle().endRendering();
	if (GpuProfiler::Enabled()) {
		GpuProfiler::SetEndCaller(__builtin_return_address(0));
	}
	GpuProfiler::EndScope(Handle());
	if (m_deferred_barrier) {
		m_deferred_barrier = false;
		vk::MemoryBarrier2 barrier {};
		barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
		barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		vk::DependencyInfo dependency {};
		dependency.memoryBarrierCount = 1;
		dependency.pMemoryBarriers    = &barrier;
		Handle().pipelineBarrier2(dependency);
	}
	m_rendering    = false;
	m_render_state = {};
}

} // namespace Libs::Graphics
