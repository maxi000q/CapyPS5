// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef COMMON_ENGINE_POLICY_H
#define COMMON_ENGINE_POLICY_H

#include <cstdint>

#include "common/contentProfile.h"

// Engine tuning table: one place that maps (GPU vendor profile x workload
// tier) to the numeric engine defaults (v0.0.9proQ4 round 5).
//
// Rounds 1-4 built the knob ladder one site at a time, and every site treated
// the two mobile GPU families as one: "Mali or Adreno" shared the same submit
// batch, the same read-ahead windows and the same worker counts, with the
// render-path exceptions (render-pass mode 0, no depth kick, the full spirv-opt
// list, no extra swapchain image) living at their own call sites. But the two
// families are not the same machine:
//
//   - Arm's proprietary driver serializes every vkQueueSubmit into
//     kernel-visible command streams and builds a tiler job chain per pass;
//     its fixed per-submit cost is the largest of the mobile vendors. Deeper
//     submit batches amortize exactly that cost, so Mali batches deeper than
//     Adreno at every tier: 24/32/40 against 16/24/32.
//
//   - Mali devices are UMA and bandwidth-bound: the emulator's CPU and the GPU
//     share one LPDDR bus, and the median Mali device is a mid-range SoC
//     (Dimensity 7/8xxx, Exynos 1xxx) whose DRAM is slower still. Two
//     consequences. First, a read-ahead window fired at a scattered fault
//     copies bytes the guest never reads - on Mali that junk steals bandwidth
//     from the GPU, so the BASE windows run at half the Adreno tier of the
//     same class; the ADAPTIVE CAPS stay shared, because a cap only binds
//     after several consecutive sweep-proving doublings, by which point the
//     stream has proven itself real on any vendor. Second, every guest
//     page-walk miss costs DRAM time on that shared bus, so the regular-game
//     read cache takes 8192 slots (32 MiB coverage, 64 KiB of tags per
//     thread) against the flagship-SoC 4096: no benefit for sets under
//     16 MiB, conflict-miss removal for the 16-32 MiB sets the rounds 3-4
//     comments describe, and `debug.capy.read_cache_slots` still overrides.
//
//   - Arm's backend compiler is the slowest shader stage on mobile (the
//     reason the Mali profile's +1 compile worker exists). Under the
//     throughput tier - whose whole pipeline warm-up lands in the first
//     minutes - Mali adds TWO extra workers (pool cap 8) against Adreno's
//     one (cap 7). The CPU-bound compositing tier keeps its worker step-down
//     onto the efficiency cluster on both vendors: that placement follows
//     the workload, not the GPU.
//
//   - The huge-page sweeper reaches newly committed guest memory faster on
//     Mali under the classes (750 ms against 1 s): the MMU walks of a shared
//     bus contend with the GPU, reaching full 2 MiB coverage sooner compounds
//     the TLB win for the whole streaming period, and the sweeper stays
//     nice-8 background work either way.
//
// Knobs that do NOT diverge stay shared on purpose and say so here: the
// adaptive read-ahead CAPS (see above), the streaming buffer (an upload-ring
// property of the content, not of the GPU), the class placement policy (501
// step-down, guest slots, worker re-pinning) and the class-tier values of the
// read cache (16384/8192 - working-set sizes are content-determined). The
// render-path vendor choices (mode 0 hardware clears on Mali, the Adreno
// depth kick, AFBC/UBWC view lists, the spirv-opt pass lists, the Adreno
// extra swapchain image) keep living where they are applied.
//
// Every value below is a default, never an override: an explicit user value
// for any affected knob (setprop debug.capy.*) always wins at the call site,
// and the vendor profiles themselves are kill-switchable
// (debug.capy.mali_profile / debug.capy.adreno_profile / profile500 /
// profile501 / profile502 = 0) before they reach this table.
namespace Common::EnginePolicy {

// The resolved workload tier: which behavioural class (if any) the runtime
// classifier has locked, after the debug.capy.profile500/501/502 kill-switches.
enum class WorkloadTier : uint32_t {
        General        = 0,
        Throughput500  = 500, // high-throughput 3D family
        Compositing501 = 501, // layered alpha compositing family
        Forward3D502   = 502, // forward-rendered 3D family
};

/** The tier whose knob set the engine should be running right now.
 *
 * v0.0.10pro phase 4: the resolution-keyed policy now floors the tier at
 * Throughput500. Rounds 4-5 gave the multi-megapixel policy exactly one lever
 * (the render-pass clear mode) and that lever is a no-op on Mali - mode 0 is
 * already the cheapest clear strategy there, so `SelectMode` ignores the
 * policy on Mali by design. The measured consequence on a Mali device: a 4K
 * game flipped the policy latch and NOTHING changed - no knob moved anywhere.
 * From now on the 4K policy carries the full throughput knob set (deep submit
 * batches, the 16384-slot read cache, the extra compile workers, the fast
 * huge-page sweep) on EVERY vendor, until the game's own class locks and takes
 * over. A locked class always wins over the floor: it is the more specific
 * measurement of the same content. */
[[nodiscard]] inline WorkloadTier CurrentTier() {
        if (Common::ContentProfile::Profile501Enabled()) {
                return WorkloadTier::Compositing501;
        }
        if (Common::ContentProfile::Profile500Enabled()) {
                return WorkloadTier::Throughput500;
        }
        if (Common::ContentProfile::Profile502Enabled()) {
                return WorkloadTier::Forward3D502;
        }
        if (Common::ContentProfile::HighResolutionPolicyActive()) {
                return WorkloadTier::Throughput500;
        }
        return WorkloadTier::General;
}

struct EngineTuning {
        /** Semaphore-free flushes merged into one vkQueueSubmit. */
        uint32_t submit_batch;
        /** Read-ahead window on the first fault of a region, in KiB. */
        uint32_t read_ahead_base_kb;
        /** Adaptive read-ahead ceiling, in bytes (shared across vendors). */
        uint64_t read_ahead_cap;
        /** Guest read-cache slots per thread (power of two). */
        uint32_t read_cache_slots;
        /** Extra pipeline-compile workers for the GPU profile (Mali/Adreno +1). */
        uint32_t compile_profile_extra;
        /** Extra compile workers for the throughput tier (Mali 2, others 1). */
        uint32_t compile_class_extra;
        /** Absolute compile worker pool cap. */
        uint32_t compile_pool_cap;
        /** Huge-page sweeper active cadence, milliseconds. */
        uint32_t thp_active_sweep_ms;
};

// The table. Rows: (vendor profile x tier); the Adreno row is byte-identical
// to the pre-round-5 ladders at every site - the Adreno and no-profile paths
// are regression-locked by the p8 battery, so Mali is the only column that
// moves. When both profiles claim to be active (a forced-on pair of setprops)
// the Mali row wins: its driver model is the more conservative one to assume.
//
// v0.0.10pro phase 4 adds the Forward3D502 row per vendor, at the same values
// as that vendor's Throughput500 row: a forward-rendered 3D title wants the
// same throughput preset (deep submits, big read caches, doubled compile pool
// growth on Mali) - the row exists separately so the two families can diverge
// once on-device measurements say they should.
[[nodiscard]] inline const EngineTuning& TuningFor(bool mali_profile, bool adreno_profile,
                                                   WorkloadTier tier) {
        static const EngineTuning table[] = {
                // ------------------------------------------------- Mali (Arm)
                //       submit base  cap     slots  prof cls cap sweep
                {/*General*/      24, 256, 1ull << 20, 8192, 1, 0, 6, 2000},
                {/*Throughput*/   32, 384, 2ull << 20, 16384, 1, 2, 8, 750},
                {/*Compositing*/  40, 512, 4ull << 20, 8192, 1, 0, 6, 750},
                {/*Forward3D*/    32, 384, 2ull << 20, 16384, 1, 2, 8, 750},
                // ------------------------------------------------- Adreno
                {/*General*/      16, 512, 1ull << 20, 4096, 1, 0, 6, 2000},
                {/*Throughput*/   24, 768, 2ull << 20, 16384, 1, 1, 7, 1000},
                {/*Compositing*/  32, 1024, 4ull << 20, 8192, 1, 0, 6, 1000},
                {/*Forward3D*/    24, 768, 2ull << 20, 16384, 1, 1, 7, 1000},
                // ------------------------------------------------- everything else
                {/*General*/      8, 256, 1ull << 20, 512, 0, 0, 6, 2000},
                {/*Throughput*/   24, 768, 2ull << 20, 16384, 0, 1, 7, 1000},
                {/*Compositing*/  32, 1024, 4ull << 20, 8192, 0, 0, 6, 1000},
                {/*Forward3D*/    24, 768, 2ull << 20, 16384, 0, 1, 7, 1000},
        };
        const auto row = mali_profile ? 0 : (adreno_profile ? 4 : 8);
        switch (tier) {
                case WorkloadTier::Throughput500:  return table[row + 1];
                case WorkloadTier::Compositing501: return table[row + 2];
                case WorkloadTier::Forward3D502:   return table[row + 3];
                default:                           return table[row];
        }
}

} // namespace Common::EnginePolicy

#endif /* COMMON_ENGINE_POLICY_H */
