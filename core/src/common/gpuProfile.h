// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef COMMON_GPU_PROFILE_H
#define COMMON_GPU_PROFILE_H

#include <atomic>
#include <cstdint>
#include <cstring>

#include "common/debugFlags.h"

// GPU vendor of the selected Vulkan device, recorded once in WindowContext::CreateVulkan
// (v0.0.9pro). Consumers (render-pass clears, stream-buffer sizing, pipeline workers) read
// the cached vendor to pick per-GPU defaults, so every GPU family gets tuned settings
// without touching what is drawn.
namespace Common::GpuProfile {

enum class Vendor : uint32_t {
        Unknown = 0,
        Arm,         // Mali (also PanVK, the open-source Mali driver)
        Qualcomm,    // Adreno (also Turnip)
        Imagination, // PowerVR
        Intel,
        Nvidia,
        Amd,
        Apple,
};

inline std::atomic<uint32_t> g_vendor {static_cast<uint32_t>(Vendor::Unknown)};

inline void SetVendor(uint32_t vendor_id, const char* device_name) {
        Vendor vendor = Vendor::Unknown;
        if (device_name != nullptr) {
                // The name is authoritative: custom drivers (Turnip, PanVK) report their own
                // vendor ids, but keep the "Mali-*" / "Adreno *" device names of the hardware.
                if (std::strncmp(device_name, "Mali", 4) == 0) {
                        vendor = Vendor::Arm;
                } else if (std::strncmp(device_name, "Adreno", 6) == 0) {
                        vendor = Vendor::Qualcomm;
                }
        }
        if (vendor == Vendor::Unknown) {
                switch (vendor_id) {
                        case 0x13B5: vendor = Vendor::Arm; break;         // Arm Mali
                        case 0x5143: vendor = Vendor::Qualcomm; break;    // Qualcomm Adreno
                        case 0x1010: vendor = Vendor::Imagination; break; // Imagination PowerVR
                        case 0x8086: vendor = Vendor::Intel; break;
                        case 0x10DE: vendor = Vendor::Nvidia; break;
                        case 0x1002: vendor = Vendor::Amd; break;
                        case 0x106B: vendor = Vendor::Apple; break;
                        default: break;
                }
        }
        g_vendor.store(static_cast<uint32_t>(vendor), std::memory_order_release);
}

[[nodiscard]] inline Vendor GetVendor() {
        return static_cast<Vendor>(g_vendor.load(std::memory_order_acquire));
}

/** True when the game runs on a Mali GPU (Mali-G76 class and newer benefit the most). */
[[nodiscard]] inline bool IsMali() {
        return GetVendor() == Vendor::Arm;
}

/**
 * The Mali performance profile (v0.0.9pro): bandwidth-tuned defaults for Mali GPUs with the
 * same graphics — hardware render-target clears that keep AFBC compression alive, a larger
 * streaming buffer and one more pipeline-compile worker. `debug.capy.mali_profile` (frontend
 * setting "Mali GPU optimization"): 2 = auto (the default; on only when a Mali GPU is
 * present), 1 = force on, 0 = force off (the defaults of every other GPU).
 */
[[nodiscard]] inline bool MaliProfileEnabled() {
        static DebugFlags::CachedValue profile("debug.capy.mali_profile", 2);
        switch (profile.Get()) {
                case 1: return true;
                case 0: return false;
                default: return IsMali();
        }
}

} // namespace Common::GpuProfile

#endif /* COMMON_GPU_PROFILE_H */
