// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/debugFlags.h"
#include "common/gpuProfile.h"
#include "common/profiler.h"
#include "common/telemetry.h"
#include "common/threads.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_video_out_overlay_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window/windowInternal.h"
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
// v0.0.9proQ3: OpenGL compatibility backend (see eglCompat.h).
#include "graphics/presentation/window/eglCompat.h"
#include <android/hardware_buffer.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vulkan/vulkan_android.h>
#endif

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <vector>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

namespace Libs::Graphics {

// sync_wait() lives in bionic's <sync.h>, which the NDK sysroot does not ship
// (<android/sync.h> is the legacy fence-info header and does not declare it).
// The function is a one-ioctl wrapper with a stable kernel ABI, so the wait is
// done here directly: SYNC_IOC_WAIT = _IOW('>', 0, int32_t); the argument is the
// timeout in milliseconds; returns 0 when the fence signalled, -1 on error.
int SyncFenceWait(int fence_fd, int timeout_ms) {
	int timeout = timeout_ms;
	return ioctl(fence_fd, 0x40043E00u /* SYNC_IOC_WAIT */, &timeout);
}

struct Presenter::Frame {
	VulkanImage   image;
	vk::ImageView view         = nullptr;
	uint64_t      present_tick = 0;
	bool          busy         = false;

	void Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format);
	void Transit(vk::CommandBuffer command, vk::ImageLayout layout, vk::AccessFlags2 access);
	void CopyFrom(CommandBuffer& command, Image& source);
	void Clear(CommandBuffer& command, const vk::ClearColorValue& color);
};

class FramePool final {
public:
	FramePool(WindowContext& window, CommandScheduler& scheduler)
	    : m_window(window), m_scheduler(scheduler) {}
	~FramePool() {
		m_scheduler.Wait(m_scheduler.CurrentTick() - 1);
		for (auto& frame: m_frames) {
			m_window.graphic_ctx.device.destroyImageView(frame->view, nullptr);
			if (frame->image.image != nullptr) {
				m_window.graphic_ctx.DeleteImage(frame->image);
			}
		}
	}
	KYTY_CLASS_NO_COPY(FramePool);

	void Initialize(uint32_t count, vk::Format format) {
		if (count == 0 || format == vk::Format::eUndefined) {
			LOGF("prepared-frame pool deferred initialization: count=%u\n", count);
			return;
		}
		Common::LockGuard lock(m_mutex);
		if (!m_frames.empty()) {
			m_format = format;
			return;
		}
		m_format = format;
		m_frames.reserve(count);
		m_free.reserve(count);
		for (uint32_t i = 0; i < count; i++) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
	}

	bool IsInitialized() noexcept {
		Common::LockGuard lock(m_mutex);
		return !m_frames.empty();
	}

	void SetFormat(vk::Format format) {
		if (format == vk::Format::eUndefined) {
			return;
		}
		Common::LockGuard lock(m_mutex);
		m_format = format;
	}

	vk::Format GetFormat() {
		Common::LockGuard lock(m_mutex);
		if (m_format == vk::Format::eUndefined) {
			return vk::Format::eB8G8R8A8Unorm;
		}
		return m_format;
	}

	Presenter::Frame* Acquire(vk::Extent2D extent, vk::Format format) {
		m_mutex.Lock();
		if (m_frames.empty()) {
			LOGF("prepared-frame pool: initializing fallback frames before first swapchain flip\n");
			m_format = format != vk::Format::eUndefined ? format : vk::Format::eB8G8R8A8Unorm;
			for (uint32_t i = 0; i < 3; i++) {
				auto frame = std::make_unique<Presenter::Frame>();
				m_free.push_back(frame.get());
				m_frames.push_back(std::move(frame));
			}
		}
		// A synchronized flip may need more frames than the swapchain has images.
		// Queue capacity bounds growth; waiting here can prevent its master from recording.
		if (m_free.empty()) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
		const auto compatible = std::ranges::find_if(m_free, [&](const auto* frame) {
			return frame->image.extent.width == extent.width &&
			       frame->image.extent.height == extent.height && frame->image.format == format;
		});
		auto*      frame      = compatible == m_free.end() ? m_free.back() : *compatible;
		if (compatible != m_free.end()) {
			*compatible = m_free.back();
		}
		m_free.pop_back();
		if (frame->busy) {
			EXIT("prepared-frame pool returned an invalid frame\n");
		}
		frame->busy = true;
		m_mutex.Unlock();

		m_scheduler.Wait(frame->present_tick);
		return frame;
	}

	void ValidateForPresent(Presenter::Frame* frame) {
		Common::LockGuard lock(m_mutex);
		if (frame == nullptr || !frame->busy) {
			EXIT("prepared frame has invalid presentation ownership\n");
		}
	}

	void Release(Presenter::Frame* frame) {
		if (frame == nullptr) {
			EXIT("cannot release a null prepared frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!frame->busy) {
			EXIT("prepared frame was released twice\n");
		}
		frame->busy = false;
		m_free.push_back(frame);
	}

private:
	WindowContext&                                 m_window;
	CommandScheduler&                              m_scheduler;
	Common::Mutex                                  m_mutex;
	std::vector<std::unique_ptr<Presenter::Frame>> m_frames;
	std::vector<Presenter::Frame*>                 m_free;
	vk::Format                                     m_format = vk::Format::eUndefined;
};

void Presenter::Frame::Configure(GraphicContext& graphics, vk::Extent2D extent, vk::Format format) {
	if (extent.width == 0 || extent.height == 0 || format == vk::Format::eUndefined) {
		EXIT("unsupported prepared frame, extent=%ux%u format=%d\n", extent.width, extent.height,
		     static_cast<int>(format));
	}
	auto&      dst        = image;
	const bool compatible = dst.image != nullptr && dst.extent.width == extent.width &&
	                        dst.extent.height == extent.height && dst.format == format;
	if (compatible) {
		return;
	}

	const auto features = graphics.GetFormatProperties(format).optimalTilingFeatures;
	const auto required =
	    vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
	    vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferSrc |
	    vk::FormatFeatureFlagBits::eTransferDst;
	if ((features & required) != required) {
		EXIT("prepared presentation format lacks optimal blit support: format=%d features=0x%x\n",
		     static_cast<int>(format), static_cast<vk::FormatFeatureFlags::MaskType>(features));
	}

	if (dst.image != nullptr) {
		graphics.device.destroyImageView(view, nullptr);
		view = nullptr;
		graphics.DeleteImage(dst);
	}

	vk::ImageCreateInfo create {};
	create.sType         = vk::StructureType::eImageCreateInfo;
	create.imageType     = vk::ImageType::e2D;
	create.extent        = {extent.width, extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
	               vk::ImageUsageFlagBits::eSampled;
	create.sharingMode = vk::SharingMode::eExclusive;
	create.samples     = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, dst)) {
		EXIT("failed to allocate prepared presentation image, extent=%ux%u format=%d\n",
		     extent.width, extent.height, static_cast<int>(format));
	}
}

void Presenter::Frame::Transit(vk::CommandBuffer command, vk::ImageLayout layout,
                               vk::AccessFlags2 access) {
	const auto     stage  = access == vk::AccessFlagBits2::eTransferRead ||
	                                access == vk::AccessFlagBits2::eTransferWrite
	                            ? vk::PipelineStageFlagBits2::eTransfer
	                            : vk::PipelineStageFlagBits2::eAllCommands;
	constexpr auto writes = vk::AccessFlagBits2::eTransferWrite |
	                        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eMemoryWrite;
	if (image.state.layout == layout && image.state.access_mask == access &&
	    !static_cast<bool>(image.state.access_mask & writes)) {
		return;
	}
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask                    = image.state.pl_stage;
	barrier.srcAccessMask                   = image.state.access_mask;
	barrier.dstStageMask                    = stage;
	barrier.dstAccessMask                   = access;
	barrier.oldLayout                       = image.state.layout;
	barrier.newLayout                       = layout;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = image.image;
	barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
	image.state = {stage, access, layout};
	image.subresource_states.clear();
}

void Presenter::Frame::CopyFrom(CommandBuffer& command_buffer, Image& source) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, source.backing.layers};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, image.layers};
	copy.extent         = {std::min(source.backing.extent.width, image.extent.width),
	                       std::min(source.backing.extent.height, image.extent.height), 1};
	EXIT_IF(copy.srcSubresource.layerCount != copy.dstSubresource.layerCount);
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, image.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy);
}

void Presenter::Frame::Clear(CommandBuffer& command_buffer, const vk::ClearColorValue& color) {
	command_buffer.EndRendering();
	auto command = command_buffer.Handle();
	Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
	const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal, &color, 1, &range);
}

#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
// v0.0.9proQ3: after this many GL-backend recoveries in one session the backend
// gives up and the plain Vulkan swapchain takes over (a driver that cannot keep
// the EGL surface alive would otherwise rebuild it every frame).
static constexpr uint32_t kGlBackendMaxRecoveries = 5;
static uint32_t s_gl_backend_recoveries           = 0;
#endif

class Swapchain final {
public:
	enum class Status : uint8_t { Success, Recreate, SurfaceLost };

	explicit Swapchain(WindowContext& window): m_window(window) {}
	~Swapchain();
	KYTY_CLASS_NO_COPY(Swapchain);

	void                 Create();
	void                 Recreate(bool surface_lost = false);
	[[nodiscard]] bool   NeedsResize() const;
	/** Present SRGB frames to an SRGB swapchain, so the final blit does not convert them. */
	void SetWantSrgb(bool want) noexcept {
		if (!m_gl_mode) {
			m_want_srgb = want;
		}
	}
	/** v0.0.9proQ3: present through OpenGL ES (AHardwareBuffer ring) instead of a Vulkan swapchain. */
	[[nodiscard]] bool GlBackend() const noexcept { return m_gl_mode; }
	/** Ready to present: a Vulkan swapchain handle or the GL backend's buffer ring. */
	[[nodiscard]] bool IsReady() const noexcept { return m_gl_mode || m_handle != nullptr; }
	[[nodiscard]] Status AcquireNextImage();
	[[nodiscard]] bool   PrepareSystemOverlay();
	void     RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
	                               const Presenter::Layer& overlay, bool draw_system_overlay);
	uint64_t Submit(CommandScheduler& scheduler);
	[[nodiscard]] Status Present();

	[[nodiscard]] uint32_t ImageCount() const noexcept {
		return static_cast<uint32_t>(m_images.size());
	}
	[[nodiscard]] vk::Format Format() const noexcept { return m_format; }
	[[nodiscard]] vk::SwapchainKHR Handle() const noexcept { return m_handle; }

private:
	void Destroy();
	void DrawOverlay(vk::CommandBuffer command, const Presenter::Layer& layer,
	                 bool clear_attachment = false);

	WindowContext&   m_window;
	vk::SwapchainKHR m_handle = nullptr;
	vk::Format       m_format = vk::Format::eUndefined;
	bool             m_want_srgb = false;
	bool             m_srgb_unavailable = false; // the surface offers no SRGB format
	vk::Extent2D     m_extent {};
	// Drawable pixel size observed when this swapchain was created.
	vk::Extent2D                   m_window_extent {};
	std::vector<vk::Image>         m_images;
	std::vector<vk::ImageView>     m_image_views;
	std::vector<vk::Semaphore>     m_image_acquired;
	std::vector<vk::Semaphore>     m_render_complete;
	std::unique_ptr<SystemOverlay> m_system_overlay;
	vk::DescriptorSetLayout        m_overlay_descriptors = nullptr;
	vk::PipelineLayout             m_overlay_layout      = nullptr;
	vk::Pipeline                   m_overlay_pipeline    = nullptr;
	vk::Sampler                    m_overlay_sampler     = nullptr;
	uint32_t                       m_image_index         = static_cast<uint32_t>(-1);
	uint32_t                       m_frame_index         = 0;
	// v0.0.9proQ3: OpenGL compatibility backend ("Graphics API: OpenGL").
	bool m_gl_mode = false;
	CommandScheduler* m_last_submit_scheduler = nullptr;
	uint64_t          m_last_submit_tick      = 0;
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	[[nodiscard]] bool TryCreateGlBackend();
	void DestroyGlBackend();
	void RecordGlPresentCommands(CommandBuffer& command, Presenter::Frame* source,
	                             const Presenter::Layer& overlay, bool draw_system_overlay);
	std::unique_ptr<GlCompatRenderer> m_gl;
	std::vector<AHardwareBuffer*>     m_ahb;
	std::vector<vk::DeviceMemory>     m_ahb_memory;
	std::vector<int>                  m_release_fd;
#endif
};

struct Presenter::Impl {
	explicit Impl(WindowContext& owner)
	    : renderer(*owner.render_context), window(owner), swapchain(owner),
	      present_scheduler(renderer, owner.graphic_ctx), frames(owner, present_scheduler) {
		EXIT_IF(owner.render_context == nullptr);
		swapchain.Create();
		if (swapchain.ImageCount() > 0 && swapchain.Format() != vk::Format::eUndefined) {
			frames.Initialize(swapchain.ImageCount(), swapchain.Format());
		}
	}

	void RecoverSwapchain(Swapchain::Status status) {
		LOGF("Recovering Vulkan swapchain%s\n",
		     status == Swapchain::Status::SurfaceLost ? " and surface" : "");
		swapchain.Recreate(status == Swapchain::Status::SurfaceLost);
		if (swapchain.IsReady() && swapchain.Format() != vk::Format::eUndefined) {
			if (!frames.IsInitialized()) {
				frames.Initialize(swapchain.ImageCount(), swapchain.Format());
			} else {
				frames.SetFormat(swapchain.Format());
			}
		}
	}

	Image& ResolveSurface(const ImageInfo& info) {
		TextureCache::ImageDesc desc {};
		desc.info                  = info;
		desc.view_info.format      = info.pixel_format;
		desc.view_info.type        = vk::ImageViewType::e2D;
		desc.view_info.aspect      = vk::ImageAspectFlagBits::eColor;
		desc.view_info.base_level  = 0;
		desc.view_info.level_count = 1;
		desc.view_info.base_layer  = 0;
		desc.view_info.layer_count = 1;
		desc.view_info.usage       = vk::ImageUsageFlagBits::eTransferSrc;
		desc.type                  = TextureCache::BindingType::VideoOut;

		auto&      cache      = renderer.GetTextureCache();
		const auto image_id   = cache.FindImage(desc);
		auto&      image      = cache.GetImage(image_id);
		image.usage.video_out = true;
		cache.UpdateImage(image_id);
		return image;
	}
	void Present();

	RenderContext&        renderer;
	WindowContext&        window;
	Swapchain             swapchain;
	CommandScheduler      present_scheduler;
	FramePool             frames;
	Common::Mutex         present_mutex;
	std::array<Layer, 2>  layers {};
	std::atomic<uint64_t> presented_overlay_revision {0};
};

void Swapchain::Create() {
	auto& graphics = m_window.graphic_ctx;
	EXIT_IF(graphics.device == nullptr);
	if (m_window.surface == nullptr) {
		LOGF("Swapchain::Create: surface is null, deferring swapchain creation\n");
		return;
	}

	// Capture the size before querying surface capabilities. If the window changes
	// during creation, the next presentation will detect the newer size.
	{
		Common::LockGuard lock(m_window.mutex);
		m_window_extent = {graphics.screen_width, graphics.screen_height};
	}
	if (m_window_extent.width == 0 || m_window_extent.height == 0) {
		LOGF("Swapchain::Create: window extent is 0, deferring\n");
		return;
	}
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	// v0.0.9proQ3: the OpenGL compatibility backend replaces the Vulkan swapchain:
	// the final frame goes to an AHardwareBuffer ring that EGL/OpenGL ES presents.
	// Any failure inside falls back to the plain swapchain below.
	static Common::DebugFlags::CachedValue gl_backend("debug.capy.gl_backend", 0);
	if (gl_backend.Get() != 0 && s_gl_backend_recoveries < kGlBackendMaxRecoveries &&
	    graphics.ahb_import_supported) {
		m_extent = m_window_extent;
		if (TryCreateGlBackend()) {
			return;
		}
	}
#endif
	m_window.RefreshSurfaceCapabilities();
	const auto& surface = m_window.surface_capabilities;
	if (surface.formats.empty()) {
		LOGF("Swapchain::Create: surface formats empty, deferring\n");
		return;
	}

	m_extent = surface.capabilities.currentExtent;
	if (m_extent.width == std::numeric_limits<uint32_t>::max()) {
		m_extent.width =
		    std::clamp(m_window_extent.width, surface.capabilities.minImageExtent.width,
		               surface.capabilities.maxImageExtent.width);
		m_extent.height =
		    std::clamp(m_window_extent.height, surface.capabilities.minImageExtent.height,
		               surface.capabilities.maxImageExtent.height);
	}
	uint32_t image_count = surface.capabilities.minImageCount + 1;
	// v0.0.10pro phase 2: under the Adreno profile one more swapchain image keeps MAILBOX from
	// throttling vkAcquireNextImageKHR when the emulator runs ahead of a 90/120 Hz display
	// (the high-refresh unlock presents up to twice as often as the old 60 Hz lock).
	// `setprop debug.capy.swapchain_extra N` overrides (0 = the plain +1 of other GPUs).
	{
		static Common::DebugFlags::CachedValue extra_images("debug.capy.swapchain_extra", -1);
		const int64_t extra =
		    extra_images.Get() >= 0 ? extra_images.Get()
		                            : (Common::GpuProfile::AdrenoProfileEnabled() ? 1 : 0);
		image_count += static_cast<uint32_t>(std::max<int64_t>(extra, 0));
	}
	if (surface.capabilities.maxImageCount != 0) {
		image_count = std::min(image_count, surface.capabilities.maxImageCount);
	}
	const auto transform =
	    surface.capabilities.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity
	        ? vk::SurfaceTransformFlagBitsKHR::eIdentity
	        : surface.capabilities.currentTransform;
	const auto composite =
	    surface.capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque
	        ? vk::CompositeAlphaFlagBitsKHR::eOpaque
	        : vk::CompositeAlphaFlagBitsKHR::eInherit;

	vk::SurfaceFormatKHR format {vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
	if (surface.formats.size() != 1 || surface.formats.front().format != vk::Format::eUndefined) {
		auto it = surface.formats.end();
		if (m_want_srgb) {
			it = std::find_if(surface.formats.begin(), surface.formats.end(),
			                  [](const vk::SurfaceFormatKHR& candidate) {
				                  return candidate.format == vk::Format::eB8G8R8A8Srgb ||
				                         candidate.format == vk::Format::eR8G8B8A8Srgb;
			                  });
		}
		if (it == surface.formats.end()) {
			it = std::find_if(surface.formats.begin(), surface.formats.end(),
			                  [](const vk::SurfaceFormatKHR& candidate) {
				                  return candidate.format == vk::Format::eB8G8R8A8Unorm ||
				                         candidate.format == vk::Format::eR8G8B8A8Unorm;
			                  });
		}
		if (it == surface.formats.end()) {
			EXIT("no supported UNORM swapchain format\n");
		}
		format = *it;
	}
	m_format                      = format.format;
	m_srgb_unavailable            = m_want_srgb && m_format != vk::Format::eB8G8R8A8Srgb &&
	                                m_format != vk::Format::eR8G8B8A8Srgb;
	const auto swapchain_features = graphics.GetFormatProperties(m_format).optimalTilingFeatures;
	if (!static_cast<bool>(swapchain_features & vk::FormatFeatureFlagBits::eBlitDst)) {
		EXIT("swapchain format cannot be a blit destination: format=%d\n",
		     static_cast<int>(m_format));
	}

	vk::SwapchainCreateInfoKHR create_info {};
	create_info.sType            = vk::StructureType::eSwapchainCreateInfoKHR;
	create_info.surface          = m_window.surface;
	create_info.minImageCount    = image_count;
	create_info.imageFormat      = format.format;
	create_info.imageColorSpace  = format.colorSpace;
	create_info.imageExtent      = m_extent;
	create_info.imageArrayLayers = 1;
	create_info.imageUsage =
	    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
	create_info.imageSharingMode = vk::SharingMode::eExclusive;
	create_info.preTransform     = transform;
	create_info.compositeAlpha   = composite;
	switch (Config::GetPresentMode()) {
		case Config::PresentMode::Mailbox:
			create_info.presentMode = vk::PresentModeKHR::eMailbox;
			break;
		case Config::PresentMode::Immediate:
			create_info.presentMode = vk::PresentModeKHR::eImmediate;
			break;
		case Config::PresentMode::Fifo:
		default: create_info.presentMode = vk::PresentModeKHR::eFifo; break;
	}
	if (std::find(surface.present_modes.begin(), surface.present_modes.end(),
	              create_info.presentMode) == surface.present_modes.end()) {
		LOGF("warning: requested present mode is unavailable; falling back to Fifo\n");
		create_info.presentMode = vk::PresentModeKHR::eFifo;
	}
	create_info.clipped = VK_TRUE;
	RequireVulkanSuccess(graphics.device.createSwapchainKHR(&create_info, nullptr, &m_handle),
	                     "vkCreateSwapchainKHR");
	EXIT_IF(m_handle == nullptr);

	m_images = EnumerateVulkan<vk::Image>(
	    "vkGetSwapchainImagesKHR", [&](uint32_t* count, vk::Image* images) {
		    return graphics.device.getSwapchainImagesKHR(m_handle, count, images);
	    });
	EXIT_NOT_IMPLEMENTED(m_images.empty());

	m_image_views.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		vk::ImageViewCreateInfo view {};
		view.sType                           = vk::StructureType::eImageViewCreateInfo;
		view.image                           = m_images[i];
		view.viewType                        = vk::ImageViewType::e2D;
		view.format                          = m_format;
		view.components                      = {};
		view.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
		view.subresourceRange.baseArrayLayer = 0;
		view.subresourceRange.baseMipLevel   = 0;
		view.subresourceRange.layerCount     = 1;
		view.subresourceRange.levelCount     = 1;
		RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &m_image_views[i]),
		                     "vkCreateImageView");
		EXIT_IF(m_image_views[i] == nullptr);
	}

	vk::SemaphoreCreateInfo semaphore_info {};
	semaphore_info.sType = vk::StructureType::eSemaphoreCreateInfo;
	m_image_acquired.resize(m_images.size());
	m_render_complete.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_image_acquired[i]),
		    "create swapchain image-acquired semaphore");
		RequireVulkanSuccess(
		    graphics.device.createSemaphore(&semaphore_info, nullptr, &m_render_complete[i]),
		    "create swapchain render-complete semaphore");
	}
	m_image_index = static_cast<uint32_t>(-1);
	m_frame_index = 0;
}

Swapchain::~Swapchain() {
	Destroy();
}

#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)

void Swapchain::DestroyGlBackend() {
	auto& graphics = m_window.graphic_ctx;
	for (const auto fd: m_release_fd) {
		if (fd >= 0) {
			SyncFenceWait(fd, 100);
			close(fd);
		}
	}
	m_release_fd.clear();
	if (m_gl != nullptr) {
		m_gl->Destroy();
		m_gl.reset();
	}
	for (const auto view: m_image_views) {
		if (view != nullptr) {
			graphics.device.destroyImageView(view, nullptr);
		}
	}
	m_image_views.clear();
	for (const auto image: m_images) {
		if (image != nullptr) {
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImage(static_cast<VkDevice>(graphics.device),
			                                          static_cast<VkImage>(image), nullptr);
		}
	}
	m_images.clear();
	for (const auto memory: m_ahb_memory) {
		if (memory != nullptr) {
			VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory(static_cast<VkDevice>(graphics.device),
			                                     static_cast<VkDeviceMemory>(memory), nullptr);
		}
	}
	m_ahb_memory.clear();
	for (const auto buffer: m_ahb) {
		if (buffer != nullptr) {
			AHardwareBuffer_release(buffer);
		}
	}
	m_ahb.clear();
	m_gl_mode = false;
}

bool Swapchain::TryCreateGlBackend() {
	auto& graphics = m_window.graphic_ctx;
	EXIT_IF(graphics.device == nullptr);
	constexpr uint32_t kRingSize = 3;

	// The frame is drawn into the ring image with the present blit pipeline, so the
	// buffer needs sampled-image (EGL) and color-output (Vulkan) usage only.
	AHardwareBuffer_Desc description {};
	description.width  = m_extent.width;
	description.height = m_extent.height;
	description.layers = 1;
	description.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
	description.usage  = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
	                     AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;

	const auto get_ahb_properties =
	    reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
	        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(
	            static_cast<VkDevice>(graphics.device), "vkGetAndroidHardwareBufferPropertiesANDROID"));
	if (get_ahb_properties == nullptr) {
		LOGF("GL backend: vkGetAndroidHardwareBufferPropertiesANDROID is missing\n");
		return false;
	}

	m_gl_mode = true; // DestroyGlBackend() unwinds partial state on failure
	m_ahb.assign(kRingSize, nullptr);
	m_ahb_memory.assign(kRingSize, nullptr);
	m_release_fd.assign(kRingSize, -1);

	const auto fail = [&] {
		DestroyGlBackend();
		return false;
	};

	for (uint32_t i = 0; i < kRingSize; i++) {
		AHardwareBuffer* buffer = nullptr;
		if (AHardwareBuffer_allocate(&description, &buffer) != 0 || buffer == nullptr) {
			LOGF("GL backend: AHardwareBuffer_allocate failed (%ux%u)\n", m_extent.width,
			     m_extent.height);
			return fail();
		}
		m_ahb[i] = buffer;

		VkAndroidHardwareBufferFormatPropertiesANDROID format_properties {};
		format_properties.sType =
			VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
		VkAndroidHardwareBufferPropertiesANDROID properties {};
		properties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
		properties.pNext = &format_properties;
		if (get_ahb_properties(static_cast<VkDevice>(graphics.device), buffer, &properties) !=
		    VK_SUCCESS) {
			LOGF("GL backend: buffer properties query failed\n");
			return fail();
		}
		if (format_properties.format != VK_FORMAT_R8G8B8A8_UNORM ||
		    (format_properties.formatFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) {
			LOGF("GL backend: the buffer cannot be a color attachment\n");
			return fail();
		}

		VkExternalMemoryImageCreateInfo external {};
		external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
		VkImageCreateInfo image_info {};
		image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.pNext = &external;
		image_info.imageType = VK_IMAGE_TYPE_2D;
		image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
		image_info.extent = {m_extent.width, m_extent.height, 1};
		image_info.mipLevels = 1;
		image_info.arrayLayers = 1;
		image_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkImage image = VK_NULL_HANDLE;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateImage(static_cast<VkDevice>(graphics.device),
		                                              &image_info, nullptr, &image) != VK_SUCCESS ||
		    image == VK_NULL_HANDLE) {
			LOGF("GL backend: vkCreateImage failed for the AHardwareBuffer\n");
			return fail();
		}
		m_images.push_back(image);

		uint32_t memory_type = std::numeric_limits<uint32_t>::max();
		for (uint32_t type = 0; type < graphics.physical_device_memory_properties.memoryTypeCount;
		     type++) {
			if ((properties.memoryTypeBits & (1u << type)) != 0) {
				memory_type = type;
				break;
			}
		}
		if (memory_type == std::numeric_limits<uint32_t>::max()) {
			LOGF("GL backend: no compatible memory type\n");
			return fail();
		}
		VkImportAndroidHardwareBufferInfoANDROID import {};
		import.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
		import.buffer = buffer;
		VkMemoryAllocateInfo allocate {};
		allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate.pNext = &import;
		allocate.allocationSize = properties.allocationSize;
		allocate.memoryTypeIndex = memory_type;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateMemory(static_cast<VkDevice>(graphics.device),
		                                             &allocate, nullptr, &memory) != VK_SUCCESS ||
		    memory == VK_NULL_HANDLE) {
			LOGF("GL backend: vkAllocateMemory failed\n");
			return fail();
		}
		m_ahb_memory[i] = memory;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkBindImageMemory(static_cast<VkDevice>(graphics.device),
		                                                 image, memory, 0) != VK_SUCCESS) {
			LOGF("GL backend: vkBindImageMemory failed\n");
			return fail();
		}

		vk::ImageViewCreateInfo view {};
		view.image = m_images.back();
		view.viewType = vk::ImageViewType::e2D;
		view.format = vk::Format::eR8G8B8A8Unorm;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		vk::ImageView handle = nullptr;
		if (graphics.device.createImageView(&view, nullptr, &handle) != vk::Result::eSuccess ||
		    handle == nullptr) {
			LOGF("GL backend: vkCreateImageView failed\n");
			return fail();
		}
		m_image_views.push_back(handle);
	}

	m_gl = std::make_unique<GlCompatRenderer>();
	if (!m_gl->Initialize(m_extent.width, m_extent.height)) {
		return fail();
	}

	vk::SemaphoreCreateInfo semaphore_info {};
	semaphore_info.sType = vk::StructureType::eSemaphoreCreateInfo;
	m_image_acquired.resize(m_images.size());
	m_render_complete.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); i++) {
		RequireVulkanSuccess(
			graphics.device.createSemaphore(&semaphore_info, nullptr, &m_image_acquired[i]),
			"create GL-backend image-acquired semaphore");
		RequireVulkanSuccess(
			graphics.device.createSemaphore(&semaphore_info, nullptr, &m_render_complete[i]),
			"create GL-backend render-complete semaphore");
	}
	m_format = vk::Format::eR8G8B8A8Unorm;
	m_image_index = static_cast<uint32_t>(-1);
	m_frame_index = 0;
	LOGF("GL backend: %u AHardwareBuffer ring images (%ux%u), presenting through OpenGL ES\n",
	     static_cast<uint32_t>(m_images.size()), m_extent.width, m_extent.height);
	return true;
}

void Swapchain::RecordGlPresentCommands(CommandBuffer& command, Presenter::Frame* source,
						      const Presenter::Layer& overlay, bool draw_system_overlay) {
	EXIT_IF(m_image_index >= m_images.size());
	auto       vk_command   = command.Handle();
	const bool draw_overlay = overlay.frame != nullptr;

	if (source != nullptr) {
		source->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal,
		                vk::AccessFlagBits2::eShaderRead);
	}
	if (draw_overlay) {
		overlay.frame->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal,
		                       vk::AccessFlagBits2::eShaderRead);
	}

	// The ring image becomes a color attachment; whatever EGL read from it is
	// discarded - the frame triangle (or the black clear below) covers the buffer.
	vk::ImageMemoryBarrier2 to_attachment {};
	to_attachment.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
	to_attachment.srcAccessMask = vk::AccessFlagBits2::eMemoryRead;
	to_attachment.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
	to_attachment.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
	to_attachment.oldLayout = vk::ImageLayout::eUndefined;
	to_attachment.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
	to_attachment.image = m_images[m_image_index];
	to_attachment.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo to_attachment_dependency {};
	to_attachment_dependency.imageMemoryBarrierCount = 1;
	to_attachment_dependency.pImageMemoryBarriers = &to_attachment;
	vk_command.pipelineBarrier2(to_attachment_dependency);

	if (source == nullptr) {
		// No main layer this flip: clear the ring image to black first.
		vk::RenderingAttachmentInfo clear_attachment {};
		clear_attachment.imageView = m_image_views[m_image_index];
		clear_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		clear_attachment.loadOp = vk::AttachmentLoadOp::eClear;
		clear_attachment.storeOp = vk::AttachmentStoreOp::eStore;
		clear_attachment.clearValue.color.float32 = std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
		vk::RenderingInfo clear_rendering {};
		clear_rendering.renderArea.extent = m_extent;
		clear_rendering.layerCount = 1;
		clear_rendering.colorAttachmentCount = 1;
		clear_rendering.pColorAttachments = &clear_attachment;
		vk_command.beginRendering(&clear_rendering);
		vk_command.endRendering();
	} else {
		// The prepared frame fills the buffer: same blit pipeline the video-out
		// overlay uses, drawn opaque (the source is a full-frame texture).
		const Presenter::Layer base {source, 0, true};
		DrawOverlay(vk_command, base, true);
	}
	if (draw_overlay) {
		DrawOverlay(vk_command, overlay);
	}

	vk::ImageMemoryBarrier2 to_general {};
	to_general.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
	to_general.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
	to_general.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
	to_general.dstAccessMask = vk::AccessFlagBits2::eMemoryRead;
	to_general.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
	to_general.newLayout = vk::ImageLayout::eGeneral;
	to_general.image = m_images[m_image_index];
	to_general.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo to_general_dependency {};
	to_general_dependency.imageMemoryBarrierCount = 1;
	to_general_dependency.pImageMemoryBarriers = &to_general;
	vk_command.pipelineBarrier2(to_general_dependency);

	if (draw_system_overlay && m_system_overlay != nullptr) {
		m_system_overlay->Record(vk_command, m_image_views[m_image_index]);
	}
}

#endif // Android

void Swapchain::Destroy() {
	if (m_handle == nullptr && m_image_acquired.empty() && m_render_complete.empty() &&
	    m_image_views.empty()) {
		return;
	}
	auto& graphics = m_window.graphic_ctx;

	{
		Common::LockGuard queue_lock(graphics.queue_mutex);
		RequireVulkanSuccess(graphics.queue.waitIdle(), "wait for swapchain queue");
	}
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	if (m_gl_mode) {
		DestroyGlBackend();
	}
#endif
	if (m_system_overlay != nullptr) {
		m_system_overlay->ReleaseVulkan();
	}
	graphics.device.destroyPipeline(m_overlay_pipeline, nullptr);
	graphics.device.destroyPipelineLayout(m_overlay_layout, nullptr);
	graphics.device.destroyDescriptorSetLayout(m_overlay_descriptors, nullptr);
	graphics.device.destroySampler(m_overlay_sampler, nullptr);
	m_overlay_pipeline    = nullptr;
	m_overlay_layout      = nullptr;
	m_overlay_descriptors = nullptr;
	m_overlay_sampler     = nullptr;

	for (const auto semaphore: m_image_acquired) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto semaphore: m_render_complete) {
		if (semaphore != nullptr) {
			graphics.device.destroySemaphore(semaphore, nullptr);
		}
	}
	for (const auto view: m_image_views) {
		if (view != nullptr) {
			graphics.device.destroyImageView(view, nullptr);
		}
	}
	if (m_handle != nullptr) {
		graphics.device.destroySwapchainKHR(m_handle, nullptr);
	}

	m_handle        = nullptr;
	m_gl_mode       = false;
	m_format        = vk::Format::eUndefined;
	m_extent        = {};
	m_window_extent = {};
	m_image_index   = static_cast<uint32_t>(-1);
	m_frame_index   = 0;
	m_images.clear();
	m_image_views.clear();
	m_image_acquired.clear();
	m_render_complete.clear();
}

void Swapchain::Recreate(bool surface_lost) {
	Destroy();
	if (surface_lost) {
#if defined(__APPLE__)
		// Surface recreation goes through SDL_Vulkan_CreateSurface, which touches the
		// window's view/layer and must run on the main thread on macOS.
		EXIT_IF(!SDL_RunOnMainThread(
		    [](void* window) { static_cast<WindowContext*>(window)->RecreateSurface(); }, &m_window,
		    true));
#else
		m_window.RecreateSurface();
#endif
	}
	if (m_window.surface != nullptr) {
		Create();
	}
}

bool Swapchain::NeedsResize() const {
	if (m_gl_mode) {
		Common::LockGuard lock(m_window.mutex);
		return m_extent.width != m_window.graphic_ctx.screen_width ||
		       m_extent.height != m_window.graphic_ctx.screen_height;
	}
	const bool srgb = m_format == vk::Format::eB8G8R8A8Srgb || m_format == vk::Format::eR8G8B8A8Srgb;
	if (srgb != m_want_srgb && !m_srgb_unavailable) {
		return true;
	}
	Common::LockGuard lock(m_window.mutex);
	return m_window_extent.width != m_window.graphic_ctx.screen_width ||
	       m_window_extent.height != m_window.graphic_ctx.screen_height;
}

Swapchain::Status Swapchain::AcquireNextImage() {
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	if (m_gl_mode) {
		if (m_images.empty() || m_frame_index >= m_images.size() ||
		    m_release_fd.size() != m_images.size()) {
			return Status::SurfaceLost;
		}
		m_image_index = m_frame_index;
		// The previous OpenGL read of this ring slot must be finished before
		// the Vulkan pass overwrites the buffer.
		auto& fd = m_release_fd[m_image_index];
		if (fd >= 0) {
			if (SyncFenceWait(fd, 1000) != 0) {
				LOGF("GL present: release fence wait timed out\n");
			}
			close(fd);
			fd = -1;
		}
		return Status::Success;
	}
#endif
	if (m_handle == nullptr || m_images.empty() || m_frame_index >= m_image_acquired.size()) {
		return Status::SurfaceLost;
	}
	m_image_index     = static_cast<uint32_t>(-1);
	const auto result = m_window.graphic_ctx.device.acquireNextImageKHR(
	    m_handle, std::numeric_limits<uint64_t>::max(), m_image_acquired[m_frame_index], nullptr,
	    &m_image_index);
	switch (result) {
		case vk::Result::eSuccess:
		case vk::Result::eSuboptimalKHR: break;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorUnknown:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorUnknown\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default:
			LOGF("vkAcquireNextImageKHR failed: %s\n", vk::to_string(result).c_str());
			return Status::SurfaceLost;
	}
	if (m_image_index >= m_images.size()) {
		return Status::SurfaceLost;
	}
	return Status::Success;
}

bool Swapchain::PrepareSystemOverlay() {
	if (m_system_overlay == nullptr) {
		m_system_overlay = std::make_unique<SystemOverlay>(m_window.graphic_ctx);
	}
	return m_system_overlay->PrepareFrame(m_extent, m_format, ImageCount());
}

void Swapchain::DrawOverlay(vk::CommandBuffer command, const Presenter::Layer& layer,
	                           bool clear_attachment) {
	auto device = m_window.graphic_ctx.device;
	if (m_overlay_pipeline == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		RequireVulkanSuccess(device.createSampler(&sampler, nullptr, &m_overlay_sampler),
		                     "create video-out overlay sampler");
		const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eCombinedImageSampler,
		                                              1, vk::ShaderStageFlagBits::eFragment};
		vk::DescriptorSetLayoutCreateInfo    descriptors {};
		descriptors.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptors.bindingCount = 1;
		descriptors.pBindings    = &binding;
		RequireVulkanSuccess(
		    device.createDescriptorSetLayout(&descriptors, nullptr, &m_overlay_descriptors),
		    "create video-out overlay descriptor layout");
		const vk::PushConstantRange alpha {vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32_t)};
		vk::PipelineLayoutCreateInfo layout {};
		layout.setLayoutCount         = 1;
		layout.pSetLayouts            = &m_overlay_descriptors;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges    = &alpha;
		RequireVulkanSuccess(device.createPipelineLayout(&layout, nullptr, &m_overlay_layout),
		                     "create video-out overlay pipeline layout");
		const auto vertex   = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, device);
		const auto fragment = CompileSPV(GPU_VIDEO_OUT_OVERLAY_SPV, device);
		std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
		stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
		stages[0].module = vertex;
		stages[0].pName  = "main";
		stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
		stages[1].module = fragment;
		stages[1].pName  = "main";
		vk::PipelineVertexInputStateCreateInfo   vertex_input {};
		vk::PipelineInputAssemblyStateCreateInfo assembly {};
		assembly.topology = vk::PrimitiveTopology::eTriangleList;
		vk::PipelineViewportStateCreateInfo viewport {};
		viewport.viewportCount = 1;
		viewport.scissorCount  = 1;
		vk::PipelineRasterizationStateCreateInfo rasterization {};
		rasterization.lineWidth = 1.0f;
		vk::PipelineMultisampleStateCreateInfo multisample {};
		multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
		vk::PipelineColorBlendAttachmentState attachment {};
		attachment.blendEnable         = VK_TRUE;
		attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
		attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
		attachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		attachment.colorWriteMask = vk::ColorComponentFlagBits::eR |
		                            vk::ColorComponentFlagBits::eG |
		                            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
		vk::PipelineColorBlendStateCreateInfo blend {};
		blend.attachmentCount = 1;
		blend.pAttachments    = &attachment;
		const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
		vk::PipelineDynamicStateCreateInfo dynamic {};
		dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
		dynamic.pDynamicStates    = dynamic_states.data();
		vk::PipelineRenderingCreateInfo rendering {};
		rendering.colorAttachmentCount    = 1;
		rendering.pColorAttachmentFormats = &m_format;
		vk::GraphicsPipelineCreateInfo create {};
		create.pNext               = &rendering;
		create.stageCount          = static_cast<uint32_t>(stages.size());
		create.pStages             = stages.data();
		create.pVertexInputState   = &vertex_input;
		create.pInputAssemblyState = &assembly;
		create.pViewportState      = &viewport;
		create.pRasterizationState = &rasterization;
		create.pMultisampleState   = &multisample;
		create.pColorBlendState    = &blend;
		create.pDynamicState       = &dynamic;
		create.layout              = m_overlay_layout;
		RequireVulkanSuccess(
		    device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &m_overlay_pipeline),
		    "create video-out overlay pipeline");
		device.destroyShaderModule(fragment, nullptr);
		device.destroyShaderModule(vertex, nullptr);
	}
	auto& frame = *layer.frame;
	if (frame.view == nullptr) {
		vk::ImageViewCreateInfo view {};
		view.image            = frame.image.image;
		view.viewType         = vk::ImageViewType::e2D;
		view.format           = frame.image.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		RequireVulkanSuccess(device.createImageView(&view, nullptr, &frame.view),
		                     "create video-out overlay image view");
	}
	const vk::DescriptorImageInfo image {m_overlay_sampler, frame.view,
	                                     vk::ImageLayout::eShaderReadOnlyOptimal};
	vk::WriteDescriptorSet        write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo      = &image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_overlay_layout, 0, 1, &write);
	const uint32_t premultiplied = layer.premultiplied_alpha;
	command.pushConstants(m_overlay_layout, vk::ShaderStageFlagBits::eFragment, 0,
	                      sizeof(premultiplied), &premultiplied);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_overlay_pipeline);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = m_image_views[m_image_index];
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp = clear_attachment ? vk::AttachmentLoadOp::eClear
	                                   : vk::AttachmentLoadOp::eLoad;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = m_extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	const vk::Viewport viewport {
	    0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height),
	    0.0f, 1.0f};
	const vk::Rect2D scissor {{0, 0}, m_extent};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

void Swapchain::RecordPresentCommands(CommandBuffer& command, Presenter::Frame* source,
                                      const Presenter::Layer& overlay, bool draw_system_overlay) {
	EXIT_IF(m_image_index >= m_images.size());
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	if (m_gl_mode) {
		RecordGlPresentCommands(command, source, overlay, draw_system_overlay);
		return;
	}
#endif
	// The GL backend hands the buffer to EGL from GENERAL; a Vulkan swapchain
	// presentation needs PRESENT_SRC (m_gl_mode is constant false off Android).
	const auto present_layout =
	    m_gl_mode ? vk::ImageLayout::eGeneral : vk::ImageLayout::ePresentSrcKHR;
	auto       vk_command      = command.Handle();
	const bool draw_overlay    = overlay.frame != nullptr;
	const bool draw_attachment = draw_overlay || draw_system_overlay;
	if (source != nullptr) {
		source->Transit(vk_command, vk::ImageLayout::eTransferSrcOptimal,
		                vk::AccessFlagBits2::eTransferRead);
	}
	if (draw_overlay) {
		overlay.frame->Transit(vk_command, vk::ImageLayout::eShaderReadOnlyOptimal,
		                       vk::AccessFlagBits2::eShaderRead);
	}

	vk::ImageMemoryBarrier to_transfer {};
	to_transfer.sType                           = vk::StructureType::eImageMemoryBarrier;
	to_transfer.dstAccessMask                   = vk::AccessFlagBits::eTransferWrite;
	to_transfer.oldLayout                       = vk::ImageLayout::eUndefined;
	to_transfer.newLayout                       = vk::ImageLayout::eTransferDstOptimal;
	to_transfer.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.image                           = m_images[m_image_index];
	to_transfer.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_transfer.subresourceRange.baseMipLevel   = 0;
	to_transfer.subresourceRange.levelCount     = 1;
	to_transfer.subresourceRange.baseArrayLayer = 0;
	to_transfer.subresourceRange.layerCount     = 1;
	// Match the acquire wait stage so the layout transition cannot precede acquisition.
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags {}, 0,
	                           nullptr, 0, nullptr, 1, &to_transfer);

	if (source != nullptr) {
		// v0.0.9proQ1: when the frame already matches the swapchain exactly, a 1:1 copy
		// replaces the scaled blit — no sampler pass on the way out, cheaper on every GPU,
		// identical pixels (native-resolution output at a 100% scale).
		const bool exact_copy = source->image.extent.width == m_extent.width &&
		                      source->image.extent.height == m_extent.height &&
		                      source->image.format == m_format && source->image.samples == 1;
		if (exact_copy) {
			vk::ImageCopy region {};
			region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
			region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
			region.extent         = {m_extent.width, m_extent.height, 1};
			vk_command.copyImage(source->image.image, vk::ImageLayout::eTransferSrcOptimal,
			                     m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal, 1,
			                     &region);
		} else {
			vk::ImageBlit region {};
			region.srcSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
			region.srcSubresource.mipLevel       = 0;
			region.srcSubresource.baseArrayLayer = 0;
			region.srcSubresource.layerCount     = 1;
			region.srcOffsets[1].x               = static_cast<int>(source->image.extent.width);
			region.srcOffsets[1].y               = static_cast<int>(source->image.extent.height);
			region.srcOffsets[1].z               = 1;
			region.dstSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
			region.dstSubresource.mipLevel       = 0;
			region.dstSubresource.baseArrayLayer = 0;
			region.dstSubresource.layerCount     = 1;
			region.dstOffsets[1].x               = static_cast<int>(m_extent.width);
			region.dstOffsets[1].y               = static_cast<int>(m_extent.height);
			region.dstOffsets[1].z               = 1;
				vk_command.blitImage(source->image.image, vk::ImageLayout::eTransferSrcOptimal,
				                     m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal, 1,
				                     &region, vk::Filter::eLinear);
		}
	} else {
		const vk::ClearColorValue black {};
		vk_command.clearColorImage(m_images[m_image_index], vk::ImageLayout::eTransferDstOptimal,
		                           &black, 1, &to_transfer.subresourceRange);
	}

	vk::ImageMemoryBarrier to_present {};
	to_present.sType               = vk::StructureType::eImageMemoryBarrier;
	to_present.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	to_present.dstAccessMask       = draw_attachment ? vk::AccessFlagBits::eColorAttachmentRead |
	                                                       vk::AccessFlagBits::eColorAttachmentWrite
	                                                 : vk::AccessFlagBits::eMemoryRead;
	to_present.oldLayout           = vk::ImageLayout::eTransferDstOptimal;
	to_present.newLayout           = draw_attachment ? vk::ImageLayout::eColorAttachmentOptimal
	                                                 : present_layout;
	to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.image               = m_images[m_image_index];
	to_present.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
	to_present.subresourceRange.baseMipLevel   = 0;
	to_present.subresourceRange.levelCount     = 1;
	to_present.subresourceRange.baseArrayLayer = 0;
	to_present.subresourceRange.layerCount     = 1;
	vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                           draw_attachment ? vk::PipelineStageFlagBits::eColorAttachmentOutput
	                                           : vk::PipelineStageFlagBits::eAllCommands,
	                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
	                           &to_present);
	if (draw_attachment) {
		if (draw_overlay) {
			DrawOverlay(vk_command, overlay);
			if (draw_system_overlay) {
				to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
				to_present.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
				vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
				                           vk::PipelineStageFlagBits::eColorAttachmentOutput,
				                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0,
				                           nullptr, 1, &to_present);
			}
		}
		if (draw_system_overlay) {
			m_system_overlay->Record(vk_command, m_image_views[m_image_index]);
		}
		to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
		to_present.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
		to_present.oldLayout     = vk::ImageLayout::eColorAttachmentOptimal;
		to_present.newLayout     = present_layout;
		vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                           vk::PipelineStageFlagBits::eAllCommands,
		                           vk::DependencyFlagBits::eByRegion, 0, nullptr, 0, nullptr, 1,
		                           &to_present);
	}
}

uint64_t Swapchain::Submit(CommandScheduler& scheduler) {
	if (m_frame_index >= m_image_acquired.size() || m_image_index >= m_render_complete.size()) {
		return 0;
	}
	SubmitInfo submit;
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	if (m_gl_mode) {
		// The GL backend needs no external semaphores: Present() host-waits this
		// tick before handing the buffer to OpenGL.
		m_last_submit_scheduler = &scheduler;
		m_last_submit_tick      = scheduler.Submit();
		return m_last_submit_tick;
	}
#endif
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	submit.AddSignal(m_render_complete[m_image_index]);
	return scheduler.Submit(submit);
}

Swapchain::Status Swapchain::Present() {
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	if (m_gl_mode) {
		if (m_images.empty() || m_image_index >= m_images.size() || m_gl == nullptr ||
		    m_release_fd.size() != m_images.size()) {
			return Status::SurfaceLost;
		}
		// Host-wait the Vulkan pass, then let OpenGL ES present the buffer.
		if (m_last_submit_scheduler != nullptr) {
			m_last_submit_scheduler->Wait(m_last_submit_tick);
			m_last_submit_scheduler = nullptr;
		}
		const auto release = m_gl->Blit(m_ahb[m_image_index], -1);
		if (release < 0) {
			s_gl_backend_recoveries++;
			LOGF("GL present failed (%u); recovering\n", s_gl_backend_recoveries);
			if (s_gl_backend_recoveries >= kGlBackendMaxRecoveries) {
				Log::WriteToConsoleAndLog(
					"GL backend: too many failures; switching to the Vulkan swapchain\n");
			}
			return Status::Recreate;
		}
		if (m_release_fd[m_image_index] >= 0) {
			close(m_release_fd[m_image_index]);
		}
		m_release_fd[m_image_index] = release;
		m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
		Common::Telemetry::g_host_presents.fetch_add(1, std::memory_order_relaxed);
		return Status::Success;
	}
#endif
	if (m_handle == nullptr || m_images.empty() || m_image_index >= m_render_complete.size()) {
		return Status::SurfaceLost;
	}
	const auto         ready = m_render_complete[m_image_index];
	vk::PresentInfoKHR present {};
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.swapchainCount     = 1;
	present.pSwapchains        = &m_handle;
	present.pImageIndices      = &m_image_index;
	present.pWaitSemaphores    = &ready;
	present.waitSemaphoreCount = 1;

	vk::Result result;
	{
		Common::LockGuard lock(m_window.graphic_ctx.queue_mutex);
		result = m_window.graphic_ctx.queue.presentKHR(&present);
	}
	switch (result) {
		case vk::Result::eSuccess:
		case vk::Result::eSuboptimalKHR: break;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default:
			LOGF("vkQueuePresentKHR failed: %s\n", vk::to_string(result).c_str());
			return Status::SurfaceLost;
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	Common::Telemetry::g_host_presents.fetch_add(1, std::memory_order_relaxed);
	return Status::Success;
}

Presenter::Presenter(WindowContext& window): m_impl(std::make_unique<Impl>(window)) {}

Presenter::~Presenter() = default;

Presenter::Frame& Presenter::PrepareFrame(CommandBuffer& buffer, const ImageInfo& info) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
	auto&             image = m_impl->ResolveSurface(info);
	if (image.backing.format == vk::Format::eUndefined) {
		EXIT("unsupported presentation source, image=%p\n", static_cast<const void*>(&image));
	}
	// The frame takes the format of the guest image as it is stored: the Adreno driver copied
	// between different formats (SRGB into UNORM, or a backing format other than the declared
	// one) through a temporary allocation it made and freed every frame, which held Animal
	// Well at 30 FPS and Monster Boy at 20. The swapchain follows the frame (see Present()).
	const auto guest_format = image.backing.format;
	// v0.0.9proQ3: the GL backend presents through an RGBA8 UNORM AHardwareBuffer;
	// keeping the prepared frame in the UNORM variant of the guest format carries
	// the sRGB-encoded bytes to the screen untouched (Android displays them as
	// sRGB), exactly like the UNORM-swapchain fallback of the Vulkan path.
	auto frame_format = guest_format;
	if (m_impl->swapchain.GlBackend()) {
		if (frame_format == vk::Format::eB8G8R8A8Srgb) {
			frame_format = vk::Format::eB8G8R8A8Unorm;
		} else if (frame_format == vk::Format::eR8G8B8A8Srgb) {
			frame_format = vk::Format::eR8G8B8A8Unorm;
		}
	}
	Common::DebugFlags::NoteOnce("CapyPS5-Present", 0x50464d54u, "frame format %d (declared %d)",
	                             static_cast<int>(frame_format), static_cast<int>(info.pixel_format));
	auto* frame = m_impl->frames.Acquire({info.extent.width, info.extent.height}, frame_format);
	frame->Configure(m_impl->window.graphic_ctx,
	                 {image.backing.extent.width, image.backing.extent.height}, frame_format);
	frame->CopyFrom(buffer, image);
	return *frame;
}

Presenter::Frame& Presenter::PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
                                               CommandBuffer* producer) {
	KYTY_PROFILER_FUNCTION();
	auto              format = m_impl->frames.GetFormat();
	auto*             frame  = m_impl->frames.Acquire({width, height}, format);
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
	frame->Configure(m_impl->window.graphic_ctx, {width, height}, format);
	vk::ClearColorValue clear {};
	clear.float32[3] = opaque ? 1.0f : 0.0f;
	if (producer != nullptr) {
		EXIT_IF(producer->IsInvalid());
		frame->Clear(*producer, clear);
	} else {
		auto& command = m_impl->present_scheduler.BeginCommand();
		frame->Clear(command, clear);
		frame->present_tick = m_impl->present_scheduler.Submit();
	}
	return *frame;
}

bool Presenter::PresentLastFrame() {
	Common::LockGuard lock(m_impl->present_mutex);
	if (m_impl->layers[0].frame == nullptr && m_impl->layers[1].frame == nullptr) {
		return false;
	}
	m_impl->Present();
	return true;
}

bool Presenter::IsGuestPaused() const noexcept {
	return m_impl->window.loop.paused.load(std::memory_order_acquire);
}

bool Presenter::NeedsSystemOverlayRefresh() const noexcept {
	const auto visual = GetSystemOverlayVisualState();
	return visual.active ||
	       visual.revision != m_impl->presented_overlay_revision.load(std::memory_order_acquire);
}

RenderContext& Presenter::Renderer() const noexcept {
	return m_impl->renderer;
}

void Presenter::Present(Frame& frame) {
	const Layer layer {&frame, 0, false};
	Present(std::span(&layer, 1));
}

void Presenter::Present(std::span<const Layer> layers) {
	Common::LockGuard lock(m_impl->present_mutex);
	for (const auto& layer: layers) {
		EXIT_IF(layer.bus < 0 || layer.bus >= static_cast<int>(m_impl->layers.size()));
		m_impl->frames.ValidateForPresent(layer.frame);
		auto& previous = m_impl->layers[layer.bus];
		if (previous.frame != nullptr) {
			m_impl->frames.Release(previous.frame);
		}
		previous = layer;
	}
	m_impl->Present();
}

void Presenter::ClearLayer(int bus) {
	if (static_cast<size_t>(bus) >= m_impl->layers.size()) {
		return;
	}
	Common::LockGuard lock(m_impl->present_mutex);
	auto& layer = m_impl->layers[bus];
	if (layer.frame != nullptr) {
		m_impl->frames.Release(layer.frame);
		layer = {};
	}
}

void Presenter::Impl::Present() {
	KYTY_PROFILER_FUNCTION();

	const auto overlay_visual = GetSystemOverlayVisualState();
	if (Common::DebugFlags::Enabled(Common::DebugFlags::IdleBeforePresent)) {
		Common::LockGuard queue_lock(window.graphic_ctx.queue_mutex);
		(void)window.graphic_ctx.device.waitIdle();
	}
	if (layers[0].frame != nullptr) {
		const auto format = layers[0].frame->image.format;
		swapchain.SetWantSrgb(format == vk::Format::eB8G8R8A8Srgb || format == vk::Format::eR8G8B8A8Srgb);
	}
	// Some window systems keep presenting an old swapchain after a resize.
	if (swapchain.IsReady() && swapchain.NeedsResize()) {
		RecoverSwapchain(Swapchain::Status::Recreate);
	}
	for (uint32_t attempt = 0; attempt < 2; attempt++) {
		if (!swapchain.IsReady()) {
			RecoverSwapchain(Swapchain::Status::SurfaceLost);
			if (!swapchain.IsReady()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(16));
				return;
			}
		}
		auto status = swapchain.AcquireNextImage();
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			continue;
		}
		{
			Common::LockGuard render_lock(renderer.GetMutex());
			auto&             command = present_scheduler.BeginCommand();
			const bool        draw_system_overlay =
			    overlay_visual.active && swapchain.PrepareSystemOverlay();
			swapchain.RecordPresentCommands(command, layers[0].frame, layers[1],
			                                draw_system_overlay);
			const auto tick = swapchain.Submit(present_scheduler);
			for (const auto& layer: layers) {
				if (layer.frame != nullptr) {
					layer.frame->present_tick = tick;
				}
			}
		}
		status = swapchain.Present();
		if (status != Swapchain::Status::Success) {
			RecoverSwapchain(status);
			continue;
		}

		presented_overlay_revision.store(overlay_visual.revision, std::memory_order_release);
		window.loop.presented_frames.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	LOGF("Vulkan presentation retry exhausted; dropping frame\n");
}

void Presenter::Discard(Frame& frame) {
	m_impl->frames.Release(&frame);
}

WindowContext::WindowContext() = default;

} // namespace Libs::Graphics
