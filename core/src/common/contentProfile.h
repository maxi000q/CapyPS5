// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef COMMON_CONTENT_PROFILE_H
#define COMMON_CONTENT_PROFILE_H

#include <cstdint>

// Runtime workload profiles.
//
// The GPU profiles (common/gpuProfile.h) tune the engine for the host device;
// content profiles tune it for what is being run. Some workloads - very high
// draw counts with heavy per-draw geometry, or thousands of small blended
// layers, or multi-megapixel render targets - benefit from a different set of
// engine defaults than the general-purpose baseline, while everything else
// must keep the baseline untouched.
//
// Console Five Pro (v0.0.9proQ4 round 4): classification is purely
// behavioural. The engine watches its own command stream - how many draws and
// dispatches each guest frame contains, how large the draws are, what fraction
// of them blend, sample mesh paths or write multiple colour targets, and how
// big the colour targets are - and matches the measured mix against two broad
// workload families:
//
//   class 500  high-throughput 3D - large draws, deferred-style multi-target
//              passes, geometry/compute pipelines in active use, moderate
//              transparency. Needs: the full high-throughput preset (big guest
//              read caches, wide adaptive read-ahead, deep submit batching,
//              an extra compile worker, aggressive huge-page sweeping).
//   class 501  layered alpha compositing - most draws blend, draw sizes are
//              sprite-scale, geometry pipelines nearly absent, and the frame
//              time is guest-CPU-bound (the game logic, not the GPU, is the
//              bottleneck). Needs: the big cores for its own threads - compile
//              workers step down to the efficiency cluster - plus very deep
//              submit batching and the widest read-ahead windows.
//
// Nothing about the running content's identity is read, stored, hashed or
// compared anywhere: no file or directory names, no package identifiers, no
// content markers, no per-title tables. The classifier only ever sees counts
// and ratios of the engine's own work. Any content with the same measured
// mix gets the same engine policy; content with a different mix keeps the
// baseline.
//
// A separate resolution-keyed policy (HighResolutionPolicyActive) applies to
// any content that renders multi-megapixel class targets, regardless of its
// workload class.
//
// The whole mechanism is internal: no setting, no UI, and no log output
// unless explicitly asked for (debug.capy.workload_log). An explicit user
// value for any affected knob always wins over the profile - the profile only
// replaces built-in defaults.
namespace Common::ContentProfile {

enum class WorkloadClass : uint32_t {
	None     = 0,
	Turbo500 = 500, // high-throughput 3D family
	Turbo501 = 501, // layered alpha compositing family
};

// ------------------------------------------------------------------ producers

/**
 * Record one guest draw. All parameters come from the command buffer being
 * processed; nothing about content identity is involved. Cheap (a handful of
 * relaxed atomic adds); called from the command-processing thread.
 */
void ObserveDraw(uint32_t index_count, uint32_t instance_count, bool blend_enable,
                 uint32_t color_targets, bool mesh_path, uint32_t target_width,
                 uint32_t target_height);

/** Record one guest compute dispatch. */
void ObserveDispatch();

/** Fold the frame's counters and run the classifier. Called once per guest frame. */
void OnFrameEnd();

// ------------------------------------------------------------------ queries

/**
 * Whether workload class 500 is active for the content currently running.
 * `debug.capy.profile500`: 1 = force on, 0 = force off, anything else = the
 * classifier decides (the default).
 */
[[nodiscard]] bool Profile500Enabled();

/** Same for workload class 501, via `debug.capy.profile501`. */
[[nodiscard]] bool Profile501Enabled();

/**
 * The resolution-keyed policy: active once multi-megapixel class targets have
 * been observed. `debug.capy.profile4k`: 1 = force on, 0 = force off,
 * anything else = the observation latch decides (the default).
 */
[[nodiscard]] bool HighResolutionPolicyActive();

/** The locked workload class (None until classification settles). */
[[nodiscard]] WorkloadClass ActiveClass();

/**
 * Bumps every time thread-placement-affecting state changes (a class locks,
 * re-locks, or the state resets for the next game). Compile workers compare
 * it against their last pin and re-pin when it moves.
 */
[[nodiscard]] uint32_t PlacementGeneration();

// ------------------------------------------------------------------ lifecycle

/**
 * Drop all observed state and unlock every profile. Called at the top of
 * Emulator::Run for every game, so two games launched from one process never
 * inherit each other's classification.
 */
void ResetForNextGame();

} // namespace Common::ContentProfile

#endif /* COMMON_CONTENT_PROFILE_H */
