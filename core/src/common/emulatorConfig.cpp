// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "common/emulatorConfig.h"

#include "common/assert.h"
#include "common/debugFlags.h"

#include <algorithm>
#include <memory>

namespace Config {

static std::unique_ptr<ConfigOptions> g_config;

// Console Five Pro fix (v0.0.8pro): the getters dereferenced g_config
// unconditionally, so a read before Initialize() or after Shutdown() crashed
// the process; fall back to the built-in defaults instead.
static const ConfigOptions* SafeConfig() {
	static const ConfigOptions default_config;
	return g_config != nullptr ? g_config.get() : &default_config;
}

void Initialize() {
	EXIT_IF(g_config != nullptr);

	g_config = std::make_unique<ConfigOptions>();
}

void Shutdown() {
	g_config.reset();
}

void Load(const ConfigOptions& cfg) {
	EXIT_IF(g_config == nullptr);
	EXIT_IF(cfg.user_name.empty() || cfg.user_name.size() > MAX_USER_NAME_LENGTH);
	EXIT_IF(!IsConfiguredUserIdValid(cfg.user_id));
	EXIT_IF(cfg.controller_speaker_volume > 100 || cfg.controller_vibration_intensity > 100);

	*g_config = cfg;
}

uint32_t GetScreenWidth() {
	return SafeConfig()->screen_width;
}

uint32_t GetScreenHeight() {
	return SafeConfig()->screen_height;
}

const std::string& GetUserName() {
	return SafeConfig()->user_name;
}

int32_t GetUserId() {
	return SafeConfig()->user_id;
}

const std::string& GetAudioInputDevice() {
	return SafeConfig()->audio_input_device;
}

const std::optional<ControllerColor>& GetControllerColor() {
	return SafeConfig()->controller_color;
}

uint32_t GetControllerSpeakerVolume() {
	return SafeConfig()->controller_speaker_volume;
}

uint32_t GetControllerVibrationIntensity() {
	return SafeConfig()->controller_vibration_intensity;
}

PresentMode GetPresentMode() {
#if defined(__ANDROID__) || defined(ANDROID) || defined(KYTY_PLATFORM_ANDROID)
	// Console Five Pro tweak (v0.0.7pro): frontend option (Settings -> Performance).
	// 0 keeps the built-in low-latency default (mailbox), 1 asks for classic VSync
	// (fifo, smoothest pacing) and 2 uncaps presenting entirely (immediate, may tear).
	// The swapchain falls back to fifo when the driver lacks the requested mode.
	static Common::DebugFlags::CachedValue mode("debug.capy.present_mode", 0);
	switch (mode.Get()) {
		case 1: return PresentMode::Fifo;
		case 2: return PresentMode::Immediate;
		default: break;
	}
#endif
	return SafeConfig()->present_mode;
}

int32_t GetGpuIndex() {
	return SafeConfig()->gpu_index;
}

bool FullscreenEnabled() {
	return SafeConfig()->fullscreen_enabled;
}

bool HideCursorEnabled() {
	return SafeConfig()->hide_cursor_enabled;
}

bool VrEnabled() {
	return SafeConfig()->vr_enabled;
}

bool AmdCpuEnabled() {
	return SafeConfig()->amd_cpu_enabled;
}

uint32_t GetVblankFrequency() {
	return std::clamp(SafeConfig()->vblank_frequency, 30u, 360u);
}

uint32_t GetConsoleLanguage() {
	return SafeConfig()->console_language;
}

bool VulkanValidationEnabled() {
	return SafeConfig()->vulkan_validation_enabled;
}

bool ShaderValidationEnabled() {
	return SafeConfig()->shader_validation_enabled;
}

ShaderOptimizationType GetShaderOptimizationType() {
	return SafeConfig()->shader_optimization_type;
}

LogDirection GetShaderLogDirection() {
	return SafeConfig()->shader_log_direction;
}

std::filesystem::path GetShaderLogFolder() {
	return SafeConfig()->shader_log_folder;
}

bool CommandBufferDumpEnabled() {
	return SafeConfig()->command_buffer_dump_enabled;
}

std::filesystem::path GetCommandBufferDumpFolder() {
	return SafeConfig()->command_buffer_dump_folder;
}

bool GraphicsDebugDumpEnabled() {
	return SafeConfig()->graphics_debug_dump_enabled;
}

LogDirection GetPrintfDirection() {
	return SafeConfig()->printf_direction;
}

std::filesystem::path GetPrintfOutputFile() {
	return SafeConfig()->printf_output_file;
}

bool ProfilerEnabled() {
	return SafeConfig()->profiler_enabled;
}

bool SpirvDebugPrintfEnabled() {
	return SafeConfig()->spirv_debug_printf_enabled;
}

bool GpuAssistedValidationEnabled() {
	return SafeConfig()->gpu_assisted_validation_enabled && SafeConfig()->vulkan_validation_enabled;
}

bool RenderDocEnabled() {
	return SafeConfig()->renderdoc_enabled;
}

bool ReadbackLinearImagesEnabled() {
	return SafeConfig()->readback_linear_images;
}

bool SyncRawImageBuffersEnabled() {
	return SafeConfig()->sync_raw_image_buffers;
}

bool TessellationEnabled() {
	return SafeConfig()->tessellation_enabled;
}

bool TrophyEnabled() {
	return SafeConfig()->trophy_enabled;
}

bool PlayGoHackEnabled() {
	return SafeConfig()->playgo_hack_enabled;
}

bool SkipNoticeScreen() {
	return SafeConfig()->skip_notice_screen;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled() {
	return SafeConfig()->red_zone_protection_enabled;
}
#endif

const Keymap& GetKeymap() {
	return SafeConfig()->keymap;
}

const std::vector<std::string>& GetGuestArguments() {
	return SafeConfig()->guest_arguments;
}

} // namespace Config
