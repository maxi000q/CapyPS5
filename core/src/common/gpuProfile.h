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

// The Adreno model number when the selected device is one ("Adreno (TM) 740" -> 740), else 0.
// Recorded next to the vendor; the frame generator's default keys off it.
inline std::atomic<uint32_t> g_adreno_model {0};

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
        // "Adreno (TM) 740" -> 740: the model generation decides the frame-generator default.
        uint32_t adreno_model = 0;
        if (vendor == Vendor::Qualcomm && device_name != nullptr) {
                for (const char* c = device_name; *c != '\0'; ++c) {
                        if (*c >= '0' && *c <= '9') {
                                adreno_model = static_cast<uint32_t>(*c - '0');
                                for (c++; *c >= '0' && *c <= '9'; ++c) {
                                        adreno_model = adreno_model * 10 + static_cast<uint32_t>(*c - '0');
                                }
                                break;
                        }
                }
        }
        g_adreno_model.store(adreno_model, std::memory_order_release);
        g_vendor.store(static_cast<uint32_t>(vendor), std::memory_order_release);
}

[[nodiscard]] inline Vendor GetVendor() {
        return static_cast<Vendor>(g_vendor.load(std::memory_order_acquire));
}

/** True when the game runs on a Mali GPU (Mali-G76 class and newer benefit the most). */
[[nodiscard]] inline bool IsMali() {
        return GetVendor() == Vendor::Arm;
}

/** True on a Qualcomm Adreno GPU (proprietary driver or Turnip). */
[[nodiscard]] inline bool IsAdreno() {
        return GetVendor() == Vendor::Qualcomm;
}

// Console Five Pro (v0.0.10pro phase 2): whether the selected Vulkan device runs an open
// driver (Turnip reports driverName "freedreno"), recorded next to the vendor in
// WindowContext::CreateVulkan. Open drivers compile shaders slower than Qualcomm's
// proprietary one, which the Adreno profile offsets with an extra pipeline worker.
inline std::atomic<uint32_t> g_open_driver {0};

inline void SetOpenDriver(const char* driver_name) {
        uint32_t open = 0;
        if (driver_name != nullptr) {
                if (std::strncmp(driver_name, "freedreno", 9) == 0 ||
                    std::strncmp(driver_name, "Turnip", 6) == 0 ||
                    std::strncmp(driver_name, "panvk", 5) == 0 ||
                    std::strncmp(driver_name, "Mesa", 4) == 0) {
                        open = 1;
                }
        }
        g_open_driver.store(open, std::memory_order_release);
}

[[nodiscard]] inline bool IsOpenDriver() {
        return g_open_driver.load(std::memory_order_acquire) != 0;
}

/**
 * Whether this GPU has headroom for the frame generator's per-tick composite (the
 * default of `debug.capy.frame_gen`). Mali parts are bandwidth-bound UMA designs and
 * pre-7xx Adrenos sit too close to saturation: the generator's extra work would eat the
 * headroom it was meant to spend, so its default is OFF there. The frontend pushes its
 * own GPU-gated default at every launch, so this fallback mainly governs adb-set values
 * and non-mobile hosts (where it stays permissive).
 */
[[nodiscard]] inline bool FrameGenerationHeadroom() {
        switch (GetVendor()) {
                case Vendor::Arm:      return false; // every Mali generation
                case Vendor::Qualcomm: return g_adreno_model.load(std::memory_order_acquire) >= 700;
                default:               return true;  // desktop/unknown hosts stay permissive
        }
}

/**
 * The Adreno performance profile (v0.0.10pro phase 2): Snapdragon-tuned defaults with the
 * same graphics - one extra pipeline-compile worker (Turnip compiles slower than the
 * proprietary driver, so shader warm-up during material streaming finishes sooner), a 128 MiB
 * streaming buffer (streaming-heavy games wait on the GPU less) and one more swapchain image
 * (MAILBOX stops throttling vkAcquireNextImageKHR when the emulator runs ahead of a 90/120 Hz
 * display). `debug.capy.adreno_profile` (frontend setting "Adreno GPU optimization"):
 * 2 = auto (the default; on only when an Adreno GPU is present), 1 = force on, 0 = force off.
 */
[[nodiscard]] inline bool AdrenoProfileEnabled() {
        static DebugFlags::CachedValue profile("debug.capy.adreno_profile", 2);
        switch (profile.Get()) {
                case 1: return true;
                case 0: return false;
                default: return IsAdreno();
        }
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

/**
 * The framebuffer-compression profile (v0.0.9proQ1): render targets are created with a
 * single-format view list, which lets the driver keep its lossless framebuffer compression
 * alive — AFBC (Arm Frame Buffer Compression) on Mali, UBWC on Adreno. An open-ended
 * eMutableFormat (no list) forces both driver families to disable compression, roughly
 * doubling the memory traffic of every render target store/reload on GPUs that are
 * bandwidth-bound. Games that reinterpret target memory (view it in another format) pay a
 * one-time image copy; the range is then remembered (FormatReinterpret) so images of that
 * memory are created fully mutable again. Identical pixels: both compressors are lossless.
 * `debug.capy.rt_format_list`: 1 on, 0 off, 2 auto (on for Mali and Adreno, the default).
 */
[[nodiscard]] inline bool FramebufferCompressionProfileEnabled() {
        static DebugFlags::CachedValue profile("debug.capy.rt_format_list", 2);
        switch (profile.Get()) {
                case 1: return true;
                case 0: return false;
                default: {
                        const auto vendor = GetVendor();
                        return vendor == Vendor::Arm || vendor == Vendor::Qualcomm;
                }
        }
}

} // namespace Common::GpuProfile

#endif /* COMMON_GPU_PROFILE_H */
