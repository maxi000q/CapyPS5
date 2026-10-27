// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// Per-vendor spirv-opt pass lists (see spirvOptimizer.h). The SPIRV-Tools
// optimizer is already linked (spirv-tools-opt); this file only wires it into
// the shader compilation path with a conservative, interface-preserving pass
// list and a validate-before-replace guard.

#include "graphics/host_gpu/renderer/pipeline/spirvOptimizer.h"

#include "common/debugFlags.h"
#include "common/gpuProfile.h"
#include "common/logging/log.h"

#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>

#include <atomic>
#include <cstring>

namespace Libs::Graphics::SpirvOptimizer {

namespace {

// Logged once, after the first module of the session: early confirmation the
// optimizer is active plus its savings estimate. The counters are relaxed;
// they are diagnostics, not synchronization.
struct SessionStats {
        std::atomic<uint64_t> modules {0};
        std::atomic<uint64_t> words_before {0};
        std::atomic<uint64_t> words_after {0};
        std::atomic<uint64_t> rejected {0};
        std::atomic<bool>     logged {false};
};

SessionStats& Stats() {
        static SessionStats stats;
        return stats;
}

const char* StageName(ShaderType stage) {
        switch (stage) {
                case ShaderType::Vertex: return "vs";
                case ShaderType::Fragment: return "ps";
                case ShaderType::Compute: return "cs";
                case ShaderType::Geometry: return "gs";
                case ShaderType::Export: return "es";
                case ShaderType::Hull: return "hs";
                case ShaderType::Local: return "ls";
                case ShaderType::Mesh: return "mesh";
                case ShaderType::Task: return "task";
                default: return "shader";
        }
}

enum class PassList : uint32_t { None = 0, Light, Full };

PassList VendorPassList() {
        // `debug.capy.spirv_opt`: 0 = off everywhere, 1 = on everywhere,
        // 2 = auto (on for Arm/Mali and Qualcomm/Adreno - the vendors whose
        // compilers measurably benefit).
        static Common::DebugFlags::CachedValue enabled("debug.capy.spirv_opt", 2);
        switch (enabled.Get()) {
                case 0: return PassList::None;
                case 1: return PassList::Full;
                default: break;
        }
        switch (Common::GpuProfile::GetVendor()) {
                case Common::GpuProfile::Vendor::Arm: return PassList::Full;
                case Common::GpuProfile::Vendor::Qualcomm: return PassList::Light;
                default: return PassList::None;
        }
}

void RegisterCommonPasses(spvtools::Optimizer& optimizer) {
        // Purely local, semantics- and interface-preserving cleanups. Ordered
        // so early passes expose work for later ones.
        optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass());
        optimizer.RegisterPass(spvtools::CreateBlockMergePass());
        optimizer.RegisterPass(spvtools::CreateInsertExtractElimPass());
        optimizer.RegisterPass(spvtools::CreateDeadInsertElimPass());
        optimizer.RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass());
        optimizer.RegisterPass(spvtools::CreateLocalMultiStoreElimPass());
        optimizer.RegisterPass(spvtools::CreateLocalRedundancyEliminationPass());
        optimizer.RegisterPass(spvtools::CreateCCPPass());
        optimizer.RegisterPass(spvtools::CreateSimplificationPass());
        optimizer.RegisterPass(spvtools::CreateStrengthReductionPass());
        optimizer.RegisterPass(spvtools::CreateVectorDCEPass());
        optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass());
        optimizer.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
        optimizer.RegisterPass(spvtools::CreateEliminateDeadConstantPass());
        optimizer.RegisterPass(spvtools::CreateUnifyConstantPass());
        optimizer.RegisterPass(spvtools::CreateRemoveDuplicatesPass());
        optimizer.RegisterPass(spvtools::CreateCFGCleanupPass());
        optimizer.RegisterPass(spvtools::CreateCompactIdsPass());
}

void RegisterFullPasses(spvtools::Optimizer& optimizer) {
        // The Mali list: everything above plus the cross-block global passes.
        // Arm's compiler benefits the most from small, straight-line modules -
        // its SPIR-V front end is the slowest of the mobile vendors.
        RegisterCommonPasses(optimizer);
        // RedundancyElimination is the global value numbering pass.
        optimizer.RegisterPass(spvtools::CreateRedundancyEliminationPass());
        optimizer.RegisterPass(spvtools::CreateTrimCapabilitiesPass());
        // A second sweep: the global passes expose more dead code, and the
        // final compact-ids shrinks what they left behind.
        optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass());
        optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass());
        optimizer.RegisterPass(spvtools::CreateVectorDCEPass());
        optimizer.RegisterPass(spvtools::CreateCompactIdsPass());
}

void RegisterLightPasses(spvtools::Optimizer& optimizer) {
        // The Adreno list: the local cleanups without the structural global
        // passes, which the Qualcomm and Turnip compilers do not need.
        RegisterCommonPasses(optimizer);
}

bool ValidateModule(const std::vector<uint32_t>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        return tools.Validate(spirv.data(), spirv.size());
}

void LogSummary() {
        auto& stats = Stats();
        if (stats.logged.exchange(true, std::memory_order_relaxed)) {
                return;
        }
        const auto modules = stats.modules.load(std::memory_order_relaxed);
        if (modules == 0) {
                return;
        }
        const auto before = stats.words_before.load(std::memory_order_relaxed);
        const auto after  = stats.words_after.load(std::memory_order_relaxed);
        const auto rejected = stats.rejected.load(std::memory_order_relaxed);
        LOGF("spirv-opt: %llu modules, %llu -> %llu words (%.1f%% smaller), %llu kept unoptimized\n",
             static_cast<unsigned long long>(modules), static_cast<unsigned long long>(before),
             static_cast<unsigned long long>(after),
             before != 0 ? 100.0 * static_cast<double>(before - after) / static_cast<double>(before)
                         : 0.0,
             static_cast<unsigned long long>(rejected));
}

} // namespace

void Optimize(ShaderType stage, std::vector<uint32_t>& spirv) {
        const auto list = VendorPassList();
        if (list == PassList::None || spirv.size() < 16) {
                return;
        }

        spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
        optimizer.SetMessageConsumer([stage](spv_message_level_t level, const char*,
                                             const spv_position_t& position, const char* message) {
                if (level == SPV_MSG_ERROR || level == SPV_MSG_INTERNAL_ERROR) {
                        LOGF("spirv-opt (%s) line %u: %s\n", StageName(stage),
                             static_cast<unsigned>(position.line), message);
                }
        });
        if (list == PassList::Full) {
                RegisterFullPasses(optimizer);
        } else {
                RegisterLightPasses(optimizer);
        }

        auto&    stats        = Stats();
        const auto words_before = spirv.size();
        stats.words_before.fetch_add(words_before, std::memory_order_relaxed);

        std::vector<uint32_t> optimized;
        if (!optimizer.Run(spirv.data(), spirv.size(), &optimized) || optimized.empty()) {
                stats.rejected.fetch_add(1, std::memory_order_relaxed);
                stats.modules.fetch_add(1, std::memory_order_relaxed);
                LogSummary();
                return;
        }

        if (!ValidateModule(optimized)) {
                // Never replace a module the validator would reject - the
                // original is known good.
                LOGF("spirv-opt (%s): optimized module failed validation; keeping the original\n",
                     StageName(stage));
                stats.rejected.fetch_add(1, std::memory_order_relaxed);
                stats.modules.fetch_add(1, std::memory_order_relaxed);
                LogSummary();
                return;
        }

        spirv = std::move(optimized);
        stats.modules.fetch_add(1, std::memory_order_relaxed);
        stats.words_after.fetch_add(spirv.size(), std::memory_order_relaxed);
        LogSummary();
}

} // namespace Libs::Graphics::SpirvOptimizer
