// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#if defined(__ANDROID__)
#include <android/log.h>
#endif
#if defined(__ANDROID__) || defined(__linux__)
#include <sys/resource.h>
#endif
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "graphics/shader/recompiler/ir/Serialize.h"
#include <zstd.h>

#include <ranges>
// v0.0.9proQ3: per-vendor spirv-opt pass lists over translated shaders.
#include "graphics/host_gpu/renderer/pipeline/spirvOptimizer.h"

#include "common/assert.h"
#include "common/contentProfile.h"
#include "common/debugFlags.h"
#include "common/enginePolicy.h"
#include "graphics/shader/recompiler/HostFeatures.h"
#include "common/emulatorConfig.h"
#include "common/gpuProfile.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/perfCores.h"
#include "common/threads.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "kytyGitVersion.h"
#if defined(__ANDROID__)
#include "android/capyps5_jni.h"
#endif
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <atomic>
#if defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#endif
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <string_view>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

uint8_t RemapSourceAlphaFactor(uint8_t factor) {
	switch (static_cast<Prospero::BlendFactor>(factor)) {
		case Prospero::BlendFactor::kSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Alpha);
		case Prospero::BlendFactor::kOneMinusSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
		default: return factor;
	}
}

vk::PolygonMode ResolvePolygonMode(const HW::ModeControl& mode, bool cull_front, bool cull_back) {
	// CxPrimitiveSetup::PolygonMode disables both per-face modes when it is zero.
	if (mode.poly_mode == 0) {
		return vk::PolygonMode::eFill;
	}
	EXIT_NOT_IMPLEMENTED(mode.poly_mode != 1);
	if (cull_front && cull_back) {
		return vk::PolygonMode::eFill;
	}
	if (!cull_front && !cull_back && mode.polymode_front_ptype != mode.polymode_back_ptype) {
		EXIT("Pipeline: different polygon modes for two visible faces are unsupported\n");
	}
	// Vulkan has one polygon mode. A culled face does not constrain that mode.
	const auto polygon_mode = cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
	switch (polygon_mode) {
		case 0: return vk::PolygonMode::ePoint;
		case 1: return vk::PolygonMode::eLine;
		case 2: return vk::PolygonMode::eFill;
		default: EXIT("Pipeline: invalid polygon mode %u\n", polygon_mode);
	}
}

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	return fmt::format("KytyPC1:{}:{:08x}:{:08x}:{:08x}:{}\n", KYTY_GIT_REVISION,
	                   properties.vendorID, properties.deviceID, properties.driverVersion, uuid);
}

std::string PipelineCacheTitleId() {
	std::string title_id;
	if ((!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) &&
	    (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &title_id) || title_id.empty())) {
		return {};
	}
	if (!std::ranges::all_of(title_id, [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '-' || c == '_';
	    })) {
		return {};
	}
	return title_id;
}

// Record fields shared by writing and reading: a program cache key.
template <class S, class T> void IoProgramKey(S& io, T& key) {
	auto& [stage, hash, user_data_count, code_size, static_state] = key;
	ShaderRecompiler::IR::IoAll(io, stage, hash, user_data_count, code_size, static_state);
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "CapyPS5-PipelineCache", "%s", message.c_str());
#endif
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	// Scalar and unformatted buffer dependencies use the same backing as native raw loads.
	// Image synchronization belongs to formatted buffer bindings, not these reads.
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadBufferBacking(address, values.data(), values.size_bytes());
}

// Ordinary (non-specializing) SRT reads. Upstream Kyty reads these straight from guest memory;
// the backing store only covers shared mappings, so fall back to the live pages for the rest.
bool ReadShaderLiveMemory(void* userdata, uint64_t address, std::span<uint32_t> values) {
	if (ReadShaderGuestMemory(userdata, address, values)) {
		return true;
	}
	if (values.empty() ||
	    !Libs::LibKernel::Memory::ReadGuestMemory(address, values.data(), values.size_bytes())) {
		return false;
	}
	Common::DebugFlags::NoteOnce("CapyPS5-Srt", address & ~uint64_t {0xfff},
	                             "SRT read 0x%llx (%zu bytes) is outside the backing store",
	                             static_cast<unsigned long long>(address), values.size_bytes());
	return true;
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
#if defined(__ANDROID__)
	// `setprop debug.capy.dump_spirv 1` before launching: write every generated module to
	// <files>/spirv/<stage>_<hash>.spv (read them with spirv-dis from the NDK shader-tools).
	static Common::DebugFlags::CachedValue dump_spirv("debug.capy.dump_spirv", 0);
	if (dump_spirv.Get() != 0) {
		const auto dir = AndroidGetFilesDir() + "/spirv";
		Common::File::CreateDirectories(dir);
		const auto path = fmt::format("{}/{}_{:016x}.spv", dir, stage_name, shader_hash);
		if (auto* out = std::fopen(path.c_str(), "wb"); out != nullptr) {
			std::fwrite(spirv.data(), sizeof(uint32_t), spirv.size(), out);
			std::fclose(out);
		}
	}
#endif
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / fmt::format("{:04d}_new_shader_{}_{:016x}.spv",
	                                                             id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(spirv.data(), spirv.size() * sizeof(uint32_t));
}

void DumpShaderOriginal(const char* stage_name, uint64_t shader_hash,
                        std::span<const uint32_t> code) {
#if defined(__ANDROID__)
	// `setprop debug.capy.dump_shaders 1` before a shader is first compiled writes its
	// disassembly to <files>/shaders/<stage>_<guest address>.txt.
	static Common::DebugFlags::CachedValue dump_text("debug.capy.dump_shaders", 0);
	// With `debug.capy.dump_spirv 1`, log which guest address (as shown by gpu_profile) each
	// dumped <stage>_<hash>.spv belongs to.
	static Common::DebugFlags::CachedValue dump_spirv("debug.capy.dump_spirv", 0);
	if (dump_spirv.Get() != 0 && !code.empty()) {
		__android_log_print(ANDROID_LOG_INFO, "CapyPS5-Shader", "%s 0x%llx -> %016llx", stage_name,
		                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(code.data())),
		                    static_cast<unsigned long long>(shader_hash));
	}
	if (dump_text.Get() != 0 && !code.empty()) {
		// Stages that hand over to another one end in S_SETPC_B64 with the footer right after it
		// ("sl00" in Little Nightmares), which a full decode would run into.
		const auto end = std::ranges::find_if(code, [](uint32_t word) {
			return word == 0xbf810000u || (word & 0xffffff00u) == 0xbe802000u;
		});
		ShaderRecompiler::Decoder::Program program;
		if (end != code.end() && *end != 0xbf810000u) {
			program = ShaderRecompiler::Decoder::DecodeFrontProgram(code);
		} else {
			ShaderRecompiler::Decoder::DecodeProgram(code, program);
		}
		const auto text = ShaderRecompiler::Decoder::ProgramToString(program);
		const auto dir  = AndroidGetFilesDir() + "/shaders";
		Common::File::CreateDirectories(dir);
		const auto path = fmt::format("{}/{}_{:x}_{:016x}.txt", dir, stage_name,
		                              reinterpret_cast<uintptr_t>(code.data()), shader_hash);
		if (auto* out = std::fopen(path.c_str(), "wb"); out != nullptr) {
			std::fwrite(text.data(), 1, text.size(), out);
			std::fclose(out);
		}
	}
#endif
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / "original" /
	                  fmt::format("{:04d}_new_shader_{}_{:016x}.bin", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(code.data(), code.size_bytes());
}

bool ValidateShaderSpirv(const char* label, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv) {
	if (!Config::ShaderValidationEnabled()) {
		return true;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	if (tools.Validate(spirv)) {
		return true;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF_COLOR(Log::Color::BrightRed, "%s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s",
	           label, shader_hash, messages.c_str());
	LOGF("%s\n", text.c_str());
	return false;
}

} // namespace

std::size_t PipelineCache::GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const {
	std::size_t hash = 0;
	PipelineKeyHash::Mix(hash, key.rendering.color_count);
	for (uint32_t i = 0; i < key.rendering.color_count; i++) {
		PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.color_formats[i]));
	}
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.depth_format));
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.stencil_format));
	for (const auto id: key.vertex_shader_ids) {
		PipelineKeyHash::Mix(hash, id);
	}
	PipelineKeyHash::Mix(hash, key.ps_shader_id);
	PipelineKeyHash::Mix(hash, key.vertex_input.binding_count);
	for (uint32_t i = 0; i < key.vertex_input.binding_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].stride);
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].instance);
	}
	PipelineKeyHash::Mix(hash, key.vertex_input.attribute_count);
	for (uint32_t i = 0; i < key.vertex_input.attribute_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].offset);
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].binding);
	}
	PipelineKeyHash::Mix(hash, XXH3_64bits(&key.static_params, sizeof(key.static_params)));
	return hash;
}

struct PipelineCache::ProgramCache {
	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		uint32_t              code_size       = 0;
		std::vector<uint32_t> static_state;

		bool operator==(const ProgramKey&) const = default;
	};

	struct Permutation {
		std::vector<uint32_t>                       function_code;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
		// The shader-store record of this permutation: the stage input info it was
		// compiled from (raw bytes; `stage` is rebuilt on load) and its final SPIR-V.
		uint8_t                stage_kind = 0; // 0 vertex family, 1 pixel, 2 compute
		ShaderVertexInputInfo  vertex_info;
		ShaderPixelInputInfo   pixel_info;
	};

	struct SourceEntry {
		SourceEntry(ShaderRecompiler::IR::ResourcePlan plan,
		            std::unique_ptr<ShaderRecompiler::ShaderSource> call_source)
		    : call_source(std::move(call_source)), resource_plan(std::move(plan)) {
			permutations.reserve(8);
		}

		std::span<const uint32_t> FunctionCode() const {
			return call_source
			    ? std::span<const uint32_t>(call_source->linked->code).subspan(call_source->code.size())
			    : std::span<const uint32_t>{};
		}

		bool RefreshCallSource(const ShaderRecompiler::IR::SrtRuntime& runtime) {
			const auto revision = call_source->revision;
			ShaderRecompiler::RefreshShaderSource(*call_source, runtime);
			resource_plan.source_reads = call_source->reads;
			return revision != call_source->revision;
		}

		template <bool HasCalls>
		Permutation* FindPermutation(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                             uint32_t push_data_cursor) {
			EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(
			    resource_plan, runtime, resources, specialization));
			for (auto& candidate: permutations) {
				const auto& layout = candidate.program.bindings;
				if (layout.push_data_start_dword != ShaderRecompiler::IR::PushData::StartFor(
				        push_data_cursor, layout.ShaderDataDwords()) ||
				    candidate.specialization != specialization) {
					continue;
				}
				if constexpr (HasCalls) {
					if (!std::ranges::equal(candidate.function_code, FunctionCode())) continue;
				}
				return &candidate;
			}
			return nullptr;
		}

		std::unique_ptr<ShaderRecompiler::ShaderSource> call_source;
		ShaderRecompiler::IR::ResourcePlan           resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		std::vector<Permutation>                    permutations;
	};

	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			PipelineKeyHash::Mix(hash, key.user_data_count);
			PipelineKeyHash::Mix(hash, key.code_size);
			PipelineKeyHash::Mix(hash, key.static_state.size());
			// Bucket same-shape static variants by source. ProgramKey equality performs the one
			// exact state comparison needed on a stable hit without hashing the full state first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 32 + ShaderVertexInputInfo::RES_MAX * 6;

	Permutation CompilePermutation(const char*                                  stage_name,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t push_data_start_dword,
	                               std::span<const uint32_t> function_code,
	                               std::vector<uint32_t>& spirv_out) {
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);
		if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
			     options.shader_hash);
		}
		// v0.0.9proQ3: per-vendor spirv-opt pass list (see spirvOptimizer.h) - smaller,
		// simpler modules compile faster and drive better codegen on mobile compilers.
		SpirvOptimizer::Optimize(options.stage, result.spirv);
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);

		const auto module = CompileSPV(result.spirv, device);
		EXIT_IF(module == nullptr);
		if (options.dump_ir) {
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		spirv_out = std::move(result.spirv);
		return {
		    .function_code  = {function_code.begin(), function_code.end()},
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id = ++next_shader_id, .module = module},
		};
	}

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor) {
		ShaderType stage;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage = input_info.logical_stage;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage = ShaderType::Pixel;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			stage = ShaderType::Compute;
		}

		const auto user_data = std::span(params.user_data).first(params.user_data_count);
		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = params.user_data_count;
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		BuildStageStaticKey(input_info, lookup_key.static_state);
		auto                                         entry = programs.find(lookup_key);
		ShaderRecompiler::IR::SrtRuntime             runtime {
		    .user_data                  = user_data,
		    .shader_base                = params.Base(),
		    .read_memory                = ReadShaderLiveMemory,
		    .read_specialization_memory = ReadShaderGuestMemory,
		    .image_heap                 = image_heap,
		};
		if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
			runtime.workgroup_counts = input_info.workgroup_counts;
		}
		bool source_changed = false;
		if (entry != programs.end()) {
			auto& cached = entry->second;
			Permutation* permutation = nullptr;
			if (cached.call_source) {
				source_changed = cached.RefreshCallSource(runtime);
				if (!source_changed) permutation = cached.FindPermutation<true>(runtime, push_data_cursor);
			} else {
				permutation = cached.FindPermutation<false>(runtime, push_data_cursor);
			}
			if (permutation != nullptr) {
				input_info.stage = {.program = &permutation->program, .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}

		ShaderStageInputInfo stage_input {};
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage_input.vertex = &input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage_input.pixel = &input_info;
		} else {
			stage_input.compute = &input_info;
		}
		const char* label = nullptr;
		const char* stage_name = nullptr;
		switch (stage) {
			case ShaderType::Vertex: label = "ShaderRecompiler VS"; stage_name = "vs"; break;
			case ShaderType::Mesh: label = "ShaderRecompiler MS"; stage_name = "ms"; break;
			case ShaderType::Local: label = "ShaderRecompiler LS"; stage_name = "ls"; break;
			case ShaderType::TessellationControl: label = "ShaderRecompiler HS"; stage_name = "hs"; break;
			case ShaderType::TessellationEvaluation: label = "ShaderRecompiler DS"; stage_name = "ds"; break;
			case ShaderType::Pixel: label = "ShaderRecompiler PS"; stage_name = "ps"; break;
			case ShaderType::Compute: label = "ShaderRecompiler CS"; stage_name = "cs"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.shader_hash = params.hash;
		options.user_data   = user_data;
		options.back_code      = params.back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		options.input_info  = stage_input;

		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			options.wave_size = input_info.wave_size;
			if (stage == ShaderType::Mesh || stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
				options.wave_size = stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
			}
		} else {
			options.wave_size = input_info.wave_size;
		}
		DumpShaderOriginal(stage_name, options.shader_hash, params.code);
		ShaderRecompiler::ShaderSource source;
		if (entry == programs.end()) source = ShaderRecompiler::PrepareShaderSource(params.code, options);
		auto* current = entry == programs.end() ? &source : entry->second.call_source.get();
		ShaderRecompiler::TranslateResult translated;
		if (current != nullptr) {
			if (current->call) {
				if (entry == programs.end()) ShaderRecompiler::RefreshShaderSource(*current, runtime);
				translated = ShaderRecompiler::TranslateProgram(current->linked->decoded, options);
				translated.program.source_reads = current->reads;
			} else {
				translated = ShaderRecompiler::TranslateProgram(current->decoded, options);
			}
		} else {
			translated = ShaderRecompiler::TranslateProgram(params.code, options);
		}
		if (entry == programs.end()) {
			auto retained = source.call ? std::make_unique<ShaderRecompiler::ShaderSource>(std::move(source)) : nullptr;
			entry = programs.try_emplace(lookup_key,
			    ShaderRecompiler::IR::ExtractResourcePlan(translated.program), std::move(retained)).first;
			EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(
			    entry->second.resource_plan, runtime, entry->second.resources,
			    entry->second.specialization));
		} else if (source_changed) {
			entry->second.resource_plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
			if (const auto* permutation = entry->second.FindPermutation<true>(runtime, push_data_cursor)) {
				input_info.stage = {.program = &permutation->program, .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}
		std::vector<uint32_t> stored_spirv;
		entry->second.permutations.push_back(CompilePermutation(
		    stage_name, options, std::move(translated), entry->second.specialization,
		    push_data_cursor, entry->second.FunctionCode(), stored_spirv));
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			entry->second.permutations.back().stage_kind  = 0;
			entry->second.permutations.back().vertex_info = input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			entry->second.permutations.back().stage_kind = 1;
			entry->second.permutations.back().pixel_info = input_info;
		} else {
			entry->second.permutations.back().stage_kind = 2;
		}
		if (store != nullptr) {
			Store(lookup_key, entry->second.permutations.back(), stored_spirv);
		}
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			entry->second.permutations.back().program.mesh_compute =
			    stage == ShaderType::Mesh && input_info.mesh.emulate;
		}
		const auto& permutation = entry->second.permutations.back();
#if defined(__ANDROID__)
		{
			static std::atomic<uint32_t> compiles {0};
			const auto n = compiles.fetch_add(1) + 1;
			if (n > 200 || entry->second.permutations.size() > 1) {
				__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Compile",
				    "compile #%u %s hash=0x%016llx permutations=%zu", n, stage_name,
				    static_cast<unsigned long long>(options.shader_hash), entry->second.permutations.size());
			}
		}
#endif
		input_info.stage = {.program = &permutation.program, .resources = &entry->second.resources};
		permutation.program.bindings.AdvancePushData(push_data_cursor);

		std::array<size_t, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1> counts {};
		for (const auto& [key, source]: programs) {
			counts[static_cast<size_t>(key.stage)] += source.permutations.size();
		}
		// Guest geometry shaders are compiled through the host mesh stage.
		std::printf("Shaders: VS %zu | PS %zu | CS %zu | GS %zu | LS %zu | HS %zu | TES %zu\n",
		            counts[static_cast<size_t>(ShaderType::Vertex)],
		            counts[static_cast<size_t>(ShaderType::Pixel)],
		            counts[static_cast<size_t>(ShaderType::Compute)],
		            counts[static_cast<size_t>(ShaderType::Mesh)],
		            counts[static_cast<size_t>(ShaderType::Local)],
		            counts[static_cast<size_t>(ShaderType::TessellationControl)],
		            counts[static_cast<size_t>(ShaderType::TessellationEvaluation)]);
		return permutation.handle;
	}

	explicit ProgramCache(vk::Device device,
	                     ShaderRecompiler::IR::ImageHeapSource* image_heap,
	                     ShaderStoreWriter* shader_store)
	    : device(device), image_heap(image_heap), store(shader_store) {
		lookup_key.static_state.reserve(MaxStaticKeyWords);
	}
	~ProgramCache() {
		for (const auto& [key, entry]: programs) {
			(void)key;
			for (const auto& permutation: entry.permutations) {
				device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	ProgramKey                                                  lookup_key;
	vk::Device                                                  device;
	ShaderRecompiler::IR::ImageHeapSource* image_heap = nullptr;
	ShaderStoreWriter*                   store  = nullptr;

	// Persists a first-run permutation and its final SPIR-V.
	void Store(const ProgramKey& key, const Permutation& permutation,
	          const std::vector<uint32_t>& spirv);
	uint64_t                                                    next_shader_id = 0;
};

void PipelineCache::ProgramCache::Store(const ProgramKey& key, const Permutation& permutation,
                                       const std::vector<uint32_t>& spirv) {
	if (store == nullptr || !*store) {
		return;
	}
	using ShaderRecompiler::IR::Io;
	using ShaderRecompiler::IR::IoAll;
	using ShaderRecompiler::IR::Writer;
	Writer record;
	IoAll(record, ShaderStoreRecord::Module);
	IoProgramKey(record, key);
	const auto found = programs.find(key);
	if (found == programs.end()) {
		return;
	}
	const auto stored_plan =
	    ShaderRecompiler::IR::SaveResourcePlan(found->second.resource_plan);
	Io(record, stored_plan);
	Io(record, permutation.function_code);
	Io(record, permutation.specialization);
	Io(record, permutation.program);
	Io(record, permutation.stage_kind);
	if (permutation.stage_kind == 0) record.Raw(&permutation.vertex_info,
	                                         sizeof(permutation.vertex_info));
	if (permutation.stage_kind == 1) record.Raw(&permutation.pixel_info,
	                                        sizeof(permutation.pixel_info));
	Io(record, spirv);
	store->Append(std::move(record.bytes));
}

void PipelineCache::WarmUp() {
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	std::filesystem::path path;
#if defined(__ANDROID__)
	// The working directory is not writable on Android; keep the store in the app's files.
	path = std::filesystem::path(AndroidGetFilesDir()) / "_ShaderStore" / (title_id + ".bin");
#else
	if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
		PipelineCacheLog("Shader store: disabled (non-Release build)");
		return;
	}
	path = std::filesystem::path("_ShaderStore") / (title_id + ".bin");
#endif
	const auto header = ShaderStoreHeader(m_graphics);
	const auto start   = std::chrono::steady_clock::now();
	std::vector<uint8_t> bytes;
	if (Common::File file; Common::File::IsFileExisting(path) &&
	                       file.Open(path, Common::File::Mode::Read)) {
		bytes.resize(file.Size());
		uint32_t read = 0;
		file.Read(bytes.data(), static_cast<uint32_t>(bytes.size()), &read);
		bytes.resize(read);
	}
	auto       records = std::span<const uint8_t>(bytes);
	const bool reusable =
	    records.size() >= header.size() &&
	    std::ranges::equal(records.first(header.size()), header);
	if (reusable) {
		PipelineCacheLog("Shader store: warming up from {} ({:.1f} MB)",
		                Common::PathToString(path),
		                static_cast<double>(bytes.size()) / 1'000'000.0);
		records = records.subspan(header.size());
		while (LoadRecord(records)) {
		}
	} else {
		PipelineCacheLog("Shader store: starting fresh {}", Common::PathToString(path));
	}
	// A damaged record ends the store; the records before it stay.
	m_store.Open(path, header, reusable ? bytes.size() - records.size() : 0);

	// Rebuild every stored pipeline against the loaded permutations.
	struct StoredStage {
		ProgramCache::SourceEntry*  entry       = nullptr;
		ProgramCache::Permutation*  permutation = nullptr;
	};
	auto FindStage = [this](uint64_t id) -> StoredStage {
		for (auto& [key, entry]: m_program_cache->programs) {
			for (auto& permutation: entry.permutations) {
				if (permutation.handle.id == id) return {&entry, &permutation};
			}
		}
		return {};
	};
	uint32_t graphics_created = 0;
	for (const auto& [key, job]: m_pending_pipelines) {
		(void)job;
		std::array<ShaderVertexInputInfo, 3> vertex_infos {};
		uint32_t stage_count = 0;
		bool     valid       = true;
		for (uint32_t index = 0; index < 3 && key.vertex_shader_ids[index] != 0; index++) {
			const auto stage = FindStage(key.vertex_shader_ids[index]);
			if (stage.permutation == nullptr || stage.permutation->stage_kind != 0) {
				valid = false;
				break;
			}
			vertex_infos[stage_count]    = stage.permutation->vertex_info;
			vertex_infos[stage_count].stage = {&stage.permutation->program,
			                                  &stage.entry->resources};
			stage_count++;
		}
		if (!valid) continue;
		ShaderPixelInputInfo pixel_info;
		const ShaderPixelInputInfo* ps_info = nullptr;
		if (key.ps_shader_id != 0) {
			const auto stage = FindStage(key.ps_shader_id);
			if (stage.permutation == nullptr || stage.permutation->stage_kind != 1) continue;
			pixel_info       = stage.permutation->pixel_info;
			pixel_info.stage = {&stage.permutation->program, &stage.entry->resources};
			ps_info          = &pixel_info;
		}
		GraphicsPrograms programs;
		for (uint32_t index = 0; index < stage_count; index++) {
			const auto stage = FindStage(key.vertex_shader_ids[index]);
			programs.vertex[index] = stage.permutation->handle;
		}
		if (key.ps_shader_id != 0) {
			programs.pixel = FindStage(key.ps_shader_id).permutation->handle;
		}
		Pipeline pipeline;
		CreatePipelineInternal(m_graphics, pipeline, key.rendering, key.vertex_input,
		                       std::span<const ShaderVertexInputInfo>(vertex_infos.data(),
		                                                              stage_count),
		                       ps_info, programs, key.static_params, m_driver_cache);
		if (pipeline.pipeline != nullptr) {
			m_graphics_pipelines.emplace(key,
			                          std::make_unique<Pipeline>(std::move(pipeline)));
			graphics_created++;
		}
	}
	uint32_t compute_created = 0;
	for (const auto& [id, job]: m_pending_compute) {
		(void)job;
		const auto stage = FindStage(id >> 32);
		if (stage.permutation == nullptr) continue;
		Pipeline pipeline;
		CreatePipelineInternal(m_graphics, pipeline,
		                       {&stage.permutation->program, &stage.entry->resources},
		                       stage.permutation->handle.module, m_driver_cache,
		                       static_cast<uint32_t>(id));
		if (pipeline.pipeline != nullptr) {
			m_compute_pipelines.emplace(id,
			                         std::make_unique<Pipeline>(std::move(pipeline)));
			compute_created++;
		}
	}
	m_pending_pipelines.clear();
	m_pending_compute.clear();
	if (graphics_created + compute_created != 0) {
		SavePeriodically();
	}
	PipelineCacheLog("Shader store: loaded {} programs, {} graphics + {} compute pipelines in {}"
	                " ms",
	                m_program_cache->programs.size(), graphics_created, compute_created,
	                std::chrono::duration_cast<std::chrono::milliseconds>(
	                    std::chrono::steady_clock::now() - start)
	                    .count());
}

bool PipelineCache::LoadRecord(std::span<const uint8_t>& records) {
	ShaderStoreFrame frame;
	if (records.size() < sizeof(frame)) {
		return false;
	}
	std::memcpy(&frame, records.data(), sizeof(frame));
	if (records.size() - sizeof(frame) < frame.compressed_size) {
		return false;
	}
	const auto compressed = records.subspan(sizeof(frame), frame.compressed_size);
	if (XXH3_64bits(compressed.data(), compressed.size()) != frame.checksum ||
	    ZSTD_getFrameContentSize(compressed.data(), compressed.size()) != frame.raw_size) {
		return false;
	}
	std::vector<uint8_t> raw(frame.raw_size);
	if (ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size()) !=
	    raw.size()) {
		return false;
	}
	using ShaderRecompiler::IR::Io;
	using ShaderRecompiler::IR::IoAll;
	using ShaderRecompiler::IR::Reader;
	using ShaderRecompiler::IR::Writer;
	Reader     reader {raw};
	auto       type     = ShaderStoreRecord::Module;
	const auto complete = [&] { return reader.ok && reader.bytes.empty(); };
	IoAll(reader, type);
	switch (type) {
		case ShaderStoreRecord::Module: {
			ProgramCache::ProgramKey   key;
			std::vector<uint8_t>      plan;
			ProgramCache::Permutation permutation;
			std::vector<uint32_t>     spirv;
			IoProgramKey(reader, key);
			Io(reader, plan);
			Io(reader, permutation.function_code);
			Io(reader, permutation.specialization);
			Io(reader, permutation.program);
			Io(reader, permutation.stage_kind);
			if (permutation.stage_kind == 0) reader.Raw(&permutation.vertex_info,
			                                        sizeof(permutation.vertex_info));
			if (permutation.stage_kind == 1) reader.Raw(&permutation.pixel_info,
			                                       sizeof(permutation.pixel_info));
			Io(reader, spirv);
			if (!complete()) {
				return false;
			}
			auto entry = m_program_cache->programs.find(key);
			if (entry == m_program_cache->programs.end()) {
				entry = m_program_cache->programs
				             .try_emplace(key, ShaderRecompiler::IR::ResourcePlan(), nullptr)
				             .first;
				if (!ShaderRecompiler::IR::LoadResourcePlan(
				        plan, entry->second.resource_plan)) {
					m_program_cache->programs.erase(entry);
					return false;
				}
			}
			permutation.handle = {.id = ++m_program_cache->next_shader_id,
			                      .module = CompileSPV(spirv, m_graphics.device)};
			if (permutation.handle.module == nullptr) {
				return false;
			}
			entry->second.permutations.push_back(std::move(permutation));
			break;
		}
		case ShaderStoreRecord::GraphicsPipeline: {
			GraphicsPipelineKey key;
			reader.Raw(&key, sizeof(key));
			if (!complete()) {
				return false;
			}
			m_pending_pipelines.emplace(std::move(key), std::make_shared<AsyncCompile>());
			break;
		}
		case ShaderStoreRecord::ComputePipeline: {
			uint64_t key = 0;
			Io(reader, key);
			if (!complete()) {
				return false;
			}
			m_pending_compute.emplace(key, std::make_shared<AsyncCompile>());
			break;
		}
		default: return false;
	}
	records = records.subspan(sizeof(frame) + compressed.size());
	return true;
}

void PipelineCache::StorePipeline(const GraphicsPipelineKey& key) {
	if (!m_store) {
		return;
	}
	using ShaderRecompiler::IR::IoAll;
	using ShaderRecompiler::IR::Writer;
	Writer record;
	IoAll(record, ShaderStoreRecord::GraphicsPipeline);
	record.Raw(&key, sizeof(key));
	m_store.Append(std::move(record.bytes));
}

void PipelineCache::StoreComputePipeline(uint64_t key) {
	if (!m_store) {
		return;
	}
	using ShaderRecompiler::IR::IoAll;
	using ShaderRecompiler::IR::Writer;
	Writer record;
	IoAll(record, ShaderStoreRecord::ComputePipeline, key);
	m_store.Append(std::move(record.bytes));
}

PipelineCache::PipelineCache(GraphicContext& graphics,
                               ShaderRecompiler::IR::ImageHeapSource* image_heap)
    : m_graphics(graphics), m_image_heap(image_heap),
      m_program_cache(std::make_unique<ProgramCache>(graphics.device, image_heap,
	                                               &m_store)) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	InitializeDriverCache();
	WarmUp();
	// Background PSO compilation workers. Two on smaller SoCs, two more on 8+-core devices
	// (Snapdragon 8/Dimensity class) and two more on 12+-core devices (big mid-range SoCs
	// ship 4x A7x + 8x A5x), so shader warm-up during material streaming finishes sooner,
	// while the emulated game threads keep at least eight cores to themselves.
	const uint32_t host_cores = std::thread::hardware_concurrency();
	// v0.0.9pro: one more worker under the Mali profile - Mali shader compiles are
	// slower, so finishing the warm-up sooner means fewer hitches while a game
	// streams new materials.
	// v0.0.10pro phase 2: the Adreno profile adds one too (Turnip compiles slower
	// than Qualcomm's proprietary driver; UE material streaming hitches less).
	// v0.0.9proQ4 round 3: profile 500 adds one more worker and raises the cap
	// by one (7): the matched content class pays its whole pipeline warm-up in
	// the first minutes, and an extra compile thread shortens exactly that
	// phase; the workers stay nice'd and pinned off the prime core.
	// v0.0.9proQ4 round 5: the extras and the cap come from the vendor-aware
	// table (common/enginePolicy.h). Under the throughput tier Mali adds TWO
	// extra workers (cap 8) against everyone else's one (cap 7): Arm's backend
	// compiler is the slowest shader stage on mobile, and that tier pays its
	// whole pipeline warm-up in the first minutes. The compositing tier keeps
	// its efficiency-cluster step-down on every vendor - that placement
	// follows the workload, not the GPU.
	const auto& engine_tuning = Common::EnginePolicy::TuningFor(
	    Common::GpuProfile::MaliProfileEnabled(), Common::GpuProfile::AdrenoProfileEnabled(),
	    Common::EnginePolicy::CurrentTier());
	const uint32_t profile_extra = engine_tuning.compile_profile_extra;
	const uint32_t turbo_extra   = engine_tuning.compile_class_extra;
	const uint32_t worker_cap    = engine_tuning.compile_pool_cap;
	const uint32_t worker_count =
		std::min(worker_cap,
		          (host_cores >= 12 ? 5u : host_cores >= 8 ? 4u : 2u) + profile_extra + turbo_extra);
	for (uint32_t i = 0; i < worker_count; i++) {
		m_async_workers.emplace_back([this] { AsyncWorker(); });
	}
}

void PipelineCache::StopAsyncWorkers() {
	{
		std::lock_guard lock(m_async_mutex);
		m_async_stop = true;
	}
	m_async_available.notify_all();
	for (auto& worker: m_async_workers) {
		worker.join();
	}
	m_async_workers.clear();
}

PipelineCache::~PipelineCache() {
	StopAsyncWorkers();
	Save();
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

void PipelineCache::InitializeDriverCache() {
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
#if defined(__ANDROID__)
	// `setprop debug.capy.pipeline_cache 0` (read at start) creates pipelines without a driver
	// cache, as before the cache was enabled on Android.
	if (Common::DebugFlags::CachedValue("debug.capy.pipeline_cache", 1).Get() == 0) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (debug.capy.pipeline_cache 0)");
		return;
	}
#endif
#if !defined(__ANDROID__)
	// On Android the cache is always on: the driver keys entries by shader contents and pipeline
	// state, so entries from another emulator build simply miss.
	if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (non-Release build)");
		return;
	}
	const std::string_view git_hash     = KYTY_GIT_HASH;
	const std::string_view git_revision = KYTY_GIT_REVISION;
	if (git_hash == "unknown" || git_revision == "unknown") {
		PipelineCacheLog("Vulkan pipeline cache: disabled (unknown git revision)");
		return;
	}
	if (git_hash.ends_with("-dirty")) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (dirty build)");
		return;
	}
#endif

#if defined(__ANDROID__)
	// The working directory is not writable on Android; keep the cache in the app's files.
	m_driver_cache_path = std::filesystem::path(AndroidGetFilesDir()) / "_PipelineCache" / (title_id + ".bin");
#else
	m_driver_cache_path     = std::filesystem::path("_PipelineCache") / (title_id + ".bin");
#endif
	const auto path         = Common::PathToString(m_driver_cache_path);
	const bool cache_exists = Common::File::IsFileExisting(m_driver_cache_path);
	if (cache_exists) {
		PipelineCacheLog("Vulkan pipeline cache: loading {}", path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initializing {}", path);
	}
	std::vector<uint8_t> initial_data;
	if (cache_exists) {
		Common::File file(m_driver_cache_path, Common::File::Mode::Read);
		const auto   file_size = file.IsInvalid() ? 0 : file.Size();
		const auto   signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
		if (file_size >= signature.size() + sizeof(uint64_t) &&
		    file_size <= std::numeric_limits<uint32_t>::max()) {
			std::string cached_signature(signature.size(), '\0');
			uint64_t    payload_hash = 0;
			initial_data.resize(file_size - signature.size() - sizeof(payload_hash));
			uint32_t signature_read = 0;
			uint32_t hash_read      = 0;
			uint32_t payload_read   = 0;
			file.Read(cached_signature.data(), static_cast<uint32_t>(cached_signature.size()),
			          &signature_read);
			file.Read(&payload_hash, sizeof(payload_hash), &hash_read);
			file.Read(initial_data.data(), static_cast<uint32_t>(initial_data.size()),
			          &payload_read);
			file.Close();
			if (signature_read != cached_signature.size() || hash_read != sizeof(payload_hash) ||
			    payload_read != initial_data.size() || cached_signature != signature ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver, emulator, or data mismatch)",
				    path);
			}
		} else {
			file.Close();
			PipelineCacheLog("Vulkan pipeline cache: invalidating {} (invalid file size)", path);
		}
	}

	vk::PipelineCacheCreateInfo create {};
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: driver rejected {} ({}); starting empty", path,
		                 vk::to_string(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", vk::to_string(result));
		m_driver_cache = nullptr;
		return;
	}
	if (!initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: loaded {} bytes from {}", initial_data.size(),
		                 path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
}

void PipelineCache::Save() {
	StopAsyncWorkers();
	if (m_driver_cache == nullptr) {
		return;
	}
	(void)WriteDriverCache();
	m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	m_driver_cache = nullptr;
}

void PipelineCache::SavePeriodically() {
	if (m_driver_cache == nullptr || m_pipelines_since_save == 0) {
		return;
	}
	const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	                        std::chrono::steady_clock::now().time_since_epoch())
	                        .count();
	if (m_last_save_ms == 0) {
		m_last_save_ms = now_ms;
		return;
	}
	// Flush every 10 s instead of 20 s: the atomic temp-file rename keeps each save cheap,
	// and half the interval halves the shader-cache loss when a game crashes or is killed.
	// Also flush once 250 pipelines were built since the last save: the first minutes
	// of a game create thousands of PSOs while materials stream in, and a crash would
	// otherwise lose up to a full 10 s window of compilation work.
	if (now_ms - m_last_save_ms < 10000 && m_pipelines_since_save < 250) {
		return;
	}
	m_last_save_ms = now_ms;
	if (WriteDriverCache()) {
		m_pipelines_since_save = 0;
	}
}

bool PipelineCache::WriteDriverCache() {
	if (m_driver_cache == nullptr) {
		return false;
	}

	size_t               size = 0;
	vk::Result           result;
	std::vector<uint8_t> payload;
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {} bytes)",
		                 vk::to_string(result), size);
		return false;
	}
	payload.resize(size);
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return false;
	}
	auto temp_path = m_driver_cache_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     prefix_written  = 0;
	uint32_t     payload_written = 0;
	if (file.Create(temp_path)) {
		file.Write(prefix.data(), static_cast<uint32_t>(prefix.size()), &prefix_written);
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &payload_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (prefix_written != prefix.size() || payload_written != payload.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, m_driver_cache_path)) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return false;
	}
	PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {}", payload.size(),
	                 Common::PathToString(m_driver_cache_path));
	return true;
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info,
    uint32_t indirect_vertex_sgpr, uint32_t indirect_instance_sgpr) {
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		vertex_params = PrepareTessellationPrograms(vertex_regs, context, vertex_info);
	} else {
		vertex_params[0] =
		    PrepareProgram(vertex_regs, context, user_config, vertex_info[0], m_graphics.mesh_shader_enabled,
		                   true);
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	if (indirect_vertex_sgpr != UINT32_MAX) {
		// The command processor only sends plain vertex stages down the GPU indirect path.
		EXIT_IF(mesh_active || tess_active);
		vertex_info[0].indirect_vertex_sgpr   = indirect_vertex_sgpr;
		vertex_info[0].indirect_instance_sgpr = indirect_instance_sgpr;
	}
	if (mesh_active && vertex_info[0].mesh.emulate) {
		// Same subgroup choice as guest compute shaders, which the dispatch also mirrors.
		auto& mesh              = vertex_info[0].mesh;
		mesh.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
		const auto& limits      = m_graphics.GetPhysicalDeviceProperties().limits;
		const auto  host_threads = ((mesh.threads_num[0] + mesh.wave_size - 1u) / mesh.wave_size) *
		                          std::min(mesh.host_subgroup_size, mesh.wave_size);
		if (host_threads > limits.maxComputeWorkGroupInvocations ||
		    host_threads > limits.maxComputeWorkGroupSize[0] ||
		    mesh.lds_size_dwords * sizeof(uint32_t) > limits.maxComputeSharedMemorySize) {
			EXIT("emulated mesh shader exceeds compute limits: threads=%u LDS=%u\n", host_threads,
			     mesh.lds_size_dwords);
		}
	} else if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled);
		auto& mesh              = vertex_info[0].mesh;
		mesh.host_subgroup_size = m_graphics.subgroup_size;
		const auto& limits      = m_graphics.mesh_shader_properties;
		const auto  logical_threads =
		    mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
		const auto host_threads = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
		                          std::min(mesh.host_subgroup_size, mesh.wave_size);
		if (host_threads > limits.maxMeshWorkGroupInvocations ||
		    host_threads > limits.maxMeshWorkGroupSize[0] ||
		    mesh.max_vertices > limits.maxMeshOutputVertices ||
		    mesh.max_primitives > limits.maxMeshOutputPrimitives ||
		    mesh.lds_size_dwords * sizeof(uint32_t) > limits.maxMeshSharedMemorySize) {
			EXIT("mesh shader exceeds host limits: threads=%u vertices=%u primitives=%u LDS=%u\n",
			     host_threads, mesh.max_vertices, mesh.max_primitives, mesh.lds_size_dwords);
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		pixel_params      = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
		if (!mesh_active && !tess_active && Prospero::IsRectList(user_config.GetPrimType())) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::Rectangle;
		} else if (context.GetModeControl().provoking_vtx_last) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::LastVertex;
		} else {
			pixel_info.parameter_mode = ShaderPixelParameterMode::FirstVertex;
		}
		const auto& blend = context.GetBlendControl(0);
		pixel_info.dual_source_blending =
		    blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		    (BlendFactorIsDualSource(blend.color_srcblend) ||
		     BlendFactorIsDualSource(blend.color_destblend) ||
		     (blend.separate_alpha_blend && (BlendFactorIsDualSource(blend.alpha_srcblend) ||
		                                     BlendFactorIsDualSource(blend.alpha_destblend))));
		if (pixel_info.dual_source_blending) {
			// MRT1 supplies the second blend source for target 0.
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = pixel_info.target_export_mapping[0];
		} else if (blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		           pixel_info.target_output_mode[0] != 0 && pixel_info.target_output_mode[0] != 7 &&
		           std::all_of(std::begin(pixel_info.target_output_mode) + 1,
		                       std::end(pixel_info.target_output_mode),
		                       [](uint8_t mode) { return mode == 0; })) {
			switch (ClassifyBlendMapping(blend, pixel_info.target_export_mapping[0])) {
				case BlendMappingSupport::SourceAlpha:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlpha;
					break;
				case BlendMappingSupport::SourceAlphaOne:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaOne;
					break;
				case BlendMappingSupport::SourceAlphaZero:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaZero;
					break;
				default: break;
			}
			if (pixel_info.alpha_blend_source != ShaderAlphaBlendSource::None) {
				pixel_info.dual_source_blending     = true;
				pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
				pixel_info.target_export_mapping[1] = {};
			}
		}
	}
	auto&       last_stage   = vertex_info[tess_active ? 2u : 0u];
	const auto& clip_control = context.GetClipControl();
	if (clip_control.clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = last_stage.clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	} else if (clip_control.user_clip_planes != 0) {
		// CLIP_DISABLE also disables user planes. They become clip distances of a plain vertex
		// shader; with shader clip/cull distances, tessellation, mesh or rect lists (whose
		// generated stages do not forward distances), or without host clip distances, the planes
		// are ignored as before.
		const bool supported = (last_stage.pa_cl_vs_out_cntl & 0xffffu) == 0 && !tess_active &&
		                       !mesh_active && !Prospero::IsRectList(user_config.GetPrimType()) &&
		                       ShaderRecompiler::g_host_features.clip_distance;
		if (supported) {
			last_stage.user_clip_planes = clip_control.user_clip_planes;
		} else {
			Common::DebugFlags::NoteOnce("CapyPS5-Clip", 0x55435000ull | clip_control.user_clip_planes,
			                             "user clip planes 0x%x ignored", clip_control.user_clip_planes);
		}
	}
	uint32_t push_data_cursor =
	    last_stage.user_clip_planes != 0
	        ? ShaderRecompiler::IR::PushData::UserClipPlaneDword +
	              4u * static_cast<uint32_t>(std::bit_width(last_stage.user_clip_planes))
	        : mesh_active ? ShaderRecompiler::IR::PushData::MeshDrawDwordCount : 0;
	GraphicsPrograms  result;
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor);
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		result.vertex[i] = m_program_cache->Get(vertex_params[i], vertex_info[i], push_data_cursor);
	}
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	input_info.lds_storage = input_info.lds_size_dwords * 4u >
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize;
	uint32_t          push_data_cursor = 0;
	return m_program_cache->Get(params, input_info, push_data_cursor);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

struct PipelineCache::AsyncCompile {
	PipelineRenderingState             rendering;
	PipelineVertexInputState           vertex_input;
	std::vector<ShaderVertexInputInfo> vertex_info;
	ShaderPixelInputInfo               pixel_info {};
	bool                               has_pixel = false;
	GraphicsPrograms                   programs;
	PipelineStaticParameters           static_params;
	// v0.0.9proQ4 round 2: compute/mesh jobs (is_compute = true) reuse the same
	// queue, priority accounting and done/notify protocol as graphics jobs.
	bool                               is_compute            = false;
	ShaderStageRuntime                 compute_stage {};
	vk::ShaderModule                   compute_module        = nullptr;
	uint32_t                           compute_scaled_images = 0;
	std::unique_ptr<Pipeline>          pipeline;
	std::atomic<bool>                  done {false};
	// v0.0.10pro phase 3 (hot pipelines): every re-request of a still-pending pipeline bumps
	// this - the game is actively drawing with it, so it must compile before the one-shot
	// background backlog. Workers pick the highest count first (ties: oldest).
	std::atomic<uint32_t>              requests {1};
};

PipelineCache::Pipeline* PipelineCache::GetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const GraphicsPrograms& programs, uint32_t vs_scaled_images,
    uint32_t ps_scaled_images, bool allow_async) {
	SavePeriodically();
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

	auto&             ctx = command.GetRegisters();

	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto vs_id = vertex_program.id;
	const auto ps_id = ps_active ? pixel_program.id : 0;

	GraphicsPipelineKey key {};
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		key.vertex_shader_ids[i] = programs.vertex[i].id;
	}
	key.ps_shader_id            = ps_id;
	auto& static_params         = key.static_params;
	auto& rendering             = key.rendering;
	rendering.color_count       = 0;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		const auto slot = colors[i].target_slot;
		EXIT_IF(slot >= RENDER_COLOR_ATTACHMENTS_MAX);
		rendering.color_count = std::max(rendering.color_count, slot + 1);
		EXIT_IF(!colors[i].image_id || colors[i].desc.view_info.format == vk::Format::eUndefined);
		static_params.color_mask[slot] = colors[i].export_mapping.ApplyMask(
		    render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot));
		rendering.color_formats[slot] = colors[i].desc.view_info.format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].desc.info.samples;
		} else if (attachment_samples != colors[i].desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].desc.info.samples);
		}
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		auto alpha_source = ShaderAlphaBlendSource::None;
		if (slot == 0 && ps_input_info != nullptr) {
			alpha_source = ps_input_info->alpha_blend_source;
		}
		static_params.blend_enable[slot] = bc.enable && !rt.info.blend_bypass;
		if (static_params.blend_enable[slot] && alpha_source == ShaderAlphaBlendSource::None &&
		    ClassifyBlendMapping(bc, colors[i].export_mapping) != BlendMappingSupport::Direct) {
			static_params.blend_enable[slot] = false;
			static std::atomic_bool warned = false;
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Warning: blending disabled for unsupported color mapping "
				    "(slot={} mapping=0x{:02x} color={}/{} alpha={}/{} separate={}).\n",
				    slot, colors[i].export_mapping.packed, bc.color_srcblend, bc.color_destblend,
				    bc.alpha_srcblend, bc.alpha_destblend, bc.separate_alpha_blend ? 1 : 0));
			}
		}
		Common::DebugFlags::FrameTrace(
		    "    rt[%u] slot=%u mask=0x%x blend=%d->%d src=%u dst=%u fn=%u sep=%d asrc=%u adst=%u map=0x%02x alpha_src=%u",
		    i, slot, static_params.color_mask[slot], bc.enable && !rt.info.blend_bypass ? 1 : 0,
		    static_params.blend_enable[slot] ? 1 : 0, bc.color_srcblend, bc.color_destblend,
		    bc.color_comb_fcn, bc.separate_alpha_blend ? 1 : 0, bc.alpha_srcblend, bc.alpha_destblend,
		    colors[i].export_mapping.packed, static_cast<uint32_t>(alpha_source));
		if (static_params.blend_enable[slot]) {
			auto blend = bc;
			switch (alpha_source) {
				case ShaderAlphaBlendSource::SourceAlpha:
					blend.color_srcblend  = RemapSourceAlphaFactor(blend.color_srcblend);
					blend.color_destblend = RemapSourceAlphaFactor(blend.color_destblend);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::SourceAlphaOne:
				case ShaderAlphaBlendSource::SourceAlphaZero:
					// The second source carries the mapped source factor; its alpha stays logical Sa.
					blend.color_srcblend = static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Color);
					blend.color_destblend =
					    static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::None: break;
			}
			static_params.color_srcblend[slot]       = blend.color_srcblend;
			static_params.color_comb_fcn[slot]       = blend.color_comb_fcn;
			static_params.color_destblend[slot]      = blend.color_destblend;
			static_params.separate_alpha_blend[slot] = blend.separate_alpha_blend;
			if (blend.separate_alpha_blend) {
				static_params.alpha_srcblend[slot]  = blend.alpha_srcblend;
				static_params.alpha_comb_fcn[slot]  = blend.alpha_comb_fcn;
				static_params.alpha_destblend[slot] = blend.alpha_destblend;
			}
		}
	}
	const bool with_depth =
	    depth.desc.view_info.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects       = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		rendering.depth_format   = aspects & vk::ImageAspectFlagBits::eDepth
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		rendering.stencil_format = aspects & vk::ImageAspectFlagBits::eStencil
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.desc.info.samples;
		} else if (attachment_samples != depth.desc.info.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.desc.info.samples);
		}
	}
	if (color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		EXIT_IF(!static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples)));
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = topology;
	static_params.primitive_restart_enable = primitive_restart_enable;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info->ps_sample_shading;
	if (static_params.sample_shading_enable && !m_graphics.sample_rate_shading_enabled) {
		EXIT("Pipeline: sample-rate shading is required but unsupported by the host\n");
	}
	static_params.alpha_to_coverage_enable = ps_active && ctx.GetShaderRegisters().AlphaToMask();
	const bool rect_list = Prospero::IsRectList(command.GetUserConfig().GetPrimType());
	// Bisection aid: `setprop debug.capy.no_cull 1` disables face culling.
	static Common::DebugFlags::CachedValue no_cull("debug.capy.no_cull", 0);
	static_params.cull_back  = !rect_list && mc.cull_back && no_cull.Get() == 0;
	static_params.cull_front = !rect_list && mc.cull_front && no_cull.Get() == 0;
	// Diagnostic winding reversal; included in the static key so it can change live.
	static Common::DebugFlags::CachedValue flip_front("debug.capy.flip_front", 0);
	static_params.face       = mc.face ^ (flip_front.Get() != 0);
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	std::memcpy(static_params.vs_scaled_images, &vs_scaled_images, sizeof(vs_scaled_images));
	std::memcpy(static_params.ps_scaled_images, &ps_scaled_images, sizeof(ps_scaled_images));
	static_params.polygon_mode =
	    ResolvePolygonMode(mc, static_params.cull_front, static_params.cull_back);
	if (!m_graphics.fill_mode_non_solid_enabled) {
		// Wireframe and point fill need fillModeNonSolid (missing on Mali); draw solid instead.
		static_params.polygon_mode = vk::PolygonMode::eFill;
	}

	if (vs_input_info.stage.program->stage != ShaderType::Mesh) {
		EXIT_IF(vs_input_info.buffers_num < 0 ||
		        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX ||
		        vs_input_info.resources_num < 0 ||
		        vs_input_info.resources_num > ShaderVertexInputInfo::RES_MAX);
		key.vertex_input.binding_count   = static_cast<uint8_t>(vs_input_info.buffers_num);
		key.vertex_input.attribute_count = static_cast<uint8_t>(vs_input_info.resources_num);
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
		}
		for (int attribute = 0; attribute < vs_input_info.resources_num; attribute++) {
			const auto binding = vs_input_info.resources_dst[attribute].buffer_index;
			EXIT_IF(binding < 0 || binding >= vs_input_info.buffers_num);
			key.vertex_input.attributes[attribute] = {
			    .offset = static_cast<uint32_t>(vs_input_info.resources[attribute].Base48() -
			                                    vs_input_info.buffers[binding].addr),
			    .binding = static_cast<uint8_t>(binding),
			};
		}
	}

	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		return iter->second.get();
	}
	if (auto pending = m_pending_pipelines.find(key); pending != m_pending_pipelines.end()) {
		auto job = pending->second;
		if (!job->done.load(std::memory_order_acquire)) {
			// v0.0.10pro phase 3: the game asked for this pipeline again while it is still
			// compiling - it is on the critical path right now (draws are being skipped or a
			// flush is waiting on it), so raise its compile priority.
			(void)job->requests.fetch_add(1, std::memory_order_relaxed);
			if (!allow_async) {
				// Console Five Pro (v0.0.9proQ4): this used to be an UNBOUNDED wait on the compile
				// queue. A storm of first-visit pipelines (new area, material streaming, LUT passes)
				// serialized the GPU thread behind the workers for whole seconds: the guest's render
				// worker timed out (~28 s), tore its objects down and died on a null-pointer write -
				// the freeze-then-crash reports. Wait a few milliseconds instead (the priority bump
				// above already jumps this pipeline to the front of the queue); on timeout skip this
				// draw exactly like the async path - the compile lands and the next occurrence
				// renders for real. `setprop debug.capy.pipeline_wait_ms N` tunes the bound
				// (0 = never wait, a large N restores the old blocking behaviour).
				//
				// v0.0.10pro phase 4: the default is tier-aware. General content keeps the
				// 4 ms bound. Under ANY locked turbo class (500/501/502) or the resolution
				// policy the bound drops to 1 ms: those are exactly the titles whose
				// warm-up storms matter (hundreds of unique pipelines), and every 4 ms
				// park here was 4 ms the frame did not advance - the guest flip thread
				// starved and the compile queue drained no faster for it (the workers,
				// not the waiter, do the compiling). At 1 ms the draw skips sooner, the
				// frame reaches the flip, and the next occurrence of the same draw
				// finds the pipeline ready.
				static Common::DebugFlags::CachedValue wait_ms("debug.capy.pipeline_wait_ms", -1);
				const int64_t tier_default =
				    Common::EnginePolicy::CurrentTier() ==
				            Common::EnginePolicy::WorkloadTier::General
				        ? 4
				        : 1;
				const int64_t requested = wait_ms.Get();
				const int64_t ms =
				    std::max<int64_t>(requested >= 0 ? requested : tier_default, 0);
				bool ready = false;
				if (ms == 0) {
					ready = job->done.load(std::memory_order_acquire);
				} else {
					std::unique_lock lock(m_async_mutex);
					ready = m_async_available.wait_for(
						lock, std::chrono::milliseconds(ms),
						[&] { return job->done.load(std::memory_order_acquire); });
				}
				if (!ready) {
					return nullptr;
				}
			} else {
				return nullptr;
			}
		}
		EXIT_NOT_IMPLEMENTED(job->pipeline->pipeline == nullptr || job->pipeline->pipeline_layout == nullptr);
		auto [iter, inserted] = m_graphics_pipelines.emplace(pending->first, std::move(job->pipeline));
		EXIT_IF(!inserted);
		m_pending_pipelines.erase(pending);
		return iter->second.get();
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64 " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program.module), ps_id,
		     static_cast<void*>(pixel_program.module));
	}

#if defined(__ANDROID__)
	// `setprop debug.capy.pipeline_log 1`: every 2 s log how many graphics pipelines were
	// created and the shaders and vertex layout of the last one (tag CapyPS5-Pipeline).
	static Common::DebugFlags::CachedValue pipeline_log("debug.capy.pipeline_log", 0);
	if (pipeline_log.Get() != 0) {
		static uint32_t created     = 0;
		static auto     next_report = std::chrono::steady_clock::now();
		created++;
		std::string layout;
		for (uint32_t a = 0; a < key.vertex_input.attribute_count; a++) {
			layout += fmt::format(" a{}=b{}+{}", a, key.vertex_input.attributes[a].binding,
			                      key.vertex_input.attributes[a].offset);
		}
		for (uint32_t b = 0; b < key.vertex_input.binding_count; b++) {
			layout += fmt::format(" b{}s{}", b, key.vertex_input.bindings[b].stride);
		}
		__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Pipeline", "new vs=%llu ps=%llu topo=%u colors=%u%s",
		                    static_cast<unsigned long long>(vs_id), static_cast<unsigned long long>(ps_id),
		                    static_cast<uint32_t>(topology), rendering.color_count, layout.c_str());
		if (const auto now = std::chrono::steady_clock::now(); now >= next_report) {
			next_report = now + std::chrono::seconds(2);
			__android_log_print(ANDROID_LOG_WARN, "CapyPS5-Pipeline", "---- %u graphics pipelines created, %zu cached",
			                    created, m_graphics_pipelines.size());
			created = 0;
		}
	}
#endif
	StorePipeline(key);
	// `setprop debug.capy.async_pipelines 0` compiles every pipeline on the GPU thread.
	static Common::DebugFlags::CachedValue async_pipelines("debug.capy.async_pipelines", 1);
	// Console Five Pro (v0.0.9proQ4): the first miss is queued for EVERY draw, not only
	// the skippable ones - an inline compile on the GPU thread is a stall by definition,
	// and `allow_async` now only picks the wait bound above (a few ms) vs. skipping at
	// once. A draw whose pipeline is not ready is skipped either way; the compile lands
	// and the next occurrence draws for real.
	if (async_pipelines.Get() != 0 && !m_async_workers.empty()) {
		auto job          = std::make_shared<AsyncCompile>();
		job->rendering    = rendering;
		job->vertex_input = key.vertex_input;
		job->vertex_info.assign(vertex_info.begin(), vertex_info.end());
		job->has_pixel = ps_input_info != nullptr;
		if (job->has_pixel) {
			job->pixel_info = *ps_input_info;
		}
		job->programs      = programs;
		job->static_params = static_params;
		m_pipelines_since_save++;
		m_pending_pipelines.emplace(std::move(key), job);
		{
			std::lock_guard lock(m_async_mutex);
			m_async_queue.push_back(std::move(job));
		}
		m_async_available.notify_all();
		return nullptr;
	}

	auto cached = std::make_unique<Pipeline>();
	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	m_pipelines_since_save++;
	CreatePipelineInternal(m_graphics, *cached, rendering, key.vertex_input, vertex_info,
	                       ps_input_info, programs, static_params, m_driver_cache);
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	return iter->second.get();
}

void PipelineCache::AsyncWorker() {
#if defined(__ANDROID__) || defined(__linux__)
	pthread_setname_np(pthread_self(), "CapyPipeline");
	// Lower the scheduling priority: background pipeline compilation must never steal CPU
	// from the emulated game threads and the GPU submission thread. Dropping priority
	// (positive nice) needs no privileges; failure is harmless and ignored.
	if (setpriority(PRIO_PROCESS, 0, 4) != 0) {
		PipelineCacheLog("Vulkan pipeline cache: could not lower worker priority");
	}
#endif
#if defined(__ANDROID__)
	// Console Five Pro tweak (v0.0.7pro): the scheduler tends to park background-niced
	// threads on the little cores entirely, which makes shader warm-up crawl exactly when
	// the game streams new materials. Keep the workers on the big cores; their low nice
	// value already yields to the game threads, so they only use spare big-core time.
	// v0.0.9proQ4 round 2: workers take the performance tier WITHOUT the prime core -
	// during a compile storm the emulation and GPU threads keep the fastest core to
	// themselves (best effort; debug.capy.worker_cores 0 restores the Q2 placement,
	// debug.capy.perf_cores 0 disables pinning entirely).
	// v0.0.9proQ4 round 4: placement follows the runtime workload class -
	// the CPU-bound layered-alpha class steps the compile workers down onto
	// the efficiency cluster (the big cores belong to the content's own
	// threads there); every other class keeps the round-2
	// performance-minus-prime placement. The loop below re-pins when the
	// classification moves.
	Common::PinToWorkerCoresForClass("Pipeline worker");
#endif
	// v0.0.10pro phase 3: compile what the game is waiting on first. FIFO made a streaming
	// storm's hundreds of one-shot pipelines delay the handful the current frame re-draws with,
	// so draws were skipped and heavy titles crawled exactly during material streaming. The
	// scan is O(queue) under the lock - trivial next to a ~100 ms pipeline compile.
	// `setprop debug.capy.hot_pipelines 0` restores FIFO.
	static Common::DebugFlags::CachedValue hot_pipelines("debug.capy.hot_pipelines", 1);
	// v0.0.9proQ4 round 4: re-pin when the workload classification moves
	// (the generation bumps on every lock/re-lock/reset), and grow the pool
	// by one under the throughput class the first time the queue backs up
	// (cap 7, one shot per run - PipelineCache is constructed per run, so
	// this is per-game correct by construction).
	thread_local uint32_t pinned_generation = 0;
	for (;;) {
		if (const uint32_t generation = Common::ContentProfile::PlacementGeneration();
		    generation != pinned_generation) {
			pinned_generation = generation;
			Common::PinToWorkerCoresForClass("Pipeline worker");
		}
		std::shared_ptr<AsyncCompile> job;
		{
			std::unique_lock lock(m_async_mutex);
			m_async_available.wait(lock, [this] { return m_async_stop || !m_async_queue.empty(); });
			if (m_async_stop) {
				return;
			}
			// v0.0.9proQ4 round 5: the growth size and the cap come from the
			// vendor-aware table too - Mali under the throughput tier grows by
			// two (cap 8), everyone else by one. Still one shot per run.
			//
			// v0.0.10pro phase 4: the gate is now the resolved tier, not
			// profile500 alone. Rounds 4-5 only ever grew the pool under class
			// 500 - a 501 or (new) 502 title that locked its class after the
			// constructor ran kept the General-sized pool for the whole run,
			// exactly the games whose warm-up needs the workers most. The
			// resolution-policy floor (a 4K target set with no class locked
			// yet) grows the pool too, for the same reason.
			if (!m_turbo_worker_applied && m_async_queue.size() >= m_async_workers.size() &&
			    Common::EnginePolicy::CurrentTier() !=
			        Common::EnginePolicy::WorkloadTier::General) {
				const auto& growth_tuning = Common::EnginePolicy::TuningFor(
				    Common::GpuProfile::MaliProfileEnabled(),
				    Common::GpuProfile::AdrenoProfileEnabled(),
				    Common::EnginePolicy::CurrentTier());
				for (uint32_t grown = 0;
				     grown < growth_tuning.compile_class_extra &&
				     m_async_workers.size() < growth_tuning.compile_pool_cap;
				     ++grown) {
					m_async_workers.emplace_back([this] { AsyncWorker(); });
				}
				m_turbo_worker_applied = true;
			}
			if (hot_pipelines.Get() != 0 && m_async_queue.size() > 1) {
				auto best = m_async_queue.begin();
				for (auto it = m_async_queue.begin() + 1; it != m_async_queue.end(); ++it) {
					if ((*it)->requests.load(std::memory_order_relaxed) >
				    (*best)->requests.load(std::memory_order_relaxed)) {
						best = it;
					}
				}
				job = std::move(*best);
				m_async_queue.erase(best);
			} else {
				job = std::move(m_async_queue.front());
				m_async_queue.pop_front();
			}
		}
		auto pipeline = std::make_unique<Pipeline>();
		if (job->is_compute) {
			CreatePipelineInternal(m_graphics, *pipeline, job->compute_stage, job->compute_module,
		                       m_driver_cache, job->compute_scaled_images);
		} else {
			CreatePipelineInternal(m_graphics, *pipeline, job->rendering, job->vertex_input,
			                       job->vertex_info, job->has_pixel ? &job->pixel_info : nullptr,
			                       job->programs, job->static_params, m_driver_cache);
		}
		job->pipeline = std::move(pipeline);
		size_t queue_left = 0;
		{
			std::lock_guard lock(m_async_mutex);
			job->done.store(true, std::memory_order_release);
			queue_left = m_async_queue.size();
		}
#if defined(__ANDROID__)
		{
			const auto waits = job->requests.load(std::memory_order_relaxed);
			if (waits >= 8) {
				__android_log_print(ANDROID_LOG_INFO, "CapyPS5-Pipeline",
				    "hot pipeline compiled after %u re-requests (queue %zu left)", waits, queue_left);
			}
		}
#endif
		m_async_available.notify_all();
	}
}

PipelineCache::Pipeline*
PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
                                  const ShaderProgram&          compute_program,
                                  uint32_t                      scaled_images) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program || compute_program.id >= (1ull << 32u));
	const auto key = compute_program.id << 32u | scaled_images;

	if (auto iter = m_compute_pipelines.find(key);
	    iter != m_compute_pipelines.end()) {
		return iter->second.get();
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	// Console Five Pro (v0.0.9proQ4 round 2): compute pipelines used to compile INLINE
	// on the GPU thread - one half of the mesh/compute warm-up storms that held heavy
	// 3D titles at whole seconds per frame. First misses now go to the async workers.
	// Unlike a draw, a dispatch's writes are guest-visible and must not be silently
	// skipped, so the GPU thread awaits a compute-sized bound and falls back to the old
	// inline compile on timeout: the dispatch always runs, the compile just happens
	// off-thread in the common case (and the driver disk cache makes later boots free).
	static Common::DebugFlags::CachedValue async_pipelines("debug.capy.async_pipelines", 1);
	static Common::DebugFlags::CachedValue compute_wait_ms("debug.capy.compute_wait_ms", 150);
	if (async_pipelines.Get() != 0 && !m_async_workers.empty()) {
		std::shared_ptr<AsyncCompile> job;
		if (auto pending = m_pending_compute.find(key); pending != m_pending_compute.end()) {
			job = pending->second;
			// The dispatch path re-requests this program: compile it first.
			(void)job->requests.fetch_add(1, std::memory_order_relaxed);
		} else {
			job                        = std::make_shared<AsyncCompile>();
			job->is_compute            = true;
			job->compute_stage         = input_info.stage;
			job->compute_module        = compute_program.module;
			job->compute_scaled_images = scaled_images;
			m_pipelines_since_save++;
			m_pending_compute.emplace(key, job);
			{
				std::lock_guard lock(m_async_mutex);
				m_async_queue.push_back(job);
			}
			m_async_available.notify_all();
		}
		if (AwaitPending(job, std::max<int64_t>(compute_wait_ms.Get(), 0))) {
			auto [iter, inserted] = m_compute_pipelines.emplace(key, std::move(job->pipeline));
	if (inserted) StoreComputePipeline(key);
			EXIT_IF(!inserted);
			m_pending_compute.erase(key);
			return iter->second.get();
		}
		// Storm backlog timeout (the wait expired with the queue still draining):
		// finish inline - correctness over latency. The abandoned worker job is
		// harmless: it completes, nobody promotes it, the shared_ptr frees it.
		m_pending_compute.erase(key);
	}

	auto cached = std::make_unique<Pipeline>();
	m_pipelines_since_save++;
	CreatePipelineInternal(m_graphics, *cached, input_info.stage, compute_program.module,
	                       m_driver_cache, scaled_images);

	if (cached->pipeline == nullptr) {
		// Kept as a null entry so the dispatch is skipped instead of recompiled every time (Born
		// of Bread: Adreno "Failed to link shaders").
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_ERROR, "CapyPS5-Compile",
		                    "compute pipeline rejected by the driver: program=%llu lds=%u dwords (storage %d) threads=%ux%ux%u",
		                    static_cast<unsigned long long>(compute_program.id),
		                    input_info.lds_size_dwords, input_info.lds_storage ? 1 : 0, input_info.threads_num[0], input_info.threads_num[1],
		                    input_info.threads_num[2]);
#endif
	}
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(key, std::move(cached));
	if (inserted) StoreComputePipeline(key);
	EXIT_IF(!inserted);

	return iter->second.get();
}

PipelineCache::Pipeline*
PipelineCache::GetMeshComputePipeline(const ShaderVertexInputInfo& input_info,
                                      const ShaderProgram&         mesh_program,
                                      uint32_t                     scaled_images) {
	EXIT_IF(!mesh_program || !input_info.stage || !input_info.stage.program->mesh_compute ||
	        mesh_program.id >= (1ull << 32u));
	// Program ids are unique across stages, so mesh programs share the compute pipeline map.
	const auto key = mesh_program.id << 32u | scaled_images;
	if (auto iter = m_compute_pipelines.find(key); iter != m_compute_pipelines.end()) {
		return iter->second.get();
	}
	// Console Five Pro (v0.0.9proQ4 round 2): the mesh emulation feeds a DRAW, so a
	// not-yet-compiled pipeline skips this draw exactly like a graphics-pipeline miss
	// (heavy 3D titles run dozens of emulated mesh draws per frame; the old inline
	// ~100 ms compile per first-visit program was the 1-3 FPS warm-up disaster). The
	// compile lands on the async workers; the next occurrence draws for real.
	static Common::DebugFlags::CachedValue async_pipelines("debug.capy.async_pipelines", 1);
	static Common::DebugFlags::CachedValue wait_ms("debug.capy.pipeline_wait_ms", 0);
	if (auto pending = m_pending_compute.find(key); pending != m_pending_compute.end()) {
		auto job = pending->second;
		(void)job->requests.fetch_add(1, std::memory_order_relaxed);
		// Skip at once: a frame full of re-requested first-visit mesh draws must not
		// pay even a few ms each (30 x 4 ms = an 8 FPS frame); the priority bump above
		// already jumps this program to the front of the compile queue, so the next
		// frame very likely has it.  makes the
		// re-request wait N ms for near-done compiles instead of skipping at once.
		if (!AwaitPending(job, std::max<int64_t>(wait_ms.Get(), 0))) {
			return nullptr;
		}
		auto [iter, inserted] = m_compute_pipelines.emplace(key, std::move(job->pipeline));
	if (inserted) StoreComputePipeline(key);
		EXIT_IF(!inserted);
		m_pending_compute.erase(pending);
		return iter->second.get();
	}
	if (async_pipelines.Get() != 0 && !m_async_workers.empty()) {
		auto job                    = std::make_shared<AsyncCompile>();
		job->is_compute             = true;
		job->compute_stage          = input_info.stage;
		job->compute_module         = mesh_program.module;
		job->compute_scaled_images  = scaled_images;
		m_pipelines_since_save++;
		m_pending_compute.emplace(key, job);
		{
			std::lock_guard lock(m_async_mutex);
			m_async_queue.push_back(job);
		}
		m_async_available.notify_all();
		return nullptr;
	}
	auto cached = std::make_unique<Pipeline>();
	m_pipelines_since_save++;
	CreatePipelineInternal(m_graphics, *cached, input_info.stage, mesh_program.module,
	                       m_driver_cache, scaled_images);
	auto [iter, inserted] = m_compute_pipelines.emplace(key, std::move(cached));
	if (inserted) StoreComputePipeline(key);
	EXIT_IF(!inserted);
	return iter->second.get();
}

bool PipelineCache::AwaitPending(std::shared_ptr<AsyncCompile>& job, int64_t wait_ms) {
	if (job->done.load(std::memory_order_acquire)) {
		return true;
	}
	if (wait_ms <= 0) {
		return false;
	}
	std::unique_lock lock(m_async_mutex);
	return m_async_available.wait_for(lock, std::chrono::milliseconds(wait_ms),
	                                  [&] { return job->done.load(std::memory_order_acquire); });
}
} // namespace Libs::Graphics
