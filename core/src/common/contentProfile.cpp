// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "common/contentProfile.h"

#include "common/debugFlags.h"

#include <atomic>
#include <cstdint>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

// Runtime workload classification. See contentProfile.h for the design; the
// short version: the engine measures its own command stream (draw sizes,
// blend/mesh/multi-target mix, compute dispatches, colour-target extents),
// folds one measurement per guest frame and, once two consecutive 32-frame
// windows agree on a workload family, locks that family's engine policy for
// the rest of the run. No content identity is read, stored or compared
// anywhere in this file - only counts and ratios of the engine's own work.
namespace Common::ContentProfile {

namespace {

// ------------------------------------------------------------------ tuning

// One classification window = 32 guest frames. At 60 flips/s that is half a
// second of gameplay; at low frame rates it stretches, but the observations
// are per frame, not per second, so the mix is measured either way.
constexpr uint32_t kWindowFrames = 32;

// A window with less than this much work in it (boot logos, static menus,
// loading screens) carries no classification signal - it neither locks nor
// breaks a class by itself, it simply does not count. The per-frame floor
// matches family 502's own draw floor (60): a small-scene 3D title drawing 60
// heavy geometry passes per frame is real 3D work, not a menu.
constexpr uint64_t kWindowMinDraws        = 1920;
constexpr double   kWindowMinDrawsPerFrame = 60.0;

// Consecutive agreeing windows needed to lock a class, and to re-lock a
// different one once another class already holds the lock (transitions must
// be more decisive than the initial pick so menu-to-gameplay switches cannot
// thrash the engine policy back and forth).
constexpr uint32_t kLockStreak   = 2;
constexpr uint32_t kRelockStreak = 4;

// Colour targets at or above this many pixels count as multi-megapixel class
// (3840x2160 is 8.29M; 2560x1440 at 3.69M stays below on purpose - the
// policy targets the bandwidth regime of the biggest consumer targets).
constexpr uint64_t kHighResPixels = 5832000ull; // 3840x2160 * 0.7

// ------------------------------------------------------------------ counters
//
// Producers run on the command-processing thread; OnFrameEnd runs on the
// flip thread and exchanges everything to zero. Relaxed ordering is enough:
// read-modify-writes on one atomic are totally ordered, so the fold sees
// every increment of the frame exactly once. The ratio counters are packed
// three-to-a-word (blend / mesh / multi-target draw counts, 21 bits each -
// two million draws per frame would be needed to overflow, and the fold runs
// every frame) so a draw costs three relaxed RMWs, not six.

constexpr uint64_t kRatioBlendShift = 0;
constexpr uint64_t kRatioMeshShift  = 21;
constexpr uint64_t kRatioMrtShift   = 42;
constexpr uint64_t kRatioOne        = 1ull;

std::atomic<uint64_t> g_draws {0};
std::atomic<uint64_t> g_ratios {0};
std::atomic<uint64_t> g_index {0};
std::atomic<uint64_t> g_dispatches {0};
std::atomic<uint64_t> g_pixels {0};

// ------------------------------------------------------------------ classification state
//
// Everything in this block is touched only from OnFrameEnd (the flip thread)
// except the atomics the queries read, so the ring and the streak need no
// synchronization of their own.

struct WindowStats {
        bool     sparse              = true;
        double   draws_per_frame     = 0.0;
        double   alpha_ratio         = 0.0;
        double   mesh_ratio          = 0.0;
        double   mrt_ratio           = 0.0;
        double   avg_index_per_draw  = 0.0;
        double   dispatches_per_frame = 0.0;
        uint64_t max_pixels          = 0;
};

// The per-class gates. All families describe how the engine's work is
// SHAPED, never what the content is: a family matches any content whose
// measured mix falls inside its ranges. Families are checked most-specific
// first; each pair of ranges is disjoint by construction (the 500/501 alpha
// bands do not overlap and 502 excludes 501's exact conjunction), so a
// window can never match two families at once.
WorkloadClass ClassifyWindow(const WindowStats& s) {
        if (s.sparse) {
                return WorkloadClass::None;
        }
        // Family 500 - high-throughput deferred 3D: large draws (tens of
        // thousands of indices each), deferred-style multi-target passes,
        // geometry/compute pipelines in active use, transparency present but
        // not dominant. v0.0.10pro phase 4: the transparency band is
        // [0, 0.55) - the old [0.02, 0.45] had two dead zones (a fully
        // opaque scene below 0.02, and the 0.45..0.55 gap between the two
        // families) that a plain forward renderer could land in and classify
        // as nothing at all.
        const bool family500 = s.alpha_ratio < 0.55 &&
                               s.avg_index_per_draw >= 4000.0 &&
                               (s.mesh_ratio >= 0.01 || s.dispatches_per_frame >= 8.0) &&
                               s.mrt_ratio >= 0.04 && s.draws_per_frame >= 200.0;
        if (family500) {
                return WorkloadClass::Turbo500;
        }
        // Family 501 - layered alpha compositing: the majority of draws blend,
        // draw sizes are sprite-scale, geometry pipelines are essentially absent,
        // and the frame count is high (thousands of small layers).
        const bool family501 = s.alpha_ratio >= 0.55 && s.avg_index_per_draw <= 3000.0 &&
                               s.mesh_ratio <= 0.02 && s.draws_per_frame >= 250.0;
        if (family501) {
                return WorkloadClass::Turbo501;
        }
        // Family 502 (v0.0.10pro phase 4, Game502Turbo) - forward-rendered 3D:
        // real geometry volume in each draw, without the deferred signature
        // (multi-target passes, mesh/compute pipelines) the 500 gates demand.
        // Everything geometry-heavy that the two families above do not claim
        // lands here: fully opaque forward scenes, single-target action
        // renderers, transparency in the old 0.45..0.55 dead zone. The draw
        // floor is deliberately low (60/frame): a forward 3D title can batch
        // its whole scene into a few dozen large draws and still be completely
        // GPU-bound. Sprite-scale work (menus, text, 2D layers - draws of a
        // handful of indices) stays far below the 700-index geometry bar.
        const bool family502 = (s.alpha_ratio < 0.55 || s.avg_index_per_draw > 3000.0) &&
                               s.avg_index_per_draw >= 700.0 && s.draws_per_frame >= 60.0;
        if (family502) {
                return WorkloadClass::Turbo502;
        }
        return WorkloadClass::None;
}

std::atomic<uint32_t> g_class {0};
std::atomic<uint32_t> g_high_res {0};
std::atomic<uint32_t> g_placement_generation {0};

WorkloadClass g_streak_class     = WorkloadClass::None;
uint32_t      g_streak_count     = 0;
uint32_t      g_frames_seen      = 0;
uint32_t      g_high_res_streak  = 0;

uint64_t g_ring_draws[kWindowFrames]     = {};
uint64_t g_ring_ratios[kWindowFrames]    = {};
uint64_t g_ring_index[kWindowFrames]     = {};
uint64_t g_ring_dispatches[kWindowFrames] = {};
uint64_t g_ring_pixels[kWindowFrames]    = {};

uint64_t RatioField(uint64_t packed, uint64_t shift) {
        return (packed >> shift) & 0x1FFFFFull;
}

void LogWindow(const WindowStats& s, WorkloadClass window_class, WorkloadClass locked) {
#if defined(__ANDROID__)
        static DebugFlags::CachedValue telemetry("debug.capy.workload_log", 0);
        if (telemetry.Get() == 0) {
                return;
        }
        __android_log_print(
            ANDROID_LOG_INFO, "CapyPS5-Profile",
            "workload window: draws/frame=%.0f alpha=%.2f mesh=%.3f mrt=%.2f idx=%.0f "
            "dispatch/frame=%.0f maxpx=%.1fM -> window=%u locked=%u",
            s.draws_per_frame, s.alpha_ratio, s.mesh_ratio, s.mrt_ratio, s.avg_index_per_draw,
            s.dispatches_per_frame, double(s.max_pixels) / 1000000.0,
            static_cast<uint32_t>(window_class), static_cast<uint32_t>(locked));
#else
        (void)s;
        (void)window_class;
        (void)locked;
#endif
}

void LockClass(WorkloadClass cls) {
        g_class.store(static_cast<uint32_t>(cls), std::memory_order_release);
        // Compile workers compare this against their last pin and re-pin when it
        // moves: the CPU-bound family demotes them to the efficiency cluster, a
        // re-lock back lifts them onto the performance tier again.
        g_placement_generation.fetch_add(1, std::memory_order_acq_rel);
#if defined(__ANDROID__)
        static DebugFlags::CachedValue telemetry("debug.capy.workload_log", 0);
        if (telemetry.Get() != 0) {
                __android_log_print(ANDROID_LOG_INFO, "CapyPS5-Profile",
                                    "workload class locked: %u", static_cast<uint32_t>(cls));
        }
#endif
}

} // namespace

// ------------------------------------------------------------------ producers

void ObserveDraw(uint32_t index_count, uint32_t instance_count, bool blend_enable,
                 uint32_t color_targets, bool mesh_path, uint32_t target_width,
                 uint32_t target_height) {
        g_draws.fetch_add(1, std::memory_order_relaxed);
        uint64_t ratio = 0;
        if (blend_enable) {
                ratio |= kRatioOne << kRatioBlendShift;
        }
        if (mesh_path) {
                ratio |= kRatioOne << kRatioMeshShift;
        }
        if (color_targets > 1) {
                ratio |= kRatioOne << kRatioMrtShift;
        }
        if (ratio != 0) {
                g_ratios.fetch_add(ratio, std::memory_order_relaxed);
        }
        // The draw size discriminator is the index count of one draw command:
        // sprite layers issue thousands of tiny draws, heavy geometry issues
        // fewer but far larger ones. (Instance counts are deliberately NOT
        // multiplied in - a six-index quad instanced a hundred times is still a
        // sprite-shaped draw for this purpose.)
        g_index.fetch_add(index_count, std::memory_order_relaxed);
        (void)instance_count;
        if (target_width != 0 && target_height != 0) {
                const uint64_t pixels = static_cast<uint64_t>(target_width) * target_height;
                uint64_t current = g_pixels.load(std::memory_order_relaxed);
                while (pixels > current && !g_pixels.compare_exchange_weak(current, pixels,
                                                                            std::memory_order_relaxed)) {
                }
        }
}

void ObserveDispatch() {
        g_dispatches.fetch_add(1, std::memory_order_relaxed);
}

void OnFrameEnd() {
        const uint64_t draws   = g_draws.exchange(0, std::memory_order_relaxed);
        const uint64_t ratios  = g_ratios.exchange(0, std::memory_order_relaxed);
        const uint64_t index   = g_index.exchange(0, std::memory_order_relaxed);
        const uint64_t dispatches = g_dispatches.exchange(0, std::memory_order_relaxed);
        const uint64_t pixels  = g_pixels.exchange(0, std::memory_order_relaxed);

        const uint32_t slot = g_frames_seen % kWindowFrames;
        g_ring_draws[slot] = draws;
        g_ring_ratios[slot] = ratios;
        g_ring_index[slot] = index;
        g_ring_dispatches[slot] = dispatches;
        g_ring_pixels[slot] = pixels;
        g_frames_seen++;

        // Fold one window every kWindowFrames frames.
        if (g_frames_seen < kWindowFrames || (g_frames_seen % kWindowFrames) != 0) {
                return;
        }

        WindowStats stats;
        uint64_t total_draws = 0;
        uint64_t total_blend = 0;
        uint64_t total_mesh  = 0;
        uint64_t total_mrt   = 0;
        uint64_t total_index = 0;
        uint64_t total_dispatches = 0;
        for (uint32_t i = 0; i < kWindowFrames; i++) {
                total_draws      += g_ring_draws[i];
                total_blend      += RatioField(g_ring_ratios[i], kRatioBlendShift);
                total_mesh       += RatioField(g_ring_ratios[i], kRatioMeshShift);
                total_mrt        += RatioField(g_ring_ratios[i], kRatioMrtShift);
                total_index      += g_ring_index[i];
                total_dispatches += g_ring_dispatches[i];
                if (g_ring_pixels[i] > stats.max_pixels) {
                        stats.max_pixels = g_ring_pixels[i];
                }
        }
        stats.draws_per_frame      = double(total_draws) / double(kWindowFrames);
        stats.alpha_ratio          = total_draws != 0 ? double(total_blend) / double(total_draws) : 0.0;
        stats.mesh_ratio           = total_draws != 0 ? double(total_mesh) / double(total_draws) : 0.0;
        stats.mrt_ratio            = total_draws != 0 ? double(total_mrt) / double(total_draws) : 0.0;
        stats.avg_index_per_draw   = total_draws != 0 ? double(total_index) / double(total_draws) : 0.0;
        stats.dispatches_per_frame = double(total_dispatches) / double(kWindowFrames);
        stats.sparse               = total_draws < kWindowMinDraws ||
                                     stats.draws_per_frame < kWindowMinDrawsPerFrame;

        // The resolution latch is independent of the workload class and of the
        // sparseness gate: a window that only shows the game's big colour targets
        // still counts for it, and once two consecutive windows have seen
        // multi-megapixel targets the policy stays on for the run (render-scale
        // dips must not flip the engine policy back and forth).
        if (stats.max_pixels >= kHighResPixels) {
                g_high_res_streak++;
                if (g_high_res_streak >= 2) {
                        g_high_res.store(1, std::memory_order_release);
                }
        } else {
                g_high_res_streak = 0;
        }

        const WorkloadClass window_class = ClassifyWindow(stats);
        const WorkloadClass locked =
            static_cast<WorkloadClass>(g_class.load(std::memory_order_acquire));
        LogWindow(stats, window_class, locked);

        // Hysteresis: no signal breaks the streak; agreement builds it; the
        // initial lock needs kLockStreak consecutive windows, a change of mind
        // needs kRelockStreak.
        if (window_class == WorkloadClass::None) {
                g_streak_count = 0;
                return;
        }
        if (g_streak_class != window_class) {
                g_streak_class = window_class;
                g_streak_count = 1;
        } else {
                g_streak_count++;
        }
        if (locked == WorkloadClass::None) {
                if (g_streak_count >= kLockStreak) {
                        LockClass(window_class);
                }
                return;
        }
        if (window_class != locked && g_streak_count >= kRelockStreak) {
                LockClass(window_class);
        }
}

// ------------------------------------------------------------------ queries

bool Profile500Enabled() {
        static DebugFlags::CachedValue force("debug.capy.profile500", -1);
        switch (force.Get()) {
                case 1: return true;
                case 0: return false;
                default: break;
        }
        return g_class.load(std::memory_order_acquire) == static_cast<uint32_t>(WorkloadClass::Turbo500);
}

bool Profile501Enabled() {
        static DebugFlags::CachedValue force("debug.capy.profile501", -1);
        switch (force.Get()) {
                case 1: return true;
                case 0: return false;
                default: break;
        }
        return g_class.load(std::memory_order_acquire) == static_cast<uint32_t>(WorkloadClass::Turbo501);
}

bool Profile502Enabled() {
        static DebugFlags::CachedValue force("debug.capy.profile502", -1);
        switch (force.Get()) {
                case 1: return true;
                case 0: return false;
                default: break;
        }
        return g_class.load(std::memory_order_acquire) == static_cast<uint32_t>(WorkloadClass::Turbo502);
}

bool HighResolutionPolicyActive() {
        static DebugFlags::CachedValue force("debug.capy.profile4k", -1);
        switch (force.Get()) {
                case 1: return true;
                case 0: return false;
                default: break;
        }
        return g_high_res.load(std::memory_order_acquire) != 0;
}

WorkloadClass ActiveClass() {
        return static_cast<WorkloadClass>(g_class.load(std::memory_order_acquire));
}

uint32_t PlacementGeneration() {
        return g_placement_generation.load(std::memory_order_acquire);
}

// ------------------------------------------------------------------ lifecycle

void ResetForNextGame() {
        // Observers first (a draw racing this reset can only lose its own count
        // for the frame the reset happens in - harmless), then the classification
        // state, then the generation bump so placement follows the new run.
        g_draws.store(0, std::memory_order_relaxed);
        g_ratios.store(0, std::memory_order_relaxed);
        g_index.store(0, std::memory_order_relaxed);
        g_dispatches.store(0, std::memory_order_relaxed);
        g_pixels.store(0, std::memory_order_relaxed);

        g_class.store(0, std::memory_order_release);
        g_high_res.store(0, std::memory_order_release);
        g_streak_class    = WorkloadClass::None;
        g_streak_count    = 0;
        g_frames_seen     = 0;
        g_high_res_streak = 0;
        for (uint32_t i = 0; i < kWindowFrames; i++) {
                g_ring_draws[i]      = 0;
                g_ring_ratios[i]     = 0;
                g_ring_index[i]      = 0;
                g_ring_dispatches[i] = 0;
                g_ring_pixels[i]     = 0;
        }

        // Workers re-pin onto the default placement for the new run's warm-up;
        // the new game's own class (when it locks) moves them again.
        g_placement_generation.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace Common::ContentProfile
