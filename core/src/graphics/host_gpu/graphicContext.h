// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/vulkanCommon.h" // IWYU pragma: export

#include <map>
#include <mutex>
#include <tuple>
#include <vector>
#include <vk_mem_alloc.h>

namespace Libs::Graphics {

struct VulkanImage;

inline constexpr uint32_t VULKAN_TARGET_API_VERSION = VK_API_VERSION_1_3;

// v0.0.10pro phase 3: Mali devices can run the Vulkan backend on a Vulkan 1.1 instance
// (frontend setting "Vulkan version", property debug.capy.vk_api, default 1.3). Some Mali
// drivers implement the features the renderer needs only through extensions, or misbehave
// on their 1.3 instance path; with the option at 1.1 the instance is created at 1.1 and
// the promoted functionality is enabled through its extensions instead.
inline constexpr uint32_t VULKAN_COMPAT_API_VERSION = VK_API_VERSION_1_1;

// Functionality the renderer requires that Vulkan 1.2/1.3 promoted from extensions. On a
// 1.1 instance each entry must be enabled through its extension at device creation, and a
// device that lacks one cannot run the renderer - VulkanFindPhysicalDevice rejects it.
inline constexpr const char* VULKAN11_MANDATORY_EXTENSIONS[] = {
	"VK_KHR_dynamic_rendering",             // dynamicRendering (core 1.3)
	"VK_KHR_synchronization2",             // synchronization2 (core 1.3)
	"VK_KHR_timeline_semaphore",           // timelineSemaphore (core 1.2)
	"VK_KHR_buffer_device_address",        // bufferDeviceAddress (core 1.2)
	"VK_EXT_descriptor_indexing",          // shaderSampledImageArrayNonUniformIndexing (core 1.2)
	"VK_KHR_sampler_mirror_clamp_to_edge", // samplerMirrorClampToEdge (core 1.2)
	"VK_EXT_image_robustness",             // robustImageAccess (core 1.3)
};

// Promoted functionality the renderer only requests when the driver reports the feature:
// each entry is enabled when present and the existing fallback paths cover its absence.
inline constexpr const char* VULKAN11_OPTIONAL_EXTENSIONS[] = {
	"VK_KHR_8bit_storage",                       // storageBuffer8BitAccess (core 1.2)
	"VK_KHR_shader_draw_parameters",             // shaderDrawParameters (core 1.1)
	"VK_EXT_subgroup_size_control",              // subgroupSizeControl (core 1.3)
	"VK_EXT_shader_demote_to_helper_invocation", // shaderDemoteToHelperInvocation (core 1.3)
	"VK_KHR_shader_terminate_invocation",        // shaderTerminateInvocation (core 1.3)
	"VK_KHR_spirv_1_4",                          // SPIR-V 1.4 modules (mesh shaders) under 1.1
};

[[nodiscard]] inline constexpr bool Vulkan11Instance(uint32_t api_version) {
	return api_version <= VULKAN_COMPAT_API_VERSION;
}

struct GraphicContext {
	vk::Instance                       instance                              = nullptr;
	vk::DebugUtilsMessengerEXT         debug_messenger                       = nullptr;
	vk::PhysicalDevice                 physical_device                       = nullptr;
	vk::PhysicalDeviceProperties       physical_device_properties            = {};
	vk::PhysicalDeviceMemoryProperties physical_device_memory_properties     = {};
	vk::Device                         device                                = nullptr;
	VmaAllocator                       allocator                             = nullptr;
	bool                               memory_budget_ext_enabled             = false;
	bool                               ahb_import_supported                   = false; // v0.0.9proQ3: AHardwareBuffer import for the OpenGL present backend
	bool                               compute_subgroup_size_control_enabled = false;
	bool                               sample_rate_shading_enabled           = false;
	bool                               sampler_anisotropy_enabled            = false;
	bool                               shader_image_int64_atomics_enabled    = false;
	// bool fp64_denorm_preserve = false; // Temporarily disabled.
	bool                               attachment_feedback_loop_enabled      = false;
	bool                               provoking_vertex_last_enabled         = false;
	bool                               depth_color_copy_enabled               = false;
	bool                               supports_block_texel_view              = false;
	bool                                      mesh_shader_enabled                   = false;
	// Optional features: some mobile GPUs (Mali) lack them, so the renderer falls back.
	bool                               color_write_enable_enabled            = false;
	bool                               image_view_min_lod_enabled            = false;
	bool                               depth_clip_control_enabled            = false;
	bool                               depth_bounds_enabled                  = false;
	bool                               multi_viewport_enabled                = false;
	bool                               fill_mode_non_solid_enabled           = false;
	bool                               draw_indirect_first_instance_enabled  = false;
	bool                               pipeline_statistics_enabled           = false;
	bool                               occlusion_queries_enabled             = false;
	bool                               occlusion_query_precise_enabled       = false;
	bool                               conditional_rendering_enabled         = false;
	bool                               pipeline_executable_info_enabled      = false;
	// 8-bit storage buffer access: false on Adreno 6xx + Turnip; the texture tiler then
	// uses compute pipelines compiled without VK_KHR_8bit_storage.
	bool                               storage_buffer_8bit_enabled           = true;
	vk::PhysicalDeviceMeshShaderPropertiesEXT mesh_shader_properties                = {};
	uint32_t                           subgroup_size                         = 0;
	uint32_t                           min_subgroup_size                     = 0;
	uint32_t                           max_subgroup_size                     = 0;
	uint32_t                           max_push_descriptors                  = 0;
	vk::DescriptorSetLayout            image_heap_layout                     = nullptr;
	vk::ShaderStageFlags               required_subgroup_size_stages         = {};
	Common::Mutex                      queue_mutex;
	uint32_t                           queue_family = static_cast<uint32_t>(-1);
	vk::Queue                          queue        = nullptr;
	// v0.0.10pro phase 3: API version of this session's instance - the 1.3 default, or 1.1
	// when the frontend's Vulkan-version option picks the Mali compatibility mode. Set once
	// in WindowContext::CreateVulkan before the instance exists; every later step (loader
	// gate, application info, device selection, the allocator) follows it.
	uint32_t                           api_version  = VULKAN_TARGET_API_VERSION;

	[[nodiscard]] const vk::PhysicalDeviceProperties& GetPhysicalDeviceProperties() const {
		return physical_device_properties;
	}

	[[nodiscard]] const vk::PhysicalDeviceMemoryProperties&
	GetPhysicalDeviceMemoryProperties() const {
		return physical_device_memory_properties;
	}

	[[nodiscard]] vk::FormatProperties GetFormatProperties(vk::Format format) const {
		std::scoped_lock lock(m_format_properties_mutex);
		auto [it, inserted] = m_format_properties.try_emplace(format);
		if (inserted) {
			physical_device.getFormatProperties(format, &it->second);
		}
		return it->second;
	}

	[[nodiscard]] vk::Result GetImageFormatProperties(vk::Format format, vk::ImageType type,
	                                                  vk::ImageTiling            tiling,
	                                                  vk::ImageUsageFlags        usage,
	                                                  vk::ImageCreateFlags       flags,
	                                                  vk::ImageFormatProperties* properties) const {
		using Key = std::tuple<vk::Format, vk::ImageType, vk::ImageTiling, vk::ImageUsageFlags,
		                       vk::ImageCreateFlags>;
		std::scoped_lock lock(m_image_format_properties_mutex);
		auto [it, inserted] =
		    m_image_format_properties.try_emplace(Key {format, type, tiling, usage, flags});
		if (inserted) {
			it->second.first = physical_device.getImageFormatProperties(format, type, tiling, usage,
			                                                            flags, &it->second.second);
		}
		if (properties != nullptr) {
			*properties = it->second.second;
		}
		return it->second.first;
	}

	[[nodiscard]] bool SupportsComputeWave64() const noexcept {
		return subgroup_size == 64u || compute_subgroup_size_control_enabled;
	}

	[[nodiscard]] vk::DeviceSize StorageMinAlignment() const {
		const auto alignment = physical_device_properties.limits.minStorageBufferOffsetAlignment;
		return alignment != 0 ? alignment : 1;
	}

	[[nodiscard]] bool CreateAllocator();
	void               DestroyAllocator();
	void               LogMemoryBudget() const;
	[[nodiscard]] bool CanReportMemoryUsage() const noexcept { return memory_budget_ext_enabled; }
	[[nodiscard]] uint64_t GetDeviceMemoryUsage() const;
	[[nodiscard]] uint64_t GetTotalMemoryBudget() const;
	[[nodiscard]] bool     CreateImage(const vk::ImageCreateInfo& info, VulkanImage& image);
	void                   DeleteImage(VulkanImage& image);

	uint32_t screen_width  = 0;
	uint32_t screen_height = 0;

private:
	mutable std::mutex                                 m_format_properties_mutex;
	mutable std::map<vk::Format, vk::FormatProperties> m_format_properties;
	mutable std::mutex                                 m_image_format_properties_mutex;
	mutable std::map<std::tuple<vk::Format, vk::ImageType, vk::ImageTiling, vk::ImageUsageFlags,
	                            vk::ImageCreateFlags>,
	                 std::pair<vk::Result, vk::ImageFormatProperties>>
	    m_image_format_properties;
};

struct VulkanImageState {
	vk::PipelineStageFlags2 pl_stage    = vk::PipelineStageFlagBits2::eAllCommands;
	vk::AccessFlags2        access_mask = vk::AccessFlagBits2::eNone;
	vk::ImageLayout         layout      = vk::ImageLayout::eUndefined;
};

struct VulkanImage {
	VulkanImage() = default;
	KYTY_CLASS_NO_COPY(VulkanImage);

	vk::Format                    format      = vk::Format::eUndefined;
	vk::ImageType                 image_type  = vk::ImageType::e2D;
	vk::Extent3D                  extent      = {1, 1, 1};
	uint32_t                      layers      = 1;
	uint32_t                      mip_levels  = 1;
	uint32_t                      samples     = 1;
	vk::ImageUsageFlags           usage       = {};
	vk::ImageCreateFlags          flags       = {};
	vk::Image                     image       = nullptr;
	VulkanImageState              state;
	std::vector<VulkanImageState> subresource_states;
	VmaAllocation                allocation = nullptr;
};



} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICCONTEXT_H_ */
